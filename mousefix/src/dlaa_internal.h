// Shared between dlaa.cpp (DLSS anti-aliasing and the D3D11 context hooks) and dlaa_capture.cpp (the
// frame capture used to find out how DS3 renders).
#pragma once
#include "shared.h"

extern wchar_t g_dlaaDir[MAX_PATH]; // <dll dir>DS3DLAA\ (nvngx_dlss.dll, NGX logs, dumps, captures)
extern bool g_capture;              // dlaa_capture = 1
extern ID3D11DeviceContext* g_immediate;

// 64-bit ID of a shader object: the first half of the checksum in its DXBC header (0 = none bound,
// 1 = created before the hooks).
unsigned long long ShaderId(ID3D11DeviceChild* shader);

// ---- Device context vtable entries ----

enum
{
    CtxPSSetShader = 9, CtxDrawIndexed = 12, CtxDraw = 13, CtxMap = 14, CtxUnmap = 15, CtxDrawIndexedInstanced = 20,
    CtxDrawInstanced = 21, CtxOMSetRenderTargets = 33, CtxOMSetRenderTargetsAndUAVs = 34, CtxDrawIndexedInstancedIndirect = 39,
    CtxDrawInstancedIndirect = 40, CtxDispatch = 41, CtxDispatchIndirect = 42, CtxRSSetViewports = 44, CtxCopySubresourceRegion = 46,
    CtxCopyResource = 47, CtxUpdateSubresource = 48, CtxClearRTV = 50, CtxClearDSV = 53, CtxResolveSubresource = 57,
    CtxExecuteCommandList = 58, CtxClearState = 110, CtxFlush = 111, CtxFinishCommandList = 114, CtxMethodCount = 115,
};

typedef void(STDMETHODCALLTYPE* PSSetShader_t)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* Unmap_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* DrawIndexed_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* Draw_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* DrawIndexedInstanced_t)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* DrawInstanced_t)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* OMSetRenderTargets_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* OMSetRenderTargetsAndUAVs_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                             UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* DrawIndirect_t)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void(STDMETHODCALLTYPE* Dispatch_t)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* RSSetViewports_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(STDMETHODCALLTYPE* CopySubresourceRegion_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                                         const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* CopyResource_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* UpdateSubresource_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* ClearRTV_t)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* ClearDSV_t)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
typedef void(STDMETHODCALLTYPE* ResolveSubresource_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
typedef void(STDMETHODCALLTYPE* ExecuteCommandList_t)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef void(STDMETHODCALLTYPE* ClearState_t)(ID3D11DeviceContext*);
typedef void(STDMETHODCALLTYPE* Flush_t)(ID3D11DeviceContext*);
typedef HRESULT(STDMETHODCALLTYPE* FinishCommandList_t)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);

// What DLAA tracks about a context's pipeline state (set by the hooks; a context is only used by one
// thread at a time, so no locking).
struct ContextState
{
    int depth;                 // DepthMain / DepthHalf / DepthOther / DepthNone
    bool depthReadOnly;
    bool ldrTarget;            // render target 0 is an 8-bit full-screen target (HUD, post effects)
    ID3D11RenderTargetView* rt0;
    UINT rtCount;
    D3D11_VIEWPORT viewport;   // as the game set it
    bool haveViewport;
    bool jittered;             // the viewport currently set is the jittered one
    int pixelShader;           // PsOther or one of the special pixel shaders (see dlaa.cpp)
    ID3D11Resource* rt0Resource;
    // Per command list being recorded: scene frame of the jitter used, the cbSceneParam buffer the
    // G-buffer draws use, camera constants written, whether the game's velocity pass is in it.
    LONG sceneFrame;
    ID3D11Resource* cameraBuffer;
    struct CameraWrite
    {
        ID3D11Resource* buffer;
        void* mapped;          // between Map and Unmap
        bool valid;
        float camera[2][16];   // inverse view-projection, view-projection (rows)
    } writes[4];
    bool velocity;
};
enum { DepthNone, DepthMain, DepthHalf, DepthOther };

struct ContextHooks
{
    void** vtable;
    void* real[128];
    ID3D11DeviceContext* ctx;
    ContextState state;
    BYTE* counters;            // capture diagnostics (dlaa_capture.cpp)
    ULONGLONG baseline[128];
};
extern ContextHooks g_ctxHooks[64];
extern volatile LONG g_ctxHookCount;

ContextHooks* HooksOf(ID3D11DeviceContext* ctx);
#define REAL(ctx, index, type) ((type)HooksOf(ctx)->real[index])
bool IsHookedIndex(int index);

// ---- dlaa_capture.cpp ----

extern volatile bool g_capturing;
void CaptureInstallCounters(ContextHooks* h);
void CaptureDraw(ID3D11DeviceContext* ctx, const char* call, bool fullscreen, bool compute);
void CaptureEvent(ID3D11DeviceContext* ctx, const char* format, ...);
int CaptureResId(ID3D11Resource* r);
int CaptureViewResId(ID3D11View* view);
void CaptureCbMapped(ID3D11DeviceContext* ctx, ID3D11Resource* r, void* data);
void CaptureCbUnmapped(ID3D11DeviceContext* ctx, ID3D11Resource* r);
void CaptureCbUpdated(ID3D11DeviceContext* ctx, ID3D11Resource* r, const void* data);
void CaptureCommandListFinished(ID3D11DeviceContext* ctx);
void CaptureOnPresent(IDXGISwapChain* swapChain); // F9 / capture.trigger, and ending a capture
