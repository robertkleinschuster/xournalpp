/*
 * Xournal++
 *
 * Fork: when the device goes to sleep or wakes up (Windows)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>  // for function

#include <gtk/gtk.h>

namespace PowerEvents {

enum class Event {
    /// The user stops using the device: the lid is closed, the screen turns off, or the system suspends. Work should
    /// be on disk now; the process may be frozen or the battery may run out before the next event.
    Standby,
    /// The screen is on again after a Standby event (also after resuming from suspend)
    Wake,
};

using Listener = std::function<void(Event)>;

/// Watches the power events of the main window (Windows; does nothing elsewhere)
void install(GtkWidget* mainWindow);

/// Listeners are called on the main loop, each event once per transition; returns an id for removeListener
guint addListener(Listener listener);
void removeListener(guint id);

}  // namespace PowerEvents
