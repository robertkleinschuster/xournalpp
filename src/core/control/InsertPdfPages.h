/*
 * Xournal++
 *
 * Fork: "Insert Pages from PDF" - inserts pages of another PDF into the document
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
 * Asks for a PDF, its pages and the position, then inserts the pages. A document has only one background PDF, so the
 * pages are appended to a combined copy of it next to the document ("<name> (combined).pdf", or "<name>.pdf" if the
 * document has no PDF yet); the source PDF is never changed. Undoable.
 */
void run(Control* control);

/**
 * The file for the combined background PDF of the document xopp with the background PDF current (empty if none): the
 * current one if it is a combined PDF already, otherwise a new name that does not exist yet
 */
fs::path combinedPdfPath(const fs::path& xopp, const fs::path& current);

/// Inserts the pages (0-based) of pdf at position. Returns false (after telling the user) if that failed.
bool insert(Control* control, const fs::path& pdf, const std::vector<size_t>& pages, size_t position);

}  // namespace InsertPdfPages
