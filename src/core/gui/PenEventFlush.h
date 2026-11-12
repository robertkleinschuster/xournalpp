/*
 * Xournal++
 *
 * Fork: delivers pen and mouse motion at once instead of holding the newest one back
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gtk/gtk.h>

/**
 * GDK 3 delivers a motion event only once another event follows it or the frame clock's next "flush-events" phase
 * releases it (_gdk_event_queue_find_first): the newest pen position waits up to one report period of the pen (7 ms
 * at 140 Hz) or a frame. After each pointer message this releases it right away by queueing an empty event after it.
 * Windows only; does nothing elsewhere.
 */
namespace PenEventFlush {
void install(GtkWidget* toplevel);
}
