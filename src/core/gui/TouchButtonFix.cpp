#include "TouchButtonFix.h"

#include <cmath>

#include <gtk/gtk.h>

#ifdef _WIN32
namespace {

/// Movement (in pixels) after which a touch counts as a drag (e.g. scrolling a list) instead of a tap
constexpr double TAP_SLOP = 12.0;

/// The touch sequence currently tapping a button, if any
struct Tap {
    GdkEventSequence* sequence = nullptr;
    GtkWidget* button = nullptr;  ///< weak pointer, reset when the button is destroyed
    double startX = 0;
    double startY = 0;
    /// The button is inside a scrollable area: GTK keeps getting the events so dragging still scrolls
    bool passThrough = false;
    bool moved = false;
};
Tap tap;

void releaseTap() {
    if (tap.button) {
        gtk_widget_unset_state_flags(tap.button, GTK_STATE_FLAG_ACTIVE);
        g_object_remove_weak_pointer(G_OBJECT(tap.button), reinterpret_cast<gpointer*>(&tap.button));
    }
    tap = {};
}

/// The button a touch event is delivered to, or nullptr
auto buttonFor(GdkEvent* event) -> GtkWidget* {
    GtkWidget* widget = gtk_get_event_widget(event);
    if (!widget || !GTK_IS_BUTTON(widget) || !gtk_widget_is_sensitive(widget)) {
        return nullptr;
    }
    // While a menu or another grab is active, GTK routes the input to it
    GtkWidget* grab = gtk_grab_get_current();
    if (grab && grab != widget && !gtk_widget_is_ancestor(widget, grab)) {
        return nullptr;
    }
    return widget;
}

/// Whether a touch event lies within its window, i.e. the finger was lifted on the button
auto isInside(GdkEvent* event) -> bool {
    const auto width = gdk_window_get_width(event->touch.window);
    const auto height = gdk_window_get_height(event->touch.window);
    return event->touch.x >= 0 && event->touch.y >= 0 && event->touch.x <= width && event->touch.y <= height;
}

auto isInScrolledWindow(GtkWidget* widget) -> bool {
    return gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW) != nullptr;
}

/// Returns true if the event was consumed
auto handleTouch(GdkEvent* event) -> bool {
    switch (event->type) {
        case GDK_TOUCH_BEGIN:
            if (auto* button = buttonFor(event)) {
                releaseTap();
                tap.sequence = event->touch.sequence;
                tap.button = button;
                tap.startX = event->touch.x_root;
                tap.startY = event->touch.y_root;
                tap.passThrough = isInScrolledWindow(button);
                g_object_add_weak_pointer(G_OBJECT(button), reinterpret_cast<gpointer*>(&tap.button));
                if (!tap.passThrough) {
                    gtk_widget_set_state_flags(button, GTK_STATE_FLAG_ACTIVE, false);
                }
                return !tap.passThrough;
            }
            return false;
        case GDK_TOUCH_UPDATE:
            if (!tap.sequence || event->touch.sequence != tap.sequence) {
                return false;
            }
            if (std::hypot(event->touch.x_root - tap.startX, event->touch.y_root - tap.startY) > TAP_SLOP) {
                tap.moved = true;
            }
            return !tap.passThrough;
        case GDK_TOUCH_END:
        case GDK_TOUCH_CANCEL:
            if (tap.sequence && event->touch.sequence == tap.sequence) {
                GtkWidget* button = tap.button;
                const bool passThrough = tap.passThrough;
                const bool clicked = event->type == GDK_TOUCH_END && button && !(passThrough && tap.moved) &&
                                     buttonFor(event) == button && isInside(event);
                if (button) {
                    g_object_ref(button);
                }
                releaseTap();
                if (clicked) {
                    gtk_button_clicked(GTK_BUTTON(button));
                }
                if (button) {
                    g_object_unref(button);
                }
                return !passThrough;
            }
            return false;
        default:
            return false;
    }
}

}  // namespace
#endif

void TouchButtonFix::install() {
#ifdef _WIN32
    gdk_event_handler_set(
            +[](GdkEvent* event, gpointer) {
                if (!handleTouch(event)) {
                    gtk_main_do_event(event);
                }
            },
            nullptr, nullptr);
#endif
}
