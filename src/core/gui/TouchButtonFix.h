/*
 * Xournal++
 *
 * Makes GTK buttons react to touch taps on Windows
 *
 * @license GNU GPLv2 or later
 */

#pragma once

namespace TouchButtonFix {

/**
 * On Windows, GTK 3 delivers the end of a touch to a button but loses its beginning, so buttons never react to a
 * finger tap (the tree view and the canvas handle touch themselves and are not affected). This installs a GDK event
 * handler that recognizes taps on buttons from the raw touch events and clicks the button itself.
 * Does nothing on other platforms.
 */
void install();

}  // namespace TouchButtonFix
