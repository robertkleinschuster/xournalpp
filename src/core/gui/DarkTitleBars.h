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
 * The main window draws its own (dark) header bar, but dialogs get the native Windows title bar, which stays light.
 * This gives every window with a native title bar the dark Windows title bar while GTK uses the dark theme
 * ("gtk-application-prefer-dark-theme"), also after switching. Does nothing on other platforms.
 */
void install();

}  // namespace DarkTitleBars
