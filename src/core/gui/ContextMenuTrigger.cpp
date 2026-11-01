#include "ContextMenuTrigger.h"

#include <algorithm>  // for max
#include <utility>    // for move, pair

#include "gui/InputModality.h"  // for modalityOf, TOUCH_HOLD_MS

namespace {
/// Converts the event's position to the widget's coordinates (the event may come from a child window, e.g. the bin
/// window of a tree view or the input window of a button)
auto eventPositionIn(GtkWidget* widget, const GdkEvent* event) -> std::pair<double, double> {
    double rootX = 0;
    double rootY = 0;
    gdk_event_get_root_coords(event, &rootX, &rootY);
    int originX = 0;
    int originY = 0;
    gdk_window_get_origin(gtk_widget_get_window(widget), &originX, &originY);
    if (!gtk_widget_get_has_window(widget)) {
        // No-window widgets are drawn into their parent's window
        GtkAllocation allocation;
        gtk_widget_get_allocation(widget, &allocation);
        originX += allocation.x;
        originY += allocation.y;
    }
    return {rootX - originX, rootY - originY};
}
}  // namespace

// --- ContextMenu ---

ContextMenu::ContextMenu():
        menu(g_menu_new(), xoj::util::adopt),
        section(g_menu_new(), xoj::util::adopt),
        group(g_simple_action_group_new(), xoj::util::adopt) {
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(section.get()));
}

void ContextMenu::add(const std::string& label, std::function<void()> action, bool enabled) {
    const std::string name = "entry" + std::to_string(count++);
    GSimpleAction* simpleAction = g_simple_action_new(name.c_str(), nullptr);
    g_signal_connect_data(
            simpleAction, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
                (*static_cast<std::function<void()>*>(data))();
            }),
            new std::function<void()>(std::move(action)),
            +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, GConnectFlags(0));
    g_simple_action_set_enabled(simpleAction, enabled);
    g_action_map_add_action(G_ACTION_MAP(group.get()), G_ACTION(simpleAction));
    g_object_unref(simpleAction);
    const std::string detailed = std::string(ACTION_PREFIX) + "." + name;
    g_menu_append(section.get(), label.c_str(), detailed.c_str());
}

void ContextMenu::addSeparator() {
    if (g_menu_model_get_n_items(G_MENU_MODEL(section.get())) == 0) {
        return;
    }
    section.reset(g_menu_new(), xoj::util::adopt);
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(section.get()));
}

auto ContextMenu::model() const -> GMenuModel* { return G_MENU_MODEL(menu.get()); }

auto ContextMenu::actions() const -> GActionGroup* { return G_ACTION_GROUP(group.get()); }

// --- ContextMenuTrigger ---

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
    // Windows delivers taps also as emulated mouse events, so the gesture cannot be touch only; "pressed" checks
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress.get()), false);
    gint longPressTime = 500;
    g_object_get(gtk_widget_get_settings(widget), "gtk-long-press-time", &longPressTime, nullptr);
    g_object_set(longPress.get(), "delay-factor",
                 static_cast<double>(xoj::input::TOUCH_HOLD_MS) / std::max(longPressTime, 1), nullptr);

    g_signal_connect(longPress.get(), "pressed", G_CALLBACK(+[](GtkGesture* gesture, double x, double y, gpointer d) {
                         if (xoj::input::modalityOf(gesture) != xoj::input::Modality::Touch) {
                             return;  // the mouse and the pen have a right click
                         }
                         // Claimed, so that the item below the finger is not also activated
                         gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
                         static_cast<ContextMenuTrigger*>(d)->request(x, y);
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

void ContextMenuTrigger::popup(GMenuModel* model, GActionGroup* actions, GtkWidget* anchor, const Request& request) {
    GtkWidget* popover = gtk_popover_new_from_model(anchor, model);
    if (actions) {
        gtk_widget_insert_action_group(popover, ContextMenu::ACTION_PREFIX, actions);
    }
    // At the requested position for every kind of input (the pointer's position would be the mouse cursor's, which
    // is elsewhere after a touch). Below it, so that the finger does not cover the menu.
    const GdkRectangle rect{static_cast<int>(request.x), static_cast<int>(request.y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    // Built for this request only. Entries are activated before "closed"; destroy it once the main loop is idle again.
    g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* popover, gpointer) {
                         g_idle_add(
                                 +[](gpointer widget) -> gboolean {
                                     gtk_widget_destroy(GTK_WIDGET(widget));
                                     return G_SOURCE_REMOVE;
                                 },
                                 popover);
                     }),
                     nullptr);
    gtk_popover_popup(GTK_POPOVER(popover));
}

void ContextMenuTrigger::popup(const ContextMenu& menu, GtkWidget* anchor, const Request& request) {
    popup(menu.model(), menu.actions(), anchor, request);
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
