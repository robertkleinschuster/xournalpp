#include "InputSafety.h"

#ifdef _WIN32

#include <cstdint>
#include <cstdio>
#include <string>

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
    std::fprintf(log, "%10.3f %-13s src=%-8s btn=%u at=%7.1f,%7.1f widget=%s grab=%s(%s) tooltips=%d\n",
                 static_cast<double>(g_get_monotonic_time()) / 1e6, name, sourceName(event), button, x, y,
                 typeName(gtk_get_event_widget(event)), typeName(grab),
                 grab ? (gtk_widget_get_mapped(grab) ? "mapped" : "unmapped") : "", visibleTooltips());
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

}  // namespace

void InputSafety::note(const char* text) {
    if (FILE* log = openLog()) {
        std::fprintf(log, "%10.3f   %s\n", static_cast<double>(g_get_monotonic_time()) / 1e6, text);
        std::fflush(log);
    }
}

void InputSafety::handleEvent(GdkEvent* event) {
    logEvent(event);
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
}

#else

void InputSafety::handleEvent(GdkEvent*) {}
void InputSafety::note(const char*) {}

#endif
