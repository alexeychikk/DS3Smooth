// Frame capture (dlaa_capture = 1): a developer tool that records how DS3 renders, used to find where
// DLSS fits in. Pressing F9 in game (or creating DS3DLAA\capture.trigger) writes four frames of draw
// calls to DS3DLAA\capture\frame<n>.txt: shaders, targets, inputs, and the constant buffers of
// full-screen passes (f<n>_d<draw>_<stage>_b<slot>.bin). Every shader the game creates is saved to
// DS3DLAA\shaders\ (dlaa.cpp); tools\disasm_shaders.py disassembles them.
//
// Four frames, because the worker threads record up to a frame ahead of the immediate context: only the
// middle frames have all their command lists recorded inside the capture (measured: with two frames, the
// tonemapping, HUD and final copy were recorded before the capture started). Lines are tagged with the
// context ([c<n>]) because several worker threads record at once. A context's vertex-stage constant
// buffers are also saved for the first draw it records into each command list (camera constants).

#include "dlaa_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

volatile bool g_capturing;
static int g_capPresents;
static SRWLOCK g_capLock = SRWLOCK_INIT;
static char* g_capBuf;
static size_t g_capLen;
static const size_t CapBufSize = 64 << 20;
static int g_capFrame, g_capDraw;
static ID3D11Resource* g_capRes[8192]; // AddRef'd while the capture runs
static int g_capResCount;
static ID3D11DeviceContext* g_capCtx[64];
static bool g_capCtxFresh[64]; // no draw recorded since its last FinishCommandList
static int g_capCtxCount;

// ---- Call counters ----
//
// Every vtable entry that isn't hooked gets a tiny thunk that counts calls and jumps on, so a capture
// shows which context methods each context really uses. Added because nine of the game's worker contexts
// seemed to finish a command list every frame without a single hooked call (the cause turned out to be
// d3d11.dll rewriting the immediate context's entries on Flush; see dlaa.cpp).

static const char* const ContextMethodNames[CtxMethodCount] = {
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
    "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader", "PSSetSamplers", "VSSetShader", "DrawIndexed", "Draw", "Map", "Unmap",
    "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer", "DrawIndexedInstanced", "DrawInstanced",
    "GSSetConstantBuffers", "GSSetShader", "IASetPrimitiveTopology", "VSSetShaderResources", "VSSetSamplers", "Begin", "End", "GetData",
    "SetPredication", "GSSetShaderResources", "GSSetSamplers", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews",
    "OMSetBlendState", "OMSetDepthStencilState", "SOSetTargets", "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect",
    "Dispatch", "DispatchIndirect", "RSSetState", "RSSetViewports", "RSSetScissorRects", "CopySubresourceRegion", "CopyResource",
    "UpdateSubresource", "CopyStructureCount", "ClearRenderTargetView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat",
    "ClearDepthStencilView", "GenerateMips", "SetResourceMinLOD", "GetResourceMinLOD", "ResolveSubresource", "ExecuteCommandList",
    "HSSetShaderResources", "HSSetShader", "HSSetSamplers", "HSSetConstantBuffers", "DSSetShaderResources", "DSSetShader", "DSSetSamplers",
    "DSSetConstantBuffers", "CSSetShaderResources", "CSSetUnorderedAccessViews", "CSSetShader", "CSSetSamplers", "CSSetConstantBuffers",
    "VSGetConstantBuffers", "PSGetShaderResources", "PSGetShader", "PSGetSamplers", "VSGetShader", "PSGetConstantBuffers",
    "IAGetInputLayout", "IAGetVertexBuffers", "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader", "IAGetPrimitiveTopology",
    "VSGetShaderResources", "VSGetSamplers", "GetPredication", "GSGetShaderResources", "GSGetSamplers", "OMGetRenderTargets",
    "OMGetRenderTargetsAndUnorderedAccessViews", "OMGetBlendState", "OMGetDepthStencilState", "SOGetTargets", "RSGetState",
    "RSGetViewports", "RSGetScissorRects", "HSGetShaderResources", "HSGetShader", "HSGetSamplers", "HSGetConstantBuffers",
    "DSGetShaderResources", "DSGetShader", "DSGetSamplers", "DSGetConstantBuffers", "CSGetShaderResources", "CSGetUnorderedAccessViews",
    "CSGetShader", "CSGetSamplers", "CSGetConstantBuffers", "ClearState", "Flush", "GetType", "GetContextFlags", "FinishCommandList",
};

