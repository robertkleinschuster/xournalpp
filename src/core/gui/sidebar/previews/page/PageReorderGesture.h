/*
 * Xournal++
 *
 * Fork: reorders pages by drag and drop in the page preview sidebar
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>     // for size_t
#include <functional>  // for function
#include <memory>      // for unique_ptr
#include <vector>      // for vector

#include <gtk/gtk.h>

#include "util/Util.h"              // for npos
#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr, WidgetSPtr

class SidebarPreviewBaseEntry;

/**
 * Drag a page miniature to another position: with the mouse or pen as soon as it moves, with a finger after holding it
 * still for a moment (a swipe right away keeps scrolling the list). A line shows where the page goes; near the top or
 * bottom edge the list scrolls. On release, onMove(from, to) is called (to counted after removing the page).
 */
class PageReorderGesture {
public:
    using Entries = std::vector<std::unique_ptr<SidebarPreviewBaseEntry>>;

    PageReorderGesture(GtkFixed* container, GtkScrolledWindow* scrolledWindow, const Entries& entries,
                       std::function<void(size_t from, size_t to)> onMove);
    ~PageReorderGesture();

    PageReorderGesture(const PageReorderGesture&) = delete;
    PageReorderGesture& operator=(const PageReorderGesture&) = delete;

private:
    void begin(double x, double y);
    void update(double dx, double dy);
    void end();
    void startDragging();
    void reset();

    /// The entry at (x, y) in container coordinates, or npos
    size_t entryAt(double x, double y) const;
    /// The insertion position (0..count) for the pointer at (x, y), before removing the dragged page
    size_t insertionAt(double x, double y) const;
    void showMarker(size_t insertion);
    void setPointer(double x, double y);
    auto autoScroll() -> gboolean;

    GtkFixed* container;
    GtkScrolledWindow* scrolledWindow;
    const Entries& entries;
    std::function<void(size_t, size_t)> onMove;

    xoj::util::GObjectSPtr<GtkGesture> gesture;
    xoj::util::WidgetSPtr marker;  ///< the line showing where the page goes

    size_t source = npos;
    bool touch = false;
    bool dragging = false;
    double startX = 0;
    double startY = 0;
    double pointerX = 0;      ///< container coordinates
    double pointerViewY = 0;  ///< relative to the visible part of the list, for auto scrolling
    guint holdTimeout = 0;
    guint scrollTick = 0;
};
