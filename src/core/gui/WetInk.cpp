#include "WetInk.h"

#include <utility>

#ifdef _WIN32

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <gdk/gdkwin32.h>
#include <windows.h>

namespace {

struct InkPoint {
    float x;
    float y;
    float radius;
    long long received;  ///< QueryPerformanceCounter when the point was added
};

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

auto qpcNow() -> long long {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

auto qpcFrequency() -> long long {
    static const long long frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency;
}

/// Present this long before the display's refresh, so the frame still makes it into the compositor's next frame
constexpr double PRESENT_MARGIN_MS = 2.5;
/// Clear the layer at the latest this long after the stroke ended, if GTK draws no frame meanwhile
constexpr guint CLEAR_FALLBACK_MS = 150;

}  // namespace

struct WetInk::Impl {
    HWND hwnd = nullptr;
    // Direct3D, DirectComposition and Direct2D; after setUp() only used by the render thread
    ID3D11Device* d3d = nullptr;
    IDXGIDevice* dxgiDevice = nullptr;
    IDCompositionDevice* dcomp = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    IDXGISwapChain2* swapChain = nullptr;
    HANDLE frameWaitable = nullptr;
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    ID2D1Bitmap1* targetBitmap = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1StrokeStyle* roundStyle = nullptr;
    HANDLE timer = nullptr;
    UINT width = 0;
    UINT height = 0;

    // Shared with the render thread (under mutex)
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<InkPoint> points;
    size_t drawnByGtk = 0;  ///< GTK's canvas shows the points before this index
    D2D1_COLOR_F color{0, 0, 0, 1};
    bool active = false;
    bool ended = false;
    bool clearPending = false;
    unsigned version = 0;
    UINT wantWidth = 0;
    UINT wantHeight = 0;
    bool quit = false;
    bool failed = false;

    std::thread thread;
    guint clearTimer = 0;  ///< main thread

    // Statistics (render thread)
    FILE* stats = nullptr;
    struct Presented {
        UINT presentId;
        long long newestInput;
    };
    std::deque<Presented> presented;

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        wake.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
        if (clearTimer) {
            g_source_remove(clearTimer);
        }
        if (stats) {
            std::fclose(stats);
        }
        if (timer) {
            CloseHandle(timer);
        }
        release(roundStyle);
        release(brush);
        release(targetBitmap);
        release(dc);
        release(d2dDevice);
        release(d2dFactory);
        release(swapChain);
        release(visual);
        release(target);
        release(dcomp);
        release(dxgiDevice);
        release(d3d);
    }

    auto clientSize() const -> std::pair<UINT, UINT> {
        RECT r{};
        GetClientRect(hwnd, &r);
        return {static_cast<UINT>(std::max<LONG>(1, r.right - r.left)),
                static_cast<UINT>(std::max<LONG>(1, r.bottom - r.top))};
    }

