/*
 * Xournal++
 *
 * Fork: opens context menus the same way everywhere
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>  // for function
#include <optional>    // for optional
#include <utility>     // for pair

#include <gtk/gtk.h>

#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr

/**
 * Requests a widget's context menu with
 *  - a right click: the mouse, and the pen's barrel button,
 *  - a long press with a finger (xoj::input::TOUCH_HOLD_MS); the menu opens once the finger is lifted, as lifting it
 *    above an open menu would choose the entry below it. Windows does not turn touch holds into right clicks here:
 *    GTK receives touch as touch events, and PenFeedbackFix turns press-and-hold off (it fired while writing),
 *  - the Menu key or Shift+F10 (the widget needs the focus).
 *
 * The pen has no long press: it holds still while writing, and it has the barrel button.
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
     * Shows a context menu where it was requested (the click, the finger, or for the keyboard the point the callback
     * chose). Attaches it to the anchor if it is not attached yet.
     * @param destroyWhenClosed for menus built for this request only
     */
    static void popup(GtkMenu* menu, GtkWidget* anchor, const Request& request, bool destroyWhenClosed);

    /**
     * The row of a tree view the menu is for (to free with gtk_tree_path_free), or nullptr: the row at the request's
     * position, for the keyboard the row with the cursor, in which case the menu opens below it.
     */
    static auto rowOf(GtkTreeView* tree, Request& request) -> GtkTreePath*;

private:
    static auto onButtonPress(GtkWidget* widget, GdkEventButton* event, ContextMenuTrigger* self) -> gboolean;
    static auto onPopupMenu(GtkWidget* widget, ContextMenuTrigger* self) -> gboolean;

    GtkWidget* widget;  ///< weak: reset to nullptr by GLib when it is destroyed
    Callback callback;
    xoj::util::GObjectSPtr<GtkGesture> longPress;
    std::optional<std::pair<double, double>> pendingLongPress;
    gulong buttonHandler = 0;
    gulong popupHandler = 0;
};
