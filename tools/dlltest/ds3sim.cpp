// Simulates the parts of a DS3 frame that DLAA hooks into, using the game's own shader bytecode (saved
// by dlaa_capture = 1), so the DLL recognizes the same shaders and runs its whole DLSS path offline.
//
// usage: ds3sim <dir with dinput8.dll> <dir with the game's shaders> [frames]
//
// Per frame: a worker (deferred) context clears the scene depth (depth + stencil), writes cbSceneParam
// (VS b8) and draws "world" geometry into a 6-target G-buffer; a second worker clears the game's
// velocity target to 127/255 and draws one moving object with the game's velocity pixel shader, plus
// an FXAA draw; the immediate context executes both, flushes, runs YEBIS's scene copy, downsample (where
// DLSS runs) and motion blur combine (skipped by DLAA), and presents.
// The camera moves 0.01 NDC and the object 0.02 NDC to the right per frame, so the expected motion
// vectors are -0.005 * width (camera) and -0.01 * width (object) pixels.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

static const UINT W = 640, H = 360;

static const char Shaders[] =
    "cbuffer cbSceneParam : register(b8) { float4 cameraPos; float4 pad[23]; row_major float4x4 viewProj; };\n"
    "struct Full { float4 pos : SV_Position; float4 uv : TEXCOORD0; float4 uv1 : TEXCOORD1; };\n"
    "Full vsFull(uint id : SV_VertexID) { Full o; float2 p = float2(id & 1, id >> 1); o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = float4(p, 0, 0); o.uv1 = 0; return o; }\n"
    "struct Yebis { float2 uv : TEXCOORD0; float4 pos : SV_Position; };\n"
    "Yebis vsYebis(uint id : SV_VertexID) { Yebis o; float2 p = float2(id & 1, id >> 1); o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o; }\n"
    "struct World { float4 pos : SV_Position; float x : TEXCOORD0; };\n"
    "World vsWorld(float3 p : POSITION) { World o; o.pos = mul(viewProj, float4(p - cameraPos.xyz, 1)); o.x = p.x; return o; }\n"
    "struct GB { float4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; float4 o3 : SV_Target3; float4 o4 : SV_Target4; float4 o5 : SV_Target5; };\n"
    "GB psWorld(World i) { GB g; g.o0 = frac(i.x * 20) > 0.5 ? 1 : 0.2; g.o1 = g.o2 = g.o3 = g.o4 = g.o5 = 0; return g; }\n"
    "cbuffer Obj : register(b0) { float objectX; };\n"
    "struct Vel { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1; float3 t2 : TEXCOORD2; float2 t3 : TEXCOORD3; };\n"
    "Vel vsVelocity(uint id : SV_VertexID) { Vel o; float2 p = float2(id & 1, 1 - (id >> 1)) * 0.5 - 0.25; p.x += objectX; o.pos = float4(p, 0.25, 1); o.cur = o.pos; o.prev = o.pos - float4(0.02, 0, 0, 0); o.t2 = 0; o.t3 = 0; return o; }\n"
    "Texture2D src : register(t0);\n"
    "float4 psShow(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }\n";

static ID3DBlob* Compile(const char* entry, const char* target)
{
    ID3DBlob *code = nullptr, *errors = nullptr;
    if (FAILED(D3DCompile(Shaders, sizeof(Shaders) - 1, nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors)))
    {
        printf("compile %s: %s\n", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        ExitProcess(1);
    }
    return code;
}

static void* ReadFileBytes(const char* path, SIZE_T* size)
{
    FILE* f;
    if (fopen_s(&f, path, "rb")) { printf("missing %s\n", path); ExitProcess(1); }
    fseek(f, 0, SEEK_END);
    *size = (SIZE_T)ftell(f);
    fseek(f, 0, SEEK_SET);
    void* p = malloc(*size);
    fread(p, 1, *size, f);
    fclose(f);
    return p;
}

static ID3D11PixelShader* GamePs(ID3D11Device* dev, const char* dir, const char* id)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.ps", dir, id);
    SIZE_T size;
    void* code = ReadFileBytes(path, &size);
    ID3D11PixelShader* ps = nullptr;
    if (FAILED(dev->CreatePixelShader(code, size, nullptr, &ps))) { printf("CreatePixelShader %s failed\n", id); ExitProcess(1); }
    free(code);
    return ps;
}

