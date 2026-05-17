// probe-qt-dcomp-coexist — Phase F.1.b spike.
//
// Verifies whether a custom IDCompositionVisual (backed by our own
// IDXGISwapChain1 via CreateSwapChainForComposition) can coexist with
// Qt RHI's UI swapchain on the same HWND. The Phase F D3D11 renderer
// needs DComp multi-visual to compose its HDR10 player surface over
// Qt's SDR UI swapchain — but the OS rules around HWND composition
// targets are not crystal clear, so we test empirically before
// committing the F.2+ architecture to this approach.
//
// What this does:
//   1. Spin up a QQuickWindow with a distinctive QML scene (colored
//      corner rects + center text + a popup-test button).
//   2. After the window is exposed, grab its HWND.
//   3. Init a D3D11 device + DXGI factory.
//   4. Create our own swapchain via CreateSwapChainForComposition
//      (flip-model, ready for HDR10 in a follow-on).
//   5. DCompositionCreateDevice + CreateTargetForHwnd on the SAME
//      HWND as Qt's UI window. topmost=TRUE so we sit above Qt.
//   6. Create a visual, SetContent(swapchain), translate so it
//      centers a 600x400 region in the window.
//   7. Render an animated checkerboard into our swapchain at
//      ~60 Hz so it's visually obvious if it's present.
//
// Visual expected outcomes:
//   - PASS (Option B is viable):
//       Qt's QML corner rects + center text VISIBLE, AND our
//       animated centered overlay VISIBLE on top. Resizing the
//       window keeps both updating cleanly.
//   - PARTIAL (Qt content vanishes):
//       Only our animated overlay shows, Qt's content is hidden.
//       Means same-HWND DComp takes over from Qt's swapchain.
//       Fallback: child-HWND DComp (separate variant).
//   - FAIL (our overlay doesn't appear):
//       Qt content shows but our DComp visual is invisible.
//       Means Qt's swapchain is winning the HWND composition
//       battle.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtLogging>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace {

void logHr(const char *msg, HRESULT hr)
{
    std::printf("[probe] %s (hr=0x%08lX)\n", msg,
                static_cast<unsigned long>(hr));
}

void die(const char *msg, HRESULT hr = S_OK)
{
    if (hr != S_OK)
        std::fprintf(stderr, "[probe] FATAL: %s (hr=0x%08lX)\n", msg,
                     static_cast<unsigned long>(hr));
    else
        std::fprintf(stderr, "[probe] FATAL: %s\n", msg);
    std::exit(1);
}

constexpr int kVisualW = 600;
constexpr int kVisualH = 400;

// Window proc for our child HWND. We don't need input handling for the
// spike — events should pass through to Qt's QML scene where possible
// (HTTRANSPARENT in WM_NCHITTEST). The child is purely a render target.
LRESULT CALLBACK childWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCHITTEST:
            // HTTRANSPARENT lets mouse events pass through to the parent
            // — Qt-side QML still receives input even though we visually
            // own the rectangle.
            return HTTRANSPARENT;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

class DCompOverlay {
public:
    bool initialize(HWND parentHwnd, int windowW, int windowH);
    void render(float t);
    void resizeForWindow(int windowW, int windowH);
    void shutdown();

    bool initialized = false;

private:
    void positionChild(int windowW, int windowH);

    HWND childHwnd = nullptr;

    ComPtr<ID3D11Device>          device;
    ComPtr<ID3D11DeviceContext>   ctx;
    ComPtr<IDXGIFactory2>         factory;
    ComPtr<IDXGISwapChain1>       swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;

    ComPtr<IDCompositionDevice>   dcompDevice;
    ComPtr<IDCompositionTarget>   dcompTarget;
    ComPtr<IDCompositionVisual>   dcompVisual;

    HWND  parentHwndCached = nullptr;
    int   lastWindowW    = 0;
    int   lastWindowH    = 0;
};

