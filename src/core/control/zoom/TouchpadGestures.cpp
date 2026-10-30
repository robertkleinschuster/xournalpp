#include "TouchpadGestures.h"

#ifdef _WIN32
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "control/zoom/ZoomControl.h"
#include "gui/Layout.h"
#include "gui/XournalView.h"
#include "util/Point.h"

// Windows headers last: their macros clash with names in Xournal++ headers
// clang-format off
#include <windows.h>
#include <hidusage.h>
#include <hidpi.h>
// clang-format on

#include <gdk/gdkwin32.h>

using xoj::util::Point;

namespace {
// HID usages of precision touchpads
constexpr USAGE PAGE_GENERIC = 0x01;
constexpr USAGE PAGE_DIGITIZER = 0x0D;
constexpr USAGE USAGE_TOUCHPAD = 0x05;
constexpr USAGE USAGE_X = 0x30;
constexpr USAGE USAGE_Y = 0x31;
constexpr USAGE USAGE_TIP_SWITCH = 0x42;
constexpr USAGE USAGE_CONTACT_ID = 0x51;
constexpr USAGE USAGE_CONTACT_COUNT = 0x54;

/// The document moves this many times the finger distance, as the touchpad is much smaller than the screen
constexpr double PAN_GAIN = 1.7;
/// Change of the finger distance (mm) before zooming starts. The distance varies by a few millimetres during a fast
/// two-finger swipe (and when the fingers are lifted), a pinch changes it much more right away.
constexpr double ZOOM_THRESHOLD_MM = 6.0;
/// A gesture that zoomed by less than this in the end still counts as a swipe, which may glide on
constexpr double NEGLIGIBLE_ZOOM = 0.05;
/// Momentum after a swipe: speed measurement, deceleration and limits
/// (the speed is measured over the last VELOCITY_WINDOW_US of movement, so a short stop when lifting does not count)
constexpr gint64 VELOCITY_WINDOW_US = 100'000;
constexpr double MOMENTUM_TIME_CONSTANT_S = 0.325;
constexpr double MOMENTUM_MIN_SPEED = 150.0;  ///< pixels per second
constexpr double MOMENTUM_STOP_SPEED = 20.0;
constexpr gint64 MOMENTUM_MAX_REST_US = 120'000;
/// After the fingers are lifted, Windows keeps sending synthesized scroll events (inertia) with short gaps
constexpr gint64 INERTIA_GAP_US = 200'000;
constexpr gint64 INERTIA_MAX_US = 2'500'000;

using Contacts = std::map<ULONG, Point<double>>;  ///< finger id -> position in millimetres

/// One finger slot of the touchpad's reports
struct ContactSlot {
    USHORT link = 0;
    bool hasX = false;
    bool hasY = false;
    bool hasId = false;
    double xMm = 0.0;  ///< millimetres per logical unit
    double yMm = 0.0;
};

struct Device {
    std::vector<char> preparsed;
    std::vector<ContactSlot> slots;
    bool ok = false;
    // Hybrid reporting: a frame may be split over several reports, only the first one carries the contact count
    ULONG frameRemaining = 0;
    Contacts frame;
};

/// Millimetres per logical unit of a coordinate, from the HID physical range and unit
auto millimetresPerUnit(const HIDP_VALUE_CAPS& caps) -> double {
    const double logical = static_cast<double>(caps.LogicalMax) - static_cast<double>(caps.LogicalMin);
    if (logical <= 0) {
        return 0.0;
    }
    double physical = static_cast<double>(caps.PhysicalMax) - static_cast<double>(caps.PhysicalMin);
    const int exponent = (caps.UnitsExp & 0x8) ? static_cast<int>(caps.UnitsExp & 0xF) - 16 :
                                                  static_cast<int>(caps.UnitsExp & 0xF);
    double unitMm = 0.0;
    if ((caps.Units & 0xFF) == 0x11) {
        unitMm = 10.0;  // centimetre
    } else if ((caps.Units & 0xFF) == 0x13) {
        unitMm = 25.4;  // inch
    }
    if (physical <= 0 || unitMm == 0.0) {
        // Unknown size: assume a typical touchpad width of 100 mm for the logical range
        return 100.0 / logical;
    }
    physical *= std::pow(10.0, exponent) * unitMm;
    return physical / logical;
}

/**
 * Receives the raw touchpad input on its own thread with a message-only window. Windows' input processing waits for
 * the receiving thread to fetch raw input; GTK's main loop does not do that promptly enough, which made the whole
 * system's mouse pointer lag. This thread fetches every message immediately and only hands the finger positions over.
 */
class RawTouchpadReader {
public:
    /// onContacts is called on the reader thread whenever the set of fingers or their positions changed
    explicit RawTouchpadReader(std::function<void(const Contacts&)> onContacts): onContacts(std::move(onContacts)) {
        thread = std::thread([this] { run(); });
    }

