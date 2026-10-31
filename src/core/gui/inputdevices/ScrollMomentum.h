/*
 * Xournal++
 *
 * Lets the document glide on after a swipe
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <deque>       // for deque
#include <functional>  // for function
#include <utility>     // for pair

#include <gtk/gtk.h>  // for GtkWidget

#include "util/Point.h"  // for Point

/**
 * Fork: the momentum shared by touchscreen and touchpad gestures. The gesture reports the content movement it applied;
 * when the fingers are lifted while still moving fast enough, the content keeps moving and slows down exponentially,
 * like scrolling on phones and in browsers.
 */
class ScrollMomentum final {
public:
    /// @param scroll Moves the content by the given amount (in widget pixels, in the direction the content moves)
    explicit ScrollMomentum(std::function<void(xoj::util::Point<double>)> scroll);
    ~ScrollMomentum();

    ScrollMomentum(const ScrollMomentum&) = delete;
    ScrollMomentum& operator=(const ScrollMomentum&) = delete;

    /// A new gesture starts: stops the gliding and forgets the previous movement
    void begin();
    /// The gesture moved the content by this amount
    void addMovement(xoj::util::Point<double> move);
    /// Forgets the movement so far, e.g. after a pinch, which must not glide on
    void forgetMovement();
    /// The fingers were lifted: glides on with the speed of the last moments, if they still moved fast enough
    void release(GtkWidget* widget);
    void stop();

    bool isGliding() const { return tickId != 0; }

private:
    auto velocity() const -> xoj::util::Point<double>;
    auto step(gint64 frameTime) -> gboolean;

    std::function<void(xoj::util::Point<double>)> scroll;

    xoj::util::Point<double> travelled{};                             ///< content movement in this gesture
    std::deque<std::pair<gint64, xoj::util::Point<double>>> samples;  ///< recent (time, travelled)
    gint64 lastMoveTime = 0;

    xoj::util::Point<double> speed{};  ///< pixels per second while gliding
    gint64 lastFrame = 0;
    guint tickId = 0;
    GtkWidget* widget = nullptr;  ///< weak pointer, the widget whose frame clock drives the gliding
};
