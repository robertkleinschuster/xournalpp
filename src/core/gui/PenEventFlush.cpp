#include "PenEventFlush.h"

#ifdef _WIN32

#include <windows.h>

namespace {

GtkWidget* window = nullptr;  ///< weak
bool scheduled = false;

auto release(gpointer) -> gboolean {
    scheduled = false;
    if (!window) {
        return G_SOURCE_REMOVE;
    }
    // GDK hands out a held-back motion event as soon as any event follows it in the queue: queue an empty one (no
    // window: GTK drops it at once). Unlike triggering the frame clock's flush phase, this leaves the clock alone.
    GdkEvent* nothing = gdk_event_new(GDK_NOTHING);
    gdk_event_put(nothing);
    gdk_event_free(nothing);
    return G_SOURCE_REMOVE;
}

auto filter(GdkXEvent* xevent, GdkEvent*, gpointer) -> GdkFilterReturn {
    const auto* msg = static_cast<const MSG*>(xevent);
    switch (msg->message) {
        case WM_POINTERUPDATE:
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        case WM_MOUSEMOVE:
            if (!scheduled) {
                scheduled = true;
                // Before the GDK event source runs again, so the motion it just queued is delivered next
                g_idle_add_full(G_PRIORITY_HIGH, release, nullptr, nullptr);
            }
            break;
        default:
            break;
    }
    return GDK_FILTER_CONTINUE;
}

}  // namespace

void PenEventFlush::install(GtkWidget* toplevel) {
    if (window) {
        return;
    }
    window = toplevel;
    g_object_add_weak_pointer(G_OBJECT(toplevel), reinterpret_cast<gpointer*>(&window));
    gdk_window_add_filter(nullptr, filter, nullptr);
}

#else

void PenEventFlush::install(GtkWidget*) {}

#endif