static ULONGLONG CallCount(const ContextHooks* h, int index)
{
    return h->counters ? ((const ULONGLONG*)(h->counters + 2048))[index] : 0;
}

// Page layout: 128 thunks of 16 bytes, then 128 counters, then 128 jump targets.
void CaptureInstallCounters(ContextHooks* h)
{
    BYTE* page = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) return;
    ULONGLONG* counters = (ULONGLONG*)(page + 2048);
    void** targets = (void**)(page + 3072);
    void** vtable = h->vtable;
    for (int index = 3; index < CtxMethodCount; index++)
    {
        if (IsHookedIndex(index)) continue;
        BYTE* t = page + index * 16;
        targets[index] = vtable[index];
        INT32 toCounter = (INT32)((BYTE*)&counters[index] - (t + 8));
        INT32 toTarget = (INT32)((BYTE*)&targets[index] - (t + 14));
        t[0] = 0xF0; t[1] = 0x48; t[2] = 0xFF; t[3] = 0x05; // lock inc qword ptr [rip+toCounter]
        memcpy(t + 4, &toCounter, 4);
        t[8] = 0xFF; t[9] = 0x25; // jmp qword ptr [rip+toTarget]
        memcpy(t + 10, &toTarget, 4);
        DWORD oldProtect;
        if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) continue;
        vtable[index] = t;
        VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    }
    FlushInstructionCache(GetCurrentProcess(), page, 2048);
    h->counters = page;
}

// ---- Constant buffer snapshots ----
//
// The latest contents each context wrote to each constant buffer (Map/Unmap or UpdateSubresource).
// Deferred contexts can't read buffers back, so this is how their constants are captured.

struct CbSnapshot
{
    ID3D11DeviceContext* ctx;
    ID3D11Resource* buffer;
    void* mapped; // between Map and Unmap
    UINT size;
    BYTE* data;
};
static CbSnapshot g_snap[4096];
static int g_snapCount;

static CbSnapshot* FindSnapshot(ID3D11DeviceContext* ctx, ID3D11Resource* buffer, bool add)
{
    for (int i = 0; i < g_snapCount; i++)
        if (g_snap[i].ctx == ctx && g_snap[i].buffer == buffer) return &g_snap[i];
    if (!add || g_snapCount == _countof(g_snap)) return NULL;
    CbSnapshot* s = &g_snap[g_snapCount++];
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->buffer = buffer;
    return s;
}

static void StoreSnapshot(CbSnapshot* s, const void* data, UINT size)
{
    if (s->size < size)
    {
        free(s->data);
        s->data = (BYTE*)malloc(size);
        s->size = s->data ? size : 0;
    }
    if (s->data) memcpy(s->data, data, size);
}

