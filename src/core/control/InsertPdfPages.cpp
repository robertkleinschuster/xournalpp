#include "InsertPdfPages.h"

#include <fstream>    // for ofstream
#include <memory>     // for make_shared, make_unique
#include <optional>   // for optional
#include <stdexcept>  // for runtime_error
#include <utility>    // for move

#include <gtk/gtk.h>

#include "control/AnnotationFiles.h"        // for companionPdf
#include "control/Control.h"                // for Control
#include "control/ScrollHandler.h"          // for ScrollHandler
#include "gui/dialog/InsertPagesDialog.h"   // for InsertPagesDialog
#include "model/Document.h"                 // for Document
#include "model/XojPage.h"                  // for XojPage
#include "pdf/base/PdfCombiner.h"           // for PdfCombiner
#include "pdf/base/XojPdfDocument.h"        // for XojPdfDocument
#include "undo/InsertPdfPagesUndoAction.h"  // for InsertPdfPagesUndoAction
#include "undo/UndoRedoHandler.h"           // for UndoRedoHandler
#include "util/PathUtil.h"                  // for toUtf8, fromUtf8
#include "util/Util.h"                      // for npos
#include "util/XojMsgBox.h"                 // for XojMsgBox
#include "util/i18n.h"                      // for _

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void showError(Control* control, const std::string& message, const std::string& detail = {}) {
    XojMsgBox::showErrorToUser(control->getGtkWindow(), detail.empty() ? message : message + "\n\n" + detail);
}

/// The companion PDF is also hidden in the Windows Explorer (Git does not keep the attribute, the name still hides
/// it in the project explorer)
void setHidden(const fs::path& path, bool hidden) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(path.c_str(),
                           hidden ? attributes | FILE_ATTRIBUTE_HIDDEN : attributes & ~DWORD(FILE_ATTRIBUTE_HIDDEN));
    }
#else
    (void)path;
    (void)hidden;
#endif
}

auto writeFile(const fs::path& path, const std::string& data) -> bool {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    return !out.fail();
}

}  // namespace

auto InsertPdfPages::combinedPdfPath(const fs::path& xopp, const fs::path& current) -> fs::path {
    if (!current.empty() && PdfCombiner::isCombined(current)) {
        return current;
    }
    std::error_code error;
    const auto companion = AnnotationFiles::companionPdf(xopp);
    if (!fs::exists(companion, error)) {
        return companion;
    }
    // Taken (e.g. still in use): ".VO8.pages 2.pdf"
    const auto base = Util::toUtf8(companion.stem());
    for (int n = 2;; n++) {
        auto candidate = companion.parent_path() / Util::fromUtf8(base + " " + std::to_string(n) + ".pdf");
        if (!fs::exists(candidate, error)) {
            return candidate;
        }
    }
}

