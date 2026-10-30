#include "ZoomControl.h"

#include <algorithm>  // for find, max, clamp
#include <utility>    // for swap
#include <cmath>      // for isfinite

#include <glib-object.h>  // for G_CALLBACK, g_signal_connect
#include <glib.h>         // for g_warning, guint

#include "control/Control.h"  // for Control
#include "control/actions/ActionDatabase.h"
#include "control/settings/Settings.h"  // for Settings
#include "control/zoom/ZoomListener.h"  // for ZoomListener
#include "gui/Layout.h"                 // for Layout
#include "gui/MainWindow.h"
#include "gui/PageView.h"               // for XojPageView
#include "gui/XournalView.h"            // for XournalView
#include "control/zoom/TouchpadGestures.h"  // for TouchpadGestures
#include "gui/scroll/ScrollHandling.h"  // for ScrollHandling
#include "util/Assert.h"                // for xoj_assert
#include "util/Util.h"                  // for execInUiThread
#include "util/gdk4_helper.h"           // for gdk_event_get_modifier_state
#include "util/glib_casts.h"            // for wrap_for_g_callback

using xoj::util::Rectangle;

ZoomControl::ZoomControl() = default;
ZoomControl::~ZoomControl() = default;

auto onScrolledwindowMainScrollEvent(GtkWidget* widget, GdkEventScroll* event, ZoomControl* zoom) -> bool {
    auto state =
            gdk_event_get_modifier_state(reinterpret_cast<GdkEvent*>(event)) & gtk_accelerator_get_default_mod_mask();

    // do not handle e.g. ALT + Scroll (e.g. Compiz use this shortcut for setting transparency...)
    if (state & ~(GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        return true;
    }

    // Fork: wheel events Windows synthesizes for a two-finger touchpad gesture, which is handled from the fingers
    if (event->direction == GDK_SCROLL_SMOOTH && zoom->touchpadGestures) {
        const bool zoomStep = state & GDK_CONTROL_MASK;
        if (zoom->touchpadGestures->consumeSynthesizedScroll(zoomStep ? 0.0 : event->delta_x,
                                                             zoomStep ? 0.0 : event->delta_y)) {
            return true;
        }
    }

    if (state & GDK_CONTROL_MASK) {
        xoj::util::Point<double> center;
        gdk_event_get_coords((GdkEvent*)event, &center.x, &center.y);
        if (event->direction == GDK_SCROLL_SMOOTH) {
            // Fork: zoom proportionally to the scrolled amount (one wheel notch = one zoom step), so fine-grained
            // devices do not zoom a full step per event; a purely horizontal scroll does not zoom at all
            if (event->delta_y != 0.0) {
                zoom->zoomScrollBy(-event->delta_y, center);
            }
            return true;
        }
        auto direction = event->direction == GDK_SCROLL_UP ? ZOOM_IN : ZOOM_OUT;
        zoom->zoomScroll(direction, center);
        return true;
    }

    // TODO(unknown): Disabling scroll here is maybe a bit hacky find a better way
    if (zoom->isZoomPresentationMode()) {
        return true;
    }
    if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx = event->delta_x;
        double dy = event->delta_y;
        if ((state & GDK_SHIFT_MASK) && dx == 0.0) {
            std::swap(dx, dy);  // Shift + wheel scrolls horizontally
        }
        return zoom->scrollBy(dx, dy, event->time);
    }
    return false;
}

auto onTouchpadPinchEvent(GtkWidget* widget, GdkEventTouchpadPinch* event, ZoomControl* zoom) -> bool {
    if (event->type == GDK_TOUCHPAD_PINCH && event->n_fingers == 2) {
        switch (event->phase) {
            case GDK_TOUCHPAD_GESTURE_PHASE_BEGIN: {
                if (zoom->isZoomFitMode()) {
                    zoom->setZoomFitMode(false);
                }
                xoj::util::Point<double> center;
                gdk_event_get_coords((GdkEvent*)event, &center.x, &center.y);
                zoom->startZoomSequence(center);
                break;
            }
            case GDK_TOUCHPAD_GESTURE_PHASE_UPDATE:
                zoom->zoomSequenceChange(event->scale, true);
                break;
            case GDK_TOUCHPAD_GESTURE_PHASE_END:
                zoom->endZoomSequence();
                break;
            case GDK_TOUCHPAD_GESTURE_PHASE_CANCEL:
                zoom->cancelZoomSequence();
                break;
        }
        return true;
    }
    return false;
}