static bool IsConstantBuffer(ID3D11Resource* r, UINT* size)
{
    D3D11_RESOURCE_DIMENSION type;
    r->GetType(&type);
    if (type != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d;
    ((ID3D11Buffer*)r)->GetDesc(&d);
    *size = d.ByteWidth;
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

void CaptureCbMapped(ID3D11DeviceContext* ctx, ID3D11Resource* r, void* data)
{
    UINT size;
    if (!IsConstantBuffer(r, &size)) return;
    AcquireSRWLockExclusive(&g_capLock);
    if (CbSnapshot* s = FindSnapshot(ctx, r, true)) s->mapped = data;
    ReleaseSRWLockExclusive(&g_capLock);
}

void CaptureCbUnmapped(ID3D11DeviceContext* ctx, ID3D11Resource* r)
{
    UINT size;
    if (!IsConstantBuffer(r, &size)) return;
    AcquireSRWLockExclusive(&g_capLock);
    CbSnapshot* s = FindSnapshot(ctx, r, false);
    if (s && s->mapped)
    {
        StoreSnapshot(s, s->mapped, size);
        s->mapped = NULL;
    }
    ReleaseSRWLockExclusive(&g_capLock);
}

void CaptureCbUpdated(ID3D11DeviceContext* ctx, ID3D11Resource* r, const void* data)
{
    UINT size;
    if (!IsConstantBuffer(r, &size)) return;
    AcquireSRWLockExclusive(&g_capLock);
    if (CbSnapshot* s = FindSnapshot(ctx, r, true)) StoreSnapshot(s, data, size);
    ReleaseSRWLockExclusive(&g_capLock);
}

// ---- Recording ----

static void Cap(const char* format, ...)
{
    if (!g_capBuf || g_capLen >= CapBufSize - 1) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(g_capBuf + g_capLen, CapBufSize - g_capLen, format, args);
    va_end(args);
    if (n > 0) g_capLen = min(g_capLen + n, CapBufSize - 1);
}

static int ResId(ID3D11Resource* r)
{
    for (int i = 0; i < g_capResCount; i++)
        if (g_capRes[i] == r) return i;
    if (g_capResCount == _countof(g_capRes)) return -1;
    r->AddRef();
    g_capRes[g_capResCount] = r;
    return g_capResCount++;
}

static int CtxId(ID3D11DeviceContext* ctx)
{
    for (int i = 0; i < g_capCtxCount; i++)
        if (g_capCtx[i] == ctx) return i;
    if (g_capCtxCount == _countof(g_capCtx)) return _countof(g_capCtx) - 1;
    g_capCtx[g_capCtxCount] = ctx;
    g_capCtxFresh[g_capCtxCount] = true;
    return g_capCtxCount++;
}

// " name=R<id>:f<view format>" for a view, nothing for an empty slot.
static void CapView(const char* name, ID3D11View* view, DXGI_FORMAT format)
{
    if (!view) return;
    ID3D11Resource* r = NULL;
    view->GetResource(&r);
    Cap(" %s=R%d:f%d", name, r ? ResId(r) : -1, (int)format);
    if (r) r->Release();
}

static void CapSrvs(ID3D11ShaderResourceView** srvs, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (!srvs[i]) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC d;
        srvs[i]->GetDesc(&d);
        char name[8];
        sprintf_s(name, "t%d", i);
        CapView(name, srvs[i], d.Format);
        srvs[i]->Release();
    }
}

static void WriteCbFile(const void* data, UINT size, char stage, int slot)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%scapture\\f%d_d%05d_%c_b%d.bin", g_dlaaDir, g_capFrame, g_capDraw, stage, slot);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, data, size, &written, NULL);
    CloseHandle(file);
}

