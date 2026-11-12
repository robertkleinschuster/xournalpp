#include "DelegatedInk.h"

#include <utility>

#ifdef _WIN32

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi.h>
#include <gdk/gdkwin32.h>
#include <windows.h>

namespace {

// The delegated ink trail interfaces of the Windows SDK's dcomp.h (Windows 11), which MinGW's dcomp.h lacks
struct InkTrailPoint {
    float x;
    float y;
    float radius;
};
struct InkColor {  // D2D1_COLOR_F
    float r, g, b, a;
};
struct IDCompositionDelegatedInkTrail: public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AddTrailPoints(const InkTrailPoint* inkPoints, UINT inkPointsCount,
                                                     UINT* generationId) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddTrailPointsWithPrediction(const InkTrailPoint* inkPoints, UINT inkPointsCount,
                                                                   const InkTrailPoint* predictedInkPoints,
                                                                   UINT predictedInkPointsCount,
                                                                   UINT* generationId) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveTrailPoints(UINT generationId) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartNewTrail(const InkColor& color) = 0;
};
struct IDCompositionInkTrailDevice: public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDelegatedInkTrail(IDCompositionDelegatedInkTrail** inkTrail) = 0;
    virtual HRESULT STDMETHODCALLTYPE
            CreateDelegatedInkTrailForSwapChain(IUnknown* swapChain, IDCompositionDelegatedInkTrail** inkTrail) = 0;
};
// {DF0C7CEC-CDEB-4D4A-B91C-721BF22F4E6C}
const GUID IID_InkTrailDevice = {0xdf0c7cec, 0xcdeb, 0x4d4a, {0xb9, 0x1c, 0x72, 0x1b, 0xf2, 0x2f, 0x4e, 0x6c}};

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/// After the stroke ended, its trail goes once the application has surely drawn the stroke
constexpr guint END_DELAY_MS = 50;

}  // namespace

struct DelegatedInk::Impl {
    ID3D11Device* d3d = nullptr;
    IDCompositionDesktopDevice* device = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual2* visual = nullptr;
    IDCompositionDelegatedInkTrail* trail = nullptr;
    UINT lastGeneration = 0;
    UINT drawnGeneration = 0;  ///< added before the previous application frame
    bool hasPoints = false;
    guint endTimer = 0;

    ~Impl() {
        if (endTimer) {
            g_source_remove(endTimer);
        }
        release(trail);
        release(visual);
        release(target);
        release(device);
        release(d3d);
    }

    auto setUp(HWND hwnd) -> bool {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr))) {
            return false;
        }
        IDXGIDevice* dxgi = nullptr;
        if (FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            return false;
        }
        const HRESULT created = DCompositionCreateDevice2(dxgi, __uuidof(IDCompositionDesktopDevice),
                                                          reinterpret_cast<void**>(&device));
        release(dxgi);
        if (FAILED(created)) {
            return false;
        }
        IDCompositionInkTrailDevice* inkDevice = nullptr;
        if (FAILED(device->QueryInterface(IID_InkTrailDevice, reinterpret_cast<void**>(&inkDevice)))) {
            return false;  // before Windows 11
        }
        const HRESULT trailCreated = inkDevice->CreateDelegatedInkTrail(&trail);
        release(inkDevice);
        if (FAILED(trailCreated) || FAILED(device->CreateTargetForHwnd(hwnd, TRUE, &target)) ||
            FAILED(device->CreateVisual(&visual)) || FAILED(visual->SetContent(trail)) ||
            FAILED(target->SetRoot(visual)) || FAILED(device->Commit())) {
            return false;
        }
        return true;
    }

    void removeAll() {
        if (hasPoints) {
            trail->RemoveTrailPoints(lastGeneration);
            hasPoints = false;
        }
    }
};

DelegatedInk::DelegatedInk(std::unique_ptr<Impl> impl): impl(std::move(impl)) {}
DelegatedInk::~DelegatedInk() = default;

auto DelegatedInk::forWindow(GtkWidget* toplevel) -> DelegatedInk* {
    static std::unique_ptr<DelegatedInk> instance;
    static bool tried = false;
    if (!tried && toplevel && gtk_widget_get_realized(toplevel) && !g_getenv("XOPP_NO_DELEGATED_INK")) {
        tried = true;
        auto impl = std::make_unique<Impl>();
        if (impl->setUp(static_cast<HWND>(gdk_win32_window_get_handle(gtk_widget_get_window(toplevel))))) {
            instance.reset(new DelegatedInk(std::move(impl)));
        } else {
            g_message("Delegated ink (DirectComposition) is not available; ink is drawn by the application only");
        }
    }
    return instance.get();
}

void DelegatedInk::startTrail(float red, float green, float blue, float alpha) {
    if (impl->endTimer) {
        g_source_remove(impl->endTimer);
        impl->endTimer = 0;
    }
    impl->trail->StartNewTrail(InkColor{red, green, blue, alpha});
    impl->hasPoints = false;
    impl->drawnGeneration = 0;
    impl->device->Commit();
}

void DelegatedInk::addPoint(float x, float y, float radius) {
    const InkTrailPoint point{x, y, radius};
    if (SUCCEEDED(impl->trail->AddTrailPoints(&point, 1, &impl->lastGeneration))) {
        impl->hasPoints = true;
    }
}

void DelegatedInk::frameDrawn() {
    // One frame later: the application's frame reaches the screen with the compositor's next frame, so the trail
    // stays until then (removing it with the drawing would leave a gap for a frame)
    if (impl->hasPoints && impl->drawnGeneration != 0) {
        impl->trail->RemoveTrailPoints(impl->drawnGeneration);
    }
    impl->drawnGeneration = impl->hasPoints ? impl->lastGeneration : 0;
}

void DelegatedInk::endTrail() {
    if (impl->endTimer) {
        g_source_remove(impl->endTimer);
    }
    impl->endTimer = g_timeout_add(
            END_DELAY_MS,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                self->endTimer = 0;
                self->removeAll();
                self->device->Commit();
                return G_SOURCE_REMOVE;
            },
            impl.get());
}

#else

struct DelegatedInk::Impl {};
DelegatedInk::DelegatedInk(std::unique_ptr<Impl> impl): impl(std::move(impl)) {}
DelegatedInk::~DelegatedInk() = default;
auto DelegatedInk::forWindow(GtkWidget*) -> DelegatedInk* { return nullptr; }
void DelegatedInk::startTrail(float, float, float, float) {}
void DelegatedInk::addPoint(float, float, float) {}
void DelegatedInk::frameDrawn() {}
void DelegatedInk::endTrail() {}

#endif
