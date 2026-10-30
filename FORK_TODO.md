# Fork TODO

Feature ideas for this fork that are not implemented yet. See `AGENTS.md` for the scope of the fork.

## Planned

### Arrange pages

Reorder pages like in a PDF tool instead of moving them step by step.

- Today: `MOVE_PAGE_TOWARDS_BEGINNING` / `MOVE_PAGE_TOWARDS_END` move the current page by one position;
  `DUPLICATE_PAGE`, `DELETE_PAGE` and the `NEW_PAGE_*` actions exist. The page preview sidebar
  (`src/core/gui/sidebar/previews`) has no drag and drop.
- Idea: drag and drop in the page preview sidebar (also by touch), possibly multi-selection to move or delete
  several pages at once. Every move must stay undoable (existing undo actions for page moves).

### Insert pages from other PDFs

Insert single pages or ranges from another PDF into the current document.

- Today: a document has exactly one background PDF (`Document::getPdfFilepath()`); `APPEND_NEW_PDF_PAGES` only adds
  pages of that PDF that are not in the document yet. Pages from other PDFs cannot be inserted.
- Options to evaluate:
  - Merge the pages into a new combined background PDF (e.g. with QPDF, already a dependency) stored next to the
    `.xopp`, and remap the background page numbers. Keeps vector quality and text selection; needs care with
    the source/export file nesting in the project explorer and with the recorded export name.
  - Render the inserted pages as image backgrounds. Simple, but loses vectors, text selection and search.
- Needs a page picker dialog (thumbnails, ranges) and a target position.

## Ideas put on hold

Discussed during development and deliberately not done yet.

- **Upstream contributions**: the touch fixes (taps on buttons on Windows, forgetting touches that ended without an
  end event — xournalpp/xournalpp#5289, applying touch motion once per frame) and the save/export speedups are not
  fork specific and could be proposed upstream.
- **Momentum scrolling** in Xournal's own touch handling (GTK's kinetic scrolling is disabled because of side effects).
- **Zoom start threshold**: `touchZoomStartThreshold` is 0, so two-finger panning always zooms slightly; a default
  around 5–8 % might feel calmer.
- **Two-finger tap to undo**, **zoom snapping** to 100 % / page width at the end of a pinch.
- **Explorer refresh after saving**: Xournal briefly creates and deletes `<name>.xopp~` while saving, which makes
  the project explorer rebuild its tree; rebuilding only when the visible entries changed would avoid that.
- **Isolated touch testing**: injected touch only reaches the visible desktop; Windows Sandbox or a virtual display
  could allow automated touch tests without interrupting the user.
