/*
 * Xournal++
 *
 * Undo moving a page to another position (fork: drag and drop in the page preview sidebar)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <string>   // for string
#include <vector>   // for vector

#include "model/PageRef.h"  // for PageRef

#include "UndoAction.h"  // for UndoAction

class Control;

class MovePageUndoAction: public UndoAction {
public:
    MovePageUndoAction(const PageRef& page, size_t from, size_t to);

    bool undo(Control* control) override;
    bool redo(Control* control) override;
    std::vector<PageRef> getPages() override;
    std::string getText() override;

    /**
     * Moves the page at from to position to (both 0-based, to counted after removing the page), notifies the
     * listeners and selects it. Returns false if a position is out of range.
     */
    static bool move(Control* control, size_t from, size_t to);

private:
    PageRef page;
    size_t from;
    size_t to;
};
