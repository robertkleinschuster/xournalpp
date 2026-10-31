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
highlighter or lasso nothing happens, on purpose, since every stroke over a link would jump. Optional improvements:

- Make them reachable while writing, e.g.: a tap (no movement) with the finger on a link shows the popover; with the
  pen, a long press on a link; the "Select object" tool follows links on tap.
- Show where links are, e.g. a faint underline or outline of link areas (toggle), so they can be found.
- After following an internal link, a way back (upstream has NAVIGATE_BACK / NAVIGATE_FORWARD; consider a back
  button in the popover or the toolbar while a jump is pending).
- The PDF outline (bookmarks) is in the sidebar's second tab and works; consider a hint or making it easier to find.

### Clipboard buttons in the toolbar

Copy and paste without a keyboard, e.g. to insert a copied PDF area with the pen or by touch in tablet mode.

- Add COPY and PASTE (perhaps CUT) to the default "Xournal Studio" toolbar right next to the PDF tool, whose default
  is Copy PDF Area: copy an area, then paste it with the button next to it. The items exist already (`CUT`, `COPY`,
  `PASTE` in `ToolMenuHandler`); update `resources-templates/toolbar.ini.in`.
- Paste should place the content where it is visible: in the middle of the visible part of the current page (or at
  the last tap position) instead of at a position that may be scrolled out of view; check what upstream does for
  pasting without a pointer position.
- The buttons follow the clipboard state (paste disabled when there is nothing to paste); check this also works for
  content copied in other applications (images, text).

### Project explorer: move files

Move files and folders to another folder of the project, by drag and drop in the tree (also by touch, e.g. after a
long press) and with a "Move to…" entry in the context menu that offers the project's folders.

- A document moves together with its group like when renaming: source PDF (if named like the document) and
  exported PDF; the document records the new paths (`AnnotationFiles::relinkDocument`). An open document is saved,
  closed and reopened, as in `renameDocumentGroup`. Undo the moves if one fails.
- Moving a PDF that a document in another folder uses as background relinks that document.
- Refuse targets that exist already and moving a folder into itself.

### Project explorer: show deleted files

Files that git still tracks but that were deleted in the working tree (they are part of the next sync) have no row
today. Show them in their folder until the next sync, so it is visible what the sync will delete.

- Gray, but distinguishable from the unsupported file formats that are already shown dimmed: e.g. a struck-through
  name and/or a dedicated icon (a document icon with a small "x" or trash badge), plus a tooltip ("Deleted – removed
  from the repository with the next sync").
- Context menu: "Restore" (`git restore` / `git checkout -- <path>`), which brings the file back.
- Not openable; excluded from search results, or shown there marked the same way.

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
