/*
 * Xournal++
 *
 * Fork: ink drawn by the Windows compositor while writing (DirectComposition delegated ink trail)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>

#include <gtk/gtk.h>

/**
 * While a stroke is written, the Windows compositor (DWM) draws it on top of the window: from the points the stroke
 * has so far to wherever the pen is now, with every compositor frame and before the application has drawn anything.
 * That removes the wait for the application's frame (up to one or two frames). The application still draws the
 * stroke itself; the compositor's part is removed once the application's frame has it (frameDrawn()).
 *
 * Needs Windows 11 (IDCompositionInkTrailDevice); elsewhere, or if it cannot be set up, isAvailable() is false and
 * nothing changes.
 */
class DelegatedInk final {
public:
    /// The ink trail of the main window, created on first use; nullptr if not available
    static DelegatedInk* forWindow(GtkWidget* toplevel);
    ~DelegatedInk();

    /// A new stroke in this color (0..1 components)
    void startTrail(float red, float green, float blue, float alpha);
    /// Adds a point of the stroke, in pixels of the window's client area
    void addPoint(float x, float y, float radius);
    /// The application's frame shows the stroke up to the points added before it was drawn
    void frameDrawn();
    /// The stroke ended or was cancelled: remove the trail once the application has drawn the stroke
    void endTrail();

    struct Impl;

private:
    explicit DelegatedInk(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};
