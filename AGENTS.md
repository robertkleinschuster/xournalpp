# Xournal Studio (Xournal++ Fork) Work Log

## Repository

- Fork: https://github.com/robertkleinschuster/xournalpp
- Upstream: https://github.com/xournalpp/xournalpp
- Local checkout: C:\Users\rober\xournalpp
- Branch: master
- Use case: Xournal++ on a Windows convertible for university.

## Fork-specific behavior

- Keep the fork identity and configuration directory separate from upstream so both installations can run side by side.
- The fork is named "Xournal Studio": window title, About dialog, exe version info, installation folder and Start menu entry (install-fork.ps1 removes the old "Xournal++ Fork" installation). Its icon (ui/pixmaps/com.github.xournalpp.xournalpp.svg, also the .exe icon) is the Xournal++ icon on a blue grid tile. The config folder stays xournalpp-fork so existing settings keep working.
- Preserve the touch-sequence reset on window deactivation. It prevents stale touch state from breaking touchscreen scroll and pinch zoom.
- Use graph paper as the default for new documents.
- A PDF without its .xopp opens read-only (no files are created) with an info bar; only its "Enable Editing" button creates the .xopp next to the PDF or opens an existing one. Read-only is enforced in ActionDatabase (EDITING_ACTIONS), XojPageView's press handlers, PageBackgroundChangeController, drag and drop, and Control::close discards. The annotated PDF is only exported once the document has annotations (AnnotationFiles::hasAnnotations) or an export already exists.
- The "Copy PDF Area" tool (TOOL_COPY_PDF_AREA, in the PDF tool dropdown and the Tools menu) copies the rectangle drawn on a PDF page as soon as it is released and shows it for a moment (PdfFloatingToolbox::copyFinishedArea); the floating toolbox has no copy-area button. Copying renders the area into a one-page vector PDF stored in a TexImage without LaTeX source (PdfElemSelection::createAreaElement) and puts it on the clipboard like a copied selection (ClipboardHandler::copyElement, EditSelection::serializeElement). That PDF contains the whole page's content clipped to the area, so above 256 KB (e.g. scanned pages) a 300-DPI PNG Image is used instead if it is less than half the size. Other applications get the area's text and PNG/SVG images, rendered only when requested. TexImages without LaTeX source (TexImage::hasLatexSource) are not opened or replaced by the LaTeX editor. A finalized area selection shows the area lightly with an outline and the text lines that marking applies to.
- Touch: StuckTouchIdFix works around GDK 3 (win32) ignoring a reused touch id forever after a cancelled/low-confidence touch (symptom: every gesture needs one finger more). Touchscreen and touchpad swipes share ScrollMomentum, so both glide on; a pinch does not.
- Saving writes "<name>.xopp~" and only replaces the .xopp once it is complete (SaveHandler::saveTo), so a failed save or crash keeps the previous version; there is no separate backup copy. When switching documents in the project explorer fails to save the current one, it stays open (DocumentAutoSaver::saveNow reports the result). Documents are gzip-compressed with libdeflate when available (GzOutputStream, about 3x faster than zlib). Automatic saves (focus loss, switching documents) skip files loaded with errors or a newer file format (Control::isAutoSaveAllowed) until the user saves them. An explicit save always writes the file, like upstream; an export that was up to date stays marked up to date.
- The annotated PDF is exported from a deep copy of the document on its own thread (AnnotationExporter), so the window is not blocked; requests during a running export wait for it. Its time is dominated by cairo_show_page, not by QPDF. test/benchmarks/ownFilesBenchmarkTest.cpp measures load, save and export of real documents listed in XOPP_BENCHMARK_FILES.
- Git commands run in a job object and are terminated with their helpers after a timeout (2 min background fetch, 10 min otherwise); a pull failure is only reported as a conflict when git reports one. .xopp headers read by the explorer are cached by exact file stamp (AnnotationFiles cachedXoppTag).
- The sidebar keeps the tab the user chose while it has content (it does not jump to the project explorer). At startup the explorer's last document is only reopened with the upstream "Autoload most recent" setting; exported/source PDFs only redirect to their .xopp with "Autoload pdf.xoj". Renaming a PDF in the explorer relinks the document annotating it (AnnotationFiles::relinkSourcePdf). The explorer search runs on a worker thread.
- On Windows the main window turns off pen flicks, press-and-hold and pen feedback (PenFeedbackFix). Ctrl + two-finger touchpad swipe zooms. Lua functions that modify the document raise an error while it is read-only.
- UI for a right-handed convertible user: the menu bar sits in a dark GTK header bar (MainWindow::moveMenubarToTitleBar) and stays visible in tablet mode (the pen reaches every function through it); nothing interactive at the right edge (e.g. the read-only bar's button is on the left). ConvertibleSupport hides the sidebar in portrait and opens the touch keyboard for text fields tapped in tablet mode. The sidebar separator is wide and draggable by touch (TouchPanedHandle). The Insert page toolbar button has no dropdown; the PDF tool dropdown defaults to Copy PDF Area. DocumentAutoSaver also saves after 30 s without input; the explorer pauses git polling in the background.
- Keep UI changes focused: a project-folder browser in the existing sidebar and persistent document tabs. The startup recent-documents overlay has been removed. Git support stays minimal: status colors in the explorer tree (green new, blue modified) and a single sync button (commit, pull --rebase, push) shown only for git repositories. Do not add further Git UI or replace the normal document view.

## Windows build

- The local MSYS2/UCRT64 build is configured in build/.
- Run windows-setup/build-local.ps1 for an incremental build, package, and side-by-side installation.
- Use -SkipInstall if the fork is running; install-fork.ps1 refuses to overwrite files locked by a running process.
- The manually triggered CI workflow is .github/workflows/fork-windows-build.yml.

## Repository cautions

- Fork-specific UI strings have German translations appended to po/de.po (the file is CRLF in the working copy; validate with `msgfmt --check`). Never stage translation-file deletions reported by git status.
- Device-level touchscreen testing has not been performed.
