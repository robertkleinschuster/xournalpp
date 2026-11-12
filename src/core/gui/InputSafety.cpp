#include "InputSafety.h"

#ifdef _WIN32

#include <cstdint>
#include <cstdio>
#include <string>

#include <windows.h>  // for timeBeginPeriod

#include <timeapi.h>  // after windows.h

#include "gui/InputModality.h"  // for modalityOf
#include "util/PathUtil.h"

#include "filesystem.h"

namespace {

// --- Diagnostic log ---

auto openLog() -> FILE* {
    static FILE* log = []() -> FILE* {
        std::error_code error;
        if (!fs::exists(Util::getConfigFile("input-debug"), error)) {
            return nullptr;
        }
        // Start over once it is large, so the log cannot grow without limit while diagnostics stay switched on
        constexpr std::uintmax_t MAX_LOG_SIZE = 5 * 1024 * 1024;
        const auto path = Util::getConfigFile("input-debug.log");
        const auto size = fs::file_size(path, error);
        return _wfopen(path.c_str(), !error && size > MAX_LOG_SIZE ? L"w" : L"a");
    }();
    return log;
}

auto sourceName(GdkEvent* event) -> const char* {
    GdkDevice* device = gdk_event_get_source_device(event);
    if (!device) {
        return "-";
    }
    switch (gdk_device_get_source(device)) {
        case GDK_SOURCE_MOUSE:
            return "mouse";
        case GDK_SOURCE_PEN:
            return "pen";
        case GDK_SOURCE_ERASER:
            return "eraser";
        case GDK_SOURCE_TOUCHSCREEN:
            return "touch";
        case GDK_SOURCE_TOUCHPAD:
            return "touchpad";
        default:
            return "other";
    }
}

auto typeName(GtkWidget* widget) -> const char* { return widget ? G_OBJECT_TYPE_NAME(widget) : "-"; }

/// Visible tooltip windows
auto visibleTooltips() -> int {
    int count = 0;
    GList* windows = gtk_window_list_toplevels();
    for (GList* item = windows; item; item = item->next) {
        auto* window = GTK_WINDOW(item->data);
        if (gtk_widget_get_visible(GTK_WIDGET(window)) &&
            gtk_window_get_type_hint(window) == GDK_WINDOW_TYPE_HINT_TOOLTIP) {
            count++;
        }
    }
    g_list_free(windows);
    return count;
}

void logEvent(GdkEvent* event) {
    FILE* log = openLog();
    if (!log) {
        return;
    }
    const char* name = nullptr;
    switch (event->type) {
        case GDK_BUTTON_PRESS:
            name = "press";
            break;
        case GDK_BUTTON_RELEASE:
            name = "release";
            break;
        case GDK_TOUCH_BEGIN:
            name = "touch-begin";
            break;
        case GDK_TOUCH_END:
            name = "touch-end";
            break;
        case GDK_TOUCH_CANCEL:
            name = "touch-cancel";
            break;
        case GDK_PROXIMITY_IN:
            name = "proximity-in";
            break;
        case GDK_PROXIMITY_OUT:
            name = "proximity-out";
            break;
        case GDK_ENTER_NOTIFY:
            name = "enter";
            break;
        case GDK_LEAVE_NOTIFY:
            name = "leave";
            break;
        case GDK_GRAB_BROKEN:
            name = "grab-broken";
            break;
        case GDK_FOCUS_CHANGE:
            name = "focus";
            break;
        case GDK_MAP:
            name = "map";
            break;
        case GDK_UNMAP:
            name = "unmap";
            break;
        default:
            return;  // motion and the rest would flood the log
    }
    double x = 0, y = 0;
    gdk_event_get_root_coords(event, &x, &y);
    guint button = 0;
    gdk_event_get_button(event, &button);
    GtkWidget* grab = gtk_grab_get_current();
    // The touch sequence, to match the touch handler's notes (TouchInputHandler)
    char sequence[32] = "";
    if (GdkEventSequence* s = gdk_event_get_event_sequence(event)) {
        std::snprintf(sequence, sizeof(sequence), " seq=%p", static_cast<void*>(s));
    }
    std::fprintf(log, "%10.3f %-13s src=%-8s btn=%u at=%7.1f,%7.1f widget=%s grab=%s(%s) tooltips=%d%s\n",
                 static_cast<double>(g_get_monotonic_time()) / 1e6, name, sourceName(event), button, x, y,
                 typeName(gtk_get_event_widget(event)), typeName(grab),
                 grab ? (gtk_widget_get_mapped(grab) ? "mapped" : "unmapped") : "", visibleTooltips(), sequence);
    std::fflush(log);
}

// --- Tooltips ---

void hideTooltips() {
    GList* windows = gtk_window_list_toplevels();
    for (GList* item = windows; item; item = item->next) {
        auto* window = GTK_WINDOW(item->data);
        if (gtk_widget_get_visible(GTK_WIDGET(window)) &&
            gtk_window_get_type_hint(window) == GDK_WINDOW_TYPE_HINT_TOOLTIP) {
            gtk_widget_hide(GTK_WIDGET(window));
        }
    }
    g_list_free(windows);
}

auto isPenOrTouch(GdkEvent* event) -> bool { return xoj::input::modalityOf(event) != xoj::input::Modality::Mouse; }

/// The device of the last pointer input, and whether the pen hovers over the screen
xoj::input::Modality lastModality = xoj::input::Modality::Mouse;
bool penInRange = false;
gint64 lastPenTime = 0;  ///< monotonic time of the last pen event

/**
 * Windows wakes timers in steps of 15.6 ms by default, which delays GTK's frames (and so the ink) by up to a frame.
 * While the pen is near the screen the process asks for 1 ms steps; that costs a little power, so only then.
 */
void setFineTimer(bool fine) {
    static bool active = false;
    if (fine != active) {
        active = fine;
        if (fine) {
            timeBeginPeriod(1);
        } else {
            timeEndPeriod(1);
        }
    }
}

void trackModality(GdkEvent* event) {
    switch (event->type) {
        case GDK_MOTION_NOTIFY:
        case GDK_BUTTON_PRESS:
        case GDK_TOUCH_BEGIN:
        case GDK_TOUCH_UPDATE:
            lastModality = xoj::input::modalityOf(event);
            if (lastModality == xoj::input::Modality::Pen) {
                setFineTimer(true);  // also if the proximity event was missed
                lastPenTime = g_get_monotonic_time();
            }
            break;
        case GDK_PROXIMITY_IN:
            penInRange = true;
            lastModality = xoj::input::Modality::Pen;
            setFineTimer(true);
            break;
        case GDK_PROXIMITY_OUT:
            penInRange = false;
            setFineTimer(false);
            break;
        case GDK_FOCUS_CHANGE:
            if (!event->focus_change.in) {
                setFineTimer(false);  // writing in another window
            }
            break;
        default:
            break;
    }
}

/**
 * GTK shows a tooltip half a second after the pen hovered over a button, also when the pen has left the screen in
 * between, and after touches (whose emulated pointer "hovers" where the finger was). Only the pen hovering, or the
 * mouse, ask for one.
 */
auto isUnwantedTooltip(GdkEvent* event) -> bool {
    if (event->type != GDK_MAP || lastModality == xoj::input::Modality::Mouse ||
        (lastModality == xoj::input::Modality::Pen && penInRange)) {
        return false;
    }
    gpointer widget = nullptr;
    gdk_window_get_user_data(event->any.window, &widget);
    return GTK_IS_WINDOW(widget) && gtk_window_get_type_hint(GTK_WINDOW(widget)) == GDK_WINDOW_TYPE_HINT_TOOLTIP;
}

// --- Menu grabs ---

/// A menu that holds the grab although it is not shown any more blocks all input to the window: close it
void releaseStaleMenuGrab() {
    GtkWidget* grab = gtk_grab_get_current();
    if (!grab || !GTK_IS_MENU_SHELL(grab) || gtk_widget_get_mapped(grab)) {
        return;
    }
    // Close the whole menu hierarchy, from the menu bar it belongs to
    GtkWidget* shell = grab;
    while (GTK_IS_MENU(shell)) {
        GtkWidget* item = gtk_menu_get_attach_widget(GTK_MENU(shell));
        GtkWidget* parent = item ? gtk_widget_get_parent(item) : nullptr;
        if (!parent || !GTK_IS_MENU_SHELL(parent)) {
            break;
        }
        shell = parent;
    }
    gtk_menu_shell_deactivate(GTK_MENU_SHELL(shell));
    if (gtk_grab_get_current() == grab) {
        gtk_grab_remove(grab);
    }
}

// --- Stale pointer grabs ---

/**
 * A touch or pen press that GDK delivers to a window it does not lie in: GDK still holds the implicit grab of an
 * earlier press whose release never came. Seen after a tap at the very left screen edge, where Windows starts its edge
 * gesture: the tap arrived as a press on the toolbar without a release, and every tap afterwards went to the toolbar,
 * so the window ignored touch until a later grab-broken. A real new press always lies in its window; menus and
 * popovers that legitimately receive presses outside hold a GTK grab.
 */
auto isMisdirectedPress(GdkEvent* event) -> bool {
    if (event->type != GDK_BUTTON_PRESS || !isPenOrTouch(event) || gtk_grab_get_current()) {
        return false;
    }
    GdkWindow* window = event->button.window;
    if (!window || gdk_window_get_window_type(gdk_window_get_toplevel(window)) != GDK_WINDOW_TOPLEVEL) {
        return false;
    }
    const double x = event->button.x;
    const double y = event->button.y;
    return x < 0 || y < 0 || x >= gdk_window_get_width(window) || y >= gdk_window_get_height(window);
}

void releaseStalePointerGrab(GdkEvent* event) {
    if (FILE* log = openLog()) {
        std::fprintf(log, "%10.3f   releasing a stale pointer grab of %s\n",
                     static_cast<double>(g_get_monotonic_time()) / 1e6, typeName(gtk_get_event_widget(event)));
        std::fflush(log);
    }
    GdkSeat* seat = gdk_display_get_default_seat(gdk_window_get_display(event->button.window));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    // Ends the implicit grab too (and releases the Windows mouse capture); gdk_seat_ungrab only ends seat grabs
    gdk_device_ungrab(gdk_seat_get_pointer(seat), GDK_CURRENT_TIME);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

}  // namespace

auto InputSafety::penActiveWithin(gint64 durationUs) -> bool {
    // Hovering moves count too; not penInRange alone: a missed proximity-out would keep it set
    return lastPenTime != 0 && g_get_monotonic_time() - lastPenTime < durationUs;
}

void InputSafety::note(const char* text) {
    if (FILE* log = openLog()) {
        std::fprintf(log, "%10.3f   %s\n", static_cast<double>(g_get_monotonic_time()) / 1e6, text);
        std::fflush(log);
    }
}

auto InputSafety::handleEvent(GdkEvent* event) -> bool {
    logEvent(event);
    trackModality(event);
    if (isUnwantedTooltip(event)) {
        gtk_main_do_event(event);  // GTK first marks the window as mapped, then it can be hidden
        hideTooltips();
        return true;
    }
    if (isMisdirectedPress(event)) {
        // The press would only reach the widget holding the grab; the next tap goes where it lands
        hideTooltips();
        releaseStalePointerGrab(event);
        return true;
    }
    switch (event->type) {
        case GDK_BUTTON_PRESS:
        case GDK_TOUCH_BEGIN:
            if (isPenOrTouch(event)) {
                hideTooltips();
                releaseStaleMenuGrab();
            }
            break;
        case GDK_PROXIMITY_OUT:  // the pen left the screen
            hideTooltips();
            break;
        case GDK_FOCUS_CHANGE:
            if (!event->focus_change.in) {
                hideTooltips();
            }
            break;
        default:
            break;
    }
    return false;
}

#else

auto InputSafety::handleEvent(GdkEvent*) -> bool { return false; }
void InputSafety::note(const char*) {}
auto InputSafety::penActiveWithin(gint64) -> bool { return false; }

#endif
