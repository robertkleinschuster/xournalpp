//
// Created by ulrich on 08.04.19.
//

#include "TouchInputHandler.h"

#include <cmath>  // for abs

#include "control/Control.h"                        // for Control
#include "control/settings/Settings.h"              // for Settings
#include "control/zoom/ZoomControl.h"               // for ZoomControl
#include "gui/Layout.h"                             // for Layout
#include "gui/MainWindow.h"                         // for MainWindow
#include "gui/XournalView.h"                        // for XournalView
#include "gui/inputdevices/AbstractInputHandler.h"  // for AbstractInputHandler
#include "gui/inputdevices/InputEvents.h"           // for InputEvent, BUTTO...

#include "InputContext.h"  // for InputContext

#ifdef _WIN32
#include <windows.h>

namespace {
/// GDK uses the Windows pointer id as touch sequence: ask Windows whether that finger still touches the screen
auto isTouchDown(GdkEventSequence* sequence) -> bool {
    POINTER_INFO info{};
    return GetPointerInfo(static_cast<UINT32>(reinterpret_cast<uintptr_t>(sequence)), &info) &&
           (info.pointerFlags & POINTER_FLAG_INCONTACT);
}
}  // namespace
#endif

TouchInputHandler::TouchInputHandler(InputContext* inputContext): AbstractInputHandler(inputContext) {}

void TouchInputHandler::forgetEndedTouches(GdkEventSequence* current) {
#ifdef _WIN32
    // Fork: Windows sometimes loses the end of a touch, e.g. when palm rejection cancels it. The vanished finger would
    // stay "down" forever, making every new finger the second one of a pinch with a phantom finger, or marking all new
    // touches invalid. See https://github.com/xournalpp/xournalpp/issues/5289
    bool ended = (primarySequence && primarySequence != current && !isTouchDown(primarySequence)) ||
                 (secondarySequence && secondarySequence != current && !isTouchDown(secondarySequence));
    for (auto* sequence: invalidActive) {
        ended = ended || (sequence != current && !isTouchDown(sequence));
    }
    // Only earlier touches are checked: the current one may already be lifted while its events are still queued.
    // A lifted earlier touch whose end event is still queued is forgotten a moment early, which is harmless.
    if (ended) {
        g_message("Touch input: forgetting touches that ended without an end event");
        resetTouchSequence();
    }
#endif
}

auto TouchInputHandler::handleImpl(InputEvent const& event) -> bool {
    if (!event.sequence) {
        // On x11, a GDK_MOTION_EVENT with sequence == nullptr is emitted before TOUCH_BEGIN: Ignore it
        // In general, every valid touch event must have a sequence
        return false;
    }
    bool zoomGesturesEnabled = inputContext->getSettings()->isZoomGesturesEnabled();

    if (event.type == BUTTON_PRESS_EVENT) {
        forgetEndedTouches(event.sequence);
        if (invalidActive.find(event.sequence) != invalidActive.end()) {
            g_warning("Missed touch end/cancel event. Resetting touch input handler.");
            invalidActive.clear();
        }
        if (invalidActive.empty() && event.sequence != primarySequence && event.sequence != secondarySequence) {
            // All touches are previously valid and we did not miss an end/cancel event for the current touch
            if (primarySequence && secondarySequence) {
                // All touches become invalid
                g_debug("All touches become invalid");
                invalidActive.insert({primarySequence, secondarySequence, event.sequence});
                primarySequence = secondarySequence = nullptr;
            } else {
                if (primarySequence) {
                    secondarySequence = event.sequence;
                } else {
                    primarySequence = event.sequence;
                }
                sequenceStart(event);
                if (secondarySequence) {
                    startZoomReady = true;
                }
            }
        } else {
            invalidActive.insert(event.sequence);
            g_debug("Add touch as invalid. %zu touches are invalid now.", invalidActive.size());
        }
        return true;
    }

    if (event.type == MOTION_EVENT) {
        if (primarySequence == event.sequence && !secondarySequence) {
            scrollMotion(event);
            return true;
        } else if (event.sequence && (primarySequence == event.sequence || secondarySequence == event.sequence)) {
            xoj_assert(primarySequence);
            if (zoomGesturesEnabled) {
                if (startZoomReady) {
                    if (this->primarySequence == event.sequence) {
                        sequenceStart(event);
                        zoomStart();
                    }
                } else {
                    zoomMotion(event);
                }
            } else {
                scrollMotion(event);
            }
            return true;
        }
    }

    if (event.type == BUTTON_RELEASE_EVENT) {
        if (zooming && primarySequence && secondarySequence &&
            (event.sequence == primarySequence || event.sequence == secondarySequence)) {
            zoomEnd();
        }

        if (event.sequence == primarySequence) {
            // If secondarySequence is nullptr, this sets primarySequence
            // to nullptr. If it isn't, then it is now the primary sequence!
            primarySequence = std::exchange(secondarySequence, nullptr);

            this->priLastAbs = this->secLastAbs;
            this->priLastRel = this->secLastRel;
        } else if (event.sequence == secondarySequence) {
            secondarySequence = nullptr;
        } else {
            invalidActive.erase(event.sequence);
            g_debug("Removing sequence from invalid list, %zu inputs remain invalid.", invalidActive.size());
        }
        return true;
    }

    return false;
}

