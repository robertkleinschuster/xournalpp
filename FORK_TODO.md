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

A task list with reminders inside Xournal++, stored as plain [todo.txt](https://github.com/todotxt/todo.txt) files in
the project folder, so the tasks are synced with the existing Git sync button and stay readable and editable with
other todo.txt apps (e.g. on the phone).

**Files**

- Scan the whole project for task files: `todo.txt` and `*.todo.txt` (e.g. `GET/todo.txt`, `Mathe.todo.txt`).
  Skip `.git` and hidden folders. Keep the list current with the explorer's existing file monitors.
- Completed tasks stay in the file marked with `x` (optionally archived to a `done.txt` next to it).
- A task belongs to the file it is in. Tasks created from a document go into the nearest `todo.txt` up the folder
  tree (create it at the project root if there is none).
- Show task files in the project explorer with their own icon; opening one shows its tasks instead of opening it
  as a document.

**Format** — standard todo.txt plus `key:value` tags, chosen to stay compatible with common apps:

- Standard: `x` done (with completion date), `(A)`–`(Z)` priority, creation date, `+project`, `@context`.
- Dates: `due:2026-11-05` (also understood by Simpletask and others).
- Times: `due:2026-11-05T14:00` would break some apps' date parsing; alternatively keep `due:` a pure date and add
  `time:14:00`. To decide.
- Reminders: `remind:2026-11-05T13:30` or relative to the due time (`remind:-30m`, `remind:-1d`); several reminders
  per task possible. Without a `remind:` tag, an optional default (e.g. on the due day at 08:00 or 15 min before).
- Recurrence: `rec:1w`, `rec:+1d` (Simpletask syntax) for weekly lectures or exercise sheets; checking off creates
  the next occurrence.
- Start/threshold: `t:2026-11-01` hides the task until that date (also standard in Simpletask).
- Document links: `doc:GET/VO/VO8.xopp` (path relative to the task file, URL-encoded if it has spaces), optionally
  with a page: `doc:GET/VO/VO8.xopp#p12`. PDFs and exported PDFs resolve to their `.xopp` like in the explorer.

**User interface**

- A "Tasks" tab in the existing sidebar (next to the project explorer), collecting the tasks of all task files of
  the project. Groups: Overdue, Today, Upcoming, No date, Completed (collapsed). Group or filter by `+project`,
  `@context` or file (e.g. per course). Search field like in the explorer.
- Check off with one tap (touch-friendly, large hit targets); swipe or context menu for edit/delete/postpone.
- Quick add field with simple natural-language parsing: "Übungsblatt 3 abgeben morgen 14:00 +GET" → due date, time,
  project tag. German and English keywords (heute, morgen, Montag, nächste Woche, in 2 Tagen).
- Edit dialog for the remaining fields (priority, reminders, recurrence, linked document).
- Clicking a linked task opens the document at the linked page.
- Respect the "Keep UI changes focused" rule in `AGENTS.md`: one sidebar tab, no extra windows.

**Integration with documents**

- "New task for this page" in the page context menu and as an action with a keyboard shortcut: pre-fills the
  `doc:` link with the current document and page.
- Create a task from a selection: the selected PDF text becomes the task text; for handwriting, the task text is
  typed and the selection's page is linked.
- In the project explorer, documents with open tasks get a small indicator (count or dot; red when overdue);
  the tooltip lists the tasks.
- The page preview sidebar marks pages that have linked tasks.
- Tasks follow renames and moves done in the project explorer (rewrite their `doc:` paths); broken links are shown
  as such instead of being dropped.

**Reminders and Windows notifications**

- While Xournal++ runs: a timer for the next due reminder (recomputed when task files change, after sleep/resume
  and on clock or time zone changes); missed reminders are shown once at startup ("3 tasks overdue").
- Notifications as Windows toasts with actions: "Open" (opens the linked document / the task), "Done",
  "Snooze 10 min / 1 h / tomorrow". Snoozing writes the new `remind:` time back to the file.
- Implementation options:
  - GLib's `GNotification` — evaluate what its Windows backend supports (actions, activation when the app is
    closed); probably too limited.
  - WinRT toasts (`Windows.UI.Notifications`, e.g. via C++/WinRT or the plain COM interfaces from MinGW). Needs an
    AppUserModelID on the Start menu shortcut, which the fork's installer already creates; action buttons need a
    COM activator or a protocol/command-line activation (`xournalpp-fork.exe --task <id>`).
  - Fallback: a tray icon balloon (`Shell_NotifyIcon`).
- Reminders while Xournal++ is closed:
  - Schedule the toasts in advance with `ScheduledToastNotification` — Windows shows them even when the app is not
    running; reschedule whenever task files change. Probably the best option.
  - Alternatives: a small background/tray helper started with Windows, or Task Scheduler entries.
- Do-not-disturb and focus assist are handled by Windows; a fork setting to switch reminders off entirely.

**Robustness**

- Write changes line by line (only the edited task), keep unknown tags, comments and line order, so edits from
  other apps and Git merges stay clean; detect external changes before writing (reload and reapply the edit).
- Task ids for notifications and links: content-based (text + creation date), since line numbers change.
- Encoding UTF-8, line endings as found in the file.
- Unit tests for the parser/writer, date and recurrence handling (DST changes, end of month), and relative
  reminders.

**Possible later**

- Calendar/agenda view (week overview), export as `.ics` or subscription for the phone calendar.
- Deadlines from course PDFs (e.g. "Abgabe bis …" in an exercise sheet) suggested as tasks.
- Statistics per course (open/done), progress in the explorer.

## Ideas put on hold

Discussed during development and deliberately not done yet. Dropped: two-finger tap to undo (too error-prone) and
zoom snapping at the end of a pinch.

- **Isolated touch testing**: injected touch only reaches the visible desktop; Windows Sandbox or a virtual display
  could allow automated touch tests without interrupting the user.
