#include "TouchButtonFix.h"

#include <gtk/gtk.h>

#ifdef _WIN32
namespace {

/// The touch sequence currently tapping a button, if any
struct Tap {
    GdkEventSequence* sequence = nullptr;
    GtkWidget* button = nullptr;  ///< weak pointer, reset when the button is destroyed
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
    return widget && GTK_IS_BUTTON(widget) && gtk_widget_is_sensitive(widget) ? widget : nullptr;
}

/// Whether a touch event lies within its window, i.e. the finger was lifted on the button
auto isInside(GdkEvent* event) -> bool {
    const auto width = gdk_window_get_width(event->touch.window);
    const auto height = gdk_window_get_height(event->touch.window);
    return event->touch.x >= 0 && event->touch.y >= 0 && event->touch.x <= width && event->touch.y <= height;
}

/// Returns true if the event was consumed
auto handleTouch(GdkEvent* event) -> bool {
    switch (event->type) {
        case GDK_TOUCH_BEGIN:
            if (auto* button = buttonFor(event)) {
                releaseTap();
                tap.sequence = event->touch.sequence;
                tap.button = button;
                g_object_add_weak_pointer(G_OBJECT(button), reinterpret_cast<gpointer*>(&tap.button));
                gtk_widget_set_state_flags(button, GTK_STATE_FLAG_ACTIVE, false);
                return true;
            }
            return false;
        case GDK_TOUCH_UPDATE:
            return tap.sequence && event->touch.sequence == tap.sequence;
        case GDK_TOUCH_END:
        case GDK_TOUCH_CANCEL:
            if (tap.sequence && event->touch.sequence == tap.sequence) {
                GtkWidget* button = tap.button;
                const bool clicked = event->type == GDK_TOUCH_END && button && buttonFor(event) == button &&
                                     isInside(event);
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
                return true;
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
