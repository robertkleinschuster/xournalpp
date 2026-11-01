#include "PageReorderGesture.h"

#include <algorithm>  // for clamp
#include <cmath>      // for hypot
#include <utility>    // for move

#include "gui/InputModality.h"                                  // for modalityOf, TOUCH_SLOP
#include "gui/sidebar/previews/base/SidebarPreviewBaseEntry.h"  // for SidebarPreviewBaseEntry

namespace {
using xoj::input::DRAG_THRESHOLD;      // the mouse or pen moves this far before a drag starts
using xoj::input::TOUCH_HOLD_MS;       // a finger holds a miniature this long before it can be dragged
using xoj::input::TOUCH_SLOP;          // a finger moving farther while holding is a swipe (scrolling)
constexpr double SCROLL_EDGE = 48;     ///< pixels from the top or bottom edge in which the list scrolls
constexpr double SCROLL_SPEED = 0.35;  ///< pixels per frame for each pixel the pointer is into the edge
constexpr int MARKER_THICKNESS = 4;

struct Rect {
    double x, y, width, height;
};

auto rectOf(GtkWidget* widget, GtkWidget* container) -> Rect {
    int x = 0;
    int y = 0;
    gtk_widget_translate_coordinates(widget, container, 0, 0, &x, &y);
    return {static_cast<double>(x), static_cast<double>(y), static_cast<double>(gtk_widget_get_allocated_width(widget)),
            static_cast<double>(gtk_widget_get_allocated_height(widget))};
}
auto drawMarker(GtkWidget* widget, cairo_t* cr, gpointer) -> gboolean {
    GdkRGBA color{0.21, 0.52, 0.89, 1.0};  // the usual selection blue, if the theme has none
    gtk_style_context_lookup_color(gtk_widget_get_style_context(widget), "theme_selected_bg_color", &color);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_paint(cr);
    return true;
}
}  // namespace

PageReorderGesture::PageReorderGesture(GtkFixed* container, GtkScrolledWindow* scrolledWindow, const Entries& entries,
                                       std::function<void(size_t, size_t)> onMove,
                                       std::function<void(size_t, double, double)> onHold):
        container(container),
        scrolledWindow(scrolledWindow),
        entries(entries),
        onMove(std::move(onMove)),
        onHold(std::move(onHold)),
        gesture(gtk_gesture_drag_new(GTK_WIDGET(container)), xoj::util::adopt),
        marker(gtk_drawing_area_new(), xoj::util::adopt) {
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(gesture.get()), false);
    // Before the miniature buttons and the scrolled window see the events, so a drag can take them over
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture.get()), GTK_PHASE_CAPTURE);

    g_signal_connect(gesture.get(), "drag-begin", G_CALLBACK(+[](GtkGestureDrag*, double x, double y, gpointer self) {
                         static_cast<PageReorderGesture*>(self)->begin(x, y);
                     }),
                     this);
    g_signal_connect(gesture.get(), "drag-update",
                     G_CALLBACK(+[](GtkGestureDrag*, double dx, double dy, gpointer self) {
                         static_cast<PageReorderGesture*>(self)->update(dx, dy);
                     }),
                     this);
    g_signal_connect(gesture.get(), "drag-end", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer self) {
                         static_cast<PageReorderGesture*>(self)->end();
                     }),
                     this);
    g_signal_connect(gesture.get(), "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer self) {
                         static_cast<PageReorderGesture*>(self)->reset();
                     }),
                     this);

    g_signal_connect(marker.get(), "draw", G_CALLBACK(drawMarker), nullptr);
    // SidebarLayout calls gtk_widget_show_all() on the container
    gtk_widget_set_no_show_all(marker.get(), true);
    gtk_fixed_put(container, marker.get(), 0, 0);
}

PageReorderGesture::~PageReorderGesture() {
    reset();
    if (GtkWidget* parent = gtk_widget_get_parent(marker.get())) {
        gtk_container_remove(GTK_CONTAINER(parent), marker.get());
    }
}

