// DLAA for Dark Souls III: NVIDIA DLSS at native resolution, in place of the game's FXAA.
//
// DS3 has no temporal anti-aliasing, so everything DLSS needs is added here (found with the frame
// capture in dlaa_capture.cpp; render order in CLAUDE.md):
//
// - Jitter: the 3D scene draws (G-buffer, decals, transparents, particles; full and half resolution)
//   get their viewport shifted by a sub-pixel Halton(2,3) offset, a new one each frame. Shadow maps,
//   full-screen passes, light volumes (read-only depth) and the HUD are left alone. With a negative
//   offset the GPU skips the last column/row, so that edge is filled from its neighbour before DLSS.
// - Object motion: DS3 draws moving objects (characters, enemies, cloth, physics objects) into a
//   half-resolution 8-bit velocity target for its motion blur; only when Motion Blur is on. Each of
//   those draws is drawn a second time into our full-resolution R32G32_FLOAT target, depth-tested
//   against the scene depth, with the game's own shaders (skinning with the previous frame's bones,
//   alpha test) and a tiny scale in their constant buffer so the encoding doesn't clip.
// - Camera motion: a compute shader turns the depth buffer plus the camera matrices (cbSceneParam, VS
//   slot b8: inverse view-projection in registers 8-11, view-projection in 24-27) of this and the
//   previous frame into motion vectors, and uses the object motion where an object was drawn. The
//   view-projection is camera-relative but the inverse gives absolute positions, so the camera's move
//   is accounted for in one reprojection matrix (BuildReprojection).
// - DLSS runs on the immediate context on the HDR scene, just before YEBIS (the post-effects middleware)
//   starts glare and tonemapping, so the HUD is drawn on top of the result. Its colour input is a copy
//   of the scene with NaN/infinite values replaced. YEBIS's motion blur and
//   the game's FXAA are skipped while DLSS is active.
// - DS3 records almost everything on worker threads (deferred contexts), so per-frame data (jitter
//   frame, camera, velocity pass) is attached to command lists and picked up when the immediate context
//   executes them.
//
// The NGX API is called through the exports of the driver's _nvngx.dll, as NVIDIA's own loader does;
// only the SDK headers are used. No NVIDIA binaries are shipped: DS3DLAA\nvngx_dlss.dll is a copy of the
// DLSS model the driver downloaded itself, checked for NVIDIA's signature by build.bat.
//
// dlaa_dump = 1 (debugging) saves two consecutive frames of DLSS inputs and output to DS3DLAA\dump\
// after ~5 s and on F10, so motion vectors and jitter can be checked offline (tools\check_dump.py); it
// also turns on NGX's own log and extra DLAA log lines: frames drawn without DLSS, and a per-frame
// check of DLSS's output (logged and dumped when it comes out dark). With it off, F10 does nothing.
// Verified in game 2026-10-10: camera and object motion vectors (warped frame matches 3x better) and
// the jitter sign (the input is offset from DLSS's output by the jitter, same direction).

#include "dlaa_internal.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#pragma warning(push, 0)
#include "../third_party/ngx/include/nvsdk_ngx_defs.h"
#include "../third_party/ngx/include/nvsdk_ngx_params.h"
#pragma warning(pop)

// ---- Settings ----

static bool g_dlaa = true;
bool g_capture;
static bool g_dump;
static float g_jitterSign = 1; // dlaa_jitter_flip = 1 passes the jitter to DLSS negated
wchar_t g_dlaaDir[MAX_PATH];

void DlaaLoadSettings(const wchar_t* ini)
{
    g_dlaa = GetPrivateProfileIntW(L"DS3Smooth",L"dlaa", 1, ini) != 0;
    g_capture = GetPrivateProfileIntW(L"DS3Smooth",L"dlaa_capture", 0, ini) != 0;
    g_dump = GetPrivateProfileIntW(L"DS3Smooth",L"dlaa_dump", 0, ini) != 0;
    g_jitterSign = GetPrivateProfileIntW(L"DS3Smooth",L"dlaa_jitter_flip", 0, ini) ? -1.0f : 1.0f;
    swprintf_s(g_dlaaDir, L"%sDS3DLAA\\", g_dllDir);
    CreateDirectoryW(g_dlaaDir, NULL);
    if (g_capture)
    {
        wchar_t dir[MAX_PATH];
        swprintf_s(dir, L"%sshaders", g_dlaaDir);
        CreateDirectoryW(dir, NULL);
        swprintf_s(dir, L"%scapture", g_dlaaDir);
        CreateDirectoryW(dir, NULL);
    }
}

bool DlaaEnabled()
{
    return g_dlaa || g_capture;
}

bool DlaaDebug()
{
    return g_dump || g_capture;
}

// ---- NGX (DLSS) through the driver's _nvngx.dll ----

typedef NVSDK_NGX_Result(NVSDK_CONV* NgxInitProjectId_t)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D11Device*,
                                                         NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
typedef NVSDK_NGX_Result(NVSDK_CONV* NgxGetParams_t)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result(NVSDK_CONV* NgxDestroyParams_t)(NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result(NVSDK_CONV* NgxCreateFeature_t)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result(NVSDK_CONV* NgxReleaseFeature_t)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result(NVSDK_CONV* NgxEvaluateFeature_t)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);

static struct
{
    NgxInitProjectId_t init;
    NgxGetParams_t getCapabilities, allocateParams;
    NgxDestroyParams_t destroyParams;
    NgxCreateFeature_t createFeature;
    NgxReleaseFeature_t releaseFeature;
    NgxEvaluateFeature_t evaluateFeature;
} g_ngx;

// Any GUID works as the project ID of a custom engine; this one is ours.
static const char NgxProjectId[] = "6f0b3c2e-5d1a-4c8e-9b7a-d53d1aa00001";

static void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    // NGX lines end with a newline already.
    size_t n = strlen(message);
    while (n && (message[n - 1] == '\n' || message[n - 1] == '\r')) n--;
    Log("NGX: %.*s", (int)n, message);
}

// The driver installer records where NGX lives. Only that folder is used, never the game folder,
// so a stray _nvngx.dll next to the game can't be loaded.
static HMODULE LoadNgxCore()
{
    static const struct { const wchar_t* key; const wchar_t* value; } places[] = {
        { L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath" },
        { L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath" },
        { L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath" },
    };
    for (int i = 0; i < (int)_countof(places); i++)
    {
        wchar_t dir[MAX_PATH] = {};
        DWORD size = sizeof(dir) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, places[i].key, places[i].value, RRF_RT_REG_SZ, NULL, dir, &size) != ERROR_SUCCESS) continue;
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\_nvngx.dll", dir);
        HMODULE module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module)
        {
            Log("DLSS: NGX core loaded from %ls.", path);
            return module;
        }
    }
    Log("DLSS: the NVIDIA driver's NGX core (_nvngx.dll) was not found; DLSS needs an RTX card and a current driver.");
    return NULL;
}

static NVSDK_NGX_Parameter* g_ngxParams;
static NVSDK_NGX_Handle* g_dlssFeature;

static bool InitNgx(ID3D11Device* device)
{
    HMODULE core = LoadNgxCore();
    if (!core) return false;
    g_ngx.init = (NgxInitProjectId_t)GetProcAddress(core, "NVSDK_NGX_D3D11_Init_ProjectID");
    g_ngx.getCapabilities = (NgxGetParams_t)GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters");
    g_ngx.allocateParams = (NgxGetParams_t)GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters");
    g_ngx.destroyParams = (NgxDestroyParams_t)GetProcAddress(core, "NVSDK_NGX_D3D11_DestroyParameters");
    g_ngx.createFeature = (NgxCreateFeature_t)GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature");
    g_ngx.releaseFeature = (NgxReleaseFeature_t)GetProcAddress(core, "NVSDK_NGX_D3D11_ReleaseFeature");
    g_ngx.evaluateFeature = (NgxEvaluateFeature_t)GetProcAddress(core, "NVSDK_NGX_D3D11_EvaluateFeature");
    if (!g_ngx.init || !g_ngx.getCapabilities || !g_ngx.allocateParams || !g_ngx.destroyParams || !g_ngx.createFeature ||
        !g_ngx.releaseFeature || !g_ngx.evaluateFeature)
    {
        Log("DLSS: the NGX core is missing D3D11 functions.");
        return false;
    }

    // NGX looks for nvngx_dlss.dll in these folders (and the game folder).
    const wchar_t* paths[] = { g_dlaaDir };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = NgxLogCallback;
    // NGX writes ~800 lines per launch at level ON; only wanted while debugging (dlaa_dump). Our own
    // lines still report the init and feature creation results.
    info.LoggingInfo.MinimumLoggingLevel = g_dump ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = true;

    NVSDK_NGX_Result r = g_ngx.init(NgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dlaaDir, device, NVSDK_NGX_Version_API, &info);
    Log("DLSS: NGX init -> 0x%08x.", (unsigned)r);
    if (NVSDK_NGX_FAILED(r)) return false;

    NVSDK_NGX_Parameter* caps = NULL;
    r = g_ngx.getCapabilities(&caps);
    int available = 0, needsDriver = 0;
    if (!NVSDK_NGX_FAILED(r) && caps)
    {
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    }
    Log("DLSS: available %d, needs newer driver %d.", available, needsDriver);
    return available && !NVSDK_NGX_FAILED(g_ngx.allocateParams(&g_ngxParams)) && g_ngxParams;
}

static bool CreateDlssFeature(ID3D11DeviceContext* ctx, UINT width, UINT height)
{
    if (g_dlssFeature) g_ngx.releaseFeature(g_dlssFeature);
    g_dlssFeature = NULL;
    NVSDK_NGX_Parameter* p = g_ngxParams;
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, width);
    p->Set(NVSDK_NGX_Parameter_Height, height);
    p->Set(NVSDK_NGX_Parameter_OutWidth, width);
    p->Set(NVSDK_NGX_Parameter_OutHeight, height);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)NVSDK_NGX_PerfQuality_Value_DLAA);
    // The input is YEBIS's linear HDR scene (before tonemapping); DLSS works out the exposure itself.
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, (int)(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
    p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    NVSDK_NGX_Result r = g_ngx.createFeature(ctx, NVSDK_NGX_Feature_SuperSampling, p, &g_dlssFeature);
    Log("DLSS: DLAA feature for %ux%u -> 0x%08x.", width, height, (unsigned)r);
    return !NVSDK_NGX_FAILED(r) && g_dlssFeature;
}

// ---- Shader identification ----
//
// Every shader object is tagged (SetPrivateData) with a 64-bit ID: the first half of the checksum in
// its DXBC header, which identifies the bytecode (the IDs below are from the capture; DS3 1.15.2).
// The pixel shaders DLAA cares about are also remembered by pointer, so PSSetShader can classify them
// cheaply.

static const GUID ShaderIdGuid = { 0x5d3a1c2e, 0x7b44, 0x4f0e, { 0x9c, 0x61, 0x2a, 0xd5, 0x3d, 0x1a, 0xa0, 0x01 } };

