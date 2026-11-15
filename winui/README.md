# Xournal Studio (WinUI)

A small WinUI 3 / C# rewrite of Xournal Studio for Windows 11 that reads and writes `.xopp`.
It only covers what is actually used: annotating lecture PDFs and graph-paper notes with the pen.

## Build, test, install

Requires the .NET 10 SDK (no Visual Studio needed).

```powershell
dotnet build winui\XournalStudio                  # debug build
dotnet test winui\XournalStudio.Tests             # file format tests
winui\install.ps1                                 # tests + publish + install
```

`install.ps1` installs to `%LOCALAPPDATA%\Programs\Xournal Studio WinUI` with the Start menu entry
"Xournal Studio (WinUI)". It registers no file associations, so `.xopp` stays with the GTK version.

## Design

- Native controls only: one `InkCanvas` per page (Windows ink: low-latency wet ink, pressure,
  pen eraser tip), `InkToolbar` (pen, highlighter, eraser), `ScrollView` (touch pan, pinch zoom,
  touchpad), `MenuBar`, `InfoBar`, `ContentDialog`, the Windows App SDK file pickers, and
  `Windows.Data.Pdf` for PDF backgrounds. Default WinUI theme with Mica.
- `InkCanvas`/`InkToolbar` exist for WinUI 3 only in the **experimental** Windows App SDK channel
  (`Microsoft.WindowsAppSDK.WinUI` 2.6.6-experimental). The app is unpackaged and self-contained.
- Touch never draws: the presenters take pen and mouse input, touch goes to the `ScrollView`.
- Custom code is limited to:
  - `Model/` – the file format. The XML tree is kept as loaded; only `<stroke>` elements are
    replaced by ink. Unchanged strokes are written back as they were (by ink stroke id), so text,
    images, layers and unknown attributes survive a save. New strokes go to the top layer.
    Saving writes `<file>~` and then replaces the file.
  - `StrokeConverter` – Xournal's per-segment widths ↔ ink pressure. Measured: Windows draws a
    stroke of size `s` at pressure `p` about `s * (0.25 + 1.5 p)` wide.
  - `InkToolSync` – the `InkToolbar` targets one canvas; this copies its tool to all pages.
  - `InkUndoStack` – the `InkCanvas` has no undo; it records collected/erased strokes.
  - `PageView` – page background (color + Xournal ruling, or a PDF page rendered at the current
    zoom near the viewport, freed elsewhere).
- Opening `x.pdf` opens `x.xopp` next to it if it exists; otherwise the PDF is shown and `x.xopp` is
  created on the first save. Saves automatically when the window loses focus or closes.
- Settings: `%LOCALAPPDATA%\XournalStudioWinUI\settings.json` (last file, reopened on start).

## Not supported (yet)

Text, images and LaTeX are kept in the file but not shown. No selection/lasso, shapes,
partial-stroke eraser (the Windows eraser removes whole strokes), page templates other than graph
for new pages, PDF export, or project explorer.

## Testing

Test hooks (environment variables): `XOURNAL_STUDIO_CONFIG_DIR` (settings folder),
`XOURNAL_STUDIO_LOG` (debug log file), `XOURNAL_STUDIO_TEST_PAGE` (start at page N).
Ink only reacts to real pointer input (`InjectSyntheticPointerInput` on the input desktop),
not to posted window messages; rendering can be checked on a hidden desktop with `PrintWindow`.

`UiTests\Run-SandboxTest.ps1 -Document <file.xopp>` runs an end-to-end test in Windows Sandbox
(the sandbox window opens on the desktop): synthetic pen strokes, touch pan, undo/redo, eraser
tool and pen eraser end, highlighter, pinch zoom, Ctrl+S, page insert and save on close, with a
screenshot per step. Afterwards the GTK version exports the result to check it still reads it.