    ~RawTouchpadReader() {
        // Wait until the window exists (normally at once), then end its message loop
        for (int i = 0; i < 1000 && !window.load() && !failed.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (HWND hwnd = window.load()) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        thread.join();
    }

private:
    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &RawTouchpadReader::windowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"XournalppForkRawTouchpad";
        RegisterClassExW(&windowClass);  // fails harmlessly if already registered
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                                    nullptr);
        if (!hwnd) {
            failed = true;
            return;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        // INPUTSINK: also delivered while another window has the focus; the main thread decides what to use
        RAWINPUTDEVICE device{PAGE_DIGITIZER, USAGE_TOUCHPAD, RIDEV_INPUTSINK, hwnd};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            g_message("Touchpad gestures: raw touchpad input is not available");
        }
        window = hwnd;
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            DispatchMessageW(&message);
        }
        RAWINPUTDEVICE remove{PAGE_DIGITIZER, USAGE_TOUCHPAD, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&remove, 1, sizeof(remove));
    }

    static auto CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto* self = reinterpret_cast<RawTouchpadReader*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_INPUT && self) {
            self->onInput(reinterpret_cast<HRAWINPUT>(lParam));
        } else if (message == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        } else if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static auto preparsed(Device& device) -> PHIDP_PREPARSED_DATA {
        return reinterpret_cast<PHIDP_PREPARSED_DATA>(device.preparsed.data());
    }

    auto deviceFor(HANDLE handle) -> Device& {
        if (auto found = devices.find(handle); found != devices.end()) {
            return found->second;
        }
        Device device;
        UINT size = 0;
        GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr, &size);
        device.preparsed.resize(size);
        HIDP_CAPS caps{};
        if (size > 0 &&
            GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, device.preparsed.data(), &size) !=
                    static_cast<UINT>(-1) &&
            HidP_GetCaps(preparsed(device), &caps) == HIDP_STATUS_SUCCESS) {
            USHORT count = caps.NumberInputValueCaps;
            std::vector<HIDP_VALUE_CAPS> valueCaps(count);
            if (HidP_GetValueCaps(HidP_Input, valueCaps.data(), &count, preparsed(device)) == HIDP_STATUS_SUCCESS) {
                std::map<USHORT, ContactSlot> slots;
                for (USHORT i = 0; i < count; i++) {
                    const auto& value = valueCaps[i];
                    const USAGE usage = value.IsRange ? value.Range.UsageMin : value.NotRange.Usage;
                    auto& slot = slots[value.LinkCollection];
                    slot.link = value.LinkCollection;
                    if (value.UsagePage == PAGE_GENERIC && usage == USAGE_X) {
                        slot.hasX = true;
                        slot.xMm = millimetresPerUnit(value);
                    } else if (value.UsagePage == PAGE_GENERIC && usage == USAGE_Y) {
                        slot.hasY = true;
                        slot.yMm = millimetresPerUnit(value);
                    } else if (value.UsagePage == PAGE_DIGITIZER && usage == USAGE_CONTACT_ID) {
                        slot.hasId = true;
                    }
                }
                for (const auto& [link, slot]: slots) {
                    if (slot.hasX && slot.hasY) {
                        device.slots.push_back(slot);
                    }
                }
                device.ok = !device.slots.empty();
            }
        }
        return devices.emplace(handle, std::move(device)).first->second;
    }

    void onInput(HRAWINPUT input) {
        UINT size = 0;
        GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        buffer.resize(size);
        if (size == 0 || GetRawInputData(input, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
            return;
        }
        const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (raw->header.dwType != RIM_TYPEHID) {
            return;
        }
        Device& device = deviceFor(raw->header.hDevice);
        if (!device.ok) {
            return;
        }
        const auto reportSize = raw->data.hid.dwSizeHid;
        bool changed = false;
        for (DWORD r = 0; r < raw->data.hid.dwCount; r++) {
            auto* report = const_cast<PCHAR>(reinterpret_cast<const char*>(raw->data.hid.bRawData + r * reportSize));
            changed = parseReport(device, report, reportSize) || changed;
        }
        if (changed) {
            onContacts(contacts);
        }
    }

    /// Returns true when a complete frame was read
    auto parseReport(Device& device, PCHAR report, ULONG length) -> bool {
        auto* data = preparsed(device);
        ULONG contactCount = 0;
        HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, 0, USAGE_CONTACT_COUNT, &contactCount, data, report, length);
        if (contactCount > 0) {
            device.frameRemaining = contactCount;  // first report of a frame
            device.frame.clear();
        } else if (device.frameRemaining == 0) {
            contacts.clear();  // no finger on the touchpad any more
            return true;
        }
        const size_t slotsInReport = std::min<size_t>(device.slots.size(), device.frameRemaining);
        for (size_t i = 0; i < slotsInReport; i++) {
            const auto& slot = device.slots[i];
            USAGE usages[16];
            ULONG usageCount = 16;
            bool tip = false;
            if (HidP_GetUsages(HidP_Input, PAGE_DIGITIZER, slot.link, usages, &usageCount, data, report, length) ==
                HIDP_STATUS_SUCCESS) {
                for (ULONG u = 0; u < usageCount; u++) {
                    tip = tip || usages[u] == USAGE_TIP_SWITCH;
                }
            }
            ULONG id = static_cast<ULONG>(i);
            if (slot.hasId) {
                HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, slot.link, USAGE_CONTACT_ID, &id, data, report, length);
            }
            ULONG x = 0;
            ULONG y = 0;
            HidP_GetUsageValue(HidP_Input, PAGE_GENERIC, slot.link, USAGE_X, &x, data, report, length);
            HidP_GetUsageValue(HidP_Input, PAGE_GENERIC, slot.link, USAGE_Y, &y, data, report, length);
            if (tip) {
                device.frame[id] = {x * slot.xMm, y * slot.yMm};
            }
        }
        device.frameRemaining -= static_cast<ULONG>(slotsInReport);
        if (device.frameRemaining == 0) {
            contacts = device.frame;
            return true;
        }
        return false;
    }

    std::function<void(const Contacts&)> onContacts;
    std::thread thread;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> failed{false};
    // Only used on the reader thread
    std::map<HANDLE, Device> devices;
    Contacts contacts;
    std::vector<BYTE> buffer;
};
}  // namespace