    auto createTargetBitmap() -> bool {
        IDXGISurface* surface = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(&surface)))) {
            return false;
        }
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        props.dpiX = 96;  // one DIP is one pixel
        props.dpiY = 96;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        const HRESULT created = dc->CreateBitmapFromDxgiSurface(surface, &props, &targetBitmap);
        release(surface);
        if (FAILED(created)) {
            return false;
        }
        dc->SetTarget(targetBitmap);
        return true;
    }

    auto setUp() -> bool {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) ||
            FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
            FAILED(DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice),
                                            reinterpret_cast<void**>(&dcomp))) ||
            FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual))) {
            return false;
        }
        IDXGIFactory2* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
            return false;
        }
        std::tie(width, height) = clientSize();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        IDXGISwapChain1* swapChain1 = nullptr;
        const HRESULT created = factory->CreateSwapChainForComposition(d3d, &desc, nullptr, &swapChain1);
        release(factory);
        if (FAILED(created)) {
            return false;
        }
        const HRESULT queried =
                swapChain1->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&swapChain));
        release(swapChain1);
        if (FAILED(queried)) {
            return false;
        }
        // At most one frame queued: every frame shows the newest stroke instead of waiting behind an older one
        swapChain->SetMaximumFrameLatency(1);
        frameWaitable = swapChain->GetFrameLatencyWaitableObject();
        if (FAILED(visual->SetContent(swapChain)) || FAILED(target->SetRoot(visual)) || FAILED(dcomp->Commit())) {
            return false;
        }

        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                     reinterpret_cast<void**>(&d2dFactory))) ||
            FAILED(d2dFactory->CreateDevice(dxgiDevice, &d2dDevice)) ||
            FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) || !createTargetBitmap()) {
            return false;
        }
        const D2D1_COLOR_F black{0, 0, 0, 1};
        D2D1_STROKE_STYLE_PROPERTIES style{D2D1_CAP_STYLE_ROUND,
                                           D2D1_CAP_STYLE_ROUND,
                                           D2D1_CAP_STYLE_ROUND,
                                           D2D1_LINE_JOIN_ROUND,
                                           10.0f,
                                           D2D1_DASH_STYLE_SOLID,
                                           0.0f};
        if (FAILED(dc->CreateSolidColorBrush(&black, nullptr, &brush)) ||
            FAILED(d2dFactory->CreateStrokeStyle(&style, nullptr, 0, &roundStyle))) {
            return false;
        }
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (const char* path = std::getenv("XOPP_WET_INK_STATS")) {
            stats = std::fopen(path, "w");
            if (stats) {
                std::fprintf(stats, "Q %lld\n", qpcFrequency());
            }
        }
        render({}, color);  // transparent to start with
        thread = std::thread([this] { run(); });
        return true;
    }

    /// Sleeps until shortly before the display's next refresh (DWM's timing), so the frame has the newest points
    void waitForPresentTime() {
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) || info.qpcRefreshPeriod == 0) {
            return;
        }
        const long long period = static_cast<long long>(info.qpcRefreshPeriod);
        const long long margin =
                static_cast<long long>(PRESENT_MARGIN_MS * static_cast<double>(qpcFrequency()) / 1000.0);
        long long deadline = static_cast<long long>(info.qpcVBlank) + period - margin;
        const long long now = qpcNow();
        while (deadline < now) {
            deadline += period;
        }
        // Sleep with the high-resolution timer, then spin the last half millisecond
        const long long spin = qpcFrequency() / 2000;
        if (timer && deadline - now > spin) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((deadline - now - spin) * 10000000LL / qpcFrequency());
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                WaitForSingleObject(timer, INFINITE);
            }
        }
        while (qpcNow() < deadline) {
            YieldProcessor();
        }
    }

    void render(const std::vector<InkPoint>& segment, const D2D1_COLOR_F& c) {
        dc->BeginDraw();
        const D2D1_COLOR_F transparent{0, 0, 0, 0};
        dc->Clear(&transparent);
        if (!segment.empty()) {
            brush->SetColor(&c);
            if (segment.size() == 1) {
                const D2D1_POINT_2F p{segment[0].x, segment[0].y};
                dc->DrawLine(p, p, brush, 2 * segment[0].radius, roundStyle);
            }
            for (size_t i = 1; i < segment.size(); i++) {
                const D2D1_POINT_2F a{segment[i - 1].x, segment[i - 1].y};
                const D2D1_POINT_2F b{segment[i].x, segment[i].y};
                dc->DrawLine(a, b, brush, segment[i - 1].radius + segment[i].radius, roundStyle);
            }
        }
        if (FAILED(dc->EndDraw())) {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;  // e.g. the device was lost: GTK alone draws from now on
            return;
        }
        swapChain->Present(0, 0);
    }

    void resize(UINT w, UINT h) {
        dc->SetTarget(nullptr);
        release(targetBitmap);
        if (SUCCEEDED(swapChain->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN,
                                               DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) &&
            createTargetBitmap()) {
            width = w;
            height = h;
        } else {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
        }
    }

    void logStatistics(long long newestInput) {
        UINT presentId = 0;
        if (FAILED(swapChain->GetLastPresentCount(&presentId))) {
            return;
        }
        presented.push_back({presentId, newestInput});
        DXGI_FRAME_STATISTICS frame{};
        if (FAILED(swapChain->GetFrameStatistics(&frame))) {
            return;
        }
        // The frame on screen since the refresh at SyncQPCTime: input received -> on the display
        while (!presented.empty() && presented.front().presentId <= frame.PresentCount) {
            if (presented.front().presentId == frame.PresentCount && presented.front().newestInput) {
                std::fprintf(stats, "P %u %lld %lld\n", frame.PresentCount, presented.front().newestInput,
                             frame.SyncQPCTime.QuadPart);
                std::fflush(stats);
            }
            presented.pop_front();
        }
    }

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        unsigned rendered = version;
        bool shown = false;  ///< the layer shows something
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return quit || ((active || clearPending) && !failed); });
                if (quit) {
                    return;
                }
            }
            WaitForSingleObjectEx(frameWaitable, 100, TRUE);
            waitForPresentTime();

            std::vector<InkPoint> segment;
            D2D1_COLOR_F c;
            UINT w = 0, h = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (version == rendered && !clearPending) {
                    continue;
                }
                rendered = version;
                if (clearPending) {
                    clearPending = false;
                    active = false;
                    points.clear();
                    drawnByGtk = 0;
                } else if (!points.empty()) {
                    // From the last point GTK drew (so the line stays connected) to the newest
                    const size_t from = std::min(drawnByGtk > 0 ? drawnByGtk - 1 : 0, points.size() - 1);
                    segment.assign(points.begin() + static_cast<std::ptrdiff_t>(from), points.end());
                }
                c = color;
                w = wantWidth;
                h = wantHeight;
            }
            if (w && h && (w != width || h != height)) {
                resize(w, h);
            }
            if (segment.empty() && !shown) {
                continue;  // still transparent
            }
            render(segment, c);
            shown = !segment.empty();
            if (stats) {
                logStatistics(segment.empty() ? 0 : segment.back().received);
            }
        }
    }
};