enum { PsOther, PsYebisSceneCopy, PsYebisDownsample, PsMotionBlur, PsFxaa };
static const struct
{
    unsigned long long id;
    int kind;
} SpecialPixelShaders[] = {
    { 0x393a947c35535471ull, PsYebisSceneCopy },  // copies the HDR scene into YEBIS's working target
    { 0x454e47f5721b6ac0ull, PsYebisDownsample }, // half-size copy; first reader of the scene after it
    { 0xef5c2e8f8b91a269ull, PsMotionBlur },      // YEBIS motion blur: copy, camera, velocity, composite
    { 0xfb4f78e37bba3aa5ull, PsMotionBlur },
    { 0xd55e5f7c1e2c4722ull, PsMotionBlur },
    { 0xe484bfebc00c5cf7ull, PsMotionBlur },
    { 0xcaa861f2cf791746ull, PsFxaa },            // the game's anti-aliasing, all quality levels
    { 0x3b9e25fd39b0ff47ull, PsFxaa },
    { 0x4b003ed549284a31ull, PsFxaa },
    { 0x675d6c6efdec8839ull, PsFxaa },
};
static struct
{
    ID3D11PixelShader* shader;
    int kind;
} g_specialPs[64];
static volatile LONG g_specialPsCount;

typedef HRESULT(STDMETHODCALLTYPE* CreateShader_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DeviceChild**);
static CreateShader_t g_realCreateVS, g_realCreatePS, g_realCreateCS;

static void DumpShader(unsigned long long id, const void* code, SIZE_T size, const char* kind)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sshaders\\%016llx.%hs", g_dlaaDir, id, kind);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, code, (DWORD)size, &written, NULL);
    CloseHandle(file);
}

static unsigned long long TagShader(HRESULT hr, const void* code, SIZE_T size, ID3D11DeviceChild** out, const char* kind)
{
    if (FAILED(hr) || !out || !*out || !code || size < 20 || memcmp(code, "DXBC", 4) != 0) return 0;
    unsigned long long id;
    memcpy(&id, (const BYTE*)code + 4, sizeof(id));
    (*out)->SetPrivateData(ShaderIdGuid, sizeof(id), &id);
    if (g_capture) DumpShader(id, code, size, kind);
    return id;
}

static HRESULT STDMETHODCALLTYPE HookedCreateVS(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* l, ID3D11DeviceChild** out)
{
    HRESULT hr = g_realCreateVS(d, code, size, l, out);
    TagShader(hr, code, size, out, "vs");
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePS(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* l, ID3D11DeviceChild** out)
{
    HRESULT hr = g_realCreatePS(d, code, size, l, out);
    unsigned long long id = TagShader(hr, code, size, out, "ps");
    for (int i = 0; id && i < (int)_countof(SpecialPixelShaders); i++)
    {
        if (SpecialPixelShaders[i].id != id) continue;
        LONG slot = InterlockedIncrement(&g_specialPsCount) - 1;
        if (slot < (LONG)_countof(g_specialPs))
        {
            g_specialPs[slot].kind = SpecialPixelShaders[i].kind;
            g_specialPs[slot].shader = (ID3D11PixelShader*)*out;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCS(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* l, ID3D11DeviceChild** out)
{
    HRESULT hr = g_realCreateCS(d, code, size, l, out);
    TagShader(hr, code, size, out, "cs");
    return hr;
}

unsigned long long ShaderId(ID3D11DeviceChild* shader)
{
    if (!shader) return 0;
    unsigned long long id = 0;
    UINT size = sizeof(id);
    if (FAILED(shader->GetPrivateData(ShaderIdGuid, &size, &id))) return 1; // created before the hook
    return id;
}

static int PixelShaderKind(ID3D11PixelShader* ps)
{
    if (!ps) return PsOther;
    LONG n = min(g_specialPsCount, (LONG)_countof(g_specialPs));
    for (LONG i = 0; i < n; i++)
        if (g_specialPs[i].shader == ps) return g_specialPs[i].kind;
    return PsOther;
}

// ---- Device context vtables ----
//
// d3d11.dll gives every context its own heap copy of the vtable (measured with a test program: the
// vtable sits right after the object), so each context's table is patched when it is created, and the
// originals are kept per table. d3d11.dll also rewrites the immediate context's draw, dispatch, copy and
// clear entries itself to switch implementations: measured, Flush replaces all of them and the first
// draw after it switches them back. DS3 flushes every frame, so the immediate context's hooks are
// re-applied (keeping what d3d11 wrote as the new originals) after every hooked call on it.

ContextHooks g_ctxHooks[64];
volatile LONG g_ctxHookCount;
ID3D11DeviceContext* g_immediate;
static ID3D11Device* g_device;

ContextHooks* HooksOf(ID3D11DeviceContext* ctx)
{
    void** vtable = *(void***)ctx;
    for (LONG i = 0; i < g_ctxHookCount; i++)
        if (g_ctxHooks[i].vtable == vtable) return &g_ctxHooks[i];
    return &g_ctxHooks[0]; // not reached: only patched tables lead here
}

static ContextState* StateOf(ID3D11DeviceContext* ctx)
{
    return &HooksOf(ctx)->state;
}

static int HookContext(ID3D11DeviceContext* ctx, const char* why);

static inline void AfterCall(ID3D11DeviceContext* c)
{
    if (c == g_immediate) HookContext(c, NULL);
}

// ---- Frame data ----
//
// Scene frame: counted when the main depth buffer is cleared with its stencil (once per frame, at the
// start of the frame's scene). Its number picks the jitter. Command lists remember the scene frame and
// camera they were recorded with; executing them on the immediate context makes them current.

static UINT g_width, g_height;                     // swap chain size
static volatile LONG g_sceneFrame;
static volatile bool g_dlssActive;                 // jitter and skipping only while DLSS really runs
static ID3D11Resource* volatile g_mainDepth;       // D32S8 at swap chain size
static ID3D11Resource* volatile g_halfDepth;       // D32S8 at half size (half-resolution transparents)
static ID3D11Resource* volatile g_velocityTarget;  // the game's half-resolution velocity target
static ID3D11DepthStencilView* volatile g_mainDepthReadOnly;

static void JitterOf(LONG frame, float* x, float* y)
{
    // Halton(2,3), 8 phases (NVIDIA's guidance for DLAA: 8 x scale^2). Pixel units, -0.5..0.5.
    int i = (int)((frame & 7) + 1);
    float h2 = 0, h3 = 0, f = 0.5f;
    for (int n = i; n; n >>= 1, f *= 0.5f) h2 += f * (n & 1);
    f = 1.0f / 3;
    for (int n = i; n; n /= 3, f /= 3) h3 += f * (n % 3);
    *x = h2 - 0.5f;
    *y = h3 - 0.5f;
}

struct ListInfo
{
    ID3D11CommandList* list;
    LONG sceneFrame;          // -1: no jittered draw in it
    ID3D11Resource* cameraBuffer;
    ContextState::CameraWrite writes[4];
    bool velocity;            // the game's velocity pass was recorded into it
};
static ListInfo g_lists[1024];
static volatile LONG g_listNext;

// Current frame, as seen by the immediate context (render thread only).
static LONG g_execSceneFrame = -1;
static bool g_execCameraValid, g_prevCameraValid, g_execVelocity;
static float g_execCamera[2][16], g_prevCamera[2][16];
static ID3D11Resource* g_hdrScene;                 // YEBIS's working copy of the HDR scene
static bool g_dlssPending;                         // scene copied this frame, DLSS not yet run
static int g_framesWithoutDlss = 1000;

// ---- Camera constants (cbSceneParam) ----
//
// The camera matrices are in cbSceneParam (VS b8; 952 bytes). It's written with Map/Unmap or
// UpdateSubresource, on a deferred context or the immediate one. What matters is its value on the GPU
// timeline: a deferred context's writes take effect when its command list executes. So writes are
// collected per context, travel with the command list, and are applied to g_bufferValues when the
// list executes; immediate-context writes apply at once. The G-buffer list notes which buffer its draws
// use, and the camera is that buffer's value when the list executes. Which buffers are cbSceneParam is
// learnt from the G-buffer draws themselves (whatever they bind at VS b8), so only those writes are kept.

static const UINT SceneParamMinSize = 448; // up to the end of VC_MatrixViewProj
static ID3D11Resource* volatile g_sceneParamBuffers[16];
static volatile LONG g_sceneParamCount;
static struct
{
    ID3D11Resource* buffer;
    bool valid;
    float camera[2][16];
} g_bufferValues[16]; // render thread only

static bool IsSceneParamBuffer(ID3D11Resource* r)
{
    LONG n = min(g_sceneParamCount, (LONG)_countof(g_sceneParamBuffers));
    for (LONG i = 0; i < n; i++)
        if (g_sceneParamBuffers[i] == r) return true;
    return false;
}

static void LearnSceneParamBuffer(ID3D11Buffer* b)
{
    if (IsSceneParamBuffer(b)) return;
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth < SceneParamMinSize) return;
    LONG slot = InterlockedIncrement(&g_sceneParamCount) - 1;
    if (slot >= (LONG)_countof(g_sceneParamBuffers)) return;
    g_sceneParamBuffers[slot] = b;
    Log("DLAA: camera constants found (cbSceneParam, %u bytes).", d.ByteWidth);
}

static void ExtractCamera(const void* data, float camera[2][16])
{
    memcpy(camera[0], (const BYTE*)data + 128, 64); // FC_MatrixInvViewProj, registers 8-11
    memcpy(camera[1], (const BYTE*)data + 384, 64); // VC_MatrixViewProj, registers 24-27
}

static void SetBufferValue(ID3D11Resource* buffer, const float camera[2][16])
{
    int slot = -1;
    for (int i = 0; i < (int)_countof(g_bufferValues) && slot < 0; i++)
        if (g_bufferValues[i].buffer == buffer) slot = i;
    for (int i = 0; i < (int)_countof(g_bufferValues) && slot < 0; i++)
        if (!g_bufferValues[i].buffer) slot = i;
    if (slot < 0) slot = (int)((ULONG_PTR)buffer / 64 % _countof(g_bufferValues));
    g_bufferValues[slot].buffer = buffer;
    g_bufferValues[slot].valid = true;
    memcpy(g_bufferValues[slot].camera, camera, sizeof(g_bufferValues[slot].camera));
}

static bool BufferValue(ID3D11Resource* buffer, float camera[2][16])
{
    for (int i = 0; i < (int)_countof(g_bufferValues); i++)
    {
        if (g_bufferValues[i].buffer != buffer || !g_bufferValues[i].valid) continue;
        memcpy(camera, g_bufferValues[i].camera, sizeof(g_bufferValues[i].camera));
        return true;
    }
    return false;
}

// A deferred context's pending write to a buffer (owning thread only).
static ContextState::CameraWrite* PendingWrite(ContextState* s, ID3D11Resource* buffer, bool add)
{
    for (int i = 0; i < (int)_countof(s->writes); i++)
        if (s->writes[i].buffer == buffer) return &s->writes[i];
    if (!add) return NULL;
    for (int i = 0; i < (int)_countof(s->writes); i++)
    {
        if (s->writes[i].buffer) continue;
        memset(&s->writes[i], 0, sizeof(s->writes[i]));
        s->writes[i].buffer = buffer;
        return &s->writes[i];
    }
    return NULL;
}

static void CameraWritten(ID3D11DeviceContext* c, ID3D11Resource* buffer, const void* data)
{
    float camera[2][16];
    ExtractCamera(data, camera);
    if (c == g_immediate)
    {
        SetBufferValue(buffer, camera);
        return;
    }
    ContextState::CameraWrite* w = PendingWrite(&HooksOf(c)->state, buffer, true);
    if (!w) return;
    memcpy(w->camera, camera, sizeof(camera));
    w->valid = true;
}

// ---- Our resources ----

typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                      ID3DBlob**);

// Motion vectors for DLSS: pixel units, pointing from the current pixel to where it was last frame.
// The same pass makes DLSS's colour input: the scene with NaN/infinite values replaced (one such pixel
// can blank DLSS's auto exposure for frames) and the missing edges filled (FillMissingEdges). Every
// 16th pixel in each direction is added up in g_stats for the brightness check (CheckStats).
static const char MotionVectorShader[] =
    "Texture2D<float> g_depth : register(t0);\n"
    "Texture2D<float2> g_objects : register(t1);\n"
    "Texture2D<float4> g_color : register(t2);\n"
    "RWTexture2D<float2> g_mv : register(u0);\n"
    "RWTexture2D<float> g_depthOut : register(u1);\n"
    "RWTexture2D<float4> g_colorOut : register(u2);\n"
    "RWStructuredBuffer<uint> g_stats : register(u3); // non-finite input pixels, input brightness sum\n"
    "cbuffer Params : register(b0)\n"
    "{\n"
    "    row_major float4x4 g_reproject; // current NDC + depth -> previous clip position\n"
    "    float2 g_size;\n"
    "    float g_objectScale;\n"
    "    float g_useCamera;\n"
    "    float g_useObjects;\n"
    "    float2 g_last; // last pixel the scene was rendered to (the far edges are missing with negative jitter)\n"
    "    float g_measure; // fill g_stats (debugging only)\n"
    "};\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint2 id : SV_DispatchThreadID)\n"
    "{\n"
    "    if (id.x >= (uint)g_size.x || id.y >= (uint)g_size.y) return;\n"
    "    uint2 src = min(id, (uint2)g_last);\n"
    "    float z = g_depth[src];\n"
    "    g_depthOut[id] = z;\n"
    "    float2 ndc = float2((id.x + 0.5) / g_size.x * 2 - 1, 1 - (id.y + 0.5) / g_size.y * 2);\n"
    "    float2 delta = 0; // previous minus current position, NDC\n"
    "    float2 o = g_useObjects > 0 ? g_objects[src] : float2(-1, -1);\n"
    "    if (o.x >= 0) delta = -(o - 0.5) * g_objectScale; // the game's velocity: current minus previous\n"
    "    else if (g_useCamera > 0)\n"
    "    {\n"
    "        float4 prev = mul(g_reproject, float4(ndc, z, 1));\n"
    "        if (prev.w > 1e-6) delta = prev.xy / prev.w - ndc;\n"
    "    }\n"
    "    g_mv[id] = float2(delta.x * 0.5 * g_size.x, -delta.y * 0.5 * g_size.y);\n"
    "    float4 c = g_color[src];\n"
    "    bool4 bad = (asuint(c) & 0x7f800000) == 0x7f800000; // exponent all ones: NaN or infinity\n"
    "    if (any(bad))\n"
    "    {\n"
    "        if (g_measure > 0) InterlockedAdd(g_stats[0], 1);\n"
    "        c = bad ? 0 : c;\n"
    "    }\n"
    "    c.rgb = clamp(c.rgb, 0, 65504);\n"
    "    g_colorOut[id] = c;\n"
    "    if (g_measure > 0 && ((id.x | id.y) & 15) == 0)\n"
    "        InterlockedAdd(g_stats[1], (uint)(min(dot(c.rgb, float3(0.2126, 0.7152, 0.0722)), 1000) * 64));\n"
    "}\n";

// The brightness of DLSS's output on the same sample grid, for CheckStats.
static const char OutputStatsShader[] =
    "Texture2D<float4> g_output : register(t0);\n"
    "RWStructuredBuffer<uint> g_stats : register(u0); // [2] output brightness sum, [3] non-finite output pixels\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint2 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h;\n"
    "    g_output.GetDimensions(w, h);\n"
    "    uint2 p = id * 16;\n"
    "    if (p.x >= w || p.y >= h) return;\n"
    "    float4 c = g_output[p];\n"
    "    if (any((asuint(c) & 0x7f800000) == 0x7f800000)) InterlockedAdd(g_stats[3], 1);\n"
    "    else InterlockedAdd(g_stats[2], (uint)(clamp(dot(c.rgb, float3(0.2126, 0.7152, 0.0722)), 0, 1000) * 64));\n"
    "}\n";
static const UINT StatsStep = 16, StatsScale = 64;

struct MotionVectorParams
{
    float reproject[16];
    float size[2], objectScale, useCamera, useObjects, last[2], measure;
};

// The game's velocity shaders write saturate((current - previous) * scale * 0.5 + 0.5), scale from
// cbModelParam (PS b7). Ours is small enough that nothing clips: |current - previous| < 16 (NDC).
static const float VelocityScale = 1.0f / 16;

static struct
{
    UINT width, height;                 // of the scene resources below
    ID3D11Texture2D* objects;           // object motion, full resolution, R32G32_FLOAT, cleared to -1
    ID3D11RenderTargetView* objectsRtv;
    ID3D11ShaderResourceView* objectsSrv;
    ID3D11Texture2D* mv;                // R16G16_FLOAT, pixels
    ID3D11UnorderedAccessView* mvUav;
    ID3D11Texture2D* depth;             // R32_FLOAT copy of the scene depth
    ID3D11UnorderedAccessView* depthUav;
    ID3D11Texture2D* output;            // DLSS output, same format as the scene
    DXGI_FORMAT outputFormat;
    ID3D11ShaderResourceView* outputSrv;
    ID3D11Texture2D* color;             // DLSS colour input: the scene, cleaned (R16G16B16A16_FLOAT)
    ID3D11UnorderedAccessView* colorUav;
    ID3D11ShaderResourceView* mainDepthSrv;
    ID3D11Resource* mainDepthSrvOf;
    ID3D11ShaderResourceView* sceneSrv;
    ID3D11Resource* sceneSrvOf;
    ID3D11ComputeShader* mvShader;
    ID3D11ComputeShader* statsShader;
    ID3D11Buffer* stats;                // 4 counters, see the shaders
    ID3D11UnorderedAccessView* statsUav;
    ID3D11Buffer* mvParams;
    ID3D11Buffer* velocityScale;        // stands in for cbModelParam during our velocity draws
    ID3D11DepthStencilState* velocityDepth;
    ID3DDeviceContextState* state;      // a clean pipeline state to do our work in
} g_res;

// The brightness counters of recent frames, read back a few frames later so the CPU never waits.
static struct
{
    ID3D11Buffer* buffer;               // staging copy of g_res.stats
    bool pending;
    int evaluation;
    bool reset;
} g_statsReadback[4];
static int g_statsNext;

static struct
{
    ID3D11RasterizerState* game;
    ID3D11RasterizerState* biased;
} g_biasedRasterizers[64];
static SRWLOCK g_rasterizerLock = SRWLOCK_INIT;

template <class T> static void SafeRelease(T*& p)
{
    if (p) p->Release();
    p = NULL;
}

static HRESULT CompileComputeShader(D3DCompile_t compile, const char* source, size_t size, const char* name, ID3D11ComputeShader** out)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile(source, size, name, NULL, NULL, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) Log("DLAA: %s shader: %s", name, errors ? (const char*)errors->GetBufferPointer() : "compile failed");
    else
    {
        hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, out);
        code->Release();
    }
    SafeRelease(errors);
    return hr;
}

