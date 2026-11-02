#include "PowerEvents.h"

#include <map>      // for map
#include <utility>  // for move

namespace {
std::map<guint, PowerEvents::Listener> listeners;
guint nextId = 1;
}  // namespace

auto PowerEvents::addListener(Listener listener) -> guint {
    listeners.emplace(nextId, std::move(listener));
    return nextId++;
}

void PowerEvents::removeListener(guint id) { listeners.erase(id); }

#ifdef _WIN32

#include <gdk/gdkwin32.h>

// Windows headers last: their macros clash with names in other headers
#include <windows.h>

namespace {

HWND mainHwnd = nullptr;
HPOWERNOTIFY displayNotify = nullptr;
HPOWERNOTIFY lidNotify = nullptr;
/// Whether Standby was reported and Wake not yet
bool asleep = false;
/// Registering reports the current state once; only later notifications are changes
bool displayReported = false;
bool lidReported = false;

void dispatch(PowerEvents::Event event, const char* reason) {
    const bool standby = event == PowerEvents::Event::Standby;
    g_message("Power: %s%s", reason, standby == asleep ? " (no change)" : "");
    if (standby == asleep) {
        return;  // e.g. the lid closes, then the screen turns off, then the system suspends
    }
    asleep = standby;
    // Not from within the window procedure: listeners save documents and close menus
    g_idle_add_full(
            G_PRIORITY_HIGH,
            +[](gpointer data) -> gboolean {
                const auto event = static_cast<PowerEvents::Event>(GPOINTER_TO_INT(data));
                // Copied: a listener may remove itself
                const auto current = listeners;
                for (const auto& [id, listener]: current) {
                    listener(event);
                }
                return G_SOURCE_REMOVE;
            },
            GINT_TO_POINTER(static_cast<int>(event)), nullptr);
}

auto filter(GdkXEvent* xevent, GdkEvent*, gpointer) -> GdkFilterReturn {
    const MSG* msg = static_cast<MSG*>(xevent);
    if (msg->message != WM_POWERBROADCAST || !mainHwnd || msg->hwnd != mainHwnd) {
        return GDK_FILTER_CONTINUE;
    }
    switch (msg->wParam) {
        case PBT_APMSUSPEND:
            dispatch(PowerEvents::Event::Standby, "suspend");
            break;
        case PBT_APMRESUMEAUTOMATIC:
            dispatch(PowerEvents::Event::Wake, "resume");
            break;
        case PBT_POWERSETTINGCHANGE: {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(msg->lParam);
            if (!setting || setting->DataLength < sizeof(DWORD)) {
                break;
            }
            const DWORD value = *reinterpret_cast<const DWORD*>(setting->Data);
            if (IsEqualGUID(setting->PowerSetting, GUID_SESSION_DISPLAY_STATUS)) {
                if (!std::exchange(displayReported, true)) {
                    break;
                }
                // 0 off, 1 on, 2 dimmed (dimming only precedes turning off)
                if (value == 0) {
                    dispatch(PowerEvents::Event::Standby, "screen off");
                } else if (value == 1) {
                    dispatch(PowerEvents::Event::Wake, "screen on");
                }
            } else if (IsEqualGUID(setting->PowerSetting, GUID_LIDSWITCH_STATE_CHANGE)) {
                // Closed; opening it turns the screen on, which reports Wake
                if (std::exchange(lidReported, true) && value == 0) {
                    dispatch(PowerEvents::Event::Standby, "lid closed");
                }
            }
            break;
        }
        default:
            break;
    }
    return GDK_FILTER_CONTINUE;
}

void registerWindow(GtkWidget* window) {
    GdkWindow* gdkWindow = gtk_widget_get_window(window);
    if (!gdkWindow || mainHwnd) {
        return;
    }
    mainHwnd = static_cast<HWND>(gdk_win32_window_get_handle(gdkWindow));
    // Both report their current state right away: ignored
    displayReported = false;
    lidReported = false;
    displayNotify =
            RegisterPowerSettingNotification(mainHwnd, &GUID_SESSION_DISPLAY_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
    lidNotify = RegisterPowerSettingNotification(mainHwnd, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void unregisterWindow() {
    if (displayNotify) {
        UnregisterPowerSettingNotification(displayNotify);
        displayNotify = nullptr;
    }
    if (lidNotify) {
        UnregisterPowerSettingNotification(lidNotify);
        lidNotify = nullptr;
    }
    mainHwnd = nullptr;
}

}  // namespace

void PowerEvents::install(GtkWidget* mainWindow) {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    gdk_window_add_filter(nullptr, filter, nullptr);
    if (gtk_widget_get_realized(mainWindow)) {
        registerWindow(mainWindow);
    }
    g_signal_connect(mainWindow, "realize", G_CALLBACK(+[](GtkWidget* w, gpointer) { registerWindow(w); }), nullptr);
    g_signal_connect(mainWindow, "unrealize", G_CALLBACK(+[](GtkWidget*, gpointer) { unregisterWindow(); }), nullptr);
}

#else

void PowerEvents::install(GtkWidget*) {}

#endif
