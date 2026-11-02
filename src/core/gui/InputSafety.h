/*
 * Xournal++
 *
 * Safety nets and diagnostics for pen and touch input on Windows
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gtk/gtk.h>

namespace InputSafety {

/**
 * Fork: called for every GDK event before GTK handles it (Windows only).
 *  - Hides tooltips when a pen or finger touches down and when the pen leaves the screen: GTK shows tooltips while
 *    the pen hovers, but on Windows often gets no leave event afterwards, so they stayed on screen.
 *  - Hides tooltips that appear when neither the pen hovers nor the mouse was used last (after the pen left, or after
 *    a touch).
 *  - Releases the input grab of a menu that is no longer shown when the user taps, so the window does not stay
 *    blocked.
 *  - Writes a diagnostic log of pen, touch, menu and grab events to "input-debug.log" in the config folder while a
 *    file named "input-debug" exists there.
 *  - Ends a stale implicit pointer grab when a touch or pen press is delivered to a window it does not lie in, and
 *    drops that press.
 * @return whether the event is dropped
 */
bool handleEvent(GdkEvent* event);

/// Adds a line to the diagnostic log, if it is enabled
void note(const char* text);

}  // namespace InputSafety
