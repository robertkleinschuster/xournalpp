/*
 * Xournal++
 *
 * Fork: copied PDF areas that show the PDF page itself (PdfPortal, PdfPortalStore)
 *
 * @license GNU GPLv2 or later
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include <config-test.h>
#include <gtest/gtest.h>

#include "control/AnnotationFiles.h"
#include "control/settings/Settings.h"
#include "control/tools/PdfElemSelection.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Layer.h"
#include "model/PdfPortal.h"
#include "model/RectangularElement.h"
#include "model/XojPage.h"
#include "pdf/base/PdfPortalStore.h"
#include "pdf/base/XojPdfDocument.h"
#include "util/PathUtil.h"
#include "util/serializing/BinObjectEncoding.h"
#include "util/serializing/ObjectInputStream.h"
#include "util/serializing/ObjectOutputStream.h"
#include "view/View.h"

#include "filesystem.h"

namespace {
const fs::path& testPdf() {
    static const fs::path pdf = GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf");
    return pdf;
}

XojPdfPageSPtr loadTestPage() {
    static XojPdfDocument doc;
    if (doc.getPageCount() == 0) {
        EXPECT_TRUE(doc.load(testPdf(), "", nullptr));
    }
    return doc.getPage(0);
}

/// An area copied like PdfFloatingToolbox::copyAreaToClipboard() does it
auto copyArea(const xoj::util::Rectangle<double>& area) -> ElementPtr {
    auto element = PdfElemSelection::createAreaElement(*loadTestPage(), area);
    auto page = PdfPortalStore::extractPage(testPdf(), 0);
    EXPECT_TRUE(page);
    auto portal = std::make_shared<PdfPortal>();
    portal->key = page->getKey();
    portal->area = area;
    portal->page = std::move(page);
    dynamic_cast<RectangularElement&>(*element).setPdfPortal(std::move(portal));
    return element;
}

auto portalOf(const Element& element) -> const std::shared_ptr<const PdfPortal>& {
    return dynamic_cast<const RectangularElement&>(element).getPdfPortal();
}

/// The pixels of the area, drawn by the element or from the PDF page itself, at the given zoom
auto renderArea(const Element* element, const xoj::util::Rectangle<double>& area, double zoom) -> cairo_surface_t* {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(area.width * zoom),
                                                          static_cast<int>(area.height * zoom));
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, zoom, zoom);
    cairo_translate(cr, -area.x, -area.y);
    if (element) {
        xoj::view::ElementView::createFromElement(element)->draw(xoj::view::Context::createDefault(cr));
    } else {
        cairo_rectangle(cr, area.x, area.y, area.width, area.height);
        cairo_clip(cr);
        loadTestPage()->render(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    return surface;
}

/// Share of the pixels that differ noticeably
auto differingShare(cairo_surface_t* a, cairo_surface_t* b) -> double {
    const int width = cairo_image_surface_get_width(a);
    const int height = cairo_image_surface_get_height(a);
    const int stride = cairo_image_surface_get_stride(a);
    int differing = 0;
    for (int y = 0; y < height; y++) {
        const auto* rowA = cairo_image_surface_get_data(a) + y * stride;
        const auto* rowB = cairo_image_surface_get_data(b) + y * stride;
        for (int x = 0; x < width * 4; x += 4) {
            for (int c = 0; c < 4; c++) {
                if (std::abs(rowA[x + c] - rowB[x + c]) > 16) {
                    differing++;
                    break;
                }
            }
        }
    }
    return static_cast<double>(differing) / (width * height);
}

auto painted(cairo_surface_t* surface) -> int {
    int count = 0;
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < cairo_image_surface_get_height(surface); y++) {
        const auto* row = reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(surface) + y * stride);
        for (int x = 0; x < cairo_image_surface_get_width(surface); x++) {
            count += (row[x] >> 24) != 0;
        }
    }
    return count;
}

class PdfPortalFileTest: public ::testing::Test {
protected:
    void SetUp() override {
        dir = Util::getTmpDirSubfolder("pdf-portal-test");
        fs::remove_all(dir);
        fs::create_directories(dir);
        file = dir / "VO8.xopp";
    }
    void TearDown() override { fs::remove_all(dir); }

    auto save(const std::vector<ElementPtr>& elements) -> std::string {
        DocumentHandler handler;
        Document doc(&handler);
        doc.addPage(std::make_shared<XojPage>(595.0, 842.0));
        for (const auto& element: elements) {
            doc.getPage(0)->getSelectedLayer()->addElement(element->clone());
        }
        SaveHandler saver;
        saver.prepareSave(&doc, file);
        saver.saveTo(file);
        return saver.getErrorMessage();
    }

    auto loadElements() -> std::vector<ElementPtr> {
        auto doc = LoadHandler{}.loadDocument(file);
        std::vector<ElementPtr> elements;
        for (const auto& e: doc->getPage(0)->getSelectedLayer()->getElements()) {
            elements.push_back(e->clone());
        }
        return elements;
    }

    fs::path dir;
    fs::path file;
};
}  // namespace

TEST(PdfPortal, copyingAPageAgainGivesTheSamePage) {
    auto first = PdfPortalStore::extractPage(testPdf(), 0);
    auto second = PdfPortalStore::extractPage(testPdf(), 0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->getKey().size(), 32u);
    EXPECT_FALSE(PdfPortalStore::extractPage(testPdf(), 999));
}

TEST(PdfPortal, showsTheAreaLikeThePdfAtHighZoom) {
    const xoj::util::Rectangle<double> area{30, 40, 200, 120};
    auto element = copyArea(area);
    for (double zoom: {1.0, 6.0}) {
        cairo_surface_t* shown = renderArea(element.get(), area, zoom);
        cairo_surface_t* pdf = renderArea(nullptr, area, zoom);
        EXPECT_GT(painted(pdf), 0);
        EXPECT_LT(differingShare(shown, pdf), 0.001) << "at zoom " << zoom;
        cairo_surface_destroy(shown);
        cairo_surface_destroy(pdf);
    }
}

TEST(PdfPortal, survivesTheClipboard) {
    const xoj::util::Rectangle<double> area{30, 40, 200, 120};
    auto element = copyArea(area);

    ObjectOutputStream out(new BinObjectEncoding());
    element->serialize(out);
    GString* data = out.stealData();
    ObjectInputStream in;
    ASSERT_TRUE(in.read(data->str, data->len));
    auto pasted = element->clone();  // same type; readSerialized() replaces its content
    dynamic_cast<RectangularElement&>(*pasted).setPdfPortal(nullptr);
    pasted->readSerialized(in);
    g_string_free(data, true);

    const auto& portal = portalOf(*pasted);
    ASSERT_TRUE(portal);
    EXPECT_EQ(portal->key, portalOf(*element)->key);
    EXPECT_EQ(portal->page, portalOf(*element)->page);  // the same page in memory
    EXPECT_DOUBLE_EQ(portal->area.y, 40);
}

TEST_F(PdfPortalFileTest, pagesGoIntoTheHiddenPortalFile) {
    auto a = copyArea({30, 40, 200, 120});
    auto b = copyArea({100, 300, 50, 50});  // same page: stored once
    std::vector<ElementPtr> elements;
    elements.push_back(std::move(a));
    elements.push_back(std::move(b));
    ASSERT_EQ(save(elements), "");

    const auto portals = AnnotationFiles::portalsPdf(file);
    EXPECT_EQ(portals.filename(), ".VO8.portals.pdf");
    ASSERT_TRUE(fs::exists(portals));
    EXPECT_EQ(PdfPortalStore::readPages(portals).size(), 1u);

    const auto loaded = loadElements();
    ASSERT_EQ(loaded.size(), 2u);
    for (const auto& e: loaded) {
        ASSERT_TRUE(portalOf(*e));
        EXPECT_TRUE(portalOf(*e)->page);
    }
    EXPECT_DOUBLE_EQ(portalOf(*loaded[1])->area.x, 100);
}

TEST_F(PdfPortalFileTest, unchangedPortalFileIsNotRewritten) {
    std::vector<ElementPtr> elements;
    elements.push_back(copyArea({30, 40, 200, 120}));
    ASSERT_EQ(save(elements), "");
    const auto portals = AnnotationFiles::portalsPdf(file);
    const auto time = fs::last_write_time(portals);
    fs::last_write_time(portals, time - std::chrono::hours(1));
    const auto before = fs::last_write_time(portals);

    ASSERT_EQ(save(loadElements()), "");
    EXPECT_EQ(fs::last_write_time(portals), before);
}

TEST_F(PdfPortalFileTest, portalFileIsRemovedWithTheLastArea) {
    std::vector<ElementPtr> elements;
    elements.push_back(copyArea({30, 40, 200, 120}));
    ASSERT_EQ(save(elements), "");
    ASSERT_TRUE(fs::exists(AnnotationFiles::portalsPdf(file)));

    ASSERT_EQ(save({}), "");
    EXPECT_FALSE(fs::exists(AnnotationFiles::portalsPdf(file)));
}

TEST_F(PdfPortalFileTest, withoutPortalFileTheSnapshotIsShownAndTheAreaKept) {
    const xoj::util::Rectangle<double> area{30, 40, 200, 120};
    std::vector<ElementPtr> elements;
    elements.push_back(copyArea(area));
    ASSERT_EQ(save(elements), "");
    fs::remove(AnnotationFiles::portalsPdf(file));  // e.g. only the .xopp was shared

    auto loaded = loadElements();
    ASSERT_EQ(loaded.size(), 1u);
    ASSERT_TRUE(portalOf(*loaded[0]));
    EXPECT_FALSE(portalOf(*loaded[0])->page);
    cairo_surface_t* shown = renderArea(loaded[0].get(), area, 1.0);
    EXPECT_GT(painted(shown), 0);  // the snapshot
    cairo_surface_destroy(shown);

    // Saving keeps the reference (the file may come back, e.g. with the next sync)
    ASSERT_EQ(save(loaded), "");
    auto again = loadElements();
    ASSERT_TRUE(portalOf(*again[0]));
    EXPECT_EQ(portalOf(*again[0])->key, portalOf(*elements[0])->key);
}

TEST_F(PdfPortalFileTest, unreadablePortalFileIsKept) {
    std::vector<ElementPtr> elements;
    elements.push_back(copyArea({30, 40, 200, 120}));
    ASSERT_EQ(save(elements), "");
    const auto portals = AnnotationFiles::portalsPdf(file);
    {
        std::ofstream broken(portals, std::ios::binary | std::ios::trunc);
        broken << "not a PDF";
    }
    auto loaded = loadElements();
    ASSERT_EQ(save(loaded), "");
    EXPECT_TRUE(fs::exists(portals));
}

TEST_F(PdfPortalFileTest, portalFileMovesWithTheDocument) {
    std::vector<ElementPtr> elements;
    elements.push_back(copyArea({30, 40, 200, 120}));
    ASSERT_EQ(save(elements), "");
    Settings settings(dir / "settings.xml");

    const auto renamed = AnnotationFiles::groupForRename(file, dir / "VO9.xopp", settings);
    EXPECT_NE(std::find(renamed.moves.begin(), renamed.moves.end(),
                        std::pair{AnnotationFiles::portalsPdf(file), dir / ".VO9.portals.pdf"}),
              renamed.moves.end());
    const auto files = AnnotationFiles::groupFiles(file, settings);
    EXPECT_NE(std::find(files.begin(), files.end(), AnnotationFiles::portalsPdf(file)), files.end());
    EXPECT_TRUE(AnnotationFiles::isPortalsPdf(dir / ".VO9.portals.pdf"));
    EXPECT_FALSE(AnnotationFiles::isPortalsPdf(dir / "VO9.portals.pdf"));
}
