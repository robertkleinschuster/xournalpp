/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#include <config-test.h>
#include <gtest/gtest.h>

#include "control/tools/PdfElemSelection.h"
#include "model/TexImage.h"
#include "pdf/base/XojPdfDocument.h"
#include "util/serializing/BinObjectEncoding.h"
#include "util/serializing/ObjectInputStream.h"
#include "util/serializing/ObjectOutputStream.h"
#include "view/View.h"

#include "filesystem.h"

namespace {
XojPdfPageSPtr loadTestPage() {
    static XojPdfDocument doc;
    if (doc.getPageCount() == 0) {
        EXPECT_TRUE(doc.load(fs::path(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf")), "", nullptr));
    }
    return doc.getPage(0);
}

/// Number of pixels that are not transparent when the element is drawn at its position
int countPaintedPixels(const TexImage& image) {
    const auto box = image.getBoundingBox();
    cairo_surface_t* surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(box.width), static_cast<int>(box.height));
    cairo_t* cr = cairo_create(surface);
    cairo_translate(cr, -box.x, -box.y);
    xoj::view::ElementView::createFromElement(&image)->draw(xoj::view::Context::createDefault(cr));
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    int painted = 0;
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    for (int y = 0; y < cairo_image_surface_get_height(surface); y++) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + y * stride);
        for (int x = 0; x < cairo_image_surface_get_width(surface); x++) {
            painted += (row[x] >> 24) != 0;
        }
    }
    cairo_surface_destroy(surface);
    return painted;
}
}  // namespace

TEST(PdfAreaCopy, keepsAreaSizeAndPosition) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);

    auto image = PdfElemSelection::createAreaImage(*page, {30, 40, 200, 120});
    ASSERT_TRUE(image);
    EXPECT_FALSE(image->hasLatexSource());

    const auto box = image->getBoundingBox();
    EXPECT_NEAR(box.x, 30, 1e-6);
    EXPECT_NEAR(box.y, 40, 1e-6);
    EXPECT_NEAR(box.width, 200, 1e-6);
    EXPECT_NEAR(box.height, 120, 1e-6);
}

TEST(PdfAreaCopy, emptyAreaIsNotCopied) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);

    EXPECT_FALSE(PdfElemSelection::createAreaImage(*page, {30, 40, 0, 120}));
    EXPECT_FALSE(PdfElemSelection::createAreaImage(*page, {30, 40, 200, 0.5}));
}

TEST(PdfAreaCopy, containsThePageContent) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);

    auto image = PdfElemSelection::createAreaImage(*page, {0, 0, page->getWidth(), page->getHeight()});
    ASSERT_TRUE(image);
    EXPECT_GT(countPaintedPixels(*image), 0);
}

TEST(PdfAreaCopy, survivesClipboardSerialization) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);
    auto image = PdfElemSelection::createAreaImage(*page, {0, 0, page->getWidth(), page->getHeight()});
    ASSERT_TRUE(image);

    ObjectOutputStream out(new BinObjectEncoding());
    image->serialize(out);
    GString* data = out.stealData();

    ObjectInputStream in;
    ASSERT_TRUE(in.read(data->str, data->len));
    TexImage pasted;
    pasted.readSerialized(in);
    g_string_free(data, true);

    EXPECT_FALSE(pasted.hasLatexSource());
    EXPECT_EQ(pasted.getBinaryData(), image->getBinaryData());
    EXPECT_NEAR(pasted.getBoundingBox().width, page->getWidth(), 1e-6);
    EXPECT_EQ(countPaintedPixels(pasted), countPaintedPixels(*image));
}
