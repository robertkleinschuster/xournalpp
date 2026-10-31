/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <fstream>
#include <vector>

#include <config-test.h>
#include <gtest/gtest.h>

#include "control/InsertPdfPages.h"
#include "pdf/base/PdfCombiner.h"

namespace fs = std::filesystem;
using Pages = std::vector<size_t>;

TEST(PdfCombiner, parsesPageRanges) {
    EXPECT_EQ(PdfCombiner::parsePageRanges("", 3), Pages({0, 1, 2}));
    EXPECT_EQ(PdfCombiner::parsePageRanges("  ", 2), Pages({0, 1}));
    EXPECT_EQ(PdfCombiner::parsePageRanges("2", 3), Pages({1}));
    EXPECT_EQ(PdfCombiner::parsePageRanges("1-3, 5", 6), Pages({0, 1, 2, 4}));
    EXPECT_EQ(PdfCombiner::parsePageRanges("3 - 4;1", 6), Pages({2, 3, 0}));
    EXPECT_EQ(PdfCombiner::parsePageRanges("4-", 6), Pages({3, 4, 5}));
    EXPECT_EQ(PdfCombiner::parsePageRanges("2\xE2\x80\x93"
                                           "3",
                                           6),
              Pages({1, 2}));  // en dash
}

TEST(PdfCombiner, rejectsInvalidPageRanges) {
    for (const char* text: {"0", "7", "3-2", "1-9", "a", "1,", ",1", "1--2", "1 2", "-3"}) {
        EXPECT_FALSE(PdfCombiner::parsePageRanges(text, 6)) << text;
    }
    EXPECT_FALSE(PdfCombiner::parsePageRanges("", 0));
}

TEST(PdfCombiner, appendsPagesAndMarksTheResult) {
    if (!PdfCombiner::isAvailable()) {
        GTEST_SKIP() << "built without QPDF";
    }
    const auto dir = fs::temp_directory_path() / u8"xournalpp-combiner-test ä";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto source = dir / u8"Übung.pdf";
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), source);
    const size_t count = PdfCombiner::pageCount(source);
    ASSERT_GT(count, 0U);
    EXPECT_FALSE(PdfCombiner::isCombined(source));

    // Without a background PDF: only the chosen pages, a page twice is fine
    const auto first = dir / "first.pdf";
    std::ofstream(first, std::ios::binary) << PdfCombiner::append({}, source, {0, 0});
    EXPECT_EQ(PdfCombiner::pageCount(first), 2U);
    EXPECT_TRUE(PdfCombiner::isCombined(first));

    // Appending to it keeps its pages in front
    const auto second = dir / "second.pdf";
    std::ofstream(second, std::ios::binary) << PdfCombiner::append(first, source, {count - 1});
    EXPECT_EQ(PdfCombiner::pageCount(second), 3U);
    EXPECT_TRUE(PdfCombiner::isCombined(second));

    EXPECT_THROW(PdfCombiner::append({}, source, {count}), std::runtime_error);
    EXPECT_THROW(PdfCombiner::pageCount(dir / "missing.pdf"), std::runtime_error);
    fs::remove_all(dir);
}

TEST(PdfCombiner, choosesTheFileForTheCombinedPdf) {
    if (!PdfCombiner::isAvailable()) {
        GTEST_SKIP() << "built without QPDF";
    }
    const auto dir = fs::temp_directory_path() / u8"xournalpp-combined-name ä";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto xopp = dir / u8"VO8 Übung.xopp";
    // Without a PDF named like the document, the combined PDF takes that name
    EXPECT_EQ(InsertPdfPages::combinedPdfPath(xopp, {}), dir / u8"VO8 Übung.pdf");
    // The source PDF named like the document stays untouched
    const auto source = dir / u8"VO8 Übung.pdf";
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), source);
    EXPECT_EQ(InsertPdfPages::combinedPdfPath(xopp, source), dir / u8"VO8 Übung (combined).pdf");
    std::ofstream(dir / u8"VO8 Übung (combined).pdf") << "x";
    EXPECT_EQ(InsertPdfPages::combinedPdfPath(xopp, source), dir / u8"VO8 Übung (combined 2).pdf");
    // A combined PDF is reused
    const auto combined = dir / u8"VO8 Übung (combined).pdf";
    std::ofstream(combined, std::ios::binary | std::ios::trunc) << PdfCombiner::append({}, source, {0});
    EXPECT_EQ(InsertPdfPages::combinedPdfPath(xopp, combined), combined);
    fs::remove_all(dir);
}
