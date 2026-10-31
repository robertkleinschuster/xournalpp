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

// --- Menus of a menu bar ---

/// The menu bar whose menu is open (holding the grab), or nullptr
auto activeMenuBar() -> GtkMenuShell* {
    GtkWidget* grab = gtk_grab_get_current();
    // Walk from the open (sub)menu up to the menu bar it was opened from
    while (grab && GTK_IS_MENU(grab)) {
        GtkWidget* item = gtk_menu_get_attach_widget(GTK_MENU(grab));
        grab = item ? gtk_widget_get_parent(item) : nullptr;
    }
    return grab && GTK_IS_MENU_BAR(grab) ? GTK_MENU_SHELL(grab) : nullptr;
}

/// The touch sequence tapping beside the open menu, handled here
GdkEventSequence* menuTap = nullptr;

/// Whether the screen position (root coordinates) lies on the widget
auto isOnWidget(GtkWidget* widget, double x, double y) -> bool {
    GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
    GdkWindow* window = toplevel ? gtk_widget_get_window(toplevel) : nullptr;
    if (!window || !gtk_widget_get_mapped(widget)) {
        return false;
    }
    int originX = 0, originY = 0, left = 0, top = 0;
    gdk_window_get_origin(window, &originX, &originY);
    if (!gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &left, &top)) {
        return false;
    }
    const double localX = x - originX - left;
    const double localY = y - originY - top;
    return localX >= 0 && localY >= 0 && localX < gtk_widget_get_allocated_width(widget) &&
           localY < gtk_widget_get_allocated_height(widget);
}

/// Whether the screen position lies on one of the open menus of the menu bar (including submenus)
auto isOnOpenMenu(double x, double y) -> bool {
    for (GtkWidget* menu = gtk_grab_get_current(); menu && GTK_IS_MENU(menu);) {
        if (isOnWidget(gtk_widget_get_toplevel(menu), x, y)) {
            return true;
        }
        GtkWidget* item = gtk_menu_get_attach_widget(GTK_MENU(menu));
        menu = item ? gtk_widget_get_parent(item) : nullptr;
    }
    return false;
}

/// The menu bar item at the screen position, or nullptr
auto menuBarItemAt(GtkMenuShell* bar, double x, double y) -> GtkWidget* {
    GtkWidget* found = nullptr;
    GList* children = gtk_container_get_children(GTK_CONTAINER(bar));
    for (GList* child = children; child && !found; child = child->next) {
        auto* item = GTK_WIDGET(child->data);
        if (GTK_IS_MENU_ITEM(item) && isOnWidget(item, x, y)) {
            found = item;
        }
    }
    g_list_free(children);
    return found;
}

/**
 * While a menu of a menu bar is open, GTK 3 on Windows does not reliably handle taps on the menu bar or beside the
 * menu, so menus stayed open. The open menu grabs the input, so GDK reports these touches on the menu's window: what
 * is under the finger is found from its screen position. A tap on another menu bar item opens its menu, a tap on the
 * open one or anywhere beside the menus closes it. Taps on the menus are left to GTK. Returns true if consumed.
 */
auto handleMenuTouch(GdkEvent* event) -> bool {
    if (event->type != GDK_TOUCH_BEGIN && event->type != GDK_TOUCH_UPDATE && event->type != GDK_TOUCH_END &&
        event->type != GDK_TOUCH_CANCEL) {
        return false;
    }
    const double x = event->touch.x_root;
    const double y = event->touch.y_root;
    if (event->type != GDK_TOUCH_BEGIN) {
        if (!menuTap || event->touch.sequence != menuTap) {
            return false;
        }
        if (event->type == GDK_TOUCH_UPDATE) {
            return true;
        }
        menuTap = nullptr;
        GtkMenuShell* bar = activeMenuBar();
        if (!bar || event->type == GDK_TOUCH_CANCEL) {
            return true;
        }
        GtkWidget* item = menuBarItemAt(bar, x, y);
        if (item && gtk_menu_shell_get_selected_item(bar) != item) {
            gtk_menu_shell_select_item(bar, item);  // opens its menu
        } else {
            gtk_menu_shell_deactivate(bar);
        }
        return true;
    }
    GtkMenuShell* bar = activeMenuBar();
    if (!bar || isOnOpenMenu(x, y)) {
        return false;
    }
    menuTap = event->touch.sequence;
    return true;
}

/// Returns true if the event was consumed
auto handleTouch(GdkEvent* event) -> bool {
    if (handleMenuTouch(event)) {
        return true;
    }
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
                    if (!passThrough) {
                        // Some dialogs react to "released" (e.g. the alignment toggles of the link dialog). GTK never
                        // saw this touch, so the default handler of "released" does nothing and does not click again.
                        g_signal_emit_by_name(button, "released");
                    }
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
