// DS3MouseFix - dinput8.dll proxy for Dark Souls III that fixes menu stutter with high polling rate mice.
//
// The problem (measured): while a menu is open, the game's window procedure spends ~1.25 ms on every
// WM_SETCURSOR and another ~1.25 ms on every WM_MOUSEMOVE (in gameplay both take ~0.02 ms). Windows
// coalesces mouse moves, but with a 1000-8000 Hz mouse a new one is waiting as soon as the last one is
// handled, so the main thread can spend 80% of every second on mouse messages instead of frames.
//
// The fix: the game window is subclassed, and WM_MOUSEMOVE / WM_SETCURSOR are passed to the game at
// most once per interval (default 8 ms, i.e. ~125 per second). Movement in between is not lost: the
// latest position is held and delivered when the interval ends, by a short timer if the mouse stopped,
// and right before any click or wheel message so clicks always land where the cursor is.
// The camera is not affected; the game reads it through DirectInput, which this DLL does not touch.
//
// Keyboard icons: DS3 on PC only has Xbox button icons. The game's D3D11CreateDevice import is
// redirected so ID3D11Device::CreateTexture2D can be hooked; any texture whose top mip hashes to a
// file in DS3KeyIcons\<hash>.dds is created from that file instead (same size, format and mips).
// dump_textures = 1 saves every texture the game creates with initial data to texture_dump\, which is
// how the originals were found.
//
// Disable Arxan: the stall tracer showed the game's ~140 ms hitches (about one a minute) are the render
// thread running Arxan anti-tamper code (the second .text section). dearxan (tremwil, Apache-2.0,
// third_party\dearxan) patches all of Arxan's checks out before the game's entry point runs.
//
// Camera auto-follow (no_camera_auto_follow): NOPs the store of the camera's automatic turn toward the
// running direction, so only the mouse turns the camera (see DisableCameraAutoFollow).
//
// DLAA (dlaa.cpp): NVIDIA DLSS anti-aliasing at native resolution; see the comment at the top of dlaa.cpp.
//
// The DLL is only a carrier: it forwards DirectInput8Create to the real System32 dinput8.dll.

#include "shared.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#pragma warning(push)
#pragma warning(disable : 4458) // dearxan.h's C++ wrapper shadows a member; we only use its C API
#include "../third_party/dearxan/include/dearxan.h"
#pragma warning(pop)

// ---- Settings (DS3MouseFix.ini next to the DLL) ----

static int g_intervalMs = 8; // minimum time between mouse messages passed to the game, 0 = off
static bool g_keyboardIcons = true; // replace the textures found in the DS3KeyIcons folder
static bool g_dumpTextures;         // save every texture created with initial data to the texture_dump folder
wchar_t g_dllDir[MAX_PATH];         // ends with a backslash
static LARGE_INTEGER g_qpcFreq;

// ---- dinput8 forwarding ----

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
static DirectInput8Create_t g_realCreate;

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, void* outer)
{
    return g_realCreate ? g_realCreate(instance, version, riid, out, outer) : E_FAIL;
}

// ---- Log (DS3MouseFix.log next to the DLL) ----

void Log(const char* format, ...)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sDS3MouseFix.log", g_dllDir);
    FILE* f;
    if (_wfopen_s(&f, path, L"a") != 0) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---- Import hooking ----

// Points the game's import of dll!name at hook. Returns what the entry held before, or NULL.
static void* HookImport(const char* dll, const char* name, void* hook)
{
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;

    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
    {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, name) != 0) continue;
            void* previous = (void*)slots->u1.Function;
            DWORD oldProtect;
            if (!previous || !VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) return NULL;
            slots->u1.Function = (ULONGLONG)hook;
            VirtualProtect(&slots->u1.Function, sizeof(void*), oldProtect, &oldProtect);
            return previous;
        }
    }
    return NULL;
}

// ---- Texture formats ----

// Bytes per 4x4 block for block-compressed formats, 0 for anything else.
static UINT BlockBytes(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return 8;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;
    default:
        return 0;
    }
}

// Bytes per pixel for the uncompressed formats a game is likely to load from disk, 0 if unknown.
static UINT PixelBytes(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:
        return 1;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 0;
    }
}