static bool CreateStaticResources()
{
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    D3DCompile_t compile = compiler ? (D3DCompile_t)GetProcAddress(compiler, "D3DCompile") : NULL;
    if (!compile)
    {
        Log("DLAA: d3dcompiler_47.dll not found.");
        return false;
    }
    HRESULT hr = CompileComputeShader(compile, MotionVectorShader, sizeof(MotionVectorShader) - 1, "dlaa_mv", &g_res.mvShader);
    if (SUCCEEDED(hr)) hr = CompileComputeShader(compile, OutputStatsShader, sizeof(OutputStatsShader) - 1, "dlaa_stats", &g_res.statsShader);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 4 * sizeof(UINT);
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(UINT);
    hr = g_device->CreateBuffer(&bd, NULL, &g_res.stats);
    if (SUCCEEDED(hr)) hr = g_device->CreateUnorderedAccessView(g_res.stats, NULL, &g_res.statsUav);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < (int)_countof(g_statsReadback) && SUCCEEDED(hr); i++) hr = g_device->CreateBuffer(&bd, NULL, &g_statsReadback[i].buffer);

    bd = D3D11_BUFFER_DESC{};
    bd.ByteWidth = sizeof(MotionVectorParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = g_device->CreateBuffer(&bd, NULL, &g_res.mvParams);

    float scale[4] = { VelocityScale, VelocityScale, VelocityScale, VelocityScale };
    D3D11_SUBRESOURCE_DATA init = { scale, 0, 0 };
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.CPUAccessFlags = 0;
    if (SUCCEEDED(hr)) hr = g_device->CreateBuffer(&bd, &init, &g_res.velocityScale);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (SUCCEEDED(hr)) hr = g_device->CreateDepthStencilState(&dd, &g_res.velocityDepth);

    ID3D11Device1* device1 = NULL;
    if (SUCCEEDED(hr)) hr = g_device->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1);
    if (SUCCEEDED(hr))
    {
        D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
        hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), NULL, &g_res.state);
        device1->Release();
    }
    if (FAILED(hr)) Log("DLAA: could not create its resources (0x%08lx).", hr);
    return SUCCEEDED(hr);
}

static ID3D11Texture2D* CreateTexture(UINT w, UINT h, DXGI_FORMAT format, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = bind;
    ID3D11Texture2D* t = NULL;
    if (FAILED(g_device->CreateTexture2D(&d, NULL, &t))) Log("DLAA: could not create a %ux%u texture (format %d).", w, h, (int)format);
    return t;
}

// Scene-size resources. The object target is also used by worker threads, so on a size change the
// old one is not released (a resolution change is rare; the leak is one set of textures).
static void CreateObjectTarget(UINT w, UINT h)
{
    ID3D11Texture2D* objects = CreateTexture(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView* objectsRtv = NULL;
    ID3D11ShaderResourceView* objectsSrv = NULL;
    if (objects)
    {
        g_device->CreateRenderTargetView(objects, NULL, &objectsRtv);
        g_device->CreateShaderResourceView(objects, NULL, &objectsSrv);
    }
    g_res.objects = objects;
    g_res.objectsSrv = objectsSrv;
    g_res.objectsRtv = objectsRtv;
}

static DXGI_FORMAT DumpFormat(DXGI_FORMAT f);

// A shader view of a texture that may be typeless (the game's HDR scene is R16G16B16A16_TYPELESS).
static ID3D11ShaderResourceView* TypedSrv(ID3D11Resource* tex, DXGI_FORMAT format)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = DumpFormat(format);
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = NULL;
    if (FAILED(g_device->CreateShaderResourceView(tex, &vd, &srv))) return NULL;
    return srv;
}