auto ZoomControl::withZoomStep(ZoomDirection direction, double zoomStep) const -> double {
    double multiplier = 1.0 + zoomStep;
    double newZoom;

    if (direction == ZOOM_IN) {
        newZoom = this->zoom * multiplier;
    } else {
        newZoom = this->zoom / multiplier;
    }

    return newZoom;
}

void ZoomControl::zoomOneStep(ZoomDirection direction, xoj::util::Point<double> zoomCenter) {
    if (this->zoomPresentationMode) {
        return;
    }
    this->setZoomFitMode(false);

    double newZoom = this->withZoomStep(direction, this->zoomStep);

    startZoomSequence(zoomCenter);
    this->zoomSequenceChange(newZoom, false);
    endZoomSequence();
}

void ZoomControl::zoomOneStep(ZoomDirection direction) {
    Rectangle rect = getVisibleRect();
    zoomOneStep(direction, {rect.width / 2.0, rect.height / 2.0});
}


void ZoomControl::zoomScroll(ZoomDirection direction, xoj::util::Point<double> zoomCenter) {
    if (this->zoomPresentationMode) {
        return;
    }

    if (this->isZoomFitMode()) {
        this->setZoomFitMode(false);
    }

    double newZoom = this->withZoomStep(direction, this->zoomStepScroll);
    startZoomSequence(zoomCenter);
    this->zoomSequenceChange(newZoom, false);
    endZoomSequence();
}

void ZoomControl::zoomScrollBy(double steps, xoj::util::Point<double> zoomCenter) {
    if (this->zoomPresentationMode) {
        return;
    }
    if (this->isZoomFitMode()) {
        this->setZoomFitMode(false);
    }
    // Fork: Windows reports touchpad pinching as bursts of Ctrl + wheel steps. Glide towards the target zoom on the
    // frame clock instead of jumping per step, keeping the zoom center of the burst.
    if (!scrollZoomTick) {
        if (isZoomSequenceActive()) {
            return;  // another gesture (e.g. a pinch on the touchscreen) is zooming
        }
        startZoomSequence(zoomCenter);
        scrollZoomSequence = this->zoomSequenceGeneration;
        scrollZoomTarget = this->zoom;
        scrollZoomApplied = this->zoom;
        scrollZoomLastFrame = 0;
        scrollZoomTick = gtk_widget_add_tick_callback(
                view->getWidget(),
                +[](GtkWidget*, GdkFrameClock* clock, gpointer data) -> gboolean {
                    return static_cast<ZoomControl*>(data)->scrollZoomStep(gdk_frame_clock_get_frame_time(clock));
                },
                this, nullptr);
    }
    scrollZoomTarget = std::clamp(scrollZoomTarget * std::pow(1.0 + this->zoomStepScroll, steps), zoomMin, zoomMax);
}