// Tight size of one mip: bytes per row (of pixels or 4x4 blocks) and number of rows.
static bool MipLayout(DXGI_FORMAT format, UINT width, UINT height, UINT mip, UINT* rowBytes, UINT* rows)
{
    UINT w = width >> mip, h = height >> mip;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (UINT block = BlockBytes(format))
    {
        *rowBytes = ((w + 3) / 4) * block;
        *rows = (h + 3) / 4;
        return true;
    }
    if (UINT pixel = PixelBytes(format))
    {
        *rowBytes = w * pixel;
        *rows = h;
        return true;
    }
    return false;
}

// FNV-1a over the top mip (tight rows) plus size and format, so it doesn't depend on the row pitch.
static unsigned long long HashTexture(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data)
{
    UINT rowBytes, rows;
    if (!MipLayout(desc->Format, desc->Width, desc->Height, 0, &rowBytes, &rows)) return 0;
    unsigned long long hash = 1469598103934665603ULL;
    UINT header[3] = { desc->Width, desc->Height, (UINT)desc->Format };
    for (int i = 0; i < 12; i++) hash = (hash ^ ((const BYTE*)header)[i]) * 1099511628211ULL;
    for (UINT y = 0; y < rows; y++)
    {
        const BYTE* row = (const BYTE*)data->pSysMem + (size_t)y * data->SysMemPitch;
        for (UINT x = 0; x < rowBytes; x++) hash = (hash ^ row[x]) * 1099511628211ULL;
    }
    return hash;
}

// ---- DDS files (always with the DX10 header, which stores the exact DXGI format) ----

#pragma pack(push, 1)
struct DdsHeader
{
    DWORD magic, size, flags, height, width, pitchOrLinearSize, depth, mipMapCount, reserved1[11];
    DWORD pfSize, pfFlags, pfFourCC, pfRGBBitCount, pfRBitMask, pfGBitMask, pfBBitMask, pfABitMask;
    DWORD caps, caps2, caps3, caps4, reserved2;
    DWORD dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};
#pragma pack(pop)

static void DumpTexture(unsigned long long hash, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%stexture_dump\\%016llx_%ux%u_f%u.dds", g_dllDir, hash, desc->Width, desc->Height, (UINT)desc->Format);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;

    DdsHeader h = {};
    h.magic = 0x20534444; // "DDS "
    h.size = 124;
    h.flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000; // caps, height, width, pixelformat, mipmapcount
    h.height = desc->Height;
    h.width = desc->Width;
    h.mipMapCount = desc->MipLevels;
    h.pfSize = 32;
    h.pfFlags = 0x4; // fourcc
    h.pfFourCC = 0x30315844; // "DX10"
    h.caps = 0x1000 | (desc->MipLevels > 1 ? 0x400008 : 0); // texture (+ mipmap, complex)
    h.dxgiFormat = desc->Format;
    h.resourceDimension = 3; // texture2D
    h.arraySize = 1;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, &h, sizeof(h), &written, NULL);
    for (UINT mip = 0; mip < desc->MipLevels; mip++)
    {
        UINT rowBytes, rows;
        MipLayout(desc->Format, desc->Width, desc->Height, mip, &rowBytes, &rows);
        for (UINT y = 0; y < rows; y++)
            WriteFile(file, (const BYTE*)data[mip].pSysMem + (size_t)y * data[mip].SysMemPitch, rowBytes, &written, NULL);
    }
    CloseHandle(file);
}

// ---- Replacement textures (DS3KeyIcons\<16 hex digit hash>.dds) ----

struct Replacement
{
    unsigned long long hash;
    UINT width, height, mips;
    DXGI_FORMAT format;
    D3D11_SUBRESOURCE_DATA data[16];
    BYTE* bytes;
};
static Replacement g_replacements[64];
static int g_replacementCount;