static bool CreateSceneResources(UINT w, UINT h, DXGI_FORMAT sceneFormat)
{
    if (g_res.width == w && g_res.height == h && g_res.output && g_res.outputFormat == sceneFormat) return true;
    if (g_res.width != w || g_res.height != h || !g_res.objectsRtv) CreateObjectTarget(w, h);
    SafeRelease(g_res.mvUav);
    SafeRelease(g_res.mv);
    SafeRelease(g_res.depthUav);
    SafeRelease(g_res.depth);
    SafeRelease(g_res.outputSrv);
    SafeRelease(g_res.output);
    SafeRelease(g_res.colorUav);
    SafeRelease(g_res.color);
    g_res.mv = CreateTexture(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    g_res.depth = CreateTexture(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    g_res.output = CreateTexture(w, h, sceneFormat, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    g_res.color = CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    if (g_res.mv) g_device->CreateUnorderedAccessView(g_res.mv, NULL, &g_res.mvUav);
    if (g_res.depth) g_device->CreateUnorderedAccessView(g_res.depth, NULL, &g_res.depthUav);
    if (g_res.color) g_device->CreateUnorderedAccessView(g_res.color, NULL, &g_res.colorUav);
    if (g_res.output) g_res.outputSrv = TypedSrv(g_res.output, sceneFormat);
    g_res.width = w;
    g_res.height = h;
    g_res.outputFormat = sceneFormat;
    return g_res.objectsRtv && g_res.objectsSrv && g_res.mvUav && g_res.depthUav && g_res.output && g_res.colorUav;
}

// ---- Pipeline state tracking (on every context) ----

static ID3D11Resource* ResourceOf(ID3D11View* view)
{
    if (!view) return NULL;
    ID3D11Resource* r = NULL;
    view->GetResource(&r);
    if (r) r->Release(); // the view keeps it alive
    return r;
}

static int ClassifyDepth(ID3D11DepthStencilView* dsv, bool* readOnly)
{
    *readOnly = false;
    if (!dsv) return DepthNone;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd;
    dsv->GetDesc(&vd);
    *readOnly = (vd.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
    ID3D11Resource* r = NULL;
    dsv->GetResource(&r);
    int kind = DepthOther;
    if (r == g_mainDepth) kind = DepthMain;
    else if (r == g_halfDepth) kind = DepthHalf;
    else if (g_width && vd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT && vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC td;
        ((ID3D11Texture2D*)r)->GetDesc(&td);
        if (td.Width == g_width && td.Height == g_height && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        {
            if (g_mainDepth != r) Log("DLAA: scene depth buffer found (%ux%u).", td.Width, td.Height);
            g_mainDepth = r;
            kind = DepthMain;
        }
        else if (td.Width == g_width / 2 && td.Height == g_height / 2)
        {
            g_halfDepth = r;
            kind = DepthHalf;
        }
    }
    if (r) r->Release();
    return kind;
}

static void TrackTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    ContextState* s = StateOf(c);
    s->depth = ClassifyDepth(dsv, &s->depthReadOnly);
    s->rt0 = count && rtvs ? rtvs[0] : NULL;
    s->rt0Resource = ResourceOf(s->rt0);
    s->rtCount = count;
    s->ldrTarget = false;
    if (s->rt0)
    {
        D3D11_RENDER_TARGET_VIEW_DESC d;
        s->rt0->GetDesc(&d);
        s->ldrTarget = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                       d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

static void ResetState(ContextState* s)
{
    s->depth = DepthNone;
    s->depthReadOnly = false;
    s->ldrTarget = false;
    s->rt0 = NULL;
    s->rt0Resource = NULL;
    s->rtCount = 0;
    s->haveViewport = false;
    s->jittered = false;
    s->pixelShader = PsOther;
}


// ---- Jitter ----

enum DrawKind { DrawGeometry, DrawFullscreen };

// Called before every draw: shifts the viewport for scene geometry and puts it back for anything else.
static void JitterBeforeDraw(ID3D11DeviceContext* c, ContextState* s, DrawKind kind)
{
    bool want = false;
    float scale = 1;
    if (g_dlssActive && kind == DrawGeometry && s->haveViewport && !s->ldrTarget && !s->depthReadOnly)
    {
        if (s->depth == DepthMain && s->viewport.Width == (float)g_width && s->viewport.Height == (float)g_height) want = true;
        else if (s->depth == DepthHalf && s->viewport.Width == (float)(g_width / 2) && s->viewport.Height == (float)(g_height / 2))
        {
            want = true;
            scale = 0.5f;
        }
    }
    if (want)
    {
        // A command list's frame is decided by its first jittered draw (the immediate context has no lists).
        LONG frame = g_sceneFrame;
        if (c != g_immediate)
        {
            if (s->sceneFrame < 0) s->sceneFrame = frame;
            frame = s->sceneFrame;
        }
        float jx, jy;
        JitterOf(frame, &jx, &jy);
        D3D11_VIEWPORT vp = s->viewport;
        vp.TopLeftX += jx * scale;
        vp.TopLeftY += jy * scale;
        REAL(c, CtxRSSetViewports, RSSetViewports_t)(c, 1, &vp);
        s->jittered = true;
    }
    else if (s->jittered)
    {
        REAL(c, CtxRSSetViewports, RSSetViewports_t)(c, 1, &s->viewport);
        s->jittered = false;
    }
}

// ---- Velocity: the game's object draws again, at full resolution and full precision ----

static ID3D11RasterizerState* BiasedRasterizer(ID3D11RasterizerState* game)
{
    AcquireSRWLockShared(&g_rasterizerLock);
    ID3D11RasterizerState* found = NULL;
    for (int i = 0; i < (int)_countof(g_biasedRasterizers) && !found; i++)
        if (g_biasedRasterizers[i].biased && g_biasedRasterizers[i].game == game) found = g_biasedRasterizers[i].biased;
    ReleaseSRWLockShared(&g_rasterizerLock);
    if (found) return found;

    D3D11_RASTERIZER_DESC d;
    if (game) game->GetDesc(&d);
    else
    {
        memset(&d, 0, sizeof(d));
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    // The velocity shaders compute positions with other code than the G-buffer shaders, so the depth
    // may differ in the last bits; pull our draw slightly towards the camera (depth: 0 = near).
    d.DepthBias = -16;
    d.SlopeScaledDepthBias = -1.0f;
    d.DepthBiasClamp = 0;
    ID3D11RasterizerState* biased = NULL;
    if (FAILED(g_device->CreateRasterizerState(&d, &biased))) return game;
    AcquireSRWLockExclusive(&g_rasterizerLock);
    for (int i = 0; i < (int)_countof(g_biasedRasterizers); i++)
    {
        if (g_biasedRasterizers[i].biased) continue;
        g_biasedRasterizers[i].game = game;
        g_biasedRasterizers[i].biased = biased;
        break;
    }
    ReleaseSRWLockExclusive(&g_rasterizerLock);
    return biased;
}

struct DrawArgs
{
    int method;
    UINT a, b, c, d;
    INT base;
    ID3D11Buffer* args;
};

static void IssueDraw(ID3D11DeviceContext* c, const DrawArgs& d)
{
    switch (d.method)
    {
    case CtxDrawIndexed: REAL(c, CtxDrawIndexed, DrawIndexed_t)(c, d.a, d.b, d.base); break;
    case CtxDraw: REAL(c, CtxDraw, Draw_t)(c, d.a, d.b); break;
    case CtxDrawIndexedInstanced: REAL(c, CtxDrawIndexedInstanced, DrawIndexedInstanced_t)(c, d.a, d.b, d.c, d.base, d.d); break;
    case CtxDrawInstanced: REAL(c, CtxDrawInstanced, DrawInstanced_t)(c, d.a, d.b, d.c, d.d); break;
    case CtxDrawIndexedInstancedIndirect: REAL(c, CtxDrawIndexedInstancedIndirect, DrawIndirect_t)(c, d.args, d.a); break;
    case CtxDrawInstancedIndirect: REAL(c, CtxDrawInstancedIndirect, DrawIndirect_t)(c, d.args, d.a); break;
    }
}

static void ReplayVelocityDraw(ID3D11DeviceContext* c, ContextState* s, const DrawArgs& draw)
{
    ID3D11RenderTargetView* objectsRtv = g_res.objectsRtv;
    ID3D11DepthStencilView* depth = g_mainDepthReadOnly;
    if (!objectsRtv || !depth) return;

    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = NULL;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount = _countof(viewports);
    c->RSGetViewports(&viewportCount, viewports);
    ID3D11DepthStencilState* dss = NULL;
    UINT stencilRef = 0;
    c->OMGetDepthStencilState(&dss, &stencilRef);
    ID3D11RasterizerState* rs = NULL;
    c->RSGetState(&rs);
    ID3D11Buffer* cb7 = NULL;
    c->PSGetConstantBuffers(7, 1, &cb7);

    REAL(c, CtxOMSetRenderTargets, OMSetRenderTargets_t)(c, 1, &objectsRtv, depth);
    D3D11_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0, 1 };
    LONG frame = s->sceneFrame >= 0 ? s->sceneFrame : g_sceneFrame;
    float jx, jy;
    JitterOf(frame, &jx, &jy);
    vp.TopLeftX = jx;
    vp.TopLeftY = jy;
    REAL(c, CtxRSSetViewports, RSSetViewports_t)(c, 1, &vp);
    c->OMSetDepthStencilState(g_res.velocityDepth, 0);
    c->RSSetState(BiasedRasterizer(rs));
    c->PSSetConstantBuffers(7, 1, &g_res.velocityScale);

    IssueDraw(c, draw);

    UINT rtCount = 0;
    for (UINT i = 0; i < _countof(rtvs); i++)
        if (rtvs[i]) rtCount = i + 1;
    REAL(c, CtxOMSetRenderTargets, OMSetRenderTargets_t)(c, rtCount, rtvs, dsv);
    REAL(c, CtxRSSetViewports, RSSetViewports_t)(c, viewportCount, viewports);
    c->OMSetDepthStencilState(dss, stencilRef);
    c->RSSetState(rs);
    c->PSSetConstantBuffers(7, 1, &cb7);
    for (UINT i = 0; i < _countof(rtvs); i++) SafeRelease(rtvs[i]);
    SafeRelease(dsv);
    SafeRelease(dss);
    SafeRelease(rs);
    SafeRelease(cb7);
}

// ---- DLSS ----

static LARGE_INTEGER g_lastEval;
static UINT g_dlssWidth, g_dlssHeight;
static int g_evalCount, g_evalFailures;
static int g_dumpAt = -1;                 // evaluation number of the next dump (two frames)

static void WriteDds(ID3D11DeviceContext* c, ID3D11Texture2D* tex, const wchar_t* name);

// Diagnostics for the first evaluations: the depth at the screen centre, in the game's depth buffer
// and in our copy (they must match; the sky is 1.0).
static float CentreTexel(ID3D11DeviceContext* c, ID3D11Resource* tex, UINT bytesPerPixel)
{
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D*)tex)->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = NULL;
    float v = -1;
    if (FAILED(g_device->CreateTexture2D(&sd, NULL, &staging))) return v;
    REAL(c, CtxCopyResource, CopyResource_t)(c, staging, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(REAL(c, CtxMap, Map_t)(c, staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        v = *(float*)((BYTE*)m.pData + (size_t)(d.Height / 2) * m.RowPitch + (size_t)(d.Width / 2) * bytesPerPixel);
        REAL(c, CtxUnmap, Unmap_t)(c, staging, 0);
    }
    staging->Release();
    return v;
}

static void LogCentreDepth(ID3D11DeviceContext* c)
{
    Log("DLAA: centre depth: scene %.6f, our copy %.6f.", g_mainDepth ? CentreTexel(c, g_mainDepth, 8) : -1.0f, CentreTexel(c, g_res.depth, 4));
}

// DS3 renders camera-relative: VC_MatrixViewProj has no translation, but FC_MatrixInvViewProj leads to
// absolute world positions (measured in game: inv * vp = translation by the camera position, e.g.
// (66.3, -154.6, -590.9)). So the previous frame's camera position is taken out between the two:
// reproject = prevViewProj * translate(-prevCameraPos) * invViewProj, built in double because the
// inverse has entries in the thousands that cancel.
static void BuildReprojection(float out[16])
{
    const float* inv = g_execCamera[0];
    const float* prevInv = g_prevCamera[0];
    const float* prevVp = g_prevCamera[1];
    double prevPos[3];
    for (int r = 0; r < 3; r++)
    {
        double s = 0;
        for (int k = 0; k < 4; k++) s += (double)prevInv[r * 4 + k] * prevVp[k * 4 + 3];
        prevPos[r] = s;
    }
    double world[16]; // translate(-prevPos) * inv
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            world[r * 4 + col] = inv[r * 4 + col] - (r < 3 ? prevPos[r] * inv[12 + col] : 0);
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
        {
            double s = 0;
            for (int k = 0; k < 4; k++) s += (double)prevVp[r * 4 + k] * world[k * 4 + col];
            out[r * 4 + col] = (float)s;
        }
}

// The GPU clips to the viewport rounded down to whole pixels (measured with a test program: any negative
// TopLeft, even -0.05, leaves the last column/row of the target unrendered; positive ones up to 0.49
// keep the first). So in frames with negative jitter the game's scene misses its right column and/or
// bottom row: lighting leaves them a flat colour, which DLSS showed as a flickering line at the bottom
// of the screen. A viewport can't move the image without that, so the motion vector pass reads the
// missing edge's colour, depth and object motion from its neighbour (g_last).
static void RunMotionVectorPass(ID3D11DeviceContext* c, float jx, float jy)
{
    MotionVectorParams p = {};
    bool camera = g_execCameraValid && g_prevCameraValid && g_res.mainDepthSrv;
    if (camera) BuildReprojection(p.reproject);
    p.size[0] = (float)g_res.width;
    p.size[1] = (float)g_res.height;
    p.objectScale = 2 / VelocityScale;
    p.useCamera = camera ? 1.0f : 0.0f;
    p.useObjects = g_execVelocity ? 1.0f : 0.0f;
    p.last[0] = (float)(g_res.width - (jx < 0 ? 2 : 1));
    p.last[1] = (float)(g_res.height - (jy < 0 ? 2 : 1));
    p.measure = g_dump ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(REAL(c, CtxMap, Map_t)(c, g_res.mvParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &p, sizeof(p));
        REAL(c, CtxUnmap, Unmap_t)(c, g_res.mvParams, 0);
    }
    if (g_dump)
    {
        static const UINT zero[4] = {};
        c->ClearUnorderedAccessViewUint(g_res.statsUav, zero);
    }
    ID3D11ShaderResourceView* srvs[3] = { g_res.mainDepthSrv, g_res.objectsSrv, g_res.sceneSrv };
    ID3D11UnorderedAccessView* uavs[4] = { g_res.mvUav, g_res.depthUav, g_res.colorUav, g_res.statsUav };
    c->CSSetShader(g_res.mvShader, NULL, 0);
    c->CSSetShaderResources(0, 3, srvs);
    c->CSSetUnorderedAccessViews(0, 4, uavs, NULL);
    c->CSSetConstantBuffers(0, 1, &g_res.mvParams);
    REAL(c, CtxDispatch, Dispatch_t)(c, (g_res.width + 7) / 8, (g_res.height + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrvs[3] = {};
    ID3D11UnorderedAccessView* noUavs[4] = {};
    c->CSSetShaderResources(0, 3, noSrvs);
    c->CSSetUnorderedAccessViews(0, 4, noUavs, NULL);
}

// Debugging (dlaa_dump = 1): black flashes were reported 2026-10-10 (a few frames, while talking to
// NPCs; DLSS ran every frame) and haven't been seen since. This check logs any frame whose input had
// NaN/infinite pixels, or whose output came out much darker than its input or invalid, so a flash too
// short for a screenshot still shows up in the log, and saves a dump of the first one.
static void MeasureOutput(ID3D11DeviceContext* c, int evaluation, bool reset)
{
    c->CSSetShader(g_res.statsShader, NULL, 0);
    c->CSSetShaderResources(0, 1, &g_res.outputSrv);
    c->CSSetUnorderedAccessViews(0, 1, &g_res.statsUav, NULL);
    UINT samplesX = (g_res.width + StatsStep - 1) / StatsStep, samplesY = (g_res.height + StatsStep - 1) / StatsStep;
    REAL(c, CtxDispatch, Dispatch_t)(c, (samplesX + 7) / 8, (samplesY + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrv = NULL;
    ID3D11UnorderedAccessView* noUav = NULL;
    c->CSSetShaderResources(0, 1, &noSrv);
    c->CSSetUnorderedAccessViews(0, 1, &noUav, NULL);

    int slot = g_statsNext++ % (int)_countof(g_statsReadback);
    REAL(c, CtxCopyResource, CopyResource_t)(c, g_statsReadback[slot].buffer, g_res.stats);
    g_statsReadback[slot].pending = true;
    g_statsReadback[slot].evaluation = evaluation;
    g_statsReadback[slot].reset = reset;
}

static void CheckStats(ID3D11DeviceContext* c)
{
    static int loggedInput, loggedDark;
    static bool autoDumped;
    double samples = (double)((g_res.width + StatsStep - 1) / StatsStep) * ((g_res.height + StatsStep - 1) / StatsStep);
    for (int k = 1; k <= (int)_countof(g_statsReadback); k++) // oldest first
    {
        int slot = (g_statsNext + k - 1) % (int)_countof(g_statsReadback);
        if (!g_statsReadback[slot].pending) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(REAL(c, CtxMap, Map_t)(c, g_statsReadback[slot].buffer, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) continue;
        UINT v[4];
        memcpy(v, m.pData, sizeof(v));
        REAL(c, CtxUnmap, Unmap_t)(c, g_statsReadback[slot].buffer, 0);
        g_statsReadback[slot].pending = false;
        double in = v[1] / (double)StatsScale / samples, out = v[2] / (double)StatsScale / samples;
        bool broken = v[3] || (in > 0.01 && out < 0.3 * in);
        bool report = broken ? loggedDark++ < 20 : (v[0] || (g_dump && g_statsReadback[slot].evaluation <= 3)) && loggedInput++ < 10;
        if (report)
            Log("DLAA: evaluation %d: brightness in %.4f, out %.4f%s; invalid: %u input pixels (replaced), %u of %.0f output samples%s.",
                g_statsReadback[slot].evaluation, in, out, broken ? " (OUTPUT BROKEN)" : "", v[0], v[3], samples,
                g_statsReadback[slot].reset ? "; reset" : "");
        // The first broken output of a session also saves DLSS's inputs and output (two frames, ~500 MB in
        // DS3DLAA\dump\; a short hitch), so the cause can be seen offline.
        if (broken && !autoDumped && g_dumpAt < 0)
        {
            autoDumped = true;
            g_dumpAt = g_evalCount + 1;
            Log("DLAA: saving a dump of the next two evaluations.");
        }
    }
}

static NVSDK_NGX_Result EvaluateDlss(ID3D11DeviceContext* c, float jx, float jy, bool reset, float frameMs)
{
    NVSDK_NGX_Parameter* p = g_ngxParams;
    p->Set(NVSDK_NGX_Parameter_Color, (ID3D11Resource*)g_res.color);
    p->Set(NVSDK_NGX_Parameter_Output, (ID3D11Resource*)g_res.output);
    p->Set(NVSDK_NGX_Parameter_Depth, (ID3D11Resource*)g_res.depth);
    p->Set(NVSDK_NGX_Parameter_MotionVectors, (ID3D11Resource*)g_res.mv);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jx * g_jitterSign);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jy * g_jitterSign);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, g_res.width);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, g_res.height);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, frameMs);
    return g_ngx.evaluateFeature(c, g_dlssFeature, p, NULL);
}

static NVSDK_NGX_Result SafeEvaluateDlss(ID3D11DeviceContext* c, float jx, float jy, bool reset, float frameMs)
{
    __try
    {
        return EvaluateDlss(c, jx, jy, reset, frameMs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("DLSS: crashed while evaluating (exception 0x%08lx).", GetExceptionCode());
        return NVSDK_NGX_Result_Fail;
    }
}

static void Dump(ID3D11DeviceContext* c, const char* stage, float jx, float jy)
{
    wchar_t name[MAX_PATH];
    if (strcmp(stage, "input") == 0)
    {
        swprintf_s(name, L"%d_color", g_evalCount);
        WriteDds(c, g_res.color, name); // what DLSS gets: the scene, cleaned
        swprintf_s(name, L"%d_mv", g_evalCount);
        WriteDds(c, g_res.mv, name);
        swprintf_s(name, L"%d_depth", g_evalCount);
        WriteDds(c, g_res.depth, name);
        swprintf_s(name, L"%d_objects", g_evalCount);
        WriteDds(c, g_res.objects, name);

        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%sdump\\%d_info.txt", g_dlaaDir, g_evalCount);
        FILE* f;
        if (_wfopen_s(&f, path, L"w") == 0)
        {
            fprintf(f, "scene_frame %ld\njitter %.6f %.6f\nsign %.0f\ncamera %d prev_camera %d velocity %d\n", g_execSceneFrame, jx, jy,
                    g_jitterSign, g_execCameraValid, g_prevCameraValid, g_execVelocity);
            for (int m = 0; m < 2; m++)
            {
                fprintf(f, "%s\n", m ? "prev_view_proj" : "inv_view_proj");
                const float* v = m ? g_prevCamera[1] : g_execCamera[0];
                for (int r = 0; r < 4; r++) fprintf(f, "%.9g %.9g %.9g %.9g\n", v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
            }
            for (int m = 0; m < 2; m++)
            {
                fprintf(f, "%s\n", m ? "prev_inv_view_proj" : "view_proj");
                const float* v = m ? g_prevCamera[0] : g_execCamera[1];
                for (int r = 0; r < 4; r++) fprintf(f, "%.9g %.9g %.9g %.9g\n", v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
            }
            fclose(f);
        }
    }
    else
    {
        swprintf_s(name, L"%d_output", g_evalCount);
        WriteDds(c, g_res.output, name);
    }
}

// Runs on the immediate context, right before YEBIS first reads the HDR scene: motion vectors, DLSS,
// and the result copied over the scene. Done in a clean device context state that is swapped back
// afterwards, so neither our calls nor NGX's disturb the game's pipeline state.
static void RunDlssInCleanState(ID3D11DeviceContext* c);

static void RunDlss(ID3D11DeviceContext* c)
{
    ID3D11DeviceContext1* c1 = NULL;
    if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) return;
    ID3DDeviceContextState* gameState = NULL;
    c1->SwapDeviceContextState(g_res.state, &gameState);
    RunDlssInCleanState(c);
    c1->SwapDeviceContextState(gameState, NULL);
    SafeRelease(gameState);
    c1->Release();
    HookContext(c, "DLSS");
}

static void RunDlssInCleanState(ID3D11DeviceContext* c)
{
    D3D11_TEXTURE2D_DESC sd;
    ((ID3D11Texture2D*)g_hdrScene)->GetDesc(&sd);
    if (!CreateSceneResources(sd.Width, sd.Height, sd.Format)) return;
    if (!g_dlssFeature || g_dlssWidth != sd.Width || g_dlssHeight != sd.Height)
    {
        g_dlssWidth = sd.Width;
        g_dlssHeight = sd.Height;
        if (!CreateDlssFeature(c, sd.Width, sd.Height))
        {
            g_dlssActive = false;
            g_dlaa = false;
            Log("DLAA: off (DLSS could not be created).");
            return;
        }
    }
    if (g_mainDepth && g_res.mainDepthSrvOf != g_mainDepth)
    {
        SafeRelease(g_res.mainDepthSrv);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        vd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(g_device->CreateShaderResourceView(g_mainDepth, &vd, &g_res.mainDepthSrv))) g_res.mainDepthSrvOf = g_mainDepth;
    }
    if (!g_res.mainDepthSrv) return;
    if (g_res.sceneSrvOf != g_hdrScene)
    {
        // The view keeps the texture alive, so a new scene texture can't reuse the old one's address.
        SafeRelease(g_res.sceneSrv);
        g_res.sceneSrv = TypedSrv(g_hdrScene, sd.Format);
        g_res.sceneSrvOf = g_hdrScene;
        if (!g_res.sceneSrv)
        {
            g_dlssActive = false;
            g_dlaa = false;
            Log("DLAA: off (the HDR scene, format %d, can't be read by a shader).", (int)sd.Format);
            return;
        }
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float frameMs = g_lastEval.QuadPart ? (float)((now.QuadPart - g_lastEval.QuadPart) * 1000.0 / freq.QuadPart) : 8.33f;
    g_lastEval = now;
    bool reset = g_framesWithoutDlss > 1; // DLSS didn't run last frame (menu, loading screen): start over
    static int restartsLogged;
    if (g_dump && reset && g_evalCount > 0 && restartsLogged < 30)
    {
        restartsLogged++;
        Log("DLAA: DLSS restarted after %d frame(s) without it.", g_framesWithoutDlss - 1);
    }
    float jx = 0, jy = 0;
    if (g_execSceneFrame >= 0) JitterOf(g_execSceneFrame, &jx, &jy);

    if (g_dump) CheckStats(c);
    RunMotionVectorPass(c, jx, jy);
    if (g_dump && g_evalCount < 3) LogCentreDepth(c); // reads back from the GPU, so debugging only
    g_evalCount++;
    bool dump = g_dumpAt >= 0 && (g_evalCount == g_dumpAt || g_evalCount == g_dumpAt + 1);
    if (dump) Dump(c, "input", jx, jy);
    NVSDK_NGX_Result r = SafeEvaluateDlss(c, jx, jy, reset, frameMs);
    if (!NVSDK_NGX_FAILED(r))
    {
        REAL(c, CtxCopyResource, CopyResource_t)(c, g_hdrScene, g_res.output);
        if (g_dump) MeasureOutput(c, g_evalCount, reset);
        if (dump) Dump(c, "output", jx, jy);
        if (dump && g_evalCount == g_dumpAt + 1)
        {
            Log("DLAA: dump written to DS3DLAA\\dump\\ (evaluations %d and %d).", g_dumpAt, g_dumpAt + 1);
            g_dumpAt = -1;
        }
        g_framesWithoutDlss = 0;
        if (g_evalCount <= (g_dump ? 3 : 1) || (g_dump && g_evalCount % 3600 == 0))
            Log("DLAA: evaluation %d ok (jitter %.3f %.3f, camera %d/%d, objects %d, reset %d).", g_evalCount, jx, jy, g_execCameraValid,
                g_prevCameraValid, g_execVelocity, reset);
    }
    else
    {
        Log("DLAA: evaluation failed (0x%08x).", (unsigned)r);
        if (++g_evalFailures >= 3)
        {
            g_dlssActive = false;
            g_dlaa = false;
            Log("DLAA: off after repeated failures; the game's own anti-aliasing is back.");
        }
    }
}

// ---- Context hooks ----

static ID3D11Resource* g_immediateMapped; // cbSceneParam being written on the immediate context
static void* g_immediateMappedData;

static void STDMETHODCALLTYPE HookedPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n)
{
    StateOf(c)->pixelShader = PixelShaderKind(ps);
    REAL(c, CtxPSSetShader, PSSetShader_t)(c, ps, ci, n);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                       ID3D11DepthStencilView* dsv)
{
    TrackTargets(c, n, rtvs, dsv);
    REAL(c, CtxOMSetRenderTargets, OMSetRenderTargets_t)(c, n, rtvs, dsv);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargetsAndUAVs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                              ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                                              ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) TrackTargets(c, n, rtvs, dsv);
    REAL(c, CtxOMSetRenderTargetsAndUAVs, OMSetRenderTargetsAndUAVs_t)(c, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedRSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps)
{
    ContextState* s = StateOf(c);
    s->haveViewport = n == 1 && vps;
    if (s->haveViewport) s->viewport = vps[0];
    s->jittered = false;
    REAL(c, CtxRSSetViewports, RSSetViewports_t)(c, n, vps);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedClearState(ID3D11DeviceContext* c)
{
    ResetState(StateOf(c));
    REAL(c, CtxClearState, ClearState_t)(c);
    AfterCall(c);
}

static HRESULT STDMETHODCALLTYPE HookedMap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                           D3D11_MAPPED_SUBRESOURCE* out)
{
    HRESULT hr = REAL(c, CtxMap, Map_t)(c, r, sub, type, flags, out);
    AfterCall(c);
    if (SUCCEEDED(hr) && out && type != D3D11_MAP_READ)
    {
        if (g_dlaa && IsSceneParamBuffer(r))
        {
            if (c == g_immediate) g_immediateMapped = r, g_immediateMappedData = out->pData;
            else if (ContextState::CameraWrite* w = PendingWrite(StateOf(c), r, true)) w->mapped = out->pData;
        }
        if (g_capturing) CaptureCbMapped(c, r, out->pData);
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
    if (g_dlaa && IsSceneParamBuffer(r))
    {
        if (c == g_immediate)
        {
            if (g_immediateMapped == r && g_immediateMappedData) CameraWritten(c, r, g_immediateMappedData);
            g_immediateMapped = NULL;
        }
        else
        {
            ContextState::CameraWrite* w = PendingWrite(StateOf(c), r, false);
            if (w && w->mapped)
            {
                CameraWritten(c, r, w->mapped);
                w->mapped = NULL;
            }
        }
    }
    if (g_capturing) CaptureCbUnmapped(c, r);
    REAL(c, CtxUnmap, Unmap_t)(c, r, sub);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedUpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data,
                                                      UINT rowPitch, UINT depthPitch)
{
    if (!box && data)
    {
        if (g_dlaa && IsSceneParamBuffer(r)) CameraWritten(c, r, data);
        if (g_capturing) CaptureCbUpdated(c, r, data);
    }
    REAL(c, CtxUpdateSubresource, UpdateSubresource_t)(c, r, sub, box, data, rowPitch, depthPitch);
    AfterCall(c);
}

// The first G-buffer draw of a command list notes which cbSceneParam buffer it uses (VS b8); its
// value is read when the list executes.
static void NoteCamera(ID3D11DeviceContext* c, ContextState* s)
{
    if (s->cameraBuffer || s->depth != DepthMain || s->rtCount < 4 || s->ldrTarget) return;
    ID3D11Buffer* b8 = NULL;
    c->VSGetConstantBuffers(8, 1, &b8);
    if (!b8) return;
    LearnSceneParamBuffer(b8);
    if (IsSceneParamBuffer(b8)) s->cameraBuffer = b8;
    b8->Release(); // the game keeps it alive; only the pointer is compared
}

// Everything that happens around a draw, on any context. Returns false if the draw must be skipped.
static bool BeforeDraw(ID3D11DeviceContext* c, const DrawArgs& d, DrawKind kind)
{
    ContextState* s = StateOf(c);
    if (g_dlssActive && (s->pixelShader == PsMotionBlur || s->pixelShader == PsFxaa)) return false;
    if (g_dlaa)
    {
        JitterBeforeDraw(c, s, kind);
        NoteCamera(c, s);
        if (c == g_immediate && s->pixelShader == PsYebisDownsample && g_dlssPending && g_dlssActive)
        {
            ID3D11ShaderResourceView* srv = NULL;
            c->PSGetShaderResources(0, 1, &srv);
            bool readsScene = ResourceOf(srv) == g_hdrScene;
            SafeRelease(srv);
            if (readsScene)
            {
                g_dlssPending = false;
                RunDlss(c);
            }
        }
    }
    (void)d;
    return true;
}

static void AfterDraw(ID3D11DeviceContext* c, const DrawArgs& d, DrawKind kind)
{
    ContextState* s = StateOf(c);
    if (!g_dlaa) return;
    if (c == g_immediate && s->pixelShader == PsYebisSceneCopy && s->rt0)
    {
        ID3D11Resource* scene = ResourceOf(s->rt0);
        if (scene != g_hdrScene) Log("DLAA: HDR scene target found.");
        g_hdrScene = scene;
        g_dlssPending = true;
    }
    if (g_dlssActive && kind == DrawGeometry && s->rt0Resource && s->rt0Resource == g_velocityTarget) ReplayVelocityDraw(c, s, d);
}

static void STDMETHODCALLTYPE HookedDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    if (g_capturing)
    {
        char call[64];
        sprintf_s(call, "DrawIndexed(%u)", count);
        CaptureDraw(c, call, false, false);
    }
    DrawArgs d = { CtxDrawIndexed, count, start, 0, 0, base, NULL };
    if (BeforeDraw(c, d, DrawGeometry))
    {
        REAL(c, CtxDrawIndexed, DrawIndexed_t)(c, count, start, base);
        AfterCall(c);
        AfterDraw(c, d, DrawGeometry);
    }
}

static void STDMETHODCALLTYPE HookedDraw(ID3D11DeviceContext* c, UINT count, UINT start)
{
    if (g_capturing)
    {
        char call[64];
        sprintf_s(call, "Draw(%u,%u)", count, start);
        CaptureDraw(c, call, count <= 4, false);
    }
    // Full-screen passes are quads or triangles; particles are drawn with other vertex counts.
    DrawKind kind = count == 3 || count == 4 ? DrawFullscreen : DrawGeometry;
    DrawArgs d = { CtxDraw, count, start, 0, 0, 0, NULL };
    if (BeforeDraw(c, d, kind))
    {
        REAL(c, CtxDraw, Draw_t)(c, count, start);
        AfterCall(c);
        AfterDraw(c, d, kind);
    }
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start, INT base, UINT startInstance)
{
    if (g_capturing)
    {
        char call[64];
        sprintf_s(call, "DrawIndexedInstanced(%u,x%u)", count, instances);
        CaptureDraw(c, call, false, false);
    }
    DrawArgs d = { CtxDrawIndexedInstanced, count, instances, start, startInstance, base, NULL };
    if (BeforeDraw(c, d, DrawGeometry))
    {
        REAL(c, CtxDrawIndexedInstanced, DrawIndexedInstanced_t)(c, count, instances, start, base, startInstance);
        AfterCall(c);
        AfterDraw(c, d, DrawGeometry);
    }
}

static void STDMETHODCALLTYPE HookedDrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start, UINT startInstance)
{
    if (g_capturing)
    {
        char call[64];
        sprintf_s(call, "DrawInstanced(%u,x%u)", count, instances);
        CaptureDraw(c, call, count <= 4 && instances == 1, false);
    }
    DrawKind kind = (count == 3 || count == 4) && instances == 1 ? DrawFullscreen : DrawGeometry;
    DrawArgs d = { CtxDrawInstanced, count, instances, start, startInstance, 0, NULL };
    if (BeforeDraw(c, d, kind))
    {
        REAL(c, CtxDrawInstanced, DrawInstanced_t)(c, count, instances, start, startInstance);
        AfterCall(c);
        AfterDraw(c, d, kind);
    }
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    if (g_capturing) CaptureDraw(c, "DrawIndexedInstancedIndirect", false, false);
    DrawArgs d = { CtxDrawIndexedInstancedIndirect, offset, 0, 0, 0, 0, args };
    if (BeforeDraw(c, d, DrawGeometry))
    {
        REAL(c, CtxDrawIndexedInstancedIndirect, DrawIndirect_t)(c, args, offset);
        AfterCall(c);
        AfterDraw(c, d, DrawGeometry);
    }
}

static void STDMETHODCALLTYPE HookedDrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    if (g_capturing) CaptureDraw(c, "DrawInstancedIndirect", false, false);
    DrawArgs d = { CtxDrawInstancedIndirect, offset, 0, 0, 0, 0, args };
    if (BeforeDraw(c, d, DrawGeometry))
    {
        REAL(c, CtxDrawInstancedIndirect, DrawIndirect_t)(c, args, offset);
        AfterCall(c);
        AfterDraw(c, d, DrawGeometry);
    }
}

static void STDMETHODCALLTYPE HookedDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    if (g_capturing)
    {
        char call[64];
        sprintf_s(call, "Dispatch(%u,%u,%u)", x, y, z);
        CaptureDraw(c, call, true, true);
    }
    REAL(c, CtxDispatch, Dispatch_t)(c, x, y, z);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    if (g_capturing) CaptureDraw(c, "DispatchIndirect", false, true);
    REAL(c, CtxDispatchIndirect, DrawIndirect_t)(c, args, offset);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedCopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
                                                          ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
{
    if (g_capturing)
        CaptureEvent(c, "CopySubresourceRegion R%d[%u] at %u,%u <- R%d[%u]%s", CaptureResId(dst), dstSub, x, y, CaptureResId(src), srcSub,
                     box ? " (box)" : "");
    REAL(c, CtxCopySubresourceRegion, CopySubresourceRegion_t)(c, dst, dstSub, x, y, z, src, srcSub, box);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedCopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (g_capturing) CaptureEvent(c, "CopyResource R%d <- R%d", CaptureResId(dst), CaptureResId(src));
    REAL(c, CtxCopyResource, CopyResource_t)(c, dst, src);
    AfterCall(c);
}

static bool Near(float a, float b)
{
    return fabsf(a - b) < 0.002f;
}

static void STDMETHODCALLTYPE HookedClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT color[4])
{
    if (g_capturing) CaptureEvent(c, "ClearRenderTargetView R%d (%g %g %g %g)", CaptureViewResId(rtv), color[0], color[1], color[2], color[3]);
    REAL(c, CtxClearRTV, ClearRTV_t)(c, rtv, color);
    AfterCall(c);
    // The game clears its velocity target to "no motion" (127/255) with blue = 1 before drawing into it.
    if (g_dlaa && rtv && Near(color[0], 0.498039f) && Near(color[1], 0.498039f) && Near(color[2], 1) && Near(color[3], 0))
    {
        ID3D11Resource* target = ResourceOf(rtv);
        if (target != g_velocityTarget) Log("DLAA: the game's velocity target found (Motion Blur is on).");
        g_velocityTarget = target;
        ID3D11RenderTargetView* objects = g_res.objectsRtv;
        if (objects && g_dlssActive)
        {
            static const FLOAT none[4] = { -1, -1, 0, 0 }; // x < 0: no object here, use the camera motion
            REAL(c, CtxClearRTV, ClearRTV_t)(c, objects, none);
            if (c == g_immediate) g_execVelocity = true;
            else StateOf(c)->velocity = true; // travels with the command list
        }
    }
}

static void STDMETHODCALLTYPE HookedClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (g_capturing) CaptureEvent(c, "ClearDepthStencilView R%d flags=%u depth=%g stencil=%u", CaptureViewResId(dsv), flags, depth, stencil);
    REAL(c, CtxClearDSV, ClearDSV_t)(c, dsv, flags, depth, stencil);
    AfterCall(c);
    // Measured: the main depth buffer is cleared with depth and stencil exactly once per frame, first.
    if (g_dlaa && dsv && flags == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL))
    {
        bool readOnly;
        if (ClassifyDepth(dsv, &readOnly) == DepthMain) InterlockedIncrement(&g_sceneFrame);
    }
}

static void STDMETHODCALLTYPE HookedResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub,
                                                       DXGI_FORMAT format)
{
    if (g_capturing) CaptureEvent(c, "ResolveSubresource R%d <- R%d f%d", CaptureResId(dst), CaptureResId(src), (int)format);
    REAL(c, CtxResolveSubresource, ResolveSubresource_t)(c, dst, dstSub, src, srcSub, format);
    AfterCall(c);
}

static void STDMETHODCALLTYPE HookedExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore)
{
    if (g_capturing) CaptureEvent(c, "ExecuteCommandList L%p", (void*)list);
    if (g_dlaa && c == g_immediate)
    {
        // Newest first: d3d11 reuses command list addresses, so older entries for the same pointer are
        // from earlier frames (measured in the simulator: the oldest match was 88 frames old).
        LONG next = g_listNext;
        for (int k = 1; k <= (int)_countof(g_lists); k++)
        {
            const ListInfo& info = g_lists[(ULONG)(next - k) % _countof(g_lists)];
            if (info.list != list) continue;
            if (info.sceneFrame >= 0) g_execSceneFrame = info.sceneFrame;
            if (info.velocity) g_execVelocity = true;
            for (int w = 0; w < (int)_countof(info.writes); w++)
                if (info.writes[w].buffer && info.writes[w].valid) SetBufferValue(info.writes[w].buffer, info.writes[w].camera);
            if (info.cameraBuffer && BufferValue(info.cameraBuffer, g_execCamera)) g_execCameraValid = true;
            break;
        }
    }
    REAL(c, CtxExecuteCommandList, ExecuteCommandList_t)(c, list, restore);
    HookContext(c, "ExecuteCommandList");
}

