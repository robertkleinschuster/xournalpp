#include "ContextMenuTrigger.h"

#include <algorithm>  // for max
#include <utility>    // for move

#include "gui/InputModality.h"  // for modalityOf, TOUCH_HOLD_MS

namespace {
/// The widget's origin in the coordinates of its GdkWindow (no-window widgets are drawn into their parent's)
auto originInWindow(GtkWidget* widget) -> std::pair<int, int> {
    if (gtk_widget_get_has_window(widget)) {
        return {0, 0};
    }
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    return {allocation.x, allocation.y};
}

/// Converts the event's position to the widget's coordinates (the event may come from a child window, e.g. the bin
/// window of a tree view or the input window of a button)
auto eventPositionIn(GtkWidget* widget, const GdkEvent* event) -> std::pair<double, double> {
    double rootX = 0;
    double rootY = 0;
    gdk_event_get_root_coords(event, &rootX, &rootY);
    int originX = 0;
    int originY = 0;
    gdk_window_get_origin(gtk_widget_get_window(widget), &originX, &originY);
    const auto [offsetX, offsetY] = originInWindow(widget);
    return {rootX - originX - offsetX, rootY - originY - offsetY};
}
}  // namespace

ContextMenuTrigger::ContextMenuTrigger(GtkWidget* widget, Callback callback, TouchHold touchHold):
        widget(widget), callback(std::move(callback)) {
    g_object_add_weak_pointer(G_OBJECT(widget), reinterpret_cast<gpointer*>(&this->widget));
    gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
    // Before the widget's own handler, e.g. a tree view's selection on button press
    buttonHandler = g_signal_connect(widget, "button-press-event", G_CALLBACK(onButtonPress), this);
    popupHandler = g_signal_connect(widget, "popup-menu", G_CALLBACK(onPopupMenu), this);

    if (touchHold != TouchHold::OpensMenu) {
        return;
    }
    longPress.reset(gtk_gesture_long_press_new(widget), xoj::util::adopt);
    // Windows delivers taps also as emulated mouse events, so the gesture cannot be touch only; pressed() checks
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress.get()), false);
    gint longPressTime = 500;
    g_object_get(gtk_widget_get_settings(widget), "gtk-long-press-time", &longPressTime, nullptr);
    g_object_set(longPress.get(), "delay-factor",
                 static_cast<double>(xoj::input::TOUCH_HOLD_MS) / std::max(longPressTime, 1), nullptr);

    g_signal_connect(longPress.get(), "pressed", G_CALLBACK(+[](GtkGesture* gesture, double x, double y, gpointer d) {
                         auto* self = static_cast<ContextMenuTrigger*>(d);
                         if (xoj::input::modalityOf(gesture) != xoj::input::Modality::Touch) {
                             return;  // the mouse and the pen have a right click
                         }
                         // Claimed, so that lifting the finger does not also activate the item below it
                         gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
                         self->pendingLongPress = std::make_pair(x, y);
                     }),
                     this);
    g_signal_connect(longPress.get(), "cancelled", G_CALLBACK(+[](GtkGesture*, gpointer d) {
                         static_cast<ContextMenuTrigger*>(d)->pendingLongPress.reset();
                     }),
                     this);
    g_signal_connect(longPress.get(), "end", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer d) {
                         auto* self = static_cast<ContextMenuTrigger*>(d);
                         if (auto at = std::exchange(self->pendingLongPress, std::nullopt)) {
                             self->request(at->first, at->second);
                         }
                     }),
                     this);
}

ContextMenuTrigger::~ContextMenuTrigger() {
    longPress.reset();
    if (widget) {
        g_signal_handler_disconnect(widget, buttonHandler);
        g_signal_handler_disconnect(widget, popupHandler);
        g_object_remove_weak_pointer(G_OBJECT(widget), reinterpret_cast<gpointer*>(&this->widget));
    }
}

void ContextMenuTrigger::request(double x, double y) { callback({x, y, nullptr, false}); }

auto ContextMenuTrigger::onButtonPress(GtkWidget* widget, GdkEventButton* event, ContextMenuTrigger* self) -> gboolean {
    auto* ev = reinterpret_cast<GdkEvent*>(event);
    if (event->type != GDK_BUTTON_PRESS || !gdk_event_triggers_context_menu(ev)) {
        return false;
    }
    const auto [x, y] = eventPositionIn(widget, ev);
    self->callback({x, y, ev, false});
    return true;
}

auto ContextMenuTrigger::onPopupMenu(GtkWidget* widget, ContextMenuTrigger* self) -> gboolean {
    self->callback({gtk_widget_get_allocated_width(widget) / 2.0, gtk_widget_get_allocated_height(widget) / 2.0,
                    nullptr, true});
    return true;
}

void ContextMenuTrigger::popup(GtkMenu* menu, GtkWidget* anchor, const Request& request, bool destroyWhenClosed) {
    gtk_widget_show_all(GTK_WIDGET(menu));
    if (!gtk_menu_get_attach_widget(menu)) {
        gtk_menu_attach_to_widget(menu, anchor, nullptr);
    }
    if (destroyWhenClosed) {
        // Entries are activated after "deactivate", so destroy the menu only once the main loop is idle again
        g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* shell, gpointer) {
                             g_idle_add(
                                     +[](gpointer widget) -> gboolean {
                                         gtk_widget_destroy(GTK_WIDGET(widget));
                                         return G_SOURCE_REMOVE;
                                     },
                                     shell);
                         }),
                         nullptr);
    }
    // At the requested position for every kind of input (the pointer's position would be the mouse cursor's, which
    // is elsewhere after a touch)
    const auto [offsetX, offsetY] = originInWindow(anchor);
    const GdkRectangle rect{static_cast<int>(request.x) + offsetX, static_cast<int>(request.y) + offsetY, 1, 1};
    gtk_menu_popup_at_rect(menu, gtk_widget_get_window(anchor), &rect, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST,
                           request.event);
}

auto ContextMenuTrigger::rowOf(GtkTreeView* tree, Request& request) -> GtkTreePath* {
    GtkTreePath* path = nullptr;
    if (request.fromKeyboard) {
        gtk_tree_view_get_cursor(tree, &path, nullptr);
        if (path) {
            GdkRectangle cell;
            gtk_tree_view_get_cell_area(tree, path, nullptr, &cell);
            gint x = 0;
            gint y = 0;
            gtk_tree_view_convert_bin_window_to_widget_coords(tree, cell.x, cell.y + cell.height, &x, &y);
            request.x = x + 24;
            request.y = y;
        }
        return path;
    }
    gint binX = 0;
    gint binY = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(tree, static_cast<gint>(request.x), static_cast<gint>(request.y),
                                                      &binX, &binY);
    if (!gtk_tree_view_get_path_at_pos(tree, binX, binY, &path, nullptr, nullptr, nullptr)) {
        return nullptr;
    }
    return path;
}
