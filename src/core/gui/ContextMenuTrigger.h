/*
 * Xournal++
 *
 * Fork: opens context menus the same way everywhere
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>  // for function
#include <string>      // for string

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr

/**
 * The entries of a context menu built in code, each with its own callback. Menus defined in a .ui file (GMenuModel with
 * actions) are shown directly, see ContextMenuTrigger::popup().
 */
class ContextMenu {
public:
    ContextMenu();

    void add(const std::string& label, std::function<void()> action, bool enabled = true);
    /// Starts a new section (shown with a separator)
    void addSeparator();

    auto model() const -> GMenuModel*;
    auto actions() const -> GActionGroup*;

    /// The prefix of the actions' names in the menu model
    static constexpr const char* ACTION_PREFIX = "contextmenu";

private:
    xoj::util::GObjectSPtr<GMenu> menu;
    xoj::util::GObjectSPtr<GMenu> section;
    xoj::util::GObjectSPtr<GSimpleActionGroup> group;
    int count = 0;
};

/**
 * Requests a widget's context menu with
 *  - a right click: the mouse, and the pen's barrel button,
 *  - a long press with a finger (xoj::input::TOUCH_HOLD_MS): the menu opens while the finger still holds. Windows does
 *    not turn touch holds into right clicks here: GTK receives touch as touch events, and PenFeedbackFix turns
 *    press-and-hold off (it fired while writing),
 *  - the Menu key or Shift+F10 (the widget needs the focus).
 *
 * The pen has no long press: it holds still while writing, and it has the barrel button.
 *
 * Menus are shown as popovers pointing at the requested position, like the toolbar's menus. Unlike a GtkMenu, a popover
 * ignores the release of a press that started before it opened, so lifting the finger after a long press neither
 * chooses an entry nor closes it. A popover closes by itself when its widget is hidden (e.g. the sidebar in portrait).
 */
class ContextMenuTrigger {
public:
    struct Request {
        double x;               ///< in the widget's coordinates
        double y;               ///< (for the keyboard: its center; the callback may choose e.g. the selected row)
        const GdkEvent* event;  ///< the button press, or nullptr (long press, keyboard)
        bool fromKeyboard;
    };
    using Callback = std::function<void(const Request&)>;

    enum class TouchHold {
        OpensMenu,
        /// Another gesture owns holding a finger (e.g. dragging after a hold); it calls request() if the finger is
        /// lifted without moving
        HandledElsewhere,
    };

    ContextMenuTrigger(GtkWidget* widget, Callback callback, TouchHold touchHold = TouchHold::OpensMenu);
    ~ContextMenuTrigger();
    ContextMenuTrigger(const ContextMenuTrigger&) = delete;
    ContextMenuTrigger& operator=(const ContextMenuTrigger&) = delete;

    /// Requests the menu at (x, y) in the widget's coordinates, e.g. after a touch hold handled elsewhere
    void request(double x, double y);

    /**
     * Shows a menu where it was requested (the click, the finger, or for the keyboard the point the callback chose).
     * Actions of the model are looked up from the anchor (e.g. "win.") and in `actions` (ContextMenu's), if given.
     */
    static void popup(GMenuModel* model, GActionGroup* actions, GtkWidget* anchor, const Request& request);
    static void popup(const ContextMenu& menu, GtkWidget* anchor, const Request& request);

    /**
     * The row of a tree view the menu is for (to free with gtk_tree_path_free), or nullptr: the row at the request's
     * position, for the keyboard the row with the cursor, in which case the menu points below it.
     */
    static auto rowOf(GtkTreeView* tree, Request& request) -> GtkTreePath*;

private:
    static auto onButtonPress(GtkWidget* widget, GdkEventButton* event, ContextMenuTrigger* self) -> gboolean;
    static auto onPopupMenu(GtkWidget* widget, ContextMenuTrigger* self) -> gboolean;

    GtkWidget* widget;  ///< weak: reset to nullptr by GLib when it is destroyed
    Callback callback;
    xoj::util::GObjectSPtr<GtkGesture> longPress;
    gulong buttonHandler = 0;
    gulong popupHandler = 0;
};
