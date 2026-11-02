#include <gtest/gtest.h>

#include "gui/inputdevices/ScrollMomentum.h"

using xoj::util::Point;

namespace {
constexpr gint64 QUICK = 120'000;  ///< a flick, in microseconds
}

TEST(ScrollMomentum, swipeWithoutGlidingKeepsItsSpeed) {
    const auto speed = ScrollMomentum::combinedSpeed({0, 0}, {0, -2000}, QUICK);
    EXPECT_DOUBLE_EQ(speed.x, 0);
    EXPECT_DOUBLE_EQ(speed.y, -2000);
}

TEST(ScrollMomentum, quickFlickInTheSameDirectionAddsUp) {
    const auto speed = ScrollMomentum::combinedSpeed({0, -1500}, {100, -2000}, QUICK);
    EXPECT_DOUBLE_EQ(speed.x, 100);
    EXPECT_DOUBLE_EQ(speed.y, -3500);
}

TEST(ScrollMomentum, repeatedFlicksAreLimited) {
    const auto speed = ScrollMomentum::combinedSpeed({0, -9000}, {0, -6000}, QUICK);
    EXPECT_DOUBLE_EQ(speed.y, -12000);
}

TEST(ScrollMomentum, singleFastSwipeIsNotSlowedDown) {
    const auto speed = ScrollMomentum::combinedSpeed({0, -3000}, {0, -14000}, QUICK);
    EXPECT_DOUBLE_EQ(speed.y, -14000);
}

TEST(ScrollMomentum, oppositeDirectionStartsOver) {
    const auto speed = ScrollMomentum::combinedSpeed({0, -3000}, {0, 2000}, QUICK);
    EXPECT_DOUBLE_EQ(speed.y, 2000);
}

TEST(ScrollMomentum, slowDragStartsOver) {
    // Holding and dragging for a while is positioning, not flicking
    const auto speed = ScrollMomentum::combinedSpeed({0, -3000}, {0, -2000}, 600'000);
    EXPECT_DOUBLE_EQ(speed.y, -2000);
}

TEST(ScrollMomentum, tapThatStopsGlidingDoesNotGlide) {
    const auto speed = ScrollMomentum::combinedSpeed({0, -3000}, {0, -50}, QUICK);
    EXPECT_DOUBLE_EQ(speed.y, -50);
}