static ID3D11Texture2D* Tex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1; d.BindFlags = bind;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) { printf("CreateTexture2D failed\n"); ExitProcess(1); }
    return t;
}

static ID3D11Buffer* DynamicCb(ID3D11Device* dev, UINT size)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b;
    dev->CreateBuffer(&bd, nullptr, &b);
    return b;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: ds3sim <dll dir> <shader dir> [frames]\n"); return 1; }
    int frames = argc > 3 ? atoi(argv[3]) : 620;
    char dll[MAX_PATH];
    sprintf_s(dll, "%s\\dinput8.dll", argv[1]);
    if (!LoadLibraryA(dll)) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ds3sim";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("ds3sim", "DARK SOULS sim", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, W + 16, H + 39, nullptr, nullptr, wc.hInstance, nullptr);

    ID3D11Device* dev;
    ID3D11DeviceContext* imm;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &imm))) return 1;
    IDXGIFactory* factory;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = W; sd.BufferDesc.Height = H; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.OutputWindow = wnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc;
    if (FAILED(factory->CreateSwapChain(dev, &sd, &sc))) return 1;

    // Our shaders.
    ID3DBlob* b;
    ID3D11VertexShader *vsFull, *vsYebis, *vsWorld, *vsVelocity;
    ID3D11PixelShader *psWorld, *psShow;
    b = Compile("vsFull", "vs_5_0"); dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsFull);
    b = Compile("vsYebis", "vs_5_0"); dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsYebis);
    b = Compile("vsVelocity", "vs_5_0"); dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsVelocity);
    b = Compile("vsWorld", "vs_5_0"); dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsWorld);
    D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    ID3D11InputLayout* layout;
    dev->CreateInputLayout(&ie, 1, b->GetBufferPointer(), b->GetBufferSize(), &layout);
    b = Compile("psWorld", "ps_5_0"); dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psWorld);
    b = Compile("psShow", "ps_5_0"); dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psShow);

    // The game's shaders.
    ID3D11PixelShader* psSceneCopy = GamePs(dev, argv[2], "393a947c35535471");
    ID3D11PixelShader* psDownsample = GamePs(dev, argv[2], "454e47f5721b6ac0");
    ID3D11PixelShader* psBlurCombine = GamePs(dev, argv[2], "e484bfebc00c5cf7");
    ID3D11PixelShader* psFxaa = GamePs(dev, argv[2], "caa861f2cf791746");
    ID3D11PixelShader* psVelocity = GamePs(dev, argv[2], "940604d2b2bcb300");

    // Targets like the game's.
    ID3D11Texture2D* depth = Tex(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* halfDepth = Tex(dev, W / 2, H / 2, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView *dsv, *halfDsv;
    dev->CreateDepthStencilView(depth, &dvd, &dsv);
    dev->CreateDepthStencilView(halfDepth, &dvd, &halfDsv);
    ID3D11RenderTargetView* gb[6];
    DXGI_FORMAT gbFormats[6] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    ID3D11Texture2D* gbTex[6];
    for (int i = 0; i < 6; i++)
    {
        gbTex[i] = Tex(dev, W, H, gbFormats[i], D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        dev->CreateRenderTargetView(gbTex[i], nullptr, &gb[i]);
    }
    ID3D11ShaderResourceView* sceneSrv; // the G-buffer's first target stands in for the lit HDR scene
    dev->CreateShaderResourceView(gbTex[0], nullptr, &sceneSrv);
    ID3D11Texture2D* yebis = Tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView* yebisRtv;
    ID3D11ShaderResourceView* yebisSrv;
    dev->CreateRenderTargetView(yebis, nullptr, &yebisRtv);
    dev->CreateShaderResourceView(yebis, nullptr, &yebisSrv);
    ID3D11Texture2D* half = Tex(dev, W / 2, H / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET);
    ID3D11RenderTargetView* halfRtv;
    dev->CreateRenderTargetView(half, nullptr, &halfRtv);
    ID3D11Texture2D* velocity = Tex(dev, W / 2, H / 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView* velocityRtv;
    dev->CreateRenderTargetView(velocity, nullptr, &velocityRtv);
    ID3D11Texture2D* ldr = Tex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView* ldrRtv;
    dev->CreateRenderTargetView(ldr, nullptr, &ldrRtv);

    ID3D11Buffer* sceneCb = DynamicCb(dev, 960);
    ID3D11Buffer* modelCb = DynamicCb(dev, 432);
    ID3D11Buffer* objCb = DynamicCb(dev, 16);
    float verts[] = { -3, 1, 0.5f, 3, 1, 0.5f, -3, -1, 0.5f, 3, -1, 0.5f }; // wider than the screen: the camera moves
    UINT16 idx[] = { 0, 1, 2, 2, 1, 3 };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(verts); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { verts };
    ID3D11Buffer *vb, *ib, *stripIb;
    dev->CreateBuffer(&bd, &init, &vb);
    bd.ByteWidth = sizeof(idx); bd.BindFlags = D3D11_BIND_INDEX_BUFFER; init.pSysMem = idx;
    dev->CreateBuffer(&bd, &init, &ib);
    UINT16 strip[] = { 0, 1, 2, 3 };
    bd.ByteWidth = sizeof(strip); init.pSysMem = strip;
    dev->CreateBuffer(&bd, &init, &stripIb);

    ID3D11DeviceContext* workers[2];
    for (auto& w : workers) dev->CreateDeferredContext(0, &w);

    D3D11_VIEWPORT full = { 0, 0, (float)W, (float)H, 0, 1 }, halfVp = { 0, 0, W / 2.0f, H / 2.0f, 0, 1 };
    for (int frame = 0; frame < frames; frame++)
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        float camX = 0.01f * (frame % 97) - 0.5f; // stays within the world quad (x -3..3); no wrap at frames 600/601

        // Scene (worker 0).
        ID3D11DeviceContext* c = workers[0];
        c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
        D3D11_MAPPED_SUBRESOURCE m;
        c->Map(sceneCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        float* f = (float*)m.pData;
        memset(f, 0, 960);
        float* inv = f + 32; // FC_MatrixInvViewProj, registers 8-11
        float* vp = f + 96;  // VC_MatrixViewProj, registers 24-27
        // Like DS3: the view-projection is camera-relative (geometry is drawn relative to cameraPos, register
        // 0 here), but the inverse leads to absolute world positions.
        for (int i = 0; i < 4; i++) inv[i * 5] = vp[i * 5] = 1;
        f[0] = -camX;   // cameraPos.x: x' = x + camX
        inv[3] = -camX; // row 0, w column: absolute x = x' - camX
        c->Unmap(sceneCb, 0);
        c->OMSetRenderTargets(6, gb, dsv);
        c->RSSetViewports(1, &full);
        c->IASetInputLayout(layout);
        UINT stride = 12, offset = 0;
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vsWorld, nullptr, 0);
        c->VSSetConstantBuffers(8, 1, &sceneCb);
        c->PSSetShader(psWorld, nullptr, 0);
        c->DrawIndexed(6, 0, 0);
        ID3D11CommandList* sceneList;
        c->FinishCommandList(FALSE, &sceneList);

        // Velocity pass and FXAA (worker 1).
        c = workers[1];
        static const float noMotion[4] = { 127 / 255.0f, 127 / 255.0f, 1, 0 };
        c->ClearRenderTargetView(velocityRtv, noMotion);
        c->ClearDepthStencilView(halfDsv, D3D11_CLEAR_DEPTH, 1, 0);
        c->Map(objCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        *(float*)m.pData = -0.5f + 0.02f * (frame % 50);
        c->Unmap(objCb, 0);
        c->Map(modelCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        ((float*)m.pData)[0] = ((float*)m.pData)[1] = 0.5f; // the game's motion blur scale
        c->Unmap(modelCb, 0);
        c->OMSetRenderTargets(1, &velocityRtv, halfDsv);
        c->RSSetViewports(1, &halfVp);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->IASetIndexBuffer(stripIb, DXGI_FORMAT_R16_UINT, 0);
        c->VSSetShader(vsVelocity, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &objCb);
        c->PSSetShader(psVelocity, nullptr, 0);
        c->PSSetConstantBuffers(7, 1, &modelCb);
        c->DrawIndexedInstanced(4, 1, 0, 0, 0); // the object: a quad, as a strip
        c->OMSetRenderTargets(1, &ldrRtv, nullptr);
        c->RSSetViewports(1, &full);
        c->VSSetShader(vsFull, nullptr, 0);
        c->PSSetShader(psFxaa, nullptr, 0);
        c->Draw(4, 0);
        ID3D11CommandList* velocityList;
        c->FinishCommandList(FALSE, &velocityList);

        // Immediate: execute, flush, YEBIS.
        imm->ExecuteCommandList(sceneList, FALSE);
        imm->ExecuteCommandList(velocityList, FALSE);
        sceneList->Release();
        velocityList->Release();
        imm->Flush();
        imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        imm->OMSetRenderTargets(1, &yebisRtv, nullptr);
        imm->RSSetViewports(1, &full);
        imm->VSSetShader(vsFull, nullptr, 0);
        imm->PSSetShader(psSceneCopy, nullptr, 0);
        imm->PSSetShaderResources(0, 1, &sceneSrv);
        imm->Draw(4, 0); // scene copy: DLAA notes its target
        imm->OMSetRenderTargets(1, &halfRtv, nullptr);
        imm->RSSetViewports(1, &halfVp);
        imm->VSSetShader(vsYebis, nullptr, 0);
        imm->PSSetShader(psDownsample, nullptr, 0);
        imm->PSSetShaderResources(0, 1, &yebisSrv);
        imm->Draw(4, 0); // first reader of the scene: DLSS runs before it
        imm->PSSetShader(psBlurCombine, nullptr, 0);
        imm->Draw(4, 0); // motion blur: skipped while DLSS is active

        ID3D11Texture2D* back;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
        ID3D11RenderTargetView* backRtv;
        dev->CreateRenderTargetView(back, nullptr, &backRtv);
        ID3D11ShaderResourceView* none = nullptr;
        imm->PSSetShaderResources(0, 1, &none);
        imm->OMSetRenderTargets(1, &backRtv, nullptr);
        imm->RSSetViewports(1, &full);
        imm->VSSetShader(vsFull, nullptr, 0);
        imm->PSSetShader(psShow, nullptr, 0);
        imm->PSSetShaderResources(0, 1, &yebisSrv);
        imm->Draw(4, 0);
        imm->PSSetShaderResources(0, 1, &none);
        sc->Present(0, 0);
        backRtv->Release();
        back->Release();
    }
    // Check: the scene depth at the centre should be 0.5 (the world quad).
    {
        D3D11_TEXTURE2D_DESC d;
        depth->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st;
        dev->CreateTexture2D(&d, nullptr, &st);
        imm->CopyResource(st, depth);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(imm->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        {
            printf("scene depth at centre: %f\n", *(float*)((BYTE*)m.pData + (H / 2) * m.RowPitch + (W / 2) * 8));
            imm->Unmap(st, 0);
        }
    }
    printf("ok %d frames\n", frames);
    return 0;
}
