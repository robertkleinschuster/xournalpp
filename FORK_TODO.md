# Fork TODO

Feature ideas for this fork that are not implemented yet. See `AGENTS.md` for the scope of the fork.

## Planned

### Robust pen and touch input in GTK widgets

GTK 3 on Windows handles pen and touch outside the canvas poorly: menu bar menus sometimes stay open when tapping
the next menu or beside them, and tooltips shown while the pen hovers can stay on screen and block the window.
`InputSafety` hides tooltips on pen/touch contact and when the pen leaves, and releases grabs of menus that are no
longer shown; `TouchButtonFix` handles taps on buttons and the menu bar.

- Diagnose with the input log: create an empty file `input-debug` in the config folder, reproduce, then read
  `input-debug.log` there (pen, touch, menu and grab events).
- Goal: no state in which a menu, tooltip or grab can block the window; menus switch and close reliably by touch
  and pen.

### Follow PDF links without switching tools

Links in a PDF (references in the text, its own table of contents, URLs) react to the PDF tools: a tap with one of
them - also with "Copy PDF Area", the default of the PDF tool button in the toolbar - shows a popover with "Go to …"
/ "Open link" (`XojPageView::displayLinkPopover`, only called when `isPdfSelectionTool`; verified). With the pen,
highlighter or lasso nothing happens, on purpose, since every stroke over a link would jump. Done: a short tap of
one finger (no movement, under 500 ms) on a link shows the popover with every tool (`TouchInputHandler::handleTap`).
Optional improvements:

- With the pen, a long press on a link; the "Select object" tool follows links on tap.
- Show where links are, e.g. a faint underline or outline of link areas (toggle), so they can be found.
- After following an internal link, a way back (upstream has NAVIGATE_BACK / NAVIGATE_FORWARD; consider a back
  button in the popover or the toolbar while a jump is pending).
- The PDF outline (bookmarks) is in the sidebar's second tab and works; consider a hint or making it easier to find.

### Arrange pages

Done: drag and drop of a miniature in the page preview sidebar moves the page (`PageReorderGesture`,
`Control::movePage`, undoable via `MovePageUndoAction`). Mouse and pen drag right away, a finger after holding the
miniature for 0.4 s (a swipe still scrolls). Verified with the mouse on the hidden desktop; touch to be confirmed on
the device. Open:

- Multi-selection to move or delete several pages at once.

### Insert pages from other PDFs

Done: "Journal → Insert Pages from PDF…" (`InsertPdfPages`, `PdfCombiner`, undoable via
`InsertPdfPagesUndoAction`). The chosen pages are appended to a combined copy of the background PDF next to the
document, marked in its document info (`/XournalStudioCombined`); the source PDF stays untouched. Open:

- A thumbnail picker instead of typing page ranges.
- Removing pages that are no longer used from the combined PDF (it only grows; undo keeps them).

### Tasks with reminders (todo.txt)

Done (see `AGENTS.md`): the "Tasks" sidebar tab (`TasksSidebar`) over all `todo.txt` / `*.todo.txt` files of the
project, the model (`control/tasks/TodoTxt`, `TaskFiles`, unit tested), quick add with German/English dates, edit
dialog, recurrence, document/page links ("Journal → New Task for This Page", Ctrl+Shift+K), task files in the
explorer, and reminders while the app runs (`TaskNotifier`, notification area balloons shown as toasts).
Decided: `due:` stays a pure date, the time is a separate `time:` tag. Still open:

- Confirm on the device that the reminder notifications appear (they cannot be shown from the hidden test desktop:
  the notification area does not exist there, and the screen was locked during the test).
- Reminders while Xournal Studio is closed: `ScheduledToastNotification` (needs a WinRT/COM toast setup and the
  Start menu shortcut's AppUserModelID) or a small background helper.
- Toast actions ("Done", "Snooze"); a setting to switch reminders off (today: Windows notification settings / Focus
  Assist); a `remind:` field in the edit dialog.
- Filter or group by `+project` / `@context`, search; swipe gestures; a calendar/agenda view and `.ics` export.
- Mark pages with linked tasks in the page previews; indicators for open tasks in the explorer; rewrite `doc:`
  links when documents are renamed or moved in the explorer.

## Ideas put on hold

Discussed during development and deliberately not done yet. Dropped: two-finger tap to undo (too error-prone) and
zoom snapping at the end of a pinch.

- **Isolated touch testing**: injected touch only reaches the visible desktop; Windows Sandbox or a virtual display
  could allow automated touch tests without interrupting the user.