bool DCompOverlay::initialize(HWND parentHwnd, int windowW, int windowH)
{
    parentHwndCached = parentHwnd;
    lastWindowW = windowW;
    lastWindowH = windowH;

    HRESULT hr = S_OK;

    // -- Register window class + create child HWND --
    static const wchar_t *kClass = L"QcvDCompChild";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = childWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // No background brush — DComp owns the surface; we don't want
    // GDI erasing it.
    RegisterClassExW(&wc);   // safe to re-register; ignored if already present

    childHwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,   // critical for DComp on this HWND
        kClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, kVisualW, kVisualH,
        parentHwnd, nullptr, wc.hInstance, nullptr);
    if (!childHwnd) {
        std::fprintf(stderr, "[probe] CreateWindowExW child HWND failed (GLE=%lu)\n",
                     GetLastError());
        return false;
    }
    std::printf("[probe] child HWND %p created (parent %p)\n", childHwnd, parentHwnd);

    // -- D3D11 device --
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL flReq[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        flReq, _countof(flReq), D3D11_SDK_VERSION,
        device.GetAddressOf(), &flOut, ctx.GetAddressOf());
    if (FAILED(hr)) { logHr("D3D11CreateDevice failed", hr); return false; }
    std::printf("[probe] D3D11 device created (feature level 0x%X)\n", flOut);

    // -- DXGI factory --
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(hr = device.As(&dxgiDevice))) { logHr("QI IDXGIDevice", hr); return false; }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(hr = dxgiDevice->GetAdapter(adapter.GetAddressOf()))) { logHr("GetAdapter", hr); return false; }
    if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        logHr("Get IDXGIFactory2", hr); return false;
    }

    // -- Composition swapchain (no HWND target — DComp owns the
    //    output) --
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width  = kVisualW;
    desc.Height = kVisualH;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling    = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode  = DXGI_ALPHA_MODE_PREMULTIPLIED;  // blend over Qt's QML
    hr = factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr,
                                                  swapchain.GetAddressOf());
    if (FAILED(hr)) { logHr("CreateSwapChainForComposition", hr); return false; }
    std::printf("[probe] swapchain created (%dx%d, flip-model, premul-alpha)\n",
                kVisualW, kVisualH);

    // -- RTV on back buffer --
    ComPtr<ID3D11Texture2D> backbuf;
    if (FAILED(hr = swapchain->GetBuffer(0, IID_PPV_ARGS(backbuf.GetAddressOf())))) {
        logHr("GetBuffer", hr); return false;
    }
    if (FAILED(hr = device->CreateRenderTargetView(backbuf.Get(), nullptr, rtv.GetAddressOf()))) {
        logHr("CreateRenderTargetView", hr); return false;
    }

    // -- DComp device + target on the SAME HWND as Qt's UI window --
    hr = DCompositionCreateDevice(dxgiDevice.Get(),
                                    IID_PPV_ARGS(dcompDevice.GetAddressOf()));
    if (FAILED(hr)) { logHr("DCompositionCreateDevice", hr); return false; }

    // Phase F.1.b finding: same-HWND DComp on Qt's HWND fails with
    // DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED (0x88980800) when
    // topmost=FALSE — Qt RHI's D3D11 backend already owns the HWND's
    // primary DComp target. With topmost=TRUE same-HWND works but our
    // visual covers Qt popups.
    //
    // The clean answer is a child HWND for the player surface, with
    // its OWN DComp target. The child HWND has WS_EX_NOREDIRECTIONBITMAP
    // (required for DComp ownership) and HTTRANSPARENT hit-testing
    // (mouse events pass through to Qt). Window-type Qt popups (which
    // become their own top-level HWND) sit above the child in DWM
    // z-order, so popups over the player rect work correctly.
    hr = dcompDevice->CreateTargetForHwnd(childHwnd, TRUE /*topmost on the child*/,
                                            dcompTarget.GetAddressOf());
    if (FAILED(hr)) {
        logHr("CreateTargetForHwnd on child HWND FAILED", hr);
        return false;
    }
    std::printf("[probe] DComp target created on child HWND %p\n", childHwnd);

    // -- Visual fills the child HWND (no transform needed; child is
    //    already positioned + sized via SetWindowPos below). --
    if (FAILED(hr = dcompDevice->CreateVisual(dcompVisual.GetAddressOf()))) {
        logHr("CreateVisual", hr); return false;
    }
    if (FAILED(hr = dcompVisual->SetContent(swapchain.Get()))) {
        logHr("SetContent", hr); return false;
    }
    if (FAILED(hr = dcompTarget->SetRoot(dcompVisual.Get()))) {
        logHr("SetRoot", hr); return false;
    }
    if (FAILED(hr = dcompDevice->Commit())) { logHr("Commit", hr); return false; }
    std::printf("[probe] DComp visual set up + first commit OK\n");

    // Center the child within the parent window
    positionChild(windowW, windowH);

    initialized = true;
    return true;
}