void PageReorderGesture::begin(double x, double y) {
    reset();
    source = entryAt(x, y);
    if (source == npos) {
        gtk_gesture_set_state(gesture.get(), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    startX = x;
    startY = y;
    setPointer(x, y);

    moved = 0;
    touch = xoj::input::modalityOf(gesture.get()) == xoj::input::Modality::Touch;
    if (touch) {
        holdTimeout = g_timeout_add(
                TOUCH_HOLD_MS,
                +[](gpointer self) -> gboolean {
                    auto* reorder = static_cast<PageReorderGesture*>(self);
                    reorder->holdTimeout = 0;
                    reorder->startDragging();
                    return G_SOURCE_REMOVE;
                },
                this);
    }
}

void PageReorderGesture::update(double dx, double dy) {
    if (source == npos) {
        return;
    }
    moved = std::max(moved, std::hypot(dx, dy));
    if (!dragging) {
        const double distance = std::hypot(dx, dy);
        if (touch && distance > TOUCH_SLOP) {
            // A swipe: leave it to the scrolled window
            gtk_gesture_set_state(gesture.get(), GTK_EVENT_SEQUENCE_DENIED);
            reset();
            return;
        }
        if (!touch && distance > DRAG_THRESHOLD) {
            startDragging();
        }
        if (!dragging) {
            return;
        }
    }
    setPointer(startX + dx, startY + dy);
    showMarker(insertionAt(pointerX, startY + dy));
}

void PageReorderGesture::startDragging() {
    if (source == npos || source >= entries.size() || dragging) {
        return;
    }
    dragging = true;
    // Takes the sequence from the miniature button (no click) and the scrolled window (no scrolling)
    gtk_gesture_set_state(gesture.get(), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_set_opacity(entries[source]->getWidget(), 0.4);
    // Drawn above the miniatures, also those added after the marker
    g_object_ref(marker.get());
    gtk_container_remove(GTK_CONTAINER(container), marker.get());
    gtk_fixed_put(container, marker.get(), 0, 0);
    g_object_unref(marker.get());

    auto* adjustment = gtk_scrolled_window_get_vadjustment(scrolledWindow);
    showMarker(insertionAt(pointerX, pointerViewY + gtk_adjustment_get_value(adjustment)));
    scrollTick = gtk_widget_add_tick_callback(
            GTK_WIDGET(container),
            +[](GtkWidget*, GdkFrameClock*, gpointer self) -> gboolean {
                return static_cast<PageReorderGesture*>(self)->autoScroll();
            },
            this, nullptr);
}

void PageReorderGesture::end() {
    if (!dragging) {
        reset();
        return;
    }
    if (touch && moved <= TOUCH_SLOP) {
        // Held and lifted without moving: the context menu, as a long press elsewhere
        const size_t entry = source;
        int x = 0;
        int y = 0;
        if (entry < entries.size()) {
            gtk_widget_translate_coordinates(GTK_WIDGET(container), entries[entry]->getWidget(),
                                             static_cast<int>(startX), static_cast<int>(startY), &x, &y);
        }
        reset();
        if (entry < entries.size() && onHold) {
            onHold(entry, x, y);
        }
        return;
    }
    auto* adjustment = gtk_scrolled_window_get_vadjustment(scrolledWindow);
    const size_t insertion = insertionAt(pointerX, pointerViewY + gtk_adjustment_get_value(adjustment));
    const size_t from = source;
    reset();
    if (from < entries.size() && insertion != npos) {
        const size_t to = insertion > from ? insertion - 1 : insertion;
        if (to != from) {
            onMove(from, to);
        }
    }
}

void PageReorderGesture::reset() {
    if (holdTimeout) {
        g_source_remove(holdTimeout);
        holdTimeout = 0;
    }
    if (scrollTick) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(container), scrollTick);
        scrollTick = 0;
    }
    if (dragging && source < entries.size()) {
        gtk_widget_set_opacity(entries[source]->getWidget(), 1.0);
    }
    gtk_widget_hide(marker.get());
    dragging = false;
    source = npos;
}

auto PageReorderGesture::entryAt(double x, double y) const -> size_t {
    for (size_t i = 0; i < entries.size(); i++) {
        const auto r = rectOf(entries[i]->getWidget(), GTK_WIDGET(container));
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) {
            return i;
        }
    }
    return npos;
}