static HRESULT STDMETHODCALLTYPE HookedFinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out)
{
    HRESULT hr = REAL(c, CtxFinishCommandList, FinishCommandList_t)(c, restore, out);
    ContextState* s = StateOf(c);
    if (g_dlaa && SUCCEEDED(hr) && out && *out)
    {
        LONG slot = (InterlockedIncrement(&g_listNext) - 1) % (LONG)_countof(g_lists);
        ListInfo& info = g_lists[slot];
        info.list = NULL;
        MemoryBarrier();
        info.sceneFrame = s->sceneFrame;
        info.cameraBuffer = s->cameraBuffer;
        memcpy(info.writes, s->writes, sizeof(info.writes));
        info.velocity = s->velocity;
        MemoryBarrier();
        info.list = *out;
    }
    // Per command list data starts again; without restore the pipeline state is reset too.
    s->sceneFrame = -1;
    s->cameraBuffer = NULL;
    memset(s->writes, 0, sizeof(s->writes));
    s->velocity = false;
    if (!restore) ResetState(s);
    if (g_capturing)
    {
        CaptureEvent(c, "FinishCommandList -> L%p", SUCCEEDED(hr) && out ? (void*)*out : NULL);
        CaptureCommandListFinished(c);
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedFlush(ID3D11DeviceContext* c)
{
    if (g_capturing) CaptureEvent(c, "Flush");
    REAL(c, CtxFlush, Flush_t)(c);
    HookContext(c, "Flush");
}

static const struct
{
    int index;
    void* hook;
} ContextHookList[] = {
    { CtxPSSetShader, (void*)HookedPSSetShader }, { CtxMap, (void*)HookedMap }, { CtxUnmap, (void*)HookedUnmap },
    { CtxUpdateSubresource, (void*)HookedUpdateSubresource }, { CtxDrawIndexed, (void*)HookedDrawIndexed }, { CtxDraw, (void*)HookedDraw },
    { CtxDrawIndexedInstanced, (void*)HookedDrawIndexedInstanced }, { CtxDrawInstanced, (void*)HookedDrawInstanced },
    { CtxOMSetRenderTargets, (void*)HookedOMSetRenderTargets }, { CtxOMSetRenderTargetsAndUAVs, (void*)HookedOMSetRenderTargetsAndUAVs },
    { CtxDrawIndexedInstancedIndirect, (void*)HookedDrawIndexedInstancedIndirect },
    { CtxDrawInstancedIndirect, (void*)HookedDrawInstancedIndirect }, { CtxDispatch, (void*)HookedDispatch },
    { CtxDispatchIndirect, (void*)HookedDispatchIndirect }, { CtxRSSetViewports, (void*)HookedRSSetViewports },
    { CtxCopySubresourceRegion, (void*)HookedCopySubresourceRegion }, { CtxCopyResource, (void*)HookedCopyResource },
    { CtxClearRTV, (void*)HookedClearRTV }, { CtxClearDSV, (void*)HookedClearDSV }, { CtxResolveSubresource, (void*)HookedResolveSubresource },
    { CtxExecuteCommandList, (void*)HookedExecuteCommandList }, { CtxClearState, (void*)HookedClearState }, { CtxFlush, (void*)HookedFlush },
    { CtxFinishCommandList, (void*)HookedFinishCommandList },
};

bool IsHookedIndex(int index)
{
    for (int i = 0; i < (int)_countof(ContextHookList); i++)
        if (ContextHookList[i].index == index) return true;
    return false;
}

// Patches one context's own vtable, or re-patches entries d3d11.dll has rewritten since (keeping what it
// wrote as the new original). A table address seen before (a released context's memory reused) gets its
// entry refreshed. Returns how many entries were (re)patched.
static int HookContext(ID3D11DeviceContext* ctx, const char* why)
{
    void** vtable = *(void***)ctx;
    // Fast path: everything still in place.
    bool intact = false;
    for (LONG i = 0; i < g_ctxHookCount && !intact; i++)
    {
        if (g_ctxHooks[i].vtable != vtable) continue;
        intact = true;
        for (int k = 0; k < (int)_countof(ContextHookList) && intact; k++)
            intact = vtable[ContextHookList[k].index] == ContextHookList[k].hook;
    }
    if (intact) return 0;

    static SRWLOCK lock = SRWLOCK_INIT;
    static int logged;
    int patched = 0;
    AcquireSRWLockExclusive(&lock);
    ContextHooks* h = NULL;
    for (LONG i = 0; i < g_ctxHookCount && !h; i++)
        if (g_ctxHooks[i].vtable == vtable) h = &g_ctxHooks[i];
    bool fresh = false;
    if (!h && g_ctxHookCount < (LONG)_countof(g_ctxHooks))
    {
        h = &g_ctxHooks[g_ctxHookCount];
        h->vtable = vtable;
        for (int i = 0; i < (int)_countof(ContextHookList); i++) h->real[ContextHookList[i].index] = vtable[ContextHookList[i].index];
        fresh = true;
    }
    if (!h) Log("DLAA: too many device contexts; one is not hooked.");
    if (h && (fresh || h->ctx != ctx))
    {
        h->ctx = ctx;
        ResetState(&h->state);
        h->state.sceneFrame = -1;
        h->state.cameraBuffer = NULL;
        memset(h->state.writes, 0, sizeof(h->state.writes));
        h->state.velocity = false;
    }
    for (int i = 0; h && i < (int)_countof(ContextHookList); i++)
    {
        int index = ContextHookList[i].index;
        if (vtable[index] == ContextHookList[i].hook) continue;
        DWORD oldProtect;
        if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) continue;
        h->real[index] = vtable[index];
        vtable[index] = ContextHookList[i].hook;
        VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
        patched++;
    }
    if (fresh) InterlockedIncrement(&g_ctxHookCount); // published only once its originals are filled in
    if (h && g_capture && !h->counters) CaptureInstallCounters(h);
    ReleaseSRWLockExclusive(&lock);
    if (patched && why && logged < 3)
    {
        logged++;
        Log("DLAA: d3d11 replaced %d hooked context entries (seen after %s); hooked again.", patched, why);
    }
    return patched;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDeferredContext_t)(ID3D11Device*, UINT, ID3D11DeviceContext**);
static CreateDeferredContext_t g_realCreateDeferredContext;

static HRESULT STDMETHODCALLTYPE HookedCreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out)
{
    HRESULT hr = g_realCreateDeferredContext(d, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookContext(*out, NULL);
    return hr;
}

// ---- Dumps (dlaa_dump = 1 or F10) ----

#pragma pack(push, 1)
struct DdsHeaderDx10
{
    DWORD magic, size, flags, height, width, pitchOrLinearSize, depth, mipMapCount, reserved1[11];
    DWORD pfSize, pfFlags, pfFourCC, pfRGBBitCount, pfRBitMask, pfGBitMask, pfBBitMask, pfABitMask;
    DWORD caps, caps2, caps3, caps4, reserved2;
    DWORD dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};
#pragma pack(pop)

// The game creates its HDR scene as R16G16B16A16_TYPELESS (seen in game: color dumps were skipped),
// so typeless formats are written as the float format the game views them with.
static DXGI_FORMAT DumpFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return f;
    }
}

