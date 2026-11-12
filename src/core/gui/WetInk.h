/*
 * Xournal++
 *
 * Fork: the stroke being written, shown with as little delay as Windows allows
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>

#include <gtk/gtk.h>

/**
 * A transparent layer over the main window (a DirectComposition swap chain) that shows the newest part of the stroke
 * being written: its own thread draws it with Direct2D and presents it shortly before each refresh of the display, so
 * the newest pen position is on screen with the next refresh. GTK's canvas only draws at its frame clock, which adds
 * up to a frame. The layer draws exactly the points of the stroke (the stabilized ones), only the part GTK has not
 * drawn yet, and is cleared when the stroke ends: nothing differs from the line that stays.
 *
 * Windows only; isAvailable is false elsewhere, or if Direct3D/DirectComposition cannot be set up, and nothing changes.
 * XOPP_NO_WET_INK turns it off. XOPP_WET_INK_STATS=<file> logs when each frame reached the display.
 */
class WetInk final {
public:
    /// The layer of the main window, created on first use; nullptr if not available
    static WetInk* forWindow(GtkWidget* toplevel);
    ~WetInk();

    /// A new stroke in this color (0..1 components)
    void startStroke(float red, float green, float blue);
    /// Adds a point of the stroke, in pixels of the window's client area
    void addPoint(float x, float y, float radius);
    /// GTK has drawn a frame: it shows the stroke up to the points added so far
    void frameDrawn();
    /// The stroke ended or was cancelled: the layer is cleared with GTK's next frame
    void endStroke();

    struct Impl;

private:
    explicit WetInk(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};
