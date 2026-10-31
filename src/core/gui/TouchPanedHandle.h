/*
 * Xournal++
 *
 * A pane separator that can be dragged with a finger
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gtk/gtk.h>

namespace TouchPanedHandle {

/**
 * Fork: makes the separator of a horizontal GtkPaned (e.g. between the sidebar and the document) wide enough to be
 * dragged with a finger, shows a grip on it and moves it with touch drags, which GTK 3 does not do reliably on
 * Windows. Dragging with the mouse or the pen keeps working as before.
 */
void install(GtkPaned* paned);

}  // namespace TouchPanedHandle