void DCompOverlay::positionChild(int windowW, int windowH)
{
    if (!childHwnd) return;
    const int x = std::max(0, (windowW - kVisualW) / 2);
    const int y = std::max(0, (windowH - kVisualH) / 2);
    SetWindowPos(childHwnd, nullptr, x, y, kVisualW, kVisualH,
                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void DCompOverlay::resizeForWindow(int windowW, int windowH)
{
    if (!initialized) return;
    if (windowW == lastWindowW && windowH == lastWindowH) return;
    lastWindowW = windowW;
    lastWindowH = windowH;
    positionChild(windowW, windowH);
}

void DCompOverlay::render(float t)
{
    if (!initialized) return;
    // Animated color cycle so the overlay is unmistakably ours.
    const float r = 0.5f + 0.5f * std::sin(t * 1.1f + 0.0f);
    const float g = 0.5f + 0.5f * std::sin(t * 1.3f + 2.1f);
    const float b = 0.5f + 0.5f * std::sin(t * 1.7f + 4.2f);
    // Premultiplied RGBA — alpha=0.85 so Qt content shows through faintly.
    constexpr float A = 0.85f;
    const float clear[4] = { r * A, g * A, b * A, A };
    ctx->ClearRenderTargetView(rtv.Get(), clear);
    swapchain->Present(0, 0);
}

void DCompOverlay::shutdown()
{
    if (dcompDevice) dcompDevice->Commit();  // flush
    rtv.Reset();
    swapchain.Reset();
    dcompVisual.Reset();
    dcompTarget.Reset();
    dcompDevice.Reset();
    factory.Reset();
    ctx.Reset();
    device.Reset();
    if (childHwnd) {
        DestroyWindow(childHwnd);
        childHwnd = nullptr;
    }
    initialized = false;
}

} // namespace

int main(int argc, char *argv[])
{
    // Force Qt to use D3D11 RHI backend — same as the real app on Windows.
    // We want this spike to run against the same Qt-side graphics state
    // as the production code.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    engine.loadFromModule("ProbeDComp", "Main");
    if (engine.rootObjects().isEmpty()) return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window) {
        std::fprintf(stderr, "[probe] root QML object is not a QQuickWindow\n");
        return 1;
    }

    std::printf("[probe] Qt platform: %s\n",
                qPrintable(QGuiApplication::platformName()));
    std::printf("[probe] Qt RHI backend forced to D3D11\n");

    DCompOverlay overlay;
    QElapsedTimer clock;
    clock.start();

    // Set up DComp once the QQuickWindow has been exposed (so the HWND
    // exists and Qt's swapchain has been created). Try after a small
    // delay since there's no built-in "fully ready" signal that fires
    // reliably across Qt versions for this case.
    auto trySetup = [&]() {
        if (overlay.initialized) return;
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        if (!hwnd) return;
        const int w = window->width();
        const int h = window->height();
        if (w <= 0 || h <= 0) return;
        std::printf("[probe] setup: hwnd=%p window=%dx%d\n", hwnd, w, h);
        if (!overlay.initialize(hwnd, w, h)) {
            std::fprintf(stderr, "[probe] DComp setup failed — see logs above.\n");
        }
    };

    QTimer setupTimer;
    setupTimer.setSingleShot(false);
    setupTimer.setInterval(50);
    QObject::connect(&setupTimer, &QTimer::timeout, [&]() {
        if (overlay.initialized) {
            setupTimer.stop();
            return;
        }
        trySetup();
    });
    setupTimer.start();

    // Render loop — keep our DComp swapchain animated regardless of
    // Qt's own paint events.
    QTimer renderTimer;
    renderTimer.setInterval(16);
    QObject::connect(&renderTimer, &QTimer::timeout, [&]() {
        if (overlay.initialized) {
            overlay.resizeForWindow(window->width(), window->height());
            const float t = clock.elapsed() / 1000.0f;
            overlay.render(t);
        }
    });
    renderTimer.start();

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&]() {
        renderTimer.stop();
        setupTimer.stop();
        overlay.shutdown();
        std::printf("[probe] clean shutdown\n");
    });

    return app.exec();
}
