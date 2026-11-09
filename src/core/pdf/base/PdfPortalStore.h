/*
 * Xournal++
 *
 * Fork: the portal file of a document, which holds the PDF pages its copied PDF areas show (see PdfPortal)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <map>     // for map
#include <memory>  // for shared_ptr
#include <string>  // for string

#include "filesystem.h"  // for path

class PdfPortalPage;

/**
 * A document's portals show pages kept in its own hidden portal file ".<name>.portals.pdf" next to it (see
 * AnnotationFiles::portalsPdf), never pages of other PDFs on the computer: the document's files stay complete when
 * PDFs are moved, renamed, deleted or changed. It is a separate file (not inside the .xopp) so that git stores the
 * pages once instead of with every commit of the document. Each page carries its key in its page dictionary. The file
 * is written when the document is saved, with the pages its portals use (SaveHandler).
 */
namespace PdfPortalStore {

/// Whether this build can copy PDF pages (needs QPDF); without it, copied areas are snapshots only
bool isAvailable();

/**
 * Copies page (0-based) of the PDF as a PDF of its own. The same page always gives the same data, and so the same
 * key. Links and article beads are left out: they refer to other pages, which would be copied along. Returns nullptr
 * if it cannot.
 */
std::shared_ptr<PdfPortalPage> extractPage(const fs::path& pdf, size_t page);

/// The pages of a portal file, as one-page PDFs by key. Throws std::runtime_error if it cannot be read.
std::map<std::string, std::string> readPages(const fs::path& file);

/**
 * Makes the portal file hold exactly the given pages: rewrites it only if its pages differ, removes it if there are
 * none, unless keepIfEmpty (the document refers to pages it could not read). Throws std::runtime_error.
 */
void write(const fs::path& file, const std::map<std::string, std::shared_ptr<const PdfPortalPage>>& pages,
           bool keepIfEmpty);

}  // namespace PdfPortalStore