WetInk::WetInk(std::unique_ptr<Impl> impl): impl(std::move(impl)) {}
WetInk::~WetInk() = default;

auto WetInk::forWindow(GtkWidget* toplevel) -> WetInk* {
    static std::unique_ptr<WetInk> instance;
    static bool tried = false;
    if (!tried && toplevel && gtk_widget_get_realized(toplevel) && !g_getenv("XOPP_NO_WET_INK")) {
        tried = true;
        auto impl = std::make_unique<Impl>();
        impl->hwnd = static_cast<HWND>(gdk_win32_window_get_handle(gtk_widget_get_window(toplevel)));
        if (impl->setUp()) {
            instance.reset(new WetInk(std::move(impl)));
        } else {
            g_message("The low-latency ink layer (Direct2D/DirectComposition) is not available");
        }
    }
    if (instance) {
        std::lock_guard<std::mutex> lock(instance->impl->mutex);
        if (instance->impl->failed) {
            return nullptr;
        }
    }
    return instance.get();
}

void WetInk::startStroke(float red, float green, float blue) {
    if (impl->clearTimer) {
        g_source_remove(impl->clearTimer);
        impl->clearTimer = 0;
    }
    const auto [w, h] = impl->clientSize();
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->points.clear();
        impl->drawnByGtk = 0;
        impl->color = {red, green, blue, 1.0f};
        impl->active = true;
        impl->ended = false;
        impl->clearPending = false;
        impl->wantWidth = w;
        impl->wantHeight = h;
        impl->version++;
    }
    impl->wake.notify_all();
}

void WetInk::addPoint(float x, float y, float radius) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->active) {
        impl->points.push_back({x, y, radius, qpcNow()});
        impl->version++;
    }
}

void WetInk::frameDrawn() {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->active && impl->drawnByGtk != impl->points.size()) {
            impl->drawnByGtk = impl->points.size();
            impl->version++;
        }
        if (impl->active && impl->ended) {
            impl->clearPending = true;  // GTK's frame has the finished stroke
            notify = true;
        }
    }
    if (notify) {
        impl->wake.notify_all();
    }
}

void WetInk::endStroke() {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->ended = true;
    }
    if (impl->clearTimer) {
        g_source_remove(impl->clearTimer);
    }
    impl->clearTimer = g_timeout_add(
            CLEAR_FALLBACK_MS,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                self->clearTimer = 0;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (self->active) {
                        self->clearPending = true;
                    }
                }
                self->wake.notify_all();
                return G_SOURCE_REMOVE;
            },
            impl.get());
}

#else

struct WetInk::Impl {};
WetInk::WetInk(std::unique_ptr<Impl> impl): impl(std::move(impl)) {}
WetInk::~WetInk() = default;
auto WetInk::forWindow(GtkWidget*) -> WetInk* { return nullptr; }
void WetInk::startStroke(float, float, float) {}
void WetInk::addPoint(float, float, float) {}
void WetInk::frameDrawn() {}
void WetInk::endStroke() {}

#endif
