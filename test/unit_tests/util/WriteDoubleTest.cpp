/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <cmath>
#include <limits>
#include <random>
#include <string>

#include <glib.h>
#include <gtest/gtest.h>

#include "util/OutputStream.h"
#include "util/Util.h"

namespace {
class StringOutputStream: public OutputStream {
public:
    void write(const char* data, size_t len) override { text.append(data, len); }
    void close() override {}
    std::string text;
};

/// The reference: how documents were written before (glib, C locale)
auto glibFormat(double value) -> std::string {
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, G_ASCII_DTOSTR_BUF_SIZE, Util::PRECISION_FORMAT_STRING, value);
    return buffer;
}

auto fastFormat(double value) -> std::string {
    StringOutputStream out;
    Util::writeDouble(&out, value);
    return out.text;
}
}  // namespace

TEST(WriteDouble, matchesGlibForEdgeCases) {
    for (double value: {0.0, -0.0, 1.0, -1.0, 0.5, 123.456789012345, 595.27559055, 841.88976378, 1e-5, 1e-4, 1e7, 1e8,
                        1.23456785e8, 99999999.5, 0.000123456789, 2.5e-300, 1.7976931348623157e308,
                        std::numeric_limits<double>::denorm_min(), 1.0 / 3.0}) {
        EXPECT_EQ(fastFormat(value), glibFormat(value)) << "value " << value;
    }
}

TEST(WriteDouble, matchesGlibForRandomValues) {
    std::mt19937_64 random(42);
    std::uniform_real_distribution<double> mantissa(-10.0, 10.0);
    std::uniform_int_distribution<int> exponent(-12, 12);
    for (int i = 0; i < 200000; ++i) {
        const double value = mantissa(random) * std::pow(10.0, exponent(random));
        ASSERT_EQ(fastFormat(value), glibFormat(value)) << "value " << value;
    }
}

TEST(WriteDouble, matchesGlibForTypicalCoordinates) {
    // Stroke coordinates and pressure widths as they occur in documents
    std::mt19937_64 random(7);
    std::uniform_real_distribution<double> coordinate(0.0, 2000.0);
    for (int i = 0; i < 200000; ++i) {
        const double value = coordinate(random);
        ASSERT_EQ(fastFormat(value), glibFormat(value)) << "value " << value;
    }
}