auto InsertPdfPages::insert(Control* control, const fs::path& pdf, const std::vector<size_t>& pages, size_t position)
        -> bool {
    Document* doc = control->getDocument();
    doc->lock_shared();
    const fs::path xopp = doc->getFilepath();
    const fs::path base = doc->getPdfFilepath();
    const bool attached = doc->isAttachPdf();
    const size_t basePageCount = base.empty() ? 0 : doc->getPdfPageCount();
    position = std::min(position, doc->getPageCount());
    doc->unlock_shared();

    if (attached) {
        showError(control, _("Pages cannot be inserted into a document with an attached PDF."));
        return false;
    }
    std::error_code fsError;
    if (!base.empty() && !fs::exists(base, fsError)) {
        showError(control, _("The background PDF of the document is missing."), Util::toUtf8(base));
        return false;
    }

    // The new pages, sized like their PDF pages; in the combined PDF they follow the pages of the current one
    std::vector<PageRef> newPages;
    {
        XojPdfDocument source;
        GError* error = nullptr;
        if (!source.load(pdf, "", &error)) {
            showError(control, _("The PDF could not be read."), error ? error->message : Util::toUtf8(pdf));
            if (error) {
                g_error_free(error);
            }
            return false;
        }
        for (size_t i = 0; i < pages.size(); i++) {
            auto pdfPage = source.getPage(pages[i]);
            if (!pdfPage) {
                showError(control, _("The PDF could not be read."), Util::toUtf8(pdf));
                return false;
            }
            auto page = std::make_shared<XojPage>(pdfPage->getWidth(), pdfPage->getHeight());
            page->setBackgroundPdfPageNr(basePageCount + i);
            newPages.push_back(std::move(page));
        }
    }

    std::string data;
    try {
        data = PdfCombiner::append(base, pdf, pages);
    } catch (const std::exception& e) {
        showError(control, _("The pages could not be combined."), e.what());
        return false;
    }

    auto target = combinedPdfPath(xopp, base);
    auto part = target;
    part += ".part";
    if (!writeFile(part, data)) {
        fs::remove(part, fsError);
        showError(control, _("The combined PDF could not be written."), Util::toUtf8(part));
        return false;
    }
    // Loaded from memory, the document no longer holds the previous PDF open, so it can be replaced on Windows
    if (!InsertPdfPagesUndoAction::apply(control, target, std::make_unique<std::string>(std::move(data)), newPages,
                                         position)) {
        fs::remove(part, fsError);
        return false;
    }
    setHidden(target, false);  // Windows does not replace hidden files
    fs::rename(part, target, fsError);
    if (fsError) {
        // Still in use (e.g. by a running export): keep it under another name
        auto other = combinedPdfPath(xopp, {});
        fsError.clear();
        fs::rename(part, other, fsError);
        if (fsError) {
            showError(control, _("The combined PDF could not be written."), Util::toUtf8(target));
            return false;
        }
        target = other;
        doc->lock();
        doc->setPdfAttributes(target, false);
        doc->unlock();
    }

    setHidden(target, true);
    control->getUndoRedoHandler()->addUndoAction(
            std::make_unique<InsertPdfPagesUndoAction>(newPages, position, base, target));
    control->firePageSelected(position);
    control->getScrollHandler()->scrollToPage(position);
    return true;
}

auto InsertPdfPages::insertOwn(Control* control, const std::vector<size_t>& pages, size_t position) -> bool {
    Document* doc = control->getDocument();
    doc->lock_shared();
    const fs::path pdf = doc->getPdfFilepath();
    std::vector<PageRef> newPages;
    for (size_t index: pages) {
        if (auto pdfPage = doc->getPdfPage(index)) {
            auto page = std::make_shared<XojPage>(pdfPage->getWidth(), pdfPage->getHeight());
            page->setBackgroundPdfPageNr(index);
            newPages.push_back(std::move(page));
        }
    }
    position = std::min(position, doc->getPageCount());
    doc->unlock_shared();
    if (newPages.empty()) {
        return false;
    }
    // The background PDF stays: the undo action only inserts and removes the pages
    if (!InsertPdfPagesUndoAction::apply(control, pdf, nullptr, newPages, position)) {
        return false;
    }
    control->getUndoRedoHandler()->addUndoAction(
            std::make_unique<InsertPdfPagesUndoAction>(newPages, position, pdf, pdf));
    control->firePageSelected(position);
    control->getScrollHandler()->scrollToPage(position);
    return true;
}

void InsertPdfPages::run(Control* control) {
    if (control->isReadOnly()) {
        control->notifyReadOnly();
        return;
    }
    Document* doc = control->getDocument();
    InsertPagesDialog::Request request;
    doc->lock_shared();
    std::error_code error;
    if (!doc->isAttachPdf() && doc->getPdfPageCount() > 0 && fs::exists(doc->getPdfFilepath(), error)) {
        request.ownPdf = doc->getPdfFilepath();
        for (size_t i = 0; i < doc->getPageCount(); i++) {
            if (auto pdfPage = doc->getPage(i)->getPdfPageNr(); pdfPage != npos) {
                request.pagesInDocument.insert(pdfPage);
            }
        }
    }
    doc->unlock_shared();

    auto choice = InsertPagesDialog::run(control->getGtkWindow(), request);
    if (!choice || choice->pages.empty()) {
        return;
    }
    const size_t current = control->getCurrentPageNo();
    const size_t position = choice->atEnd || current == npos ? doc->getPageCount() : current + 1;
    if (choice->otherPdf.empty()) {
        insertOwn(control, choice->pages, position);
        return;
    }
    if (!PdfCombiner::isAvailable()) {
        showError(control, _("This build cannot insert pages from other PDFs (QPDF is missing)."));
        return;
    }
    if (doc->getFilepath().empty()) {
        showError(control, _("Please save the document first: the inserted pages are stored in a PDF next to it."));
        return;
    }
    insert(control, choice->otherPdf, choice->pages, position);
}
