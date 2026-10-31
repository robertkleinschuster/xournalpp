#include "MovePageUndoAction.h"

#include <mutex>  // for unique_lock

#include "control/Control.h"        // for Control
#include "control/ScrollHandler.h"  // for ScrollHandler
#include "model/Document.h"         // for Document
#include "util/i18n.h"              // for _

MovePageUndoAction::MovePageUndoAction(const PageRef& page, size_t from, size_t to):
        UndoAction("MovePageUndoAction"), page(page), from(from), to(to) {}

auto MovePageUndoAction::move(Control* control, size_t from, size_t to) -> bool {
    Document* doc = control->getDocument();
    std::unique_lock lock(*doc);
    const size_t count = doc->getPageCount();
    if (from >= count || to >= count || from == to) {
        return false;
    }
    PageRef page = doc->getPage(from);
    doc->deletePage(from);
    doc->insertPage(page, to);
    lock.unlock();

    control->firePageDeleted(from);
    control->firePageInserted(to);
    control->firePageSelected(to);
    control->getScrollHandler()->scrollToPage(to);
    return true;
}

auto MovePageUndoAction::undo(Control* control) -> bool {
    this->undone = true;
    return move(control, to, from);
}

auto MovePageUndoAction::redo(Control* control) -> bool {
    this->undone = false;
    return move(control, from, to);
}

auto MovePageUndoAction::getPages() -> std::vector<PageRef> { return {page}; }

auto MovePageUndoAction::getText() -> std::string { return _("Move page"); }
