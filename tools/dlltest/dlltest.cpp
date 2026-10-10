// Test harness for DS3MouseFix's D3D11 hooks, so changes can be checked without starting the game.
// Loads <dir>\dinput8.dll (dir = first argument), then renders like DS3 does: a D3D11 device from
// D3D11CreateDevice, a swap chain from CreateDXGIFactory, worker (deferred) contexts whose command
// lists are executed on the immediate context, and Present. Presses F9 on frame 3 for a capture.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

static const char Shader[] =
    "cbuffer cbSceneParam : register(b0) { float4 tint; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "Texture2D src : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target { return tint + src.Load(int3(pos.xy, 0)); }\n";

static ID3DBlob* Compile(const char* entry, const char* target)
{
    ID3DBlob *code = nullptr, *errors = nullptr;
    if (FAILED(D3DCompile(Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors)))
    {
        printf("compile %s: %s\n", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        ExitProcess(1);
    }
    return code;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dlltest <dir with dinput8.dll>\n"); return 1; }
    char dll[MAX_PATH];
    sprintf_s(dll, "%s\\dinput8.dll", argv[1]);
    if (!LoadLibraryA(dll)) { printf("LoadLibrary %s failed %lu\n", dll, GetLastError()); return 1; }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dlltest";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("dlltest", "DARK SOULS test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    // The capture key only counts while the "game" window has focus. Windows refuses focus changes from
    // a background process unless a key event just happened, hence the Alt tap.
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(wnd);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* imm = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &imm))) return 1;
    IDXGIFactory* factory = nullptr;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 360;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(factory->CreateSwapChain(dev, &sd, &sc))) { printf("CreateSwapChain failed\n"); return 1; }

    ID3DBlob* vsCode = Compile("vs", "vs_5_0");
    ID3DBlob* psCode = Compile("ps", "ps_5_0");
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 640;
    td.Height = 360;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* scene;
    dev->CreateTexture2D(&td, nullptr, &scene);
    ID3D11RenderTargetView* sceneRtv;
    ID3D11ShaderResourceView* sceneSrv;
    dev->CreateRenderTargetView(scene, nullptr, &sceneRtv);
    dev->CreateShaderResourceView(scene, nullptr, &sceneSrv);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb;
    dev->CreateBuffer(&bd, nullptr, &cb);

    ID3D11DeviceContext* workers[2];
    for (auto& w : workers) dev->CreateDeferredContext(0, &w);

    D3D11_VIEWPORT vp = { 0, 0, 640, 360, 0, 1 };
    for (int frame = 0; frame < 8; frame++)
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (frame == 3)
        {
            keybd_event(VK_F9, 0, 0, 0);
            keybd_event(VK_F9, 0, KEYEVENTF_KEYUP, 0);
            Sleep(50);
        }
        ID3D11Texture2D* back;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
        ID3D11RenderTargetView* backRtv;
        dev->CreateRenderTargetView(back, nullptr, &backRtv);

        ID3D11CommandList* lists[2];
        for (int i = 0; i < 2; i++)
        {
            ID3D11DeviceContext* c = workers[i];
            D3D11_MAPPED_SUBRESOURCE m;
            c->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            float tint[4] = { 0.1f * i, 0.2f, 0.3f, (float)frame };
            memcpy(m.pData, tint, 16);
            c->Unmap(cb, 0);
            c->VSSetShader(vs, nullptr, 0);
            c->PSSetShader(ps, nullptr, 0);
            c->PSSetConstantBuffers(0, 1, &cb);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->RSSetViewports(1, &vp);
            if (i == 0)
            {
                float clear[4] = { 0, 0, 0, 1 };
                c->ClearRenderTargetView(sceneRtv, clear);
                c->OMSetRenderTargets(1, &sceneRtv, nullptr);
                c->DrawIndexed(0, 0, 0); // stands in for geometry
            }
            else
            {
                c->OMSetRenderTargets(1, &backRtv, nullptr);
                c->PSSetShaderResources(0, 1, &sceneSrv);
                c->Draw(3, 0); // full-screen post pass
            }
            c->FinishCommandList(FALSE, &lists[i]);
        }
        for (auto l : lists)
        {
            imm->ExecuteCommandList(l, FALSE);
            l->Release();
        }
        // DS3 flushes every frame and then draws on the immediate context; d3d11 rewrites the
        // immediate context's draw entries on Flush, which the hooks must survive.
        imm->Flush();
        imm->OMSetRenderTargets(1, &backRtv, nullptr);
        imm->VSSetShader(vs, nullptr, 0);
        imm->PSSetShader(ps, nullptr, 0);
        imm->PSSetShaderResources(0, 1, &sceneSrv);
        imm->PSSetConstantBuffers(0, 1, &cb);
        imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        imm->RSSetViewports(1, &vp);
        imm->Draw(3, 0); // stands in for the HUD
        imm->Draw(3, 0); // a second draw: the first one after Flush switches the entries again
        ID3D11ShaderResourceView* none = nullptr;
        imm->PSSetShaderResources(0, 1, &none);
        sc->Present(1, 0);
        backRtv->Release();
        back->Release();
    }
    printf("ok\n");
    return 0;
}
