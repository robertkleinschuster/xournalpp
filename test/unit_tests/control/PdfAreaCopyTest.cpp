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

#include <cstdint>
#include <memory>
#include <random>
#include <string>

#include <cairo-pdf.h>
#include <config-test.h>
#include <gtest/gtest.h>

#include "control/tools/EditSelection.h"
#include "control/tools/PdfElemSelection.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Image.h"
#include "model/Layer.h"
#include "model/TexImage.h"
#include "model/XojPage.h"
#include "pdf/base/XojPdfDocument.h"
#include "util/PathUtil.h"
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

cairo_status_t appendToString(std::string* data, const unsigned char* bytes, unsigned int length) {
    data->append(reinterpret_cast<const char*>(bytes), length);
    return CAIRO_STATUS_SUCCESS;
}

/// A page like a scanned one: covered by a large image that does not compress
XojPdfPageSPtr createScannedPage(XojPdfDocument& doc) {
    const int size = 800;
    cairo_surface_t* noise = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
    std::mt19937 rng(42);
    auto* data = cairo_image_surface_get_data(noise);
    for (int i = 0; i < cairo_image_surface_get_stride(noise) * size; i++) {
        data[i] = static_cast<unsigned char>(rng());
    }
    cairo_surface_mark_dirty(noise);

    auto pdf = std::make_unique<std::string>();
    cairo_surface_t* surface = cairo_pdf_surface_create_for_stream(reinterpret_cast<cairo_write_func_t>(appendToString),
                                                                   pdf.get(), 595, 842);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_surface(cr, noise, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_finish(surface);
    cairo_surface_destroy(surface);
    cairo_surface_destroy(noise);

    EXPECT_TRUE(doc.load(std::move(pdf), "", nullptr));
    return doc.getPage(0);
}

/// Number of pixels that are not transparent when the element is drawn at its position
int countPaintedPixels(const Element& element) {
    const auto box = element.getBoundingBox();
    cairo_surface_t* surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(box.width), static_cast<int>(box.height));
    cairo_t* cr = cairo_create(surface);
    cairo_translate(cr, -box.x, -box.y);
    xoj::view::ElementView::createFromElement(&element)->draw(xoj::view::Context::createDefault(cr));
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

void expectBox(const Element& element, double x, double y, double width, double height) {
    const auto box = element.getBoundingBox();
    EXPECT_NEAR(box.x, x, 1e-6);
    EXPECT_NEAR(box.y, y, 1e-6);
    EXPECT_NEAR(box.width, width, 1e-6);
    EXPECT_NEAR(box.height, height, 1e-6);
}
}  // namespace

TEST(PdfAreaCopy, keepsAreaSizeAndPosition) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);

    auto element = PdfElemSelection::createAreaElement(*page, {30, 40, 200, 120});
    ASSERT_TRUE(element);
    ASSERT_EQ(element->getType(), ELEMENT_TEXIMAGE);
    EXPECT_FALSE(static_cast<TexImage*>(element.get())->hasLatexSource());
    expectBox(*element, 30, 40, 200, 120);
}

TEST(PdfAreaCopy, emptyAreaIsNotCopied) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);

    EXPECT_FALSE(PdfElemSelection::createAreaElement(*page, {30, 40, 0, 120}));
    EXPECT_FALSE(PdfElemSelection::createAreaElement(*page, {30, 40, 200, 0.5}));
}

TEST(PdfAreaCopy, containsThePageContent) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);

    auto element = PdfElemSelection::createAreaElement(*page, {0, 0, page->getWidth(), page->getHeight()});
    ASSERT_TRUE(element);
    EXPECT_GT(countPaintedPixels(*element), 0);
}

TEST(PdfAreaCopy, survivesClipboardSerialization) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);
    auto element = PdfElemSelection::createAreaElement(*page, {0, 0, page->getWidth(), page->getHeight()});
    ASSERT_TRUE(element);
    ASSERT_EQ(element->getType(), ELEMENT_TEXIMAGE);
    const auto& image = static_cast<const TexImage&>(*element);

    ObjectOutputStream out(new BinObjectEncoding());
    image.serialize(out);
    GString* data = out.stealData();

    ObjectInputStream in;
    ASSERT_TRUE(in.read(data->str, data->len));
    TexImage pasted;
    pasted.readSerialized(in);
    g_string_free(data, true);

    EXPECT_FALSE(pasted.hasLatexSource());
    EXPECT_EQ(pasted.getBinaryData(), image.getBinaryData());
    EXPECT_NEAR(pasted.getBoundingBox().width, page->getWidth(), 1e-6);
    EXPECT_EQ(countPaintedPixels(pasted), countPaintedPixels(image));
}

