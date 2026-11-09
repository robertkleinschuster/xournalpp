#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "control/shaperecognizer/ShapeRecognizer.h"
#include "model/Point.h"
#include "model/Stroke.h"

namespace {
/// A slightly wavy stroke from (x1, y1) to (x2, y2), as a hand draws it
void addWavyLine(Stroke& s, double x1, double y1, double x2, double y2, int steps = 40) {
    const double len = std::hypot(x2 - x1, y2 - y1);
    const double nx = -(y2 - y1) / len;
    const double ny = (x2 - x1) / len;
    for (int i = 0; i <= steps; i++) {
        const double t = static_cast<double>(i) / steps;
        const double wobble = 0.6 * std::sin(t * 9.0);
        s.addPoint(Point(x1 + t * (x2 - x1) + wobble * nx, y1 + t * (y2 - y1) + wobble * ny));
    }
}

auto recognize(Stroke& s) -> std::unique_ptr<Stroke> {
    ShapeRecognizer reco;
    return reco.recognizePatterns(&s, 10.0);
}
}  // namespace

TEST(ShapeRecognizer, lineEndsWhereThePenLandedAndLifted) {
    Stroke s;
    s.setWidth(1.0);
    addWavyLine(s, 10, 20, 210, 95);
    auto line = recognize(s);
    ASSERT_NE(line, nullptr);
    ASSERT_EQ(line->getPointCount(), 2u);
    EXPECT_DOUBLE_EQ(line->getPoint(0).x, s.getPoint(0).x);
    EXPECT_DOUBLE_EQ(line->getPoint(0).y, s.getPoint(0).y);
    EXPECT_DOUBLE_EQ(line->getPoint(1).x, s.getPointVector().back().x);
    EXPECT_DOUBLE_EQ(line->getPoint(1).y, s.getPointVector().back().y);
}

TEST(ShapeRecognizer, nearlyLevelLineSnapsWithoutMovingItsStart) {
    Stroke s;
    s.setWidth(1.0);
    addWavyLine(s, 10, 50, 210, 52);
    auto line = recognize(s);
    ASSERT_NE(line, nullptr);
    EXPECT_DOUBLE_EQ(line->getPoint(0).x, s.getPoint(0).x);
    EXPECT_DOUBLE_EQ(line->getPoint(0).y, s.getPoint(0).y);
    EXPECT_DOUBLE_EQ(line->getPoint(1).y, line->getPoint(0).y);
    EXPECT_DOUBLE_EQ(line->getPoint(1).x, s.getPointVector().back().x);
}

TEST(ShapeRecognizer, slantedLineIsNotSnappedFarFromItsEnd) {
    Stroke s;
    s.setWidth(1.0);
    // 4 degrees over 300 pt: snapping would move the end by 21 pt
    addWavyLine(s, 10, 50, 10 + 300 * std::cos(0.07), 50 + 300 * std::sin(0.07));
    auto line = recognize(s);
    ASSERT_NE(line, nullptr);
    EXPECT_DOUBLE_EQ(line->getPoint(1).y, s.getPointVector().back().y);
}

TEST(ShapeRecognizer, hookAtTheEndIsIgnored) {
    Stroke s;
    s.setWidth(1.0);
    addWavyLine(s, 10, 20, 210, 20);
    // The pen slides down a little when it is lifted
    for (int i = 1; i <= 6; i++) {
        s.addPoint(Point(210 - i * 0.5, 20 + i * 1.5));
    }
    auto line = recognize(s);
    ASSERT_NE(line, nullptr);
    ASSERT_EQ(line->getPointCount(), 2u);
    EXPECT_DOUBLE_EQ(line->getPoint(0).x, 10);
    EXPECT_NEAR(line->getPoint(1).x, 210, 1.0);
    EXPECT_NEAR(line->getPoint(1).y, 20, 1.0);
}