// Saves a constant buffer's contents: what this context last wrote to it, else what any context last
// wrote to it, else for the immediate context a GPU readback. Returns false if none is available.
static bool DumpConstantBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, char stage, int slot)
{
    CbSnapshot* s = FindSnapshot(ctx, cb, false);
    for (int i = 0; i < g_snapCount && !(s && s->data); i++)
        if (g_snap[i].buffer == cb) s = &g_snap[i];
    if (s && s->data)
    {
        WriteCbFile(s->data, s->size, stage, slot);
        return true;
    }
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;

    D3D11_BUFFER_DESC d;
    cb->GetDesc(&d);
    ID3D11Device* device = NULL;
    ctx->GetDevice(&device);
    D3D11_BUFFER_DESC sd = {};
    sd.ByteWidth = d.ByteWidth;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = NULL;
    bool ok = false;
    if (SUCCEEDED(device->CreateBuffer(&sd, NULL, &staging)))
    {
        REAL(ctx, CtxCopyResource, CopyResource_t)(ctx, staging, cb); // not ctx->...: the hooks would take g_capLock again
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(REAL(ctx, CtxMap, Map_t)(ctx, staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            WriteCbFile(m.pData, d.ByteWidth, stage, slot);
            REAL(ctx, CtxUnmap, Unmap_t)(ctx, staging, 0);
            ok = true;
        }
        staging->Release();
    }
    device->Release();
    return ok;
}

// One line per draw/dispatch: shaders, targets, inputs. Full-screen passes and dispatches also get
// their constant buffers saved (marked * in the line).
void CaptureDraw(ID3D11DeviceContext* ctx, const char* call, bool fullscreen, bool compute)
{
    AcquireSRWLockExclusive(&g_capLock);
    int draw = ++g_capDraw;
    Cap("#%d [c%d] %s", draw, CtxId(ctx), call);

    ID3D11ShaderResourceView* srvs[16] = {};
    ID3D11Buffer* cbs[4] = {};
    char stage;
    if (compute)
    {
        stage = 'c';
        ID3D11ComputeShader* cs = NULL;
        ctx->CSGetShader(&cs, NULL, NULL);
        Cap(" CS=%016llx", ShaderId(cs));
        if (cs) cs->Release();
        ID3D11UnorderedAccessView* uavs[8] = {};
        ctx->CSGetUnorderedAccessViews(0, 8, uavs);
        for (int i = 0; i < 8; i++)
        {
            if (!uavs[i]) continue;
            D3D11_UNORDERED_ACCESS_VIEW_DESC d;
            uavs[i]->GetDesc(&d);
            char name[8];
            sprintf_s(name, "u%d", i);
            CapView(name, uavs[i], d.Format);
            uavs[i]->Release();
        }
        ctx->CSGetShaderResources(0, 16, srvs);
        ctx->CSGetConstantBuffers(0, 4, cbs);
    }
    else
    {
        stage = 'p';
        ID3D11VertexShader* vs = NULL;
        ID3D11PixelShader* ps = NULL;
        ctx->VSGetShader(&vs, NULL, NULL);
        int c = CtxId(ctx);
        if (g_capCtxFresh[c])
        {
            g_capCtxFresh[c] = false;
            ID3D11Buffer* vcbs[16] = {};
            ctx->VSGetConstantBuffers(0, 16, vcbs);
            for (int i = 0; i < 16; i++)
            {
                if (!vcbs[i]) continue;
                if (DumpConstantBuffer(ctx, vcbs[i], 'v', i)) Cap(" vb%d*", i);
                vcbs[i]->Release();
            }
        }
        ctx->PSGetShader(&ps, NULL, NULL);
        Cap(" VS=%016llx PS=%016llx", ShaderId(vs), ShaderId(ps));
        if (vs) vs->Release();
        if (ps) ps->Release();
        D3D11_VIEWPORT vp;
        UINT vpCount = 1;
        ctx->RSGetViewports(&vpCount, &vp);
        if (vpCount) Cap(" vp=%.0fx%.0f", vp.Width, vp.Height);
        if (vpCount && (vp.TopLeftX != floorf(vp.TopLeftX) || vp.TopLeftY != floorf(vp.TopLeftY))) Cap("@%.3f,%.3f", vp.TopLeftX, vp.TopLeftY);
        ID3D11RenderTargetView* rtvs[8] = {};
        ID3D11DepthStencilView* dsv = NULL;
        ctx->OMGetRenderTargets(8, rtvs, &dsv);
        for (int i = 0; i < 8; i++)
        {
            if (!rtvs[i]) continue;
            D3D11_RENDER_TARGET_VIEW_DESC d;
            rtvs[i]->GetDesc(&d);
            char name[8];
            sprintf_s(name, "o%d", i);
            CapView(name, rtvs[i], d.Format);
            rtvs[i]->Release();
        }
        if (dsv)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC d;
            dsv->GetDesc(&d);
            CapView(d.Flags ? "ds(ro)" : "ds", dsv, d.Format);
            dsv->Release();
        }
        ctx->PSGetShaderResources(0, 16, srvs);
        ctx->PSGetConstantBuffers(0, 4, cbs);
    }
    CapSrvs(srvs, 16);
    for (int i = 0; i < 4; i++)
    {
        if (!cbs[i]) continue;
        D3D11_BUFFER_DESC d;
        cbs[i]->GetDesc(&d);
        bool saved = fullscreen && DumpConstantBuffer(ctx, cbs[i], stage, i);
        Cap(" b%d=%u%s", i, d.ByteWidth, saved ? "*" : "");
        cbs[i]->Release();
    }
    Cap("\n");
    ReleaseSRWLockExclusive(&g_capLock);
}

void CaptureEvent(ID3D11DeviceContext* ctx, const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsprintf_s(line, format, args);
    va_end(args);
    AcquireSRWLockExclusive(&g_capLock);
    Cap("   [c%d] %s\n", CtxId(ctx), line);
    ReleaseSRWLockExclusive(&g_capLock);
}

int CaptureResId(ID3D11Resource* r)
{
    AcquireSRWLockExclusive(&g_capLock);
    int id = r ? ResId(r) : -1;
    ReleaseSRWLockExclusive(&g_capLock);
    return id;
}

int CaptureViewResId(ID3D11View* view)
{
    if (!view) return -1;
    ID3D11Resource* r = NULL;
    view->GetResource(&r);
    int id = CaptureResId(r);
    if (r) r->Release();
    return id;
}

void CaptureCommandListFinished(ID3D11DeviceContext* ctx)
{
    AcquireSRWLockExclusive(&g_capLock);
    g_capCtxFresh[CtxId(ctx)] = true;
    ReleaseSRWLockExclusive(&g_capLock);
}

