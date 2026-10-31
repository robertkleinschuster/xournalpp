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
 *  - Portrait orientation: the sidebar is hidden to leave the narrow width to the page; it comes back in landscape.
 *  - In tablet mode, tapping a text field (search, rename, ...) with a finger or the pen opens the touch keyboard,
 *    which GTK 3 does not do by itself; it is closed again when the field loses focus.
 * Nothing is stored in the settings: these adaptations only last while the device is in that state.
 */
void install(MainWindow* win, Control* control);

}  // namespace ConvertibleSupport
