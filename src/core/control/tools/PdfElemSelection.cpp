#include "PdfElemSelection.h"

#include <algorithm>  // for max, min
#include <limits>     // for numeric_limits
#include <memory>     // for __shared_ptr_access
#include <tuple>      // for tuple
#include <utility>    // for move

#include <cairo-pdf.h>  // for cairo_pdf_surface_create_for_stream
#include <cairo.h>      // for cairo_line_to, cairo_region_destroy
#include <gdk/gdk.h>    // for GdkRGBA, gdk_cairo_set_source_rgba

#include "control/Control.h"      // for Control
#include "control/ToolHandler.h"  // for ToolHandler
#include "gui/PageView.h"         // for XojPageView
#include "gui/XournalView.h"      // for XournalView
#include "model/Document.h"       // for Document
#include "model/Image.h"          // for Image
#include "model/PageRef.h"        // for PageRef
#include "model/TexImage.h"       // for TexImage
#include "model/XojPage.h"        // for XojPage
#include "pdf/base/XojPdfPage.h"  // for XojPdfRectangle, XojPdfPageSelectio...
#include "util/Assert.h"          // for xoj_assert
#include "util/safe_casts.h"      // for strict_cast, as_signed, as_si...
#include "view/overlays/PdfElementSelectionView.h"

PdfElemSelection::PdfElemSelection(double x, double y, Control* control):
        pdf(nullptr),
        bounds({x, y, x, y}),
        finalized(false),
        viewPool(std::make_shared<xoj::util::DispatchPool<xoj::view::PdfElementSelectionView>>()) {

    if (auto pNr = control->getCurrentPage()->getPdfPageNr(); pNr != npos) {
        Document* doc = control->getDocument();
        doc->lock_shared();
        this->pdf = doc->getPdfPage(pNr);
        doc->unlock_shared();

        this->selectionPageNr = pNr;
    }

    this->toolType = control->getToolHandler()->getToolType();
}

PdfElemSelection::~PdfElemSelection() {
    Range rg = getRegionBbox();
    this->viewPool->dispatchAndClear(xoj::view::PdfElementSelectionView::CANCEL_SELECTION_REQUEST, rg);
}

auto PdfElemSelection::finalizeSelectionAndRepaint(XojPdfPageSelectionStyle style) -> bool {
    Range rg = getRegionBbox();
    bool result = this->finalizeSelection(style);
    rg = rg.unite(getRegionBbox());
    if (!rg.empty()) {
        this->viewPool->dispatch(xoj::view::PdfElementSelectionView::FLAG_DIRTY_REGION_REQUEST, rg);
    }
    return result;
}

bool PdfElemSelection::finalizeSelection(XojPdfPageSelectionStyle style) {
    this->finalized = true;

    XojPdfPage::TextSelection selection = this->pdf->selectTextLines(this->bounds, style);
    this->selectedTextRegion = std::move(selection.region);
    this->selectedTextRects = std::move(selection.rects);
    this->selectedText = this->pdf->selectText(this->bounds, style);
    // Informs the windowing system of the selection -- i.e. for accessibility purposes
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), this->selectedText.c_str(),
                           strict_cast<gint>(this->selectedText.length()));

    if (style == XojPdfPageSelectionStyle::Area) {
        // Fork: an area can also be copied with its graphics, so it counts even without text
        this->areaSelected = true;
        const auto area = getSelectedArea();
        return area.width >= 1 && area.height >= 1;
    }
    this->areaSelected = false;
    return !this->selectedTextRects.empty();
}

auto PdfElemSelection::getSelectedArea() const -> xoj::util::Rectangle<double> {
    if (!this->pdf) {
        return {};
    }
    const double x1 = std::clamp(std::min(bounds.x1, bounds.x2), 0.0, this->pdf->getWidth());
    const double x2 = std::clamp(std::max(bounds.x1, bounds.x2), 0.0, this->pdf->getWidth());
    const double y1 = std::clamp(std::min(bounds.y1, bounds.y2), 0.0, this->pdf->getHeight());
    const double y2 = std::clamp(std::max(bounds.y1, bounds.y2), 0.0, this->pdf->getHeight());
    return {x1, y1, x2 - x1, y2 - y1};
}

static auto writeToString(std::string* data, const unsigned char* bytes, unsigned int length) -> cairo_status_t {
    data->append(reinterpret_cast<const char*>(bytes), length);
    return CAIRO_STATUS_SUCCESS;
}

/// Renders the area into a one-page PDF. Text, formulas and graphics stay vector graphics, but the PDF contains all
/// of the page's content (clipped to the area), e.g. the complete image of a scanned page.
static auto renderAreaPdf(const XojPdfPage& page, const xoj::util::Rectangle<double>& area) -> std::string {
    std::string data;
    cairo_surface_t* surface = cairo_pdf_surface_create_for_stream(reinterpret_cast<cairo_write_func_t>(writeToString),
                                                                   &data, area.width, area.height);
    cairo_t* cr = cairo_create(surface);
    cairo_translate(cr, -area.x, -area.y);
    cairo_rectangle(cr, area.x, area.y, area.width, area.height);
    cairo_clip(cr);
    page.render(cr);  // like on screen: renderForPrinting() leaves out annotations not flagged for printing
    cairo_destroy(cr);
    cairo_surface_finish(surface);
    cairo_surface_destroy(surface);
    return data;
}