static UINT BytesPerPixel(DXGI_FORMAT f)
{
    switch (DumpFormat(f))
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
    default: return 0;
    }
}

static void WriteDds(ID3D11DeviceContext* c, ID3D11Texture2D* tex, const wchar_t* name)
{
    if (!tex) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    UINT bpp = BytesPerPixel(d.Format);
    if (!bpp)
    {
        Log("DLAA: dump of %ls skipped (format %d).", name, d.Format);
        return;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = NULL;
    if (FAILED(g_device->CreateTexture2D(&sd, NULL, &staging))) return;
    REAL(c, CtxCopyResource, CopyResource_t)(c, staging, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(REAL(c, CtxMap, Map_t)(c, staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%sdump", g_dlaaDir);
        CreateDirectoryW(path, NULL);
        swprintf_s(path, L"%sdump\\%s.dds", g_dlaaDir, name);
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            DdsHeaderDx10 h = {};
            h.magic = 0x20534444;
            h.size = 124;
            h.flags = 0x1 | 0x2 | 0x4 | 0x1000;
            h.height = d.Height;
            h.width = d.Width;
            h.mipMapCount = 1;
            h.pfSize = 32;
            h.pfFlags = 0x4;
            h.pfFourCC = 0x30315844; // "DX10"
            h.caps = 0x1000;
            h.dxgiFormat = DumpFormat(d.Format);
            h.resourceDimension = 3;
            h.arraySize = 1;
            DWORD written;
            WriteFile(file, &h, sizeof(h), &written, NULL);
            for (UINT y = 0; y < d.Height; y++) WriteFile(file, (BYTE*)m.pData + (size_t)y * m.RowPitch, d.Width * bpp, &written, NULL);
            CloseHandle(file);
        }
        REAL(c, CtxUnmap, Unmap_t)(c, staging, 0);
    }
    staging->Release();
}

// ---- Setup and per-frame ----

void DlaaOnDeviceCreated(ID3D11Device* device)
{
    static bool done;
    if (done) return;
    done = true;
    g_device = device;

    // ID3D11Device: CreateVertexShader 12, CreatePixelShader 15, CreateComputeShader 18, CreateDeferredContext 27.
    void** dv = *(void***)device;
    bool ok = PatchVtable(dv, 12, (void*)HookedCreateVS, (void**)&g_realCreateVS) &&
              PatchVtable(dv, 15, (void*)HookedCreatePS, (void**)&g_realCreatePS) &&
              PatchVtable(dv, 18, (void*)HookedCreateCS, (void**)&g_realCreateCS) &&
              PatchVtable(dv, 27, (void*)HookedCreateDeferredContext, (void**)&g_realCreateDeferredContext);
    if (!ok) Log("DLAA: could not hook the device.");
    device->GetImmediateContext(&g_immediate);
    g_immediate->Release(); // the device keeps it alive
    HookContext(g_immediate, NULL);
    Log("DLAA: context hooks installed.");
    if (g_capture) Log("DLAA capture on: shaders are saved, F9 captures a frame.");
}

static void StartDlaa(IDXGISwapChain* swapChain)
{
    ID3D11Device* device = NULL;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device))) return;
    if (device != g_device) Log("DLAA: the swap chain's device is not the hooked one.");
    device->Release();
    __try
    {
        if (!InitNgx(g_device) || !CreateStaticResources())
        {
            g_dlaa = false;
            Log("DLAA: off.");
            return;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_dlaa = false;
        Log("DLAA: crashed while starting NGX (exception 0x%08lx); DLAA off.", GetExceptionCode());
        return;
    }
    g_dlssActive = true;
    if (g_dump) g_dumpAt = 600; // about 5 s of gameplay at 120 FPS
    Log("DLAA: on.");
}

