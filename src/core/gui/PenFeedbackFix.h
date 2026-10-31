/*
 * Xournal++
 *
 * Turns off Windows pen gestures and their visual feedback for a window
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gtk/gtk.h>

namespace PenFeedbackFix {

/**
 * Fork: Windows interprets pen input on top of what the application receives: flicks (quick strokes become
 * scrolling or back/forward), press and hold (holding the pen still becomes a right-click with a growing ring) and
 * ripples for taps and the barrel button. While writing, these interfere and cost latency. This turns them off for the
 * given toplevel window only, the system settings stay as they are. Does nothing on other platforms.
 */
void apply(GtkWidget* toplevel);

}  // namespace PenFeedbackFix