void TouchInputHandler::sequenceStart(InputEvent const& event) {
    if (event.sequence == this->primarySequence) {
        this->priLastAbs = event.absolute;
        this->priLastRel = event.relative;
    } else {
        this->secLastAbs = event.absolute;
        this->secLastRel = event.relative;
    }
}

void TouchInputHandler::scrollMotion(InputEvent const& event) {
    // Will only be called if there is a single sequence (zooming handles two sequences)
    auto offset = [&]() {
        if (event.sequence == this->primarySequence) {
            auto offset = event.absolute - this->priLastAbs;
            this->priLastAbs = event.absolute;
            return offset;
        } else {
            auto offset = event.absolute - this->secLastAbs;
            this->secLastAbs = event.absolute;
            return offset;
        }
    }();

    inputContext->getView()->getLayout()->scrollRelative(-offset.x, -offset.y);
}

void TouchInputHandler::zoomStart() {
    this->zooming = true;
    this->startZoomDistance = this->priLastAbs.distance(this->secLastAbs);

    if (this->startZoomDistance == 0.0) {
        this->startZoomDistance = 0.01;
    }

    // Whether we can ignore the zoom portion of the gesture (e.g. distance between touch points
    // hasn't changed enough).
    this->canBlockZoom = true;

    ZoomControl* zoomControl = this->inputContext->getView()->getControl()->getZoomControl();

    // Disable zoom fit as we are zooming currently
    // TODO(fabian): this should happen internally!!!
    if (zoomControl->isZoomFitMode()) {
        zoomControl->setZoomFitMode(false);
    }

    // use screen pixel coordinates for the zoom center
    // as relative coordinates depend on the changing zoom level
    auto center = (this->priLastAbs + this->secLastAbs) / 2.0;
    this->lastZoomScrollCenter = center;

    zoomControl->startZoomSequence(center);

    this->startZoomReady = false;
}

void TouchInputHandler::zoomMotion(InputEvent const& event) {
    if (event.sequence == this->primarySequence) {
        this->priLastAbs = event.absolute;
    } else {
        this->secLastAbs = event.absolute;
    }

    double distance = this->priLastAbs.distance(this->secLastAbs);
    double zoom = distance / this->startZoomDistance;

    double zoomTriggerThreshold = inputContext->getSettings()->getTouchZoomStartThreshold();
    double zoomChangePercentage = std::abs(distance - startZoomDistance) / startZoomDistance * 100;

    // Has the touch points moved far enough to trigger a zoom?
    if (this->canBlockZoom && zoomChangePercentage < zoomTriggerThreshold) {
        zoom = 1.0;
    } else {
        // Touches have moved far enough from their initial location that we
        // no longer prevent touchscreen zooming.
        this->canBlockZoom = false;
    }

    ZoomControl* zoomControl = this->inputContext->getView()->getControl()->getZoomControl();
    const auto center = (this->priLastAbs + this->secLastAbs) / 2;
    zoomControl->zoomSequenceChange(zoom, true, center - lastZoomScrollCenter);
    lastZoomScrollCenter = center;
}

void TouchInputHandler::zoomEnd() {
    this->zooming = false;
    ZoomControl* zoomControl = this->inputContext->getView()->getControl()->getZoomControl();
    zoomControl->endZoomSequence();
}

void TouchInputHandler::onBlock() {
    resetTouchSequence();
}

void TouchInputHandler::onUnblock() { resetTouchSequence(); }

void TouchInputHandler::resetTouchSequence() {
    if (this->zooming) {
        zoomEnd();
    }
    this->invalidActive.clear();
    this->primarySequence = nullptr;
    this->secondarySequence = nullptr;

    this->startZoomDistance = 0.0;
    this->lastZoomScrollCenter = {};
    this->startZoomReady = false;
    this->canBlockZoom = false;

    priLastAbs = {-1.0, -1.0};
    secLastAbs = {-1.0, -1.0};
    priLastRel = {-1.0, -1.0};
    secLastRel = {-1.0, -1.0};
}