static void LoadReplacements()
{
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%sDS3KeyIcons\\*.dds", g_dllDir);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
    {
        Log("Keyboard icons: no DS3KeyIcons\\*.dds files found.");
        return;
    }
    do
    {
        if (g_replacementCount == _countof(g_replacements)) break;
        Replacement& r = g_replacements[g_replacementCount];
        r.bytes = NULL;
        if (swscanf_s(found.cFileName, L"%16llx", &r.hash) != 1) continue;

        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%sDS3KeyIcons\\%s", g_dllDir, found.cFileName);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) continue;
        DWORD size = GetFileSize(file, NULL), read = 0;
        r.bytes = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
        bool ok = r.bytes && ReadFile(file, r.bytes, size, &read, NULL) && read == size && size >= sizeof(DdsHeader);
        CloseHandle(file);

        const DdsHeader* h = (const DdsHeader*)r.bytes;
        ok = ok && h->magic == 0x20534444 && h->pfFourCC == 0x30315844 && h->mipMapCount >= 1 && h->mipMapCount <= 16;
        size_t offset = sizeof(DdsHeader);
        if (ok)
        {
            r.width = h->width;
            r.height = h->height;
            r.mips = h->mipMapCount;
            r.format = (DXGI_FORMAT)h->dxgiFormat;
            for (UINT mip = 0; ok && mip < r.mips; mip++)
            {
                UINT rowBytes, rows;
                ok = MipLayout(r.format, r.width, r.height, mip, &rowBytes, &rows) && offset + (size_t)rowBytes * rows <= size;
                if (!ok) break;
                r.data[mip].pSysMem = r.bytes + offset;
                r.data[mip].SysMemPitch = rowBytes;
                r.data[mip].SysMemSlicePitch = rowBytes * rows;
                offset += (size_t)rowBytes * rows;
            }
        }
        if (!ok)
        {
            Log("Keyboard icons: %ls is not a valid DX10 DDS file, skipped.", found.cFileName);
            if (r.bytes) HeapFree(GetProcessHeap(), 0, r.bytes);
            continue;
        }
        g_replacementCount++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    Log("Keyboard icons: %d replacement texture(s) loaded.", g_replacementCount);
}

// ---- D3D11 hooks ----

typedef HRESULT(STDMETHODCALLTYPE* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(WINAPI* D3D11CreateDevice_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static CreateTexture2D_t g_realCreateTexture2D;
static D3D11CreateDevice_t g_realCreateDevice;

static HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                                       const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out)
{
    // Textures loaded from the game's files arrive with their pixels; render targets etc. don't.
    // Only hash textures shaped like a replacement: hashing every texture (up to 4 MB each, byte by
    // byte) on the game's loading path could itself cause hitches.
    bool candidate = data && desc && desc->ArraySize == 1 && desc->MipLevels >= 1;
    if (candidate && !g_dumpTextures)
    {
        candidate = false;
        for (int i = 0; i < g_replacementCount && !candidate; i++)
        {
            const Replacement& r = g_replacements[i];
            candidate = r.width == desc->Width && r.height == desc->Height && r.format == desc->Format && r.mips == desc->MipLevels;
        }
    }
    if (candidate)
    {
        unsigned long long hash = HashTexture(desc, data);
        if (hash)
        {
            if (g_dumpTextures) DumpTexture(hash, desc, data);
            for (int i = 0; i < g_replacementCount; i++)
            {
                const Replacement& r = g_replacements[i];
                if (r.hash != hash) continue;
                if (r.width != desc->Width || r.height != desc->Height || r.format != desc->Format || r.mips != desc->MipLevels)
                {
                    Log("Keyboard icons: %016llx has a different size/format/mip count than the game's texture, skipped.", hash);
                    break;
                }
                HRESULT hr = g_realCreateTexture2D(device, desc, r.data, out);
                Log("Keyboard icons: replaced %016llx (%ux%u) -> 0x%08lx.", hash, desc->Width, desc->Height, hr);
                if (SUCCEEDED(hr)) return hr;
                break;
            }
        }
    }
    return g_realCreateTexture2D(device, desc, data, out);
}

static HRESULT WINAPI HookedD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                              const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
                                              ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
    HRESULT hr = g_realCreateDevice(adapter, type, software, flags, levels, levelCount, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device && !g_realCreateTexture2D && (g_replacementCount > 0 || g_dumpTextures))
    {
        // CreateTexture2D is entry 5 of ID3D11Device's vtable, which d3d11.dll shares between all devices.
        void** vtable = *(void***)*device;
        DWORD oldProtect;
        if (VirtualProtect(&vtable[5], sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            g_realCreateTexture2D = (CreateTexture2D_t)vtable[5];
            vtable[5] = (void*)HookedCreateTexture2D;
            VirtualProtect(&vtable[5], sizeof(void*), oldProtect, &oldProtect);
            Log("D3D11 device created, texture hook installed.");
        }
        else Log("D3D11 device created, but its vtable could not be unprotected (error %lu).", GetLastError());
    }
    if (SUCCEEDED(hr) && device && *device && DlaaEnabled()) DlaaOnDeviceCreated(*device);
    return hr;
}

// ---- Stall tracer (stall_trace = 1): where is the render thread stuck when frames stop? ----
//
// IDXGISwapChain::Present is hooked to note when, and on which thread, each frame is presented. A
// watchdog thread notices when no frame has been presented for StallMs, then samples that thread's
// call stack every SampleMs until frames resume, and logs the stall with module+offset frames.

static bool g_stallTrace;
static const double StallMs = 40, SampleMs = 15;
static volatile LONG64 g_lastPresentQpc;
static volatile DWORD g_presentThread;

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(WINAPI* CreateDXGIFactory_t)(REFIID, void**);
static Present_t g_realPresent;
static CreateSwapChain_t g_realCreateSwapChain;
static CreateDXGIFactory_t g_realCreateFactory;

bool PatchVtable(void** vtable, int index, void* hook, void** original)
{
    if (vtable[index] == hook) return true;
    if (*original && *original != vtable[index]) return false; // a different implementation; leave it
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    if (DlaaEnabled()) DlaaOnPresent(swapChain);
    HRESULT hr = g_realPresent(swapChain, syncInterval, flags);
    if (DlaaEnabled()) DlaaAfterPresent();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_presentThread = GetCurrentThreadId();
    InterlockedExchange64(&g_lastPresentQpc, now.QuadPart);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    HRESULT hr = g_realCreateSwapChain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out)
    {
        // Present is entry 8: IUnknown (3), IDXGIObject (4), IDXGIDeviceSubObject::GetDevice, then Present.
        if (PatchVtable(*(void***)*out, 8, (void*)HookedPresent, (void**)&g_realPresent)) Log("Present hooked.");
        else Log("Could not hook Present.");
    }
    return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void** out)
{
    HRESULT hr = g_realCreateFactory(riid, out);
    // CreateSwapChain is entry 10: IUnknown (3), IDXGIObject (4), EnumAdapters, MakeWindowAssociation,
    // GetWindowAssociation, then CreateSwapChain.
    if (SUCCEEDED(hr) && out && *out && !PatchVtable(*(void***)*out, 10, (void*)HookedCreateSwapChain, (void**)&g_realCreateSwapChain))
        Log("Could not hook CreateSwapChain.");
    return hr;
}

// Walks a suspended thread's stack with the unwind data in each module. Only touches memory and the
// unwind tables, so it can't deadlock on a lock the suspended thread holds (no heap, no loader calls).
static int WalkStack(CONTEXT* ctx, DWORD64* frames, int max)
{
    int n = 0;
    __try
    {
        while (n < max && ctx->Rip)
        {
            frames[n++] = ctx->Rip;
            DWORD64 imageBase;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &imageBase, NULL);
            if (fn)
            {
                void* handlerData;
                DWORD64 establisherFrame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, fn, ctx, &handlerData, &establisherFrame, NULL);
            }
            else if (n == 1)
            {
                ctx->Rip = *(DWORD64*)ctx->Rsp; // leaf function: return address is on top of the stack
                ctx->Rsp += 8;
            }
            else break; // code without unwind data (generated or obfuscated); can't go further safely
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return n;
}

static int CaptureStack(HANDLE thread, DWORD64* frames, int max)
{
    if (SuspendThread(thread) == (DWORD)-1) return 0;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int n = GetThreadContext(thread, &ctx) ? WalkStack(&ctx, frames, max) : 0;
    ResumeThread(thread);
    return n;
}

// "module!nearest_export+0xoffset", or "module+0xoffset" when there's no export close below it.
static void DescribeAddress(DWORD64 address, char* out, size_t size)
{
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module))
    {
        sprintf_s(out, size, "0x%llx", address);
        return;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    BYTE* base = (BYTE*)module;
    DWORD rva = (DWORD)(address - (DWORD64)base);

    const char* best = NULL;
    DWORD bestRva = 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress)
    {
        IMAGE_EXPORT_DIRECTORY* exports = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
        DWORD* functions = (DWORD*)(base + exports->AddressOfFunctions);
        DWORD* names = (DWORD*)(base + exports->AddressOfNames);
        WORD* ordinals = (WORD*)(base + exports->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exports->NumberOfNames; i++)
        {
            DWORD f = functions[ordinals[i]];
            if (f <= rva && f >= bestRva) { bestRva = f; best = (const char*)(base + names[i]); }
        }
    }
    if (best && rva - bestRva < 0x4000) sprintf_s(out, size, "%ls!%s+0x%x", name, best, rva - bestRva);
    else sprintf_s(out, size, "%ls+0x%x", name, rva);
}

