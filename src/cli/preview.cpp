#include "preview.h"
#include "ipc_reader.h"
#include "console.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include "preview_vs.h"
#include "preview_ps.h"

using Microsoft::WRL::ComPtr;

namespace cf {

namespace {

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        auto* closed = (std::atomic<bool>*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (closed) closed->store(true);
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool Preview::start(const std::wstring& title, std::string&) {
    title_ = title;
    stop_ = false;
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    return thread_ != nullptr;
}

void Preview::stop() {
    stop_ = true;
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

DWORD WINAPI Preview::threadProc(void* self) {
    ((Preview*)self)->run();
    return 0;
}

void Preview::run() {
    // Attach to the DirectShow section like any consumer.
    HANDLE sec = nullptr;
    while (!sec && !stop_) {
        sec = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ipc::kDsSectionName);
        if (!sec) Sleep(100);
    }
    if (!sec) return;
    void* base = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    auto* h = (ipc::RegionHeader*)base;
    const int reader = ipc::claimReader(h, 4);
    wchar_t evName[96];
    swprintf(evName, 96, ipc::kDsEventFmt, (unsigned)(reader < 0 ? 0 : reader));
    HANDLE evt = OpenEventW(SYNCHRONIZE, FALSE, evName);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CamerafyPreview";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title_.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&closed_);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    ComPtr<IDXGIDevice> dxgiDev;
    dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory5> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    BOOL tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    ComPtr<IDXGISwapChain2> sc;
    sc1.As(&sc);
    sc->SetMaximumFrameLatency(1);
    HANDLE waitable = sc->GetFrameLatencyWaitableObject();

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(g_vsMain, sizeof(g_vsMain), nullptr, &vs);
    dev->CreatePixelShader(g_psMain, sizeof(g_psMain), nullptr, &ps);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;
    dev->CreateSamplerState(&smp, &sampler);

    ComPtr<ID3D11Texture2D> ty, tuv;
    ComPtr<ID3D11ShaderResourceView> sy, suv;
    uint32_t tw = 0, th = 0;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT bw = 0, bh = 0;
    uint64_t last = h->publishCount.load();

    while (!stop_ && !closed_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (closed_) break;
        if (ipc::waitForFrame(h, reader, last, evt, 20) != ipc::WaitResult::Frame) continue;
        last = h->publishCount.load(std::memory_order_acquire);
        WaitForSingleObjectEx(waitable, 100, TRUE);

        const uint32_t fw = h->width, fh = h->height;
        if (fw && fh && (fw != tw || fh != th)) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = fw;
            td.Height = fh;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateTexture2D(&td, nullptr, ty.ReleaseAndGetAddressOf());
            td.Width = fw / 2;
            td.Height = fh / 2;
            td.Format = DXGI_FORMAT_R8G8_UNORM;
            dev->CreateTexture2D(&td, nullptr, tuv.ReleaseAndGetAddressOf());
            dev->CreateShaderResourceView(ty.Get(), nullptr, sy.ReleaseAndGetAddressOf());
            dev->CreateShaderResourceView(tuv.Get(), nullptr, suv.ReleaseAndGetAddressOf());
            tw = fw;
            th = fh;
        }
        if (!ty) continue;
        D3D11_MAPPED_SUBRESOURCE my{}, muv{};
        ctx->Map(ty.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &my);
        ctx->Map(tuv.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &muv);
        ipc::FrameInfo fi{};
        uint64_t retries = 0;
        const bool ok = ipc::readLatest(h, base, fi, [&](const uint8_t* src, const ipc::SlotHeader& s) {
            const uint32_t w = s.width < tw ? s.width : tw, hh = s.height < th ? s.height : th;
            for (uint32_t y = 0; y < hh; ++y) memcpy((uint8_t*)my.pData + (size_t)y * my.RowPitch, src + (size_t)y * s.strideY, w);
            for (uint32_t y = 0; y < hh / 2; ++y)
                memcpy((uint8_t*)muv.pData + (size_t)y * muv.RowPitch, src + s.offsetUV + (size_t)y * s.strideUV, w);
        }, &retries);
        ctx->Unmap(ty.Get(), 0);
        ctx->Unmap(tuv.Get(), 0);
        if (ok) ipc::recordConsumed(h, reader, fi, retries);

        RECT rc;
        GetClientRect(hwnd, &rc);
        const UINT cw = (UINT)(rc.right - rc.left), ch = (UINT)(rc.bottom - rc.top);
        if (cw && ch && (cw != bw || ch != bh)) {
            rtv.Reset();
            sc->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, sd.Flags);
            ComPtr<ID3D11Texture2D> bb;
            sc->GetBuffer(0, IID_PPV_ARGS(&bb));
            dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
            bw = cw;
            bh = ch;
        }
        if (!rtv) continue;
        D3D11_VIEWPORT vp{0, 0, (float)bw, (float)bh, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {sy.Get(), suv.Get()};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Draw(3, 0);
        sc->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    ipc::releaseReader(h, reader);
    if (evt) CloseHandle(evt);
    UnmapViewOfFile(base);
    CloseHandle(sec);
}

}  // namespace cf
