/*
 * Xournal++
 *
 * Fork: which kind of device an event comes from, and the tolerances shared by all touch and drag interactions, so
 * that taps, holds and drags behave the same everywhere in the window
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gtk/gtk.h>

namespace xoj::input {

enum class Modality {
    Mouse,  ///< also touchpads and anything unknown
    Pen,    ///< pen tip and eraser
    Touch,  ///< touchscreen
};

/// The physical device of the event. gdk_event_get_device() and gtk_gesture_get_device() return the logical pointer,
/// which on Windows is always a mouse, also for touch and pen input.
Modality modalityOf(const GdkEvent* event);

/// The physical device of the gesture's current sequence
Modality modalityOf(GtkGesture* gesture);

/// Pixels a finger may move during a tap or a hold; more is a swipe (scrolling)
constexpr double TOUCH_SLOP = 12;
/// How long a finger holds still for a long press: it opens the context menu or starts dragging. Taps are shorter.
constexpr guint TOUCH_HOLD_MS = 400;
/// Pixels the mouse or pen moves with the button down before a drag starts
constexpr double DRAG_THRESHOLD = 12;

}  // namespace xoj::input
