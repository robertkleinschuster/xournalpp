/*
 * Xournal++
 *
 * Two-finger touchpad gestures that behave like two fingers on a touchscreen
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>

#include <gtk/gtk.h>

class ZoomControl;
class XournalView;

/**
 * Fork: on Windows, GTK 3 sees precision touchpad gestures only as mouse wheel events that Windows synthesizes:
 * scrolling as wheel fractions and pinching as coarse Ctrl + wheel steps in bursts, so neither follows the fingers.
 *
 * This reads the finger positions passively via Raw Input (the system's pointer handling is not affected) and handles
 * two fingers on the touchpad like two fingers on the touchscreen: the document moves with the fingers in every
 * direction and zooms by the ratio of their distance, both at the same time, around the mouse pointer. Meanwhile the
 * synthesized wheel events of the gesture (including its inertia afterwards) are ignored.
 * Does nothing on other platforms or without a precision touchpad.
 */
class TouchpadGestures final {
public:
    TouchpadGestures(ZoomControl* zoom, XournalView* view, GtkWidget* toplevel);
    ~TouchpadGestures();

    TouchpadGestures(const TouchpadGestures&) = delete;
    TouchpadGestures& operator=(const TouchpadGestures&) = delete;

    /**
     * Whether a scroll event (wheel notch units) on the document is synthesized from a touchpad gesture handled here
     * and has to be ignored. Also learns the system's touchpad scroll direction from these events.
     */
    bool consumeSynthesizedScroll(double dx, double dy);

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};