struct TouchpadGestures::Impl {
    ZoomControl* zoom;
    XournalView* view;
    GtkWidget* toplevel;  ///< weak pointer, reset when destroyed
    guint tickId = 0;
    gulong activeHandler = 0;

    // Finger positions handed over from the reader thread
    std::mutex mutex;
    Contacts latest;
    guint pendingIdle = 0;  ///< guarded by mutex
    std::unique_ptr<RawTouchpadReader> reader;

    // The current two-finger gesture
    bool tracking = false;
    bool zooming = false;
    double startDistance = 0;
    double distance = 0;
    Point<double> centroid;
    Point<double> appliedCentroid;  ///< centroid of the last applied frame
    Point<double> pointer;          ///< mouse pointer at the start, in document widget coordinates

    // Synthesized wheel events to ignore after the gesture
    gint64 releasedAt = 0;
    gint64 lastIgnoredAt = 0;

    // The system scroll direction: from the touchpad setting, confirmed by comparing finger and synthesized wheel
    // movement during a gesture
    double fingerTravelY = 0;
    double wheelTravelY = 0;
    int contentDirection = systemContentDirection();  ///< +1: content follows the fingers, -1: opposite
    bool directionKnown = false;

    // Momentum after a two-finger swipe, in pixels per second of content movement
    Point<double> velocity;
    gint64 lastMoveTime = 0;
    Point<double> travelled;                               ///< content movement in this gesture
    std::deque<std::pair<gint64, Point<double>>> samples;  ///< recent (time, travelled)
    guint inertiaTickId = 0;
    gint64 inertiaLastFrame = 0;

