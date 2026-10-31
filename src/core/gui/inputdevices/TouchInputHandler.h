/*
 * Xournal++
 *
 * Handle touchscreen zooming and panning.
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <set>

#include <gtk/gtk.h>  // for GdkEventSequence, GtkWidget

#include "util/Point.h"  // for Point

#include "AbstractInputHandler.h"  // for AbstractInputHandler
#include "ScrollMomentum.h"        // for ScrollMomentum

class InputContext;
struct InputEvent;

class TouchInputHandler: public AbstractInputHandler {
private:
    bool zooming = false;
    std::set<GdkEventSequence*> invalidActive;

    GdkEventSequence* primarySequence{};
    GdkEventSequence* secondarySequence{};

    double startZoomDistance = 0.0;
    xoj::util::Point<double> lastZoomScrollCenter{};

    xoj::util::Point<double> priLastAbs{-1.0, -1.0};
    xoj::util::Point<double> secLastAbs{-1.0, -1.0};

    xoj::util::Point<double> priLastRel{-1.0, -1.0};
    xoj::util::Point<double> secLastRel{-1.0, -1.0};

    // True, if a zoom sequence may be started by a motion event.
    bool startZoomReady{false};

    bool canBlockZoom{false};

    /**
     * Fork: touch motion is applied once per frame instead of once per event. A pinch reports each finger in its own
     * event, so applying them one by one zooms around a center where only one finger has moved yet, making the zoom
     * jitter; several events per frame also redo the same layout work.
     */
    xoj::util::Point<double> pendingScroll{};
    bool zoomPending = false;
    guint frameCallbackId = 0;
    /// The canvas widget the frame callback is attached to; reset to nullptr by GLib when it is destroyed
    GtkWidget* frameWidget = nullptr;

    /// Fork: the document glides on after a swipe, like after a two-finger swipe on the touchpad
    ScrollMomentum momentum;
    /// The current two-finger gesture changed the zoom noticeably, so it does not glide on
    bool pinched = false;

private:
    void sequenceStart(InputEvent const& event);
    void scrollMotion(InputEvent const& event);
    void zoomStart();
    void zoomMotion(InputEvent const& event);
    void zoomEnd();

    void requestFrame();
    void cancelFrame();
    /// Applies the motion collected since the last frame
    void applyPendingMotion();
    void applyZoom();

public:
    explicit TouchInputHandler(InputContext* inputContext);
    ~TouchInputHandler() override;

    bool handleImpl(InputEvent const& event) override;
    void onBlock() override;
    void onUnblock() override;

    /// Clear active touch sequences when the application loses its input focus.
    void resetTouchSequence();

private:
    /// Fork: drops touches the system no longer knows about (Windows only), before handling the new touch current
    void forgetEndedTouches(GdkEventSequence* current);
};