TEST(PdfAreaCopy, clipboardStreamIsASelection) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);
    auto element = PdfElemSelection::createAreaElement(*page, {30, 40, 200, 120});
    ASSERT_TRUE(element);

    ObjectOutputStream out(new BinObjectEncoding());
    EditSelection::serializeElement(out, *element);
    GString* data = out.stealData();

    // Read it like EditSelection::readSerialized() and Control::clipboardPasteXournal()
    ObjectInputStream in;
    ASSERT_TRUE(in.read(data->str, data->len));
    in.readObject("EditSelection");
    const double x = in.readDouble();
    const double y = in.readDouble();
    const double width = in.readDouble();
    const double height = in.readDouble();
    EXPECT_NEAR(x, 30 - 12, 1e-6);  // with the padding of a selection
    EXPECT_NEAR(y, 40 - 12, 1e-6);
    EXPECT_NEAR(width, 200 + 24, 1e-6);
    EXPECT_NEAR(height, 120 + 24, 1e-6);
    for (int i = 0; i < 5; i++) {  // snapped bounds, rotation
        in.readDouble();
    }
    in.readObject("EditSelectionContents");
    EXPECT_NEAR(in.readDouble(), x, 1e-6);
    for (int i = 0; i < 10; i++) {
        in.readDouble();
    }
    in.endObject();
    in.endObject();
    EXPECT_EQ(in.readInt(), 1);
    EXPECT_EQ(in.getNextObjectName(), "TexImage");
    TexImage pasted;
    pasted.readSerialized(in);
    g_string_free(data, true);
    expectBox(pasted, 30, 40, 200, 120);
}

TEST(PdfAreaCopy, smallAreaOfScannedPageIsRaster) {
    XojPdfDocument doc;
    auto page = createScannedPage(doc);
    ASSERT_TRUE(page);

    // The vector PDF would contain the complete page image
    auto element = PdfElemSelection::createAreaElement(*page, {100, 200, 60, 40});
    ASSERT_TRUE(element);
    ASSERT_EQ(element->getType(), ELEMENT_IMAGE);
    const auto& image = static_cast<const Image&>(*element);
    EXPECT_LT(image.getBinaryData().size(), PdfElemSelection::AREA_VECTOR_SIZE_LIMIT);
    expectBox(image, 100, 200, 60, 40);
    EXPECT_EQ(countPaintedPixels(image), 60 * 40);
}

TEST(PdfAreaCopy, documentWithAreaCanBeSavedAndLoaded) {
    auto page = loadTestPage();
    ASSERT_TRUE(page);
    auto element = PdfElemSelection::createAreaElement(*page, {30, 40, 200, 120});
    ASSERT_TRUE(element);

    DocumentHandler handler;
    Document doc(&handler);
    doc.addPage(std::make_shared<XojPage>(595.0, 842.0));
    doc.getPage(0)->getSelectedLayer()->addElement(std::move(element));

    // The area has no LaTeX source, so its text attribute is empty
    const auto file = Util::getTmpDirSubfolder() / "pdf-area.xopp";
    SaveHandler saver;
    saver.prepareSave(&doc, file);
    saver.saveTo(file);
    ASSERT_EQ(saver.getErrorMessage(), "");

    std::unique_ptr<Document> loaded;
    ASSERT_NO_THROW(loaded = LoadHandler{}.loadDocument(file));
    fs::remove(file);
    ASSERT_TRUE(loaded);
    const auto& elements = loaded->getPage(0)->getSelectedLayer()->getElementsView();
    ASSERT_EQ(elements.size(), 1);
    ASSERT_EQ(elements.front()->getType(), ELEMENT_TEXIMAGE);
    EXPECT_FALSE(static_cast<const TexImage*>(elements.front())->hasLatexSource());
    expectBox(*elements.front(), 30, 40, 200, 120);
}
