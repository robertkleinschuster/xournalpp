/*
 * Xournal++
 *
 * Undo inserting pages from another PDF (fork): the pages and the switch to the combined background PDF
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <memory>   // for unique_ptr
#include <string>   // for string
#include <vector>   // for vector

#include "model/PageRef.h"  // for PageRef

#include "UndoAction.h"  // for UndoAction
#include "filesystem.h"  // for path

class Control;

class InsertPdfPagesUndoAction: public UndoAction {
public:
    InsertPdfPagesUndoAction(std::vector<PageRef> pages, size_t position, fs::path oldPdf, fs::path newPdf);

    bool undo(Control* control) override;
    bool redo(Control* control) override;
    std::vector<PageRef> getPages() override;
    std::string getText() override;

    /**
     * Makes pdf the background PDF (none if empty), inserts pages at position (none if pages is empty) and updates the
     * views; pdfData, if given, is the content of pdf, which is then not read from the file. Returns false with an
     * error message if the PDF cannot be loaded.
     */
    static bool apply(Control* control, const fs::path& pdf, std::unique_ptr<std::string> pdfData,
                      const std::vector<PageRef>& pages, size_t position);

private:
    std::vector<PageRef> pages;
    size_t position;
    fs::path oldPdf;
    fs::path newPdf;
};