struct StackSample
{
    double atMs; // since the last good frame
    int count;
    DWORD64 frames[24];
};

static DWORD WINAPI StallWatchThread(LPVOID)
{
    // A high-resolution waitable timer gives ~1 ms waits without changing the process timer resolution.
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static StackSample samples[16];
    int sampleCount = 0;
    LONG64 stallFrom = 0; // the present the current stall started after
    double nextSampleMs = 0;
    DWORD threadId = 0;
    HANDLE thread = NULL;
    char line[4096], frame[160];

    for (;;)
    {
        if (timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -50000; // 5 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        else Sleep(5);

        LONG64 last = g_lastPresentQpc;
        if (!last) continue;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double sinceMs = (now.QuadPart - last) * 1000.0 / g_qpcFreq.QuadPart;

        if (stallFrom && last != stallFrom)
        {
            // Frames are back: log the stall.
            double totalMs = (last - stallFrom) * 1000.0 / g_qpcFreq.QuadPart;
            Log("Stall %.0f ms on render thread %lu:", totalMs, threadId);
            for (int s = 0; s < sampleCount; s++)
            {
                int len = sprintf_s(line, "  at %3.0f ms:", samples[s].atMs);
                for (int f = 0; f < samples[s].count && len < (int)sizeof(line) - 200; f++)
                {
                    DescribeAddress(samples[s].frames[f], frame, sizeof(frame));
                    len += sprintf_s(line + len, sizeof(line) - len, "%s %s", f ? " <" : "", frame);
                }
                Log("%s", line);
            }
            stallFrom = 0;
            sampleCount = 0;
        }

        if (sinceMs < StallMs) continue;
        if (!stallFrom)
        {
            stallFrom = last;
            nextSampleMs = sinceMs;
            if (g_presentThread != threadId)
            {
                if (thread) CloseHandle(thread);
                threadId = g_presentThread;
                thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
            }
        }
        if (thread && sampleCount < (int)_countof(samples) && sinceMs >= nextSampleMs)
        {
            samples[sampleCount].atMs = sinceMs;
            samples[sampleCount].count = CaptureStack(thread, samples[sampleCount].frames, (int)_countof(samples[0].frames));
            sampleCount++;
            nextSampleMs = sinceMs + SampleMs;
        }
    }
}

// ---- Mouse message throttle (runs on the game's window thread) ----

static const UINT_PTR FlushTimerId = 0xD53F;

static HWND g_gameWindow;
static WNDPROC g_origWndProc;
static bool g_unicodeWindow;

static double g_lastMove, g_lastSetCursor; // ms timestamps of the last messages passed on
static bool g_movePending, g_timerSet;
static WPARAM g_pendingW;
static LPARAM g_pendingL;

static double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / g_qpcFreq.QuadPart;
}