auto ZoomControl::scrollZoomStep(gint64 frameTime) -> bool {
    // Someone else changed the zoom meanwhile (e.g. zoom to fit): stop gliding
    if (this->zoom != scrollZoomApplied || scrollZoomSequence != this->zoomSequenceGeneration) {
        scrollZoomTick = 0;
        // Only end the sequence if it is still the one started here, not one of a gesture that took over
        if (scrollZoomSequence == this->zoomSequenceGeneration && isZoomSequenceActive()) {
            endZoomSequence();
        }
        return G_SOURCE_REMOVE;
    }
    // Move a fixed share of the remaining (logarithmic) distance per time, independent of the frame rate
    constexpr double TIME_CONSTANT_US = 30'000.0;
    const double elapsed = scrollZoomLastFrame ? static_cast<double>(frameTime - scrollZoomLastFrame) : 16'000.0;
    scrollZoomLastFrame = frameTime;
    const double share = 1.0 - std::exp(-elapsed / TIME_CONSTANT_US);
    const double remaining = std::log(scrollZoomTarget / this->zoom);
    const bool done = std::abs(remaining) < 0.001;
    const double next = done ? scrollZoomTarget : this->zoom * std::exp(remaining * share);
    zoomSequenceChange(next, false);
    scrollZoomApplied = this->zoom;
    if (done) {
        scrollZoomTick = 0;
        endZoomSequence();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

auto ZoomControl::scrollBy(double dx, double dy, guint32 time) -> bool {
    // Fork: scroll a fixed distance per wheel unit (35 px per notch at 100 % display scale), instead of
    // GTK's distance that grows with the window size. Precision touchpads deliver fine fractions of a notch.
    // A new gesture starts after a short pause; it is locked to its main axis once that is clear, so slightly
    // diagonal two-finger swipes do not drift sideways.
    constexpr guint32 GESTURE_GAP_MS = 150;
    constexpr double AXIS_DECISION_UNITS = 0.25;
    if (time - scrollLastTime > GESTURE_GAP_MS) {
        scrollAxis = ScrollAxis::UNDECIDED;
        scrollGestureX = 0;
        scrollGestureY = 0;
    }
    scrollLastTime = time;
    if (scrollAxis == ScrollAxis::UNDECIDED) {
        scrollGestureX += std::abs(dx);
        scrollGestureY += std::abs(dy);
        if (scrollGestureX + scrollGestureY >= AXIS_DECISION_UNITS) {
            scrollAxis = scrollGestureY >= 2 * scrollGestureX ? ScrollAxis::VERTICAL :
                         scrollGestureX >= 2 * scrollGestureY ? ScrollAxis::HORIZONTAL :
                                                                ScrollAxis::FREE;
        }
    }
    if (scrollAxis == ScrollAxis::VERTICAL) {
        dx = 0;
    } else if (scrollAxis == ScrollAxis::HORIZONTAL) {
        dy = 0;
    }

    double resolution = gdk_screen_get_resolution(gtk_widget_get_screen(view->getWidget()));
    if (resolution <= 0) {
        resolution = 96.0;
    }
    // About 1.5 times the finger movement on precision touchpads, which report roughly 2 units per cm
    const double pixelsPerUnit = 35.0 * resolution / 96.0;
    view->getLayout()->scrollRelative(dx * pixelsPerUnit, dy * pixelsPerUnit);
    return true;
}

void ZoomControl::startZoomSequence() {
    Rectangle rect = getVisibleRect();
    startZoomSequence({rect.width / 2.0, rect.height / 2.0});
}

void ZoomControl::startZoomSequence(xoj::util::Point<double> zoomCenter) {
    this->zoomSequenceGeneration++;  // Fork: lets the gliding scroll zoom tell whether the sequence is still its own
    // * set zoom center and zoom startlevel
    this->zoomWidgetPos = zoomCenter;  // widget space coordinates of the zoomCenter!
    this->zoomSequenceStart = this->zoom;

    auto rect = getVisibleRect();
    auto view_pos = xoj::util::Point{rect.x, rect.y};

    // Not everything changes size as we zoom in/out: the padding remains constant
    auto fixedPadding = view->getLayout()->getFixedPaddingBeforePoint(view_pos + this->zoomWidgetPos);
    this->unscaledPixels = {static_cast<double>(fixedPadding.x), static_cast<double>(fixedPadding.y)};

    // Except the padding added to center the page when zoomed out
    auto centeringPadding = view->getLayout()->getCenteringPadding();

    // Use this->zoomWidgetPos to zoom into a location other than the top-left (e.g. where the user pinched).
    this->scrollPosition = (view_pos + this->zoomWidgetPos - this->unscaledPixels -
                            xoj::util::Point<double>(centeringPadding.x, centeringPadding.y)) /
                           this->zoom;
}

void ZoomControl::zoomSequenceChange(double zoom, bool relative) {
    if (relative) {
        if (isZoomSequenceActive()) {
            zoom *= zoomSequenceStart;
        } else {
            zoom *= this->zoom;
        }
    }

    setZoom(zoom);
}

void ZoomControl::zoomSequenceChange(double zoom, bool relative, xoj::util::Point<double> scrollVector) {
    if (relative) {
        if (isZoomSequenceActive()) {
            zoom *= zoomSequenceStart;
        } else {
            zoom *= this->zoom;
        }
    }

    // scroll update
    this->zoomWidgetPos += scrollVector;

    setZoom(zoom);
}

void ZoomControl::endZoomSequence() {
    scrollPosition = {-1, -1};
    zoomSequenceStart = -1;
}

void ZoomControl::cancelZoomSequence() {
    if (isZoomSequenceActive()) {
        setZoom(zoomSequenceStart);
        endZoomSequence();
    }
}

auto ZoomControl::isZoomSequenceActive() const -> bool { return zoomSequenceStart != -1; }

auto ZoomControl::getVisibleRect() -> Rectangle<double> { return view->getLayout()->getVisibleRect(); }

auto ZoomControl::getScrollPositionAfterZoom() const -> xoj::util::Point<double> {
    //  If we aren't in a zoomSequence, `unscaledPixels`, `scrollPosition`, and `zoomWidgetPos
    // can't be used to determine the scroll position! Return now.
    // NOTE: this case should never happen currently.
    //       getScrollPositionAfterZoom is called from XournalView after setZoom() fired the ZoomListeners
    if (!this->isZoomSequenceActive()) {
        xoj_assert_message(false, "ZoomControl::getScrollPositionAfterZoom() was called outside of a zoom sequence.");
        return {0, 0};
    }

    auto centeringPadding = view->getLayout()->getCenteringPadding();
    return this->scrollPosition * this->zoom - this->zoomWidgetPos + this->unscaledPixels +
           xoj::util::Point<double>(centeringPadding.x, centeringPadding.y);
}

void ZoomControl::addZoomListener(ZoomListener* l) { this->listener.emplace_back(l); }

void ZoomControl::removeZoomListener(ZoomListener* l) {
    if (auto it = std::find(this->listener.begin(), this->listener.end(), l); it != this->listener.end()) {
        this->listener.erase(it);
    }
}

void ZoomControl::initZoomHandler(GtkWidget* window, GtkWidget* widget, XournalView* v, Control* c) {
    this->control = c;
    this->view = v;
    gtk_widget_add_events(widget, GDK_TOUCHPAD_GESTURE_MASK);
    g_signal_connect(widget, "scroll-event", xoj::util::wrap_for_g_callback_v<onScrolledwindowMainScrollEvent>, this);
    g_signal_connect(widget, "event", xoj::util::wrap_for_g_callback_v<onTouchpadPinchEvent>, this);
    this->touchpadGestures = std::make_unique<TouchpadGestures>(this, v, window);
    g_signal_connect(v->getScrollHandling()->getHorizontal(), "notify::page-size",
                     G_CALLBACK(+[](GObject*, GParamSpec*, gpointer self) {
                         static_cast<ZoomControl*>(self)->updateZoomFitValue();
                         static_cast<ZoomControl*>(self)->updateZoomPresentationValue();
                     }),
                     this);
    g_signal_connect(v->getScrollHandling()->getVertical(), "notify::page-size",
                     G_CALLBACK(+[](GObject*, GParamSpec*, gpointer self) {
                         static_cast<ZoomControl*>(self)->updateZoomPresentationValue();
                     }),
                     this);

    registerListener(this->control);
}

void ZoomControl::fireZoomChanged() {
    for (ZoomListener* z: this->listener) {
        z->zoomChanged();
    }
}

void ZoomControl::fireZoomRangeValueChanged() {
    for (ZoomListener* z: this->listener) {
        z->zoomRangeValuesChanged();
    }
}

auto ZoomControl::getZoom() const -> double { return this->zoom; }

auto ZoomControl::getZoomReal() const -> double { return this->zoom / this->zoom100Value; }

void ZoomControl::setZoom(double zoomI) {
    zoomI = std::clamp(zoomI, this->zoomMin, this->zoomMax);
    if (this->zoom == zoomI) {
        return;
    }
    this->zoom = zoomI;
    this->control->getActionDatabase()->setActionState(Action::ZOOM, getZoomReal());
    fireZoomChanged();
}

void ZoomControl::setZoom100Value(double zoom100Val) {
    // Reject invalid values: they would zero out zoomMin/zoomMax and poison paste/selection math with NaNs/Infs
    // (e.g. corrupted displayDpi settings, or bogus monitor metrics on Windows)
    if (!std::isfinite(zoom100Val) || zoom100Val <= 0.0) {
        g_warning("Ignoring invalid zoom100Value: %f", zoom100Val);
        return;
    }
    // zoom100Value = DPI / Util::DPI_NORMALIZATION_FACTOR, so these bounds mirror the [36, 720] DPI range the
    // automatic detection clamps to in xoj::util::gtk::getWidgetDPI() (36 / 72 = 0.5, 720 / 72 = 10.0). They also
    // contain the manual calibration slider's range [50, 400] DPI (see the adjustmentZoom in settings.glade).
    zoom100Val = std::clamp(zoom100Val, 0.5, 10.0);
    auto setWithRelZoom = [zoomOld = this->zoom100Value, zoom100Val](double& val) { val = val / zoomOld * zoom100Val; };
    setWithRelZoom(this->zoomMax);
    setWithRelZoom(this->zoomMin);
    this->zoom100Value = zoom100Val;
    fireZoomRangeValueChanged();
}

auto ZoomControl::updateZoomFitValue(size_t pageNo) -> bool {
    if (pageNo == 0) {
        pageNo = view->getCurrentPage();
    }
    XojPageView* page = view->getViewFor(pageNo);
    if (!page) {  // no page
        return false;
    }

    Rectangle widget_rect = getVisibleRect();
    double zoom_fit_width = widget_rect.width / (page->getWidth() + 20.0);

    this->zoomFitValue = std::clamp(zoom_fit_width, this->zoomMin, this->zoomMax);
    fireZoomRangeValueChanged();
    if (this->isZoomFitMode() && !this->zoomPresentationMode) {
        this->zoomFit();
    }
    return true;
}

auto ZoomControl::getZoomFitValue() const -> double { return this->zoomFitValue; }

auto ZoomControl::updateZoomPresentationValue(size_t pageNo) -> bool {
    XojPageView* page = view->getViewFor(view->getCurrentPage());
    if (!page) {
        if (!view->getViewPages().empty()) {
            g_warning("Cannot update zoomPresentationValue: no page view for the current page.");
        }
        return false;
    }

    Rectangle widget_rect = getVisibleRect();
    double zoom_fit_width = widget_rect.width / (page->getWidth() + 14.0);
    double zoom_fit_height = widget_rect.height / (page->getHeight() + 14.0);
    double zoom_presentation = zoom_fit_width < zoom_fit_height ? zoom_fit_width : zoom_fit_height;

    this->zoomPresentationValue = std::clamp(zoom_presentation, this->zoomMin, this->zoomMax);
    if (this->zoomPresentationMode) {
        this->zoomPresentation();
    }
    return true;
}

auto ZoomControl::getZoom100Value() const -> double { return this->zoom100Value; }

void ZoomControl::zoom100() {
    if (this->zoomPresentationMode) {
        return;
    }

    if (this->zoomFitMode) {
        this->setZoomFitMode(false);
    }

    startZoomSequence();
    this->zoomSequenceChange(this->zoom100Value, false);
    endZoomSequence();
}

void ZoomControl::zoomFit() {
    // TODO: properly fix the zoom fit infinite recursion
    if (this->isZoomFittingNow) {
        return;
    }

    this->isZoomFittingNow = true;
    if (this->isZoomFitMode() && !this->zoomPresentationMode && this->zoom != this->zoomFitValue) {
        startZoomSequence();
        this->zoomSequenceChange(this->zoomFitValue, false);
        endZoomSequence();
    }
    this->isZoomFittingNow = false;
}

void ZoomControl::zoomPresentation() {
    if (this->zoomPresentationMode && this->zoom != this->zoomPresentationValue) {
        startZoomSequence();
        this->zoomSequenceChange(this->zoomPresentationValue, false);
        endZoomSequence();
    }
}

void ZoomControl::setZoomFitMode(bool isZoomFitMode) {
    if (this->zoomFitMode != isZoomFitMode) {
        this->zoomFitMode = isZoomFitMode;
        this->control->getActionDatabase()->setActionState(Action::ZOOM_FIT, this->zoomFitMode);
    }

    if (this->isZoomFitMode()) {
        zoomFit();
    }
}

auto ZoomControl::isZoomFitMode() const -> bool {
    // Todo(fabian): Remove this fix and make both modes possible in parallel, after fixing the infinite loop.
    //  Explanation: First of all, zoomFit never worked with paired pages. Also, using both resulted in a stackoverflow
    //               when different page sizes are used. This is caused by a logical loop.
    //               We decided to deactivate it in PR#2821 & I#2770. instead of fixing it, to get release 1.1.0 ready.
    //               Zoom presentation mode is also excluded, because it was never intended to work together.
    //               It is also excluded everywhere else (duplicate code).
    auto infiniteLoopFixup = !this->zoomPresentationMode && !this->control->getSettings()->isShowPairedPages();
    return this->zoomFitMode && infiniteLoopFixup;
}

void ZoomControl::setZoomPresentationMode(bool isZoomPresentationMode) {
    this->zoomPresentationMode = isZoomPresentationMode;

    if (isZoomPresentationMode) {
        zoomPresentation();
    }
}

auto ZoomControl::isZoomPresentationMode() const -> bool { return this->zoomPresentationMode; }

void ZoomControl::setZoomStep(double zoomStep) { this->zoomStep = zoomStep; }

void ZoomControl::setZoomStepScroll(double zoomStep) { this->zoomStepScroll = zoomStep; }

void ZoomControl::pageSizeChanged(size_t page) {
    updateZoomPresentationValue(page);
    updateZoomFitValue(page);
}

void ZoomControl::pageSelected(size_t page) {
    // Todo (fabian): page selected should do nothing here, since Zoom Controls, which page is selected.
    //                This results in a logical loop. See PR#2821 & I#2770
    if (current_page != page) {
        this->last_page = this->current_page;
        this->current_page = page;
    }

    updateZoomPresentationValue(this->current_page);
    if (view->isPageVisible(this->last_page, nullptr)) {
        return;
    }
    updateZoomFitValue(this->current_page);
}