    /// "Scrolling direction" of the touchpad settings; 0 (the default) moves the content with the fingers
    static auto systemContentDirection() -> int {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
                         L"ScrollDirection", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
            value != 0) {
            return -1;
        }
        return 1;
    }

    Impl(ZoomControl* zoom, XournalView* view, GtkWidget* toplevel): zoom(zoom), view(view), toplevel(toplevel) {
        g_object_add_weak_pointer(G_OBJECT(toplevel), reinterpret_cast<gpointer*>(&this->toplevel));
        activeHandler = g_signal_connect(toplevel, "notify::is-active",
                                         G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                                             auto* self = static_cast<Impl*>(data);
                                             if (!self->isActive()) {
                                                 self->finishGesture();
                                                 self->stopInertia();
                                             }
                                         }),
                                         this);
        reader = std::make_unique<RawTouchpadReader>([this](const Contacts& contacts) { handOver(contacts); });
    }

    ~Impl() {
        reader.reset();  // joins the reader thread, no more hand-overs after this
        {
            std::lock_guard lock(mutex);
            if (pendingIdle) {
                g_source_remove(pendingIdle);
                pendingIdle = 0;
            }
        }
        finishGesture();
        stopInertia();
        if (toplevel) {
            g_signal_handler_disconnect(toplevel, activeHandler);
            g_object_remove_weak_pointer(G_OBJECT(toplevel), reinterpret_cast<gpointer*>(&toplevel));
        }
    }

    /// Called on the reader thread: keep the newest positions and process them once on the main thread
    void handOver(const Contacts& contacts) {
        std::lock_guard lock(mutex);
        latest = contacts;
        if (!pendingIdle) {
            pendingIdle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, +[](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                Contacts contacts;
                {
                    std::lock_guard lock(self->mutex);
                    contacts = self->latest;
                    self->pendingIdle = 0;
                }
                self->updateGesture(contacts);
                return G_SOURCE_REMOVE;
            }, this, nullptr);
        }
    }

    auto isActive() const -> bool { return toplevel && gtk_window_is_active(GTK_WINDOW(toplevel)); }

    /// The mouse pointer in the document widget's coordinates, if it is over the document
    auto pointerOverDocument(Point<double>& position) const -> bool {
        GdkWindow* window = toplevel ? gtk_widget_get_window(toplevel) : nullptr;
        if (!window || !GDK_IS_WIN32_WINDOW(window)) {
            return false;
        }
        auto* hwnd = reinterpret_cast<HWND>(gdk_win32_window_get_handle(window));
        POINT point;
        if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point)) {
            return false;
        }
        const int scale = gdk_window_get_scale_factor(window);
        int x = 0;
        int y = 0;
        GtkWidget* widget = view->getWidget();
        if (!gtk_widget_translate_coordinates(toplevel, widget, point.x / scale, point.y / scale, &x, &y)) {
            return false;
        }
        position = {static_cast<double>(x), static_cast<double>(y)};
        return x >= 0 && y >= 0 && x < gtk_widget_get_allocated_width(widget) &&
               y < gtk_widget_get_allocated_height(widget);
    }

    void updateGesture(const Contacts& contacts) {
        if (contacts.size() != 2 || !isActive()) {
            finishGesture();
            return;
        }
        const auto& a = contacts.begin()->second;
        const auto& b = std::next(contacts.begin())->second;
        distance = std::hypot(a.x - b.x, a.y - b.y);
        const Point<double> current = (a + b) / 2.0;
        if (!tracking) {
            if (distance <= 0 || !pointerOverDocument(pointer)) {
                return;
            }
            stopInertia();  // touching the touchpad again catches the gliding document
            tracking = true;
            zooming = false;
            startDistance = distance;
            centroid = current;
            appliedCentroid = current;
            velocity = {0, 0};
            lastMoveTime = 0;
            travelled = {0, 0};
            samples.clear();
            return;
        }
        fingerTravelY += current.y - centroid.y;
        centroid = current;
        requestFrame();
    }

    void requestFrame() {
        if (tickId || !toplevel) {
            return;
        }
        // Like touchscreen gestures, apply the movement once per frame
        tickId = gtk_widget_add_tick_callback(
                toplevel,
                +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
                    auto* self = static_cast<Impl*>(data);
                    self->tickId = 0;
                    self->applyMovement();
                    return G_SOURCE_REMOVE;
                },
                this, nullptr);
    }

    auto pixelsPerMillimetre() const -> double {
        double resolution = gdk_screen_get_resolution(gtk_widget_get_screen(view->getWidget()));
        if (resolution <= 0) {
            resolution = 96.0;
        }
        return resolution / 25.4;
    }

    void applyMovement() {
        if (!tracking) {
            return;
        }
        // Finger movement since the last frame, in screen pixels, in the direction the content moves
        const double factor = PAN_GAIN * pixelsPerMillimetre() * contentDirection;
        const Point<double> move = (centroid - appliedCentroid) * factor;
        appliedCentroid = centroid;

        if (!zooming && std::abs(distance - startDistance) >= ZOOM_THRESHOLD_MM) {
            // Zoom relative to the distance from here on, so the zoom does not jump by the threshold
            zooming = true;
            startDistance = distance;
            if (zoom->isZoomFitMode()) {
                zoom->setZoomFitMode(false);
            }
            zoom->startZoomSequence(pointer);
        }
        if (zooming) {
            // Zooms around the pointer, which moves with the fingers
            zoom->zoomSequenceChange(distance / startDistance, true, move);
        } else if (move.x != 0.0 || move.y != 0.0) {
            view->getLayout()->scrollRelative(-move.x, -move.y);
        }

        // Remember where the content was recently, for the swipe speed when the fingers are lifted
        if (move.x != 0.0 || move.y != 0.0) {
            const gint64 now = g_get_monotonic_time();
            travelled = travelled + move;
            samples.emplace_back(now, travelled);
            while (!samples.empty() && now - samples.front().first > VELOCITY_WINDOW_US) {
                samples.pop_front();
            }
            lastMoveTime = now;
        }
    }

    /// Speed of the content over the last moments of the swipe, in pixels per second
    auto swipeVelocity() const -> Point<double> {
        if (samples.size() < 2) {
            return {0, 0};
        }
        const auto& [firstTime, firstPosition] = samples.front();
        const auto& [lastTime, lastPosition] = samples.back();
        const double seconds = static_cast<double>(lastTime - firstTime) / 1e6;
        return seconds > 0 ? (lastPosition - firstPosition) / seconds : Point<double>{0, 0};
    }

    void finishGesture() {
        if (tickId && toplevel) {
            gtk_widget_remove_tick_callback(toplevel, tickId);
        }
        tickId = 0;
        if (tracking) {
            applyMovement();
            releasedAt = g_get_monotonic_time();
            lastIgnoredAt = releasedAt;
            // A swipe glides on, unless it was a pinch or the fingers rested before lifting
            const bool recentlyMoved = lastMoveTime && releasedAt - lastMoveTime < MOMENTUM_MAX_REST_US;
            velocity = swipeVelocity();
            const bool pinched = zooming && std::abs(distance / startDistance - 1.0) >= NEGLIGIBLE_ZOOM;
            if (!pinched && recentlyMoved && std::hypot(velocity.x, velocity.y) > MOMENTUM_MIN_SPEED) {
                startInertia();
            }
        }
        if (zooming) {
            zoom->endZoomSequence();
        }
        tracking = false;
        zooming = false;
    }

    void startInertia() {
        if (inertiaTickId || !toplevel) {
            return;
        }
        inertiaLastFrame = 0;
        inertiaTickId = gtk_widget_add_tick_callback(
                toplevel,
                +[](GtkWidget*, GdkFrameClock* clock, gpointer data) -> gboolean {
                    return static_cast<Impl*>(data)->inertiaStep(gdk_frame_clock_get_frame_time(clock));
                },
                this, nullptr);
    }

    auto inertiaStep(gint64 frameTime) -> gboolean {
        const double seconds =
                inertiaLastFrame ? static_cast<double>(frameTime - inertiaLastFrame) / 1e6 : 1.0 / 60.0;
        inertiaLastFrame = frameTime;
        const Point<double> move = velocity * seconds;
        view->getLayout()->scrollRelative(-move.x, -move.y);
        // Exponential deceleration, like scrolling on phones and in browsers
        velocity = velocity * std::exp(-seconds / MOMENTUM_TIME_CONSTANT_S);
        if (std::hypot(velocity.x, velocity.y) < MOMENTUM_STOP_SPEED) {
            inertiaTickId = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    void stopInertia() {
        if (inertiaTickId && toplevel) {
            gtk_widget_remove_tick_callback(toplevel, inertiaTickId);
        }
        inertiaTickId = 0;
    }

    auto consumeSynthesizedScroll(double dy) -> bool {
        const gint64 now = g_get_monotonic_time();
        if (tracking) {
            // Learn the scroll direction: Windows scrolls down (positive) when the fingers move down unless the
            // touchpad is set to move the content with the fingers
            wheelTravelY += dy;
            if (!directionKnown && std::abs(fingerTravelY) > 5.0 && std::abs(wheelTravelY) > 1.0) {
                contentDirection = (fingerTravelY > 0) == (wheelTravelY > 0) ? -1 : 1;
                directionKnown = true;
            }
            lastIgnoredAt = now;
            return true;
        }
        // The inertia Windows adds after the fingers are lifted
        if (releasedAt && now - releasedAt < INERTIA_MAX_US && now - lastIgnoredAt < INERTIA_GAP_US) {
            lastIgnoredAt = now;
            return true;
        }
        // A real mouse wheel takes over from a gliding swipe
        stopInertia();
        releasedAt = 0;
        fingerTravelY = 0;
        wheelTravelY = 0;
        return false;
    }
};

TouchpadGestures::TouchpadGestures(ZoomControl* zoom, XournalView* view, GtkWidget* toplevel):
        impl(std::make_unique<Impl>(zoom, view, toplevel)) {}

auto TouchpadGestures::consumeSynthesizedScroll(double, double dy) -> bool {
    return impl->consumeSynthesizedScroll(dy);
}

#else

struct TouchpadGestures::Impl {};

TouchpadGestures::TouchpadGestures(ZoomControl*, XournalView*, GtkWidget*) {}

auto TouchpadGestures::consumeSynthesizedScroll(double, double) -> bool { return false; }

#endif

TouchpadGestures::~TouchpadGestures() = default;