static LRESULT CallGame(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return g_unicodeWindow ? CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam)
                           : CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

static void DeliverPendingMove(HWND hwnd)
{
    if (!g_movePending) return;
    g_movePending = false;
    g_lastMove = NowMs();
    CallGame(hwnd, WM_MOUSEMOVE, g_pendingW, g_pendingL);
}

static LRESULT CALLBACK ThrottleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    double now = NowMs();

    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (now - g_lastMove >= g_intervalMs)
        {
            g_movePending = false;
            g_lastMove = now;
            return CallGame(hwnd, msg, wParam, lParam);
        }
        // Too soon: keep only the newest position and make sure it arrives even if the mouse stops.
        g_movePending = true;
        g_pendingW = wParam;
        g_pendingL = lParam;
        if (!g_timerSet) g_timerSet = SetTimer(hwnd, FlushTimerId, USER_TIMER_MINIMUM, NULL) != 0;
        return 0;

    case WM_TIMER:
        if (wParam != FlushTimerId) break;
        if (g_movePending && now - g_lastMove < g_intervalMs) return 0; // still moving; the next move passes it on
        KillTimer(hwnd, FlushTimerId);
        g_timerSet = false;
        DeliverPendingMove(hwnd);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && now - g_lastSetCursor < g_intervalMs)
            return TRUE; // keep the current cursor; the game sets the same one again shortly
        g_lastSetCursor = now;
        break;

    case WM_DEVICECHANGE: case WM_INPUT_DEVICE_CHANGE:
        // Device changes make the game re-scan its input devices, a known source of hitches.
        if (g_stallTrace) Log("%s wParam=0x%llx", msg == WM_DEVICECHANGE ? "WM_DEVICECHANGE" : "WM_INPUT_DEVICE_CHANGE", (unsigned long long)wParam);
        break;

    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        DeliverPendingMove(hwnd); // the game sees the final position before the click
        break;
    }
    return CallGame(hwnd, msg, wParam, lParam);
}

