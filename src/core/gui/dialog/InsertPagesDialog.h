/*
 * Xournal++
 *
 * Fork: choosing PDF pages to insert, with thumbnails
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>   // for size_t
#include <optional>  // for optional
#include <set>       // for set
#include <vector>    // for vector

#include <gtk/gtk.h>  // for GtkWindow

#include "filesystem.h"  // for path

namespace InsertPagesDialog {

struct Request {
    /// The document's background PDF, empty if it has none (or it is attached): then only other PDFs can be chosen
    fs::path ownPdf;
    /// The pages (0-based) of the own PDF that the document shows already; the others are preselected
    std::set<size_t> pagesInDocument;
};

struct Choice {
    /// The PDF to insert pages from; empty for the document's own PDF
    fs::path otherPdf;
    std::vector<size_t> pages;  ///< 0-based, in order
    bool atEnd = false;         ///< otherwise after the current page
};

/// Shows the dialog (modal); nullopt if cancelled
std::optional<Choice> run(GtkWindow* parent, const Request& request);

}  // namespace InsertPagesDialog
