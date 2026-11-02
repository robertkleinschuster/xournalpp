#include "ScrollMomentum.h"

#include <algorithm>  // for min, max
#include <cmath>      // for exp, hypot
#include <utility>    // for move

using xoj::util::Point;

namespace {
/// The speed is measured over the last moments of movement, so a short stop when lifting does not count
constexpr gint64 VELOCITY_WINDOW_US = 100'000;
/// Fingers that rested this long before lifting do not glide on
constexpr gint64 MAX_REST_US = 120'000;
constexpr double TIME_CONSTANT_S = 0.325;
constexpr double MIN_SPEED = 150.0;  ///< pixels per second
constexpr double STOP_SPEED = 20.0;
/// Longest frame step, so the content does not jump after the window was hidden
constexpr double MAX_STEP_S = 0.05;
/// Repeated flicks: only short swipes add to the gliding speed, up to this speed (about 4 A4 pages in one glide)
constexpr gint64 MAX_FLICK_US = 300'000;
constexpr double MAX_SPEED = 12'000.0;
}  // namespace

ScrollMomentum::ScrollMomentum(std::function<void(Point<double>)> scroll): scroll(std::move(scroll)) {}

ScrollMomentum::~ScrollMomentum() { stop(); }

void ScrollMomentum::begin() {
    carried = isGliding() ? speed : Point<double>{0, 0};
    beginTime = g_get_monotonic_time();
    stop();
    forgetMovement();
}

auto ScrollMomentum::combinedSpeed(Point<double> carried, Point<double> swipe, gint64 swipeUs) -> Point<double> {
    const double carriedSpeed = std::hypot(carried.x, carried.y);
    const double swipeSpeed = std::hypot(swipe.x, swipe.y);
    if (swipeUs > MAX_FLICK_US || carriedSpeed <= 0 || swipeSpeed <= MIN_SPEED) {
        return swipe;
    }
    // Same direction: within about 30 degrees
    const double cosine = (carried.x * swipe.x + carried.y * swipe.y) / (carriedSpeed * swipeSpeed);
    if (cosine < 0.87) {
        return swipe;
    }
    Point<double> sum = swipe + carried;
    const double sumSpeed = std::hypot(sum.x, sum.y);
    const double limit = std::max(MAX_SPEED, swipeSpeed);  // never slower than the swipe itself
    return sumSpeed > limit ? sum * (limit / sumSpeed) : sum;
}

void ScrollMomentum::forgetMovement() {
    travelled = {};
    samples.clear();
    lastMoveTime = 0;
}

void ScrollMomentum::addMovement(Point<double> move) {
    if (move.x == 0.0 && move.y == 0.0) {
        return;
    }
    const gint64 now = g_get_monotonic_time();
    travelled += move;
    samples.emplace_back(now, travelled);
    while (!samples.empty() && now - samples.front().first > VELOCITY_WINDOW_US) {
        samples.pop_front();
    }
    lastMoveTime = now;
}

auto ScrollMomentum::velocity() const -> Point<double> {
    if (samples.size() < 2) {
        return {0, 0};
    }
    const auto& [firstTime, firstPosition] = samples.front();
    const auto& [lastTime, lastPosition] = samples.back();
    const double seconds = static_cast<double>(lastTime - firstTime) / 1e6;
    return seconds > 0 ? (lastPosition - firstPosition) / seconds : Point<double>{0, 0};
}

void ScrollMomentum::release(GtkWidget* w) {
    stop();
    const bool recentlyMoved = lastMoveTime && g_get_monotonic_time() - lastMoveTime < MAX_REST_US;
    speed = combinedSpeed(carried, velocity(), g_get_monotonic_time() - beginTime);
    carried = {};
    forgetMovement();
    if (!w || !recentlyMoved || std::hypot(speed.x, speed.y) <= MIN_SPEED) {
        return;
    }
    widget = w;
    g_object_add_weak_pointer(G_OBJECT(widget), reinterpret_cast<gpointer*>(&widget));
    lastFrame = 0;
    tickId = gtk_widget_add_tick_callback(
            widget,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer data) -> gboolean {
                return static_cast<ScrollMomentum*>(data)->step(gdk_frame_clock_get_frame_time(clock));
            },
            this, nullptr);
}

auto ScrollMomentum::step(gint64 frameTime) -> gboolean {
    const double elapsed = lastFrame ? static_cast<double>(frameTime - lastFrame) / 1e6 : 1.0 / 60.0;
    const double seconds = std::min(elapsed, MAX_STEP_S);
    lastFrame = frameTime;
    scroll(speed * seconds);
    speed = speed * std::exp(-seconds / TIME_CONSTANT_S);
    if (std::hypot(speed.x, speed.y) < STOP_SPEED) {
        tickId = 0;  // removed by returning G_SOURCE_REMOVE
        stop();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void ScrollMomentum::stop() {
    if (widget) {
        if (tickId) {
            gtk_widget_remove_tick_callback(widget, tickId);
        }
        g_object_remove_weak_pointer(G_OBJECT(widget), reinterpret_cast<gpointer*>(&widget));
        widget = nullptr;
    }
    tickId = 0;
}