// ---- Finding and subclassing the game window ----

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM result)
{
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[64];
    GetWindowTextW(hwnd, title, 64);
    if (_wcsnicmp(title, L"DARK SOULS", 10) != 0) return TRUE;
    *(HWND*)result = hwnd;
    return FALSE;
}

static DWORD WINAPI SetupThread(LPVOID)
{
    // The window appears a few seconds after the DLL loads. Keep watching in case the game recreates it.
    for (;;)
    {
        HWND found = NULL;
        EnumWindows(FindGameWindowProc, (LPARAM)&found);
        if (found && found != g_gameWindow)
        {
            bool unicode = IsWindowUnicode(found) != 0;
            WNDPROC current = (WNDPROC)(unicode ? GetWindowLongPtrW(found, GWLP_WNDPROC) : GetWindowLongPtrA(found, GWLP_WNDPROC));
            if (current != ThrottleWndProc)
            {
                g_unicodeWindow = unicode;
                g_origWndProc = current;
                g_gameWindow = found;
                if (unicode) SetWindowLongPtrW(found, GWLP_WNDPROC, (LONG_PTR)ThrottleWndProc);
                else SetWindowLongPtrA(found, GWLP_WNDPROC, (LONG_PTR)ThrottleWndProc);
            }
        }
        Sleep(g_gameWindow ? 2000 : 250);
    }
}

// ---- Camera auto-follow ----

// While running, the game turns the camera toward the direction of movement on its own, which fights
// the mouse. The turn is stored by one instruction in the camera update, movdqa [rsi+140h], xmm4
// (camHRotateCorrection in The Grand Archives' DS3 Cheat Engine table, "Disable Auto Follow Cam",
// which NOPs it). Their address is for 1.15.0; in 1.15.2 the instruction is at +0x516FB4, identified by
// its distance (0x7A8) to the next movdqa [rsi+140h], xmm5, the same as in the table's older listing.
// Found by pattern so a different game version is left alone instead of patched in the wrong place.
// The exe's .text is not encrypted on disk, so this can run in DllMain.
static void DisableCameraAutoFollow()
{
    // unpcklps xmm4,xmm3 / movaps [rbp+?],xmm4 / movdqa [rbp+?],xmm4 / movdqa [rsi+140h],xmm4 /
    // cmp byte [rsi+?],0 / je
    static const short pattern[] = { 0x0F, 0x14, 0xE3, 0x0F, 0x29, 0xA5, -1, -1, -1, -1, 0x66, 0x0F, 0x7F, 0xA5, -1, -1, -1, -1,
                                     0x66, 0x0F, 0x7F, 0xA6, 0x40, 0x01, 0x00, 0x00, 0x80, 0xBE, -1, -1, 0x00, 0x00, 0x00, 0x0F, 0x84 };
    const int store = 18; // offset of movdqa [rsi+140h],xmm4 in the pattern
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* text = IMAGE_FIRST_SECTION(nt); // the game's main .text (the second one is Arxan's)
    BYTE* start = base + text->VirtualAddress;
    SIZE_T size = text->Misc.VirtualSize - _countof(pattern);
    BYTE* hit = NULL;
    int hits = 0;
    for (SIZE_T i = 0; i < size; i++)
    {
        int k = 0;
        while (k < (int)_countof(pattern) && (pattern[k] < 0 || start[i + k] == pattern[k])) k++;
        if (k == (int)_countof(pattern))
        {
            hit = start + i + store;
            hits++;
        }
    }
    if (hits != 1)
    {
        Log("Camera auto-follow: code not found (%d matches); left on.", hits);
        return;
    }
    DWORD old;
    if (!VirtualProtect(hit, 8, PAGE_EXECUTE_READWRITE, &old)) return;
    memset(hit, 0x90, 8);
    VirtualProtect(hit, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 8);
    Log("Camera auto-follow: disabled (DarkSoulsIII.exe+0x%llx).", (unsigned long long)(hit - base));
}

