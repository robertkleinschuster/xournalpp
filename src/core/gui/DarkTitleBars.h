/*
 * Xournal++
 *
 * Fork: Windows title bars that match the dark theme
 *
 * @license GNU GPLv2 or later
 */

#pragma once

namespace DarkTitleBars {

/**
 * Native Windows title bars stay light with GTK's dark theme. This gives every window with a native title bar (the main
 * window and dialogs) the dark Windows title bar while GTK uses the dark theme
 * ("gtk-application-prefer-dark-theme"), also after switching. Does nothing on other platforms.
 */
void install();

}  // namespace DarkTitleBars
