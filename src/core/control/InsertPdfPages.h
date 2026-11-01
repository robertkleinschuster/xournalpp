/*
 * Xournal++
 *
 * Fork: "Insert Pages" - inserts pages of the background PDF or of another PDF
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <string>   // for string
#include <vector>   // for vector

#include "filesystem.h"  // for path

class Control;

namespace InsertPdfPages {

/**
 * "Insert Pages": shows the pages of the document's PDF (those not in the document preselected, like the former
 * "Append New PDF Pages") or of another PDF as thumbnails and inserts the chosen ones. Pages of another PDF are
 * appended to a combined copy of the background PDF next to the document ("<name> (combined).pdf", or "<name>.pdf"
 * if the document has no PDF yet), as a document has only one background PDF; the source PDF is never changed.
 * Undoable.
 */
void run(Control* control);

/// Inserts pages (0-based) of the document's own background PDF at position
bool insertOwn(Control* control, const std::vector<size_t>& pages, size_t position);

/**
 * The file for the combined background PDF of the document xopp with the background PDF current (empty if none): the
 * current one if it is a combined PDF already, otherwise a new name that does not exist yet
 */
fs::path combinedPdfPath(const fs::path& xopp, const fs::path& current);

/// Inserts the pages (0-based) of pdf at position. Returns false (after telling the user) if that failed.
bool insert(Control* control, const fs::path& pdf, const std::vector<size_t>& pages, size_t position);

}  // namespace InsertPdfPages