void DlaaOnPresent(IDXGISwapChain* swapChain)
{
    if (g_immediate) HookContext(g_immediate, "the previous frame");
    DXGI_SWAP_CHAIN_DESC sc;
    if (SUCCEEDED(swapChain->GetDesc(&sc)))
    {
        g_width = sc.BufferDesc.Width;
        g_height = sc.BufferDesc.Height;
    }

    static bool started;
    if (!started && g_dlaa && g_device)
    {
        started = true;
        StartDlaa(swapChain);
    }

    if (g_dlaa)
    {
        // The read-only view of the scene depth that our velocity draws test against (created here, on
        // the render thread, once the depth buffer is known).
        ID3D11Resource* depth = g_mainDepth;
        static ID3D11Resource* readOnlyOf;
        if (depth && depth != readOnlyOf)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC vd = {};
            vd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            vd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
            ID3D11DepthStencilView* view = NULL;
            if (SUCCEEDED(g_device->CreateDepthStencilView(depth, &vd, &view))) g_mainDepthReadOnly = view; // old one leaked on purpose
            readOnlyOf = depth;
        }
        if (g_dlssActive && g_width && !g_res.objectsRtv)
            CreateSceneResources(g_width, g_height, DXGI_FORMAT_R16G16B16A16_FLOAT);

        if (g_dump && (GetAsyncKeyState(VK_F10) & 1) && g_dumpAt < 0) g_dumpAt = g_evalCount + 2;

        // A scene drawn (with jitter) but not passed through DLSS shows the jitter as shaking. Logged
        // (dlaa_dump = 1) to find such frames (e.g. a post effects path that doesn't read the scene the usual way).
        // One line per gap (its first frame); "DLSS restarted after N frame(s)" closes it. Warps give ~250
        // frames ("seen, never read": the loading fade); single frames ("not seen") come every ~20 s.
        static LONG lastSceneFrame;
        static int skipsLogged;
        LONG sceneFrame = g_sceneFrame;
        if (g_dump && g_dlssActive && g_evalCount > 0 && sceneFrame != lastSceneFrame && g_framesWithoutDlss == 1 && skipsLogged < 30)
        {
            skipsLogged++;
            Log("DLAA: a scene was drawn without DLSS (scene copy %s).", g_dlssPending ? "seen, never read" : "not seen");
        }
        lastSceneFrame = sceneFrame;

        // Frame bookkeeping: the camera becomes last frame's; nothing executed yet for the next one.
        if (g_execCameraValid)
        {
            memcpy(g_prevCamera, g_execCamera, sizeof(g_prevCamera));
            g_prevCameraValid = true;
        }
        else g_prevCameraValid = false;
        g_execCameraValid = false;
        g_execVelocity = false;
        g_dlssPending = false;
        if (g_framesWithoutDlss < 1000000) g_framesWithoutDlss++; // set to 0 by each evaluation

        static int warned;
        if (g_dlssActive && g_evalCount > 600 && !g_velocityTarget && !warned)
        {
            warned = 1;
            Log("DLAA: no velocity pass seen; turn Motion Blur on in the game's settings so moving characters don't ghost.");
        }
    }

    if (g_capture) CaptureOnPresent(swapChain);
}

void DlaaAfterPresent()
{
    if (g_immediate) HookContext(g_immediate, "Present");
}
