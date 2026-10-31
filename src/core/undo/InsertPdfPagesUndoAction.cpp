#include "InsertPdfPagesUndoAction.h"

#include <algorithm>  // for min
#include <utility>    // for move

#include "control/Control.h"        // for Control
#include "control/ScrollHandler.h"  // for ScrollHandler
#include "gui/MainWindow.h"         // for MainWindow
#include "gui/XournalView.h"        // for XournalView
#include "model/Document.h"         // for Document
#include "util/XojMsgBox.h"         // for XojMsgBox
#include "util/i18n.h"              // for _

InsertPdfPagesUndoAction::InsertPdfPagesUndoAction(std::vector<PageRef> pages, size_t position, fs::path oldPdf,
                                                   fs::path newPdf):
        UndoAction("InsertPdfPagesUndoAction"),
        pages(std::move(pages)),
        position(position),
        oldPdf(std::move(oldPdf)),
        newPdf(std::move(newPdf)) {}

auto InsertPdfPagesUndoAction::apply(Control* control, const fs::path& pdf, std::unique_ptr<std::string> pdfData,
                                     const std::vector<PageRef>& pages, size_t position) -> bool {
    Document* doc = control->getDocument();
    if (pdf.empty()) {
        doc->lock();
        doc->resetPdf();
        doc->setPdfAttributes({}, false);
        doc->unlock();
    } else if (!doc->readPdf(pdf, false, false, std::move(pdfData))) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), FS(_F("Error reading PDF: {1}") % doc->getLastErrorMsg()));
        return false;
    }
    doc->lock();
    for (size_t i = 0; i < pages.size(); i++) {
        doc->insertPage(pages[i], position + i);
    }
    doc->unlock();

    // Rebuilds all views and caches: they refer to the pages of the previous PDF
    control->fireDocumentChanged(DOCUMENT_CHANGE_COMPLETE);
    return true;
}

auto InsertPdfPagesUndoAction::undo(Control* control) -> bool {
    Document* doc = control->getDocument();
    doc->lock();
    for (size_t i = pages.size(); i > 0; i--) {
        doc->deletePage(position + i - 1);
    }
    doc->unlock();
    this->undone = true;
    const bool loaded = apply(control, oldPdf, nullptr, {}, 0);
    const size_t pageCount = doc->getPageCount();
    if (pageCount > 0) {
        const size_t page = std::min(position, pageCount - 1);
        control->firePageSelected(page);
        control->getScrollHandler()->scrollToPage(page);
    }
    return loaded;
}

auto InsertPdfPagesUndoAction::redo(Control* control) -> bool {
    this->undone = false;
    if (!apply(control, newPdf, nullptr, pages, position)) {
        return false;
    }
    control->firePageSelected(position);
    control->getScrollHandler()->scrollToPage(position);
    return true;
}

auto InsertPdfPagesUndoAction::getPages() -> std::vector<PageRef> { return pages; }

auto InsertPdfPagesUndoAction::getText() -> std::string { return _("Insert pages from PDF"); }