/// Renders the area into a PNG with the given resolution. Returns the PNG data and its size in pixels.
static auto renderAreaPng(const XojPdfPage& page, const xoj::util::Rectangle<double>& area, double dpi)
        -> std::tuple<std::string, int, int> {
    const double factor = dpi / 72.0;
    const int width = std::max(1, ceil_cast<int>(area.width * factor));
    const int height = std::max(1, ceil_cast<int>(area.height * factor));
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, factor, factor);
    cairo_translate(cr, -area.x, -area.y);
    page.render(cr);
    cairo_destroy(cr);

    std::string data;
    if (cairo_surface_write_to_png_stream(surface, reinterpret_cast<cairo_write_func_t>(writeToString), &data) !=
        CAIRO_STATUS_SUCCESS) {
        data.clear();
    }
    cairo_surface_destroy(surface);
    return {std::move(data), width, height};
}

auto PdfElemSelection::createAreaElement(const XojPdfPage& page, const xoj::util::Rectangle<double>& area)
        -> ElementPtr {
    if (area.width < 1 || area.height < 1) {
        return nullptr;
    }

    std::string pdf = renderAreaPdf(page, area);

    if (pdf.size() > AREA_VECTOR_SIZE_LIMIT) {
        // Probably a large image, of which only a part is needed: use a raster image of the area if it is much smaller
        auto [png, width, height] = renderAreaPng(page, area, AREA_RASTER_DPI);
        if (!png.empty() && 2 * png.size() < pdf.size()) {
            auto image = std::make_unique<Image>();
            image->setImage(std::move(png));
            image->setTransformation(xoj::util::Matrix::SCALING(area.width / width, area.height / height));
            image->move(area.x, area.y);
            return image;
        }
    }

    // A TexImage displays and stores PDF data; without LaTeX source it is not opened in the LaTeX editor.
    auto image = std::make_unique<TexImage>();
    if (GError* err = nullptr; !image->loadData(std::move(pdf), &err)) {
        g_warning("Could not copy the PDF area: %s", err ? err->message : "invalid PDF data");
        g_clear_error(&err);
        return nullptr;
    }
    image->move(area.x, area.y);
    return image;
}

XojPdfPageSelectionStyle PdfElemSelection::selectionStyleForToolType(ToolType type) {
    switch (type) {
        case ToolType::TOOL_SELECT_PDF_TEXT_RECT:
        case ToolType::TOOL_COPY_PDF_AREA:
            return XojPdfPageSelectionStyle::Area;
        default:
            return XojPdfPageSelectionStyle::Linear;
    }
}

Range PdfElemSelection::getRegionBbox() const {
    Range rg;
    if (this->selectedTextRegion && cairo_region_num_rectangles(this->selectedTextRegion.get()) > 0) {
        cairo_rectangle_int_t bbox{};
        cairo_region_get_extents(this->selectedTextRegion.get(), &bbox);
        rg = Range(bbox.x, bbox.y, bbox.x + bbox.width, bbox.y + bbox.height);
    }
    if (this->areaSelected) {
        const auto area = getSelectedArea();
        // Includes the area's outline
        rg = rg.unite(Range(area.x - 2, area.y - 2, area.x + area.width + 2, area.y + area.height + 2));
    }
    return rg;
}

void PdfElemSelection::currentPos(double x, double y, XojPdfPageSelectionStyle style) {
    if (!this->pdf) {
        return;
    }

    // The area of a finalized area selection depends on the bounds, so get its old extent first
    Range rg = getRegionBbox();
    this->areaSelected = false;

    // Update the end position
    this->bounds.x2 = x;
    this->bounds.y2 = y;

    // Repaint the selected text area
    switch (style) {
        case XojPdfPageSelectionStyle::Linear:
        case XojPdfPageSelectionStyle::Word:
        case XojPdfPageSelectionStyle::Line:
            this->selectedTextRegion.reset(this->pdf->selectTextRegion(this->bounds, style), xoj::util::adopt);
            break;
        case XojPdfPageSelectionStyle::Area: {
            cairo_rectangle_int_t rect;
            rect.x = floor_cast<int>(std::min(bounds.x1, bounds.x2));
            rect.width = ceil_cast<int>(std::max(bounds.x1, bounds.x2)) - rect.x;
            rect.y = floor_cast<int>(std::min(bounds.y1, bounds.y2));
            rect.height = ceil_cast<int>(std::max(bounds.y1, bounds.y2)) - rect.y;
            this->selectedTextRegion.reset(cairo_region_create_rectangle(&rect), xoj::util::adopt);
        } break;
        default:
            xoj_assert_message(false, "Unreachable");
    }
    xoj_assert(this->selectedTextRegion);

    rg = rg.unite(getRegionBbox());
    if (!rg.empty()) {
        this->viewPool->dispatch(xoj::view::PdfElementSelectionView::FLAG_DIRTY_REGION_REQUEST, rg);
    }
}

auto PdfElemSelection::contains(double x, double y) -> bool {
    if (this->areaSelected) {
        const auto area = getSelectedArea();
        return x >= area.x && x <= area.x + area.width && y >= area.y && y <= area.y + area.height;
    }
    if (!this->selectedTextRegion) {
        return false;
    }

    return cairo_region_contains_point(this->selectedTextRegion.get(), static_cast<int>(x), static_cast<int>(y));
}

auto PdfElemSelection::getSelectedTextRects() const -> const std::vector<XojPdfRectangle>& { return selectedTextRects; }

auto PdfElemSelection::getSelectedText() const -> const std::string& { return this->selectedText; }

auto PdfElemSelection::getSelectionPageNr() const -> size_t { return selectionPageNr; }

auto PdfElemSelection::isFinalized() const -> bool { return this->finalized; }

void PdfElemSelection::setToolType(ToolType tType) { this->toolType = tType; }
