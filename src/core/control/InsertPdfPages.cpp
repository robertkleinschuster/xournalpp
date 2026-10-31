#include "InsertPdfPages.h"

#include <fstream>    // for ofstream
#include <memory>     // for make_shared, make_unique
#include <optional>   // for optional
#include <stdexcept>  // for runtime_error
#include <utility>    // for move

#include <gtk/gtk.h>

#include "control/Control.h"                // for Control
#include "control/ScrollHandler.h"          // for ScrollHandler
#include "gui/dialog/XojOpenDlg.h"          // for showInsertPdfPagesDialog
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

namespace {

void showError(Control* control, const std::string& message, const std::string& detail = {}) {
    XojMsgBox::showErrorToUser(control->getGtkWindow(), detail.empty() ? message : message + "\n\n" + detail);
}

/// The pages to insert (0-based) and whether they go to the end, or nullopt if cancelled
struct Choice {
    std::vector<size_t> pages;
    bool atEnd = false;
};

auto askForPages(Control* control, const fs::path& pdf, size_t pageCount) -> std::optional<Choice> {
    auto* dialog =
            gtk_dialog_new_with_buttons(_("Insert Pages from PDF"), control->getGtkWindow(), GTK_DIALOG_MODAL,
                                        _("Cancel"), GTK_RESPONSE_CANCEL, _("Insert"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    auto* info = gtk_label_new(FC(_F("“{1}” has {2} pages.") % Util::toUtf8(pdf.filename()) % pageCount));
    gtk_label_set_xalign(GTK_LABEL(info), 0);
    gtk_label_set_line_wrap(GTK_LABEL(info), true);
    gtk_box_pack_start(GTK_BOX(box), info, false, false, 0);

    auto* entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
    // Not as placeholder: GTK hides it while the entry has the focus, i.e. right away
    auto* entryLabel = gtk_label_new_with_mnemonic(_("_Pages (e.g. 1-3, 5; empty for all):"));
    gtk_label_set_mnemonic_widget(GTK_LABEL(entryLabel), entry);
    gtk_label_set_xalign(GTK_LABEL(entryLabel), 0);
    gtk_box_pack_start(GTK_BOX(box), entryLabel, false, false, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, false, false, 0);

    auto* afterCurrent = gtk_radio_button_new_with_mnemonic(nullptr, _("After the _current page"));
    auto* atEnd = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(afterCurrent), _("At the _end"));
    gtk_box_pack_start(GTK_BOX(box), afterCurrent, false, false, 0);
    gtk_box_pack_start(GTK_BOX(box), atEnd, false, false, 0);

    auto* error = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(error), 0);
    gtk_label_set_line_wrap(GTK_LABEL(error), true);
    gtk_style_context_add_class(gtk_widget_get_style_context(error), "error");
    gtk_box_pack_start(GTK_BOX(box), error, false, false, 0);

    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, true, true, 0);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, -1);
    gtk_widget_show_all(dialog);
    gtk_widget_hide(error);

    std::optional<Choice> choice;
    while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        if (auto pages = PdfCombiner::parsePageRanges(gtk_entry_get_text(GTK_ENTRY(entry)), pageCount)) {
            choice = Choice{std::move(*pages), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(atEnd)) != 0};
            break;
        }
        gtk_label_set_text(GTK_LABEL(error),
                           _("Please enter page numbers between 1 and the number of pages, e.g. 1-3, 5."));
        gtk_widget_show(error);
        gtk_widget_grab_focus(entry);
    }
    gtk_widget_destroy(dialog);
    return choice;
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
    const auto dir = xopp.parent_path();
    const auto stem = Util::toUtf8(xopp.stem());
    std::error_code error;
    // Named like the document, the explorer shows it as the document's PDF and renames it along with it
    if (auto own = dir / Util::fromUtf8(stem + ".pdf"); !fs::exists(own, error)) {
        return own;
    }
    for (int n = 1;; n++) {
        const auto name = n == 1 ? FS(_F("{1} (combined).pdf") % stem) : FS(_F("{1} (combined {2}).pdf") % stem % n);
        if (auto candidate = dir / Util::fromUtf8(name); !fs::exists(candidate, error)) {
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

    control->getUndoRedoHandler()->addUndoAction(
            std::make_unique<InsertPdfPagesUndoAction>(newPages, position, base, target));
    control->firePageSelected(position);
    control->getScrollHandler()->scrollToPage(position);
    return true;
}

void InsertPdfPages::run(Control* control) {
    if (control->isReadOnly()) {
        control->notifyReadOnly();
        return;
    }
    if (!PdfCombiner::isAvailable()) {
        showError(control, _("This build cannot insert pages from other PDFs (QPDF is missing)."));
        return;
    }
    if (control->getDocument()->getFilepath().empty()) {
        showError(control, _("Please save the document first: the inserted pages are stored in a PDF next to it."));
        return;
    }
    xoj::OpenDlg::showInsertPdfPagesDialog(control->getGtkWindow(), control->getSettings(), [control](fs::path pdf) {
        size_t pageCount = 0;
        try {
            pageCount = PdfCombiner::pageCount(pdf);
        } catch (const std::exception& e) {
            showError(control, _("The PDF could not be read."), e.what());
            return;
        }
        auto choice = askForPages(control, pdf, pageCount);
        if (!choice) {
            return;
        }
        const size_t current = control->getCurrentPageNo();
        const size_t position = choice->atEnd || current == npos ? control->getDocument()->getPageCount() : current + 1;
        insert(control, pdf, choice->pages, position);
    });
}