auto PageReorderGesture::insertionAt(double x, double y) const -> size_t {
    if (entries.empty()) {
        return npos;
    }
    // Several miniatures per row: within a row, the horizontal position decides; otherwise the vertical one
    bool grid = false;
    std::vector<Rect> rects;
    rects.reserve(entries.size());
    for (const auto& entry: entries) {
        rects.push_back(rectOf(entry->getWidget(), GTK_WIDGET(container)));
        grid = grid || rects.back().x > 0;
    }
    for (size_t i = 0; i < rects.size(); i++) {
        const auto& r = rects[i];
        const bool before = grid ? (y > r.y + r.height || (y >= r.y && x > r.x + r.width / 2)) : y > r.y + r.height / 2;
        if (!before) {
            return i;
        }
    }
    return rects.size();
}

void PageReorderGesture::showMarker(size_t insertion) {
    if (insertion == npos) {
        gtk_widget_hide(marker.get());
        return;
    }
    const auto rectAt = [this](size_t i) { return rectOf(entries[i]->getWidget(), GTK_WIDGET(container)); };
    const bool grid = entries.size() > 1 && std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                          return rectOf(entry->getWidget(), GTK_WIDGET(container)).x > 0;
                      });
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (grid) {
        // A vertical line before the miniature, or after the previous one at the end of a row
        const bool afterPrevious = insertion == entries.size() || (insertion > 0 && rectAt(insertion).x == 0);
        const auto r = afterPrevious ? rectAt(insertion - 1) : rectAt(insertion);
        x = static_cast<int>(afterPrevious ? r.x + r.width - MARKER_THICKNESS : r.x);
        y = static_cast<int>(r.y);
        width = MARKER_THICKNESS;
        height = static_cast<int>(r.height);
    } else {
        // A horizontal line between the miniatures
        const auto r = insertion == entries.size() ? rectAt(insertion - 1) : rectAt(insertion);
        y = static_cast<int>(insertion == entries.size() ? r.y + r.height - MARKER_THICKNESS : r.y);
        x = static_cast<int>(r.x);
        width = static_cast<int>(r.width);
        height = MARKER_THICKNESS;
    }
    gtk_widget_set_size_request(marker.get(), width, height);
    gtk_fixed_move(container, marker.get(), x, y);
    gtk_widget_show(marker.get());
}

void PageReorderGesture::setPointer(double x, double y) {
    pointerX = x;
    pointerViewY = y - gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(scrolledWindow));
}

auto PageReorderGesture::autoScroll() -> gboolean {
    auto* adjustment = gtk_scrolled_window_get_vadjustment(scrolledWindow);
    const double pageSize = gtk_adjustment_get_page_size(adjustment);
    double step = 0;
    if (pointerViewY < SCROLL_EDGE) {
        step = -(SCROLL_EDGE - pointerViewY) * SCROLL_SPEED;
    } else if (pointerViewY > pageSize - SCROLL_EDGE) {
        step = (pointerViewY - (pageSize - SCROLL_EDGE)) * SCROLL_SPEED;
    }
    if (step != 0) {
        const double value = gtk_adjustment_get_value(adjustment);
        const double target = std::clamp(value + step, gtk_adjustment_get_lower(adjustment),
                                         gtk_adjustment_get_upper(adjustment) - pageSize);
        if (target != value) {
            gtk_adjustment_set_value(adjustment, target);
            showMarker(insertionAt(pointerX, pointerViewY + target));
        }
    }
    return G_SOURCE_CONTINUE;
}
