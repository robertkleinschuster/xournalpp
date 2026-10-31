/*
 * Xournal++
 *
 * Adapts the main window to a convertible: portrait orientation and the touch keyboard in tablet mode
 *
 * @license GNU GPLv2 or later
 */

#pragma once

class Control;
class MainWindow;

namespace ConvertibleSupport {

/**
 * Fork, for convertibles like the ThinkPad X13 Yoga (Windows only, does nothing elsewhere):
 *  - Tablet mode (the keyboard is folded away): toolbar buttons get a larger area to hit with a finger.
 *  - Portrait orientation: the sidebar is hidden to leave the narrow width to the page; it comes back in landscape.
 *  - In tablet mode, tapping a text field (search, rename, LaTeX, ...) with a finger or the pen opens the touch
 *    keyboard, which GTK 3 does not do by itself, also again after it was closed; it is closed again when the field
 *    loses focus. The same for text written on the page with the text tool.
 * Nothing is stored in the settings: these adaptations only last while the device is in that state.
 */
void install(MainWindow* win, Control* control);

/// Text editing on the page started (text tool): opens the touch keyboard in tablet mode if started by finger or pen
void textEditingStarted();
/// Text editing on the page ended: closes the touch keyboard if it was opened for it
void textEditingEnded();

}  // namespace ConvertibleSupport
