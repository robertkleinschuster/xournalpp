/*
 * Xournal++
 *
 * Tool for selecting PDF content
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */
#pragma once

#include <cstddef>  // for size_t
#include <memory>   // for unique_ptr
#include <string>   // for string
#include <vector>   // for vector

#include <cairo.h>  // for cairo_region_t, cairo_t

#include "control/ToolEnums.h"  // for ToolType
#include "model/Element.h"      // for ElementPtr
#include "model/OverlayBase.h"
#include "pdf/base/XojPdfPage.h"  // for XojPdfPageSelectionStyle, XojPdfRec...
#include "util/DispatchPool.h"
#include "util/Rectangle.h"           // for Rectangle
#include "util/Util.h"                // for npos
#include "util/raii/CairoWrappers.h"  // for CairoRegionSPtr

class Range;
class Control;

namespace xoj::view {
class PdfElementSelectionView;
};

/// Represents elements selected from a PDF page, such as text.
class PdfElemSelection: public OverlayBase {
public:
    PdfElemSelection(double x, double y, Control* control);
    PdfElemSelection& operator=(const PdfElemSelection&) = delete;
    PdfElemSelection(const PdfElemSelection&) = delete;
    PdfElemSelection& operator=(PdfElemSelection&&) = default;
    PdfElemSelection(PdfElemSelection&&) = default;
    virtual ~PdfElemSelection();

public:
    /// Calls finalizeSelection() and then repaints the page.
    bool finalizeSelectionAndRepaint(XojPdfPageSelectionStyle style);

    /// Sets the final selection bounds using the provided selection style.
    /// Returns true iff there is text contained within the selection bounds.
    bool finalizeSelection(XojPdfPageSelectionStyle style);

    /// Update the (unfinalized) selection bounds with the given
    /// style.
    void currentPos(double x, double y, XojPdfPageSelectionStyle style);

    /// If the selection is a rectangle, returns true iff the given point is
    /// contained in the selection. Returns false on text selection.
    bool contains(double x, double y);

    const std::vector<XojPdfRectangle>& getSelectedTextRects() const;

    /// Returns the text contained in the selection region.
    const std::string& getSelectedText() const;

    /// Returns true iff the final selections bounds are known.
    bool isFinalized() const;

    size_t getSelectionPageNr() const;
    void setToolType(ToolType toolType);

    /// Fork: the selected rectangle of an area selection, clipped to the page (may be empty)
    xoj::util::Rectangle<double> getSelectedArea() const;

    /// Fork: true iff the selection was finalized as an area, which can be copied with its graphics
    bool isAreaSelected() const { return areaSelected; }

    /// Fork: the PDF page the selection was made on (may be nullptr)
    const XojPdfPageSPtr& getPdfPage() const { return pdf; }

    /// Fork: creates an element showing an area of a PDF page, placed at the area's position. Usually it is a
    /// TexImage holding a one-page PDF, so text, formulas and graphics stay vector graphics. If that PDF is larger
    /// than AREA_VECTOR_SIZE_LIMIT (e.g. a scanned page, whose image is embedded completely) and a raster image of
    /// the area is much smaller, it is that raster Image. Returns nullptr for an empty area or on failure.
    static ElementPtr createAreaElement(const XojPdfPage& page, const xoj::util::Rectangle<double>& area);

    static constexpr size_t AREA_VECTOR_SIZE_LIMIT = 256 * 1024;
    static constexpr double AREA_RASTER_DPI = 300;

    const cairo_region_t* getSelectedRegion() const { return selectedTextRegion.get(); }

    /// Returns the selection style corresponding to the given tool type.
    static XojPdfPageSelectionStyle selectionStyleForToolType(ToolType type);

    inline const std::shared_ptr<xoj::util::DispatchPool<xoj::view::PdfElementSelectionView>>& getViewPool() const {
        return viewPool;
    }

private:
    /// Assigns the selected text region to the current selection bounds.
    void selectTextRegion(XojPdfPageSelectionStyle style);

    Range getRegionBbox() const;

    XojPdfPageSPtr pdf;

    /// The rectangles corresponding to the lines of selected text.
    std::vector<XojPdfRectangle> selectedTextRects;

    /// The text content of the selection.
    std::string selectedText;

    /// The area containing the selected text. Used for rendering.
    xoj::util::CairoRegionSPtr selectedTextRegion;

    /// The PDF selection tool used for the selection.
    ToolType toolType;

    size_t selectionPageNr = npos;

    /// The selection bounds. Note that this does not necessarily correspond to
    /// a rectangle--it may also indicate start and end positions of a linear
    /// text selection.
    XojPdfRectangle bounds;

    bool finalized;

    bool areaSelected = false;

    std::shared_ptr<xoj::util::DispatchPool<xoj::view::PdfElementSelectionView>> viewPool;
};
