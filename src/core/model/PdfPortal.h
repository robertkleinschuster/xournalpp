/*
 * Xournal++
 *
 * Fork: an area of a PDF page that an image element shows exactly ("portal")
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>  // for shared_ptr
#include <mutex>   // for mutex
#include <string>  // for string

#include <cairo.h>    // for cairo_t
#include <poppler.h>  // for PopplerDocument

#include "util/Rectangle.h"         // for Rectangle
#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr

class ObjectInputStream;
class ObjectOutputStream;

/**
 * A PDF page copied from a PDF, as a PDF of its own (see PdfCombiner::extractPage). Its key is the hash of that data,
 * so copying the same page again gives the same key. A document stores each page once, inside the .xopp (at the first
 * element showing it), so it does not depend on other files. Immutable apart from the lazily opened PDF; rendering is
 * thread safe.
 */
class PdfPortalPage {
public:
    PdfPortalPage(std::string key, std::string data);
    PdfPortalPage(const PdfPortalPage&) = delete;
    PdfPortalPage& operator=(const PdfPortalPage&) = delete;

    /// The page with this key: the one in memory if there is one (e.g. in another document), else a new one
    static std::shared_ptr<PdfPortalPage> get(const std::string& key, std::string data);
    /// The page for this one-page PDF data, with its key
    static std::shared_ptr<PdfPortalPage> fromData(std::string data);

    const std::string& getKey() const { return key; }
    /// The page as a one-page PDF
    const std::string& getData() const { return data; }

    /// Renders the page at its PDF coordinates. Returns false if the PDF cannot be read.
    bool render(cairo_t* cr, bool forPrinting) const;

private:
    const std::string key;
    const std::string data;
    mutable std::mutex mutex;  ///< poppler documents must not render on two threads at once
    mutable xoj::util::GObjectSPtr<PopplerDocument> pdf;
    mutable bool failed = false;
};

/**
 * The area of a page that an Image or TexImage element shows instead of its own picture, exactly at any zoom. The
 * element's picture stays as a snapshot: Xournal++ without portals (upstream) shows it, as it ignores the attributes
 * the portal is saved in, and so does the fork if the page is missing.
 */
struct PdfPortal {
    std::string key;                            ///< key of the page
    xoj::util::Rectangle<double> area;          ///< in PDF coordinates of the page
    std::shared_ptr<const PdfPortalPage> page;  ///< nullptr if the page is missing: the snapshot is shown

    /**
     * Draws the area into the element's box (0, 0, width, height) in its local coordinates. Returns false if it
     * cannot, then the element draws its snapshot.
     */
    bool draw(cairo_t* cr, double width, double height) const;

    /// Serializes an optional portal (for the clipboard), with its page data
    static void serialize(ObjectOutputStream& out, const std::shared_ptr<const PdfPortal>& portal);
    static auto readSerialized(ObjectInputStream& in) -> std::shared_ptr<const PdfPortal>;
};