static void StartCapture()
{
    if (!g_capBuf) g_capBuf = (char*)VirtualAlloc(NULL, CapBufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_capBuf) return;
    AcquireSRWLockExclusive(&g_capLock);
    g_capLen = 0;
    g_capDraw = 0;
    g_capCtxCount = 0;
    g_capFrame++;
    g_capPresents = 0;
    for (LONG i = 0; i < g_ctxHookCount; i++)
        for (int m = 0; m < 128; m++) g_ctxHooks[i].baseline[m] = CallCount(&g_ctxHooks[i], m);
    ReleaseSRWLockExclusive(&g_capLock);
    g_capturing = true;
}

static void FinishCapture(IDXGISwapChain* swapChain)
{
    g_capturing = false;
    AcquireSRWLockExclusive(&g_capLock);
    ID3D11Texture2D* back = NULL;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)))
    {
        Cap("Back buffer: R%d\n", ResId(back));
        back->Release();
    }
    Cap("\nContexts:");
    for (int i = 0; i < g_capCtxCount; i++)
        Cap(" c%d=%p%s", i, (void*)g_capCtx[i], g_capCtx[i] && g_capCtx[i]->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "(immediate)" : "");
    Cap("\n\nCalls during the capture to methods that aren't hooked:\n");
    for (LONG i = 0; i < g_ctxHookCount; i++)
    {
        const ContextHooks* h = &g_ctxHooks[i];
        Cap("c%d:", CtxId(h->ctx));
        for (int m = 3; m < CtxMethodCount; m++)
        {
            ULONGLONG n = CallCount(h, m) - h->baseline[m];
            if (n) Cap(" %s=%llu", ContextMethodNames[m], n);
        }
        Cap("\n");
    }
    Cap("\nResources:\n");
    for (int i = 0; i < g_capResCount; i++)
    {
        D3D11_RESOURCE_DIMENSION type;
        g_capRes[i]->GetType(&type);
        if (type == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC d;
            ((ID3D11Texture2D*)g_capRes[i])->GetDesc(&d);
            Cap("R%d tex2d %ux%u f%d mips=%u array=%u msaa=%u bind=0x%x usage=%d misc=0x%x\n", i, d.Width, d.Height, (int)d.Format,
                d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags, (int)d.Usage, d.MiscFlags);
        }
        else if (type == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC d;
            ((ID3D11Buffer*)g_capRes[i])->GetDesc(&d);
            Cap("R%d buffer %u bytes bind=0x%x usage=%d misc=0x%x stride=%u\n", i, d.ByteWidth, d.BindFlags, (int)d.Usage, d.MiscFlags,
                d.StructureByteStride);
        }
        else if (type == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        {
            D3D11_TEXTURE3D_DESC d;
            ((ID3D11Texture3D*)g_capRes[i])->GetDesc(&d);
            Cap("R%d tex3d %ux%ux%u f%d\n", i, d.Width, d.Height, d.Depth, (int)d.Format);
        }
        else Cap("R%d type %d\n", i, (int)type);
        g_capRes[i]->Release();
    }
    g_capResCount = 0;
    for (int i = 0; i < g_snapCount; i++) free(g_snap[i].data);
    g_snapCount = 0;

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%scapture\\frame%d.txt", g_dlaaDir, g_capFrame);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, g_capBuf, (DWORD)g_capLen, &written, NULL);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_capLock);
    Log("DLAA capture: frame %d written (%d draws/dispatches).", g_capFrame, g_capDraw);
}

void CaptureOnPresent(IDXGISwapChain* swapChain)
{
    if (g_capturing)
    {
        if (++g_capPresents >= 4) FinishCapture(swapChain);
        else
        {
            AcquireSRWLockExclusive(&g_capLock);
            Cap("==== Present ====\n");
            ReleaseSRWLockExclusive(&g_capLock);
        }
        return;
    }
    // F9 (low bit: pressed since the last call), only while the game has focus.
    if ((GetAsyncKeyState(VK_F9) & 1) && g_immediate)
    {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) StartCapture();
        return;
    }
    // A file DS3DLAA\capture.trigger also starts one (checked about twice a second), so captures can be
    // taken by a script without keyboard focus.
    static int frames;
    if (g_immediate && ++frames % 60 == 0)
    {
        wchar_t trigger[MAX_PATH];
        swprintf_s(trigger, L"%scapture.trigger", g_dlaaDir);
        if (GetFileAttributesW(trigger) != INVALID_FILE_ATTRIBUTES && DeleteFileW(trigger)) StartCapture();
    }
}