// ---- Setup ----

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    QueryPerformanceFrequency(&g_qpcFreq);

    GetModuleFileNameW(instance, g_dllDir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dllDir, L'\\');
    if (slash) slash[1] = 0;
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%sDS3MouseFix.ini", g_dllDir);

    // The log is appended to on every launch; start over once it passes 1 MB.
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%sDS3MouseFix.log", g_dllDir);
    WIN32_FILE_ATTRIBUTE_DATA logInfo;
    if (GetFileAttributesExW(logPath, GetFileExInfoStandard, &logInfo) && (logInfo.nFileSizeHigh || logInfo.nFileSizeLow > (1 << 20)))
        DeleteFileW(logPath);
    g_intervalMs = GetPrivateProfileIntW(L"DS3MouseFix", L"interval_ms", 8, ini);
    if (g_intervalMs < 0) g_intervalMs = 0;
    if (g_intervalMs > 50) g_intervalMs = 50;
    g_keyboardIcons = GetPrivateProfileIntW(L"DS3MouseFix", L"keyboard_icons", 1, ini) != 0;
    g_dumpTextures = GetPrivateProfileIntW(L"DS3MouseFix", L"dump_textures", 0, ini) != 0;
    g_stallTrace = GetPrivateProfileIntW(L"DS3MouseFix", L"stall_trace", 0, ini) != 0;
    DlaaLoadSettings(ini);

    // Must run before the game's entry point; dearxan hooks it and reports back through the callback.
    if (GetPrivateProfileIntW(L"DS3MouseFix", L"disable_arxan", 1, ini) != 0)
    {
        dearxan::detail::dearxan_neuter_arxan([](const dearxan::detail::DearxanResult* r, void*) {
            if (r->status == dearxan::detail::DearxanSuccess)
                Log("Arxan: %s.", r->is_arxan_detected ? "disabled" : "not detected");
            else
                Log("Arxan: could not disable it (%.*s).", (int)r->error_msg_size, r->error_msg ? r->error_msg : "");
        }, NULL);
    }

    if (GetPrivateProfileIntW(L"DS3MouseFix", L"no_camera_auto_follow", 1, ini) != 0) DisableCameraAutoFollow();

    if (g_stallTrace || DlaaEnabled())
    {
        g_realCreateFactory = (CreateDXGIFactory_t)HookImport("dxgi.dll", "CreateDXGIFactory", (void*)HookedCreateDXGIFactory);
        if (!g_realCreateFactory) Log("Could not hook the game's CreateDXGIFactory import.");
        else if (g_stallTrace)
        {
            Log("Stall trace on.");
            CloseHandle(CreateThread(NULL, 0, StallWatchThread, NULL, 0, NULL));
        }
    }

    // The game creates its D3D11 device long after this, and its imports are already bound now.
    if (g_keyboardIcons || g_dumpTextures)
    {
        if (g_keyboardIcons) LoadReplacements();
        if (g_dumpTextures)
        {
            wchar_t dump[MAX_PATH];
            swprintf_s(dump, L"%stexture_dump", g_dllDir);
            CreateDirectoryW(dump, NULL);
            Log("Texture dump on.");
        }
    }
    if (g_replacementCount > 0 || g_dumpTextures || DlaaEnabled())
    {
        g_realCreateDevice = (D3D11CreateDevice_t)HookImport("d3d11.dll", "D3D11CreateDevice", (void*)HookedD3D11CreateDevice);
        if (!g_realCreateDevice) Log("Could not hook the game's D3D11CreateDevice import.");
    }

    wchar_t real[MAX_PATH];
    GetSystemDirectoryW(real, MAX_PATH);
    wcscat_s(real, L"\\dinput8.dll");
    HMODULE module = LoadLibraryW(real);
    if (module) g_realCreate = (DirectInput8Create_t)GetProcAddress(module, "DirectInput8Create");

    if (g_intervalMs > 0) CloseHandle(CreateThread(NULL, 0, SetupThread, NULL, 0, NULL));
    return TRUE;
}
