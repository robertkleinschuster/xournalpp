# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is a personal fork of Xournal++ (C++20, GTK3, CMake) used on a Windows convertible. **Read `AGENTS.md` first** — it holds the fork's work log, the behaviors that must be preserved, and the scope limits for UI changes.

## Build and test (Windows, MSYS2/UCRT64)

The build tree in `build/` is already configured with Ninja, `RelWithDebInfo`, and `-DENABLE_GTEST=ON -DDOWNLOAD_GTEST=on`.

```powershell
# Incremental build + package + side-by-side install into %LOCALAPPDATA%\Programs\Xournal Studio
windows-setup\build-local.ps1
# Use this while the fork is running (installer refuses to overwrite locked files)
windows-setup\build-local.ps1 -SkipInstall
```

For a quick compile check without packaging, put `C:\msys64\ucrt64\bin` and `C:\msys64\usr\bin` on `PATH` (with `MSYSTEM=UCRT64`) and run `cmake --build build`.

Tests are `EXCLUDE_FROM_ALL`, so build them explicitly:

```sh
cmake --build build --target test-units
build/test/test-units                                   # all unit tests
build/test/test-units --gtest_filter='SuiteName.*'      # single suite / test
ctest --test-dir build -R <regex>                       # via ctest (gtest_discover_tests)
```

- New test files go anywhere under `test/unit_tests/`; they are picked up by a `GLOB_RECURSE`, so touch `test/CMakeLists.txt` to force CMake to re-glob. Tests link `xoj::core`, so any header from `src/` can be included.
- `test-gtk-integration` (`test/gtk_tests/`) and `benchmarks` are separate opt-in targets.

## Formatting

`clang-format` (config in `.clang-format`) is enforced by CI on changed lines of upstream PRs; run it on files you touch. `.clang-tidy` exists for lint (`-Dxournalpp_LINT=ON`).

clang-format is installed in MSYS2 (`mingw-w64-ucrt-x86_64-clang-tools-extra`). Format only the fork's changed lines, never generated files such as `enums/generated/*`:

```sh
python3 -I /c/msys64/ucrt64/bin/git-clang-format --binary /c/msys64/ucrt64/bin/clang-format.exe 1a7c9aa59 -- src test
```

`1a7c9aa59` is the upstream commit the fork branched from.

## Architecture

- `src/util/` (`xoj::util`): GTK/GLib-independent helpers — paths (`PathUtil.cpp`, which also holds the fork's config folder name), RAII wrappers for GLib/GTK ref-counted objects (`util/raii/`), serialization, logging.
- `src/core/` (`xoj::core`), split by layer:
  - `model/` — document data: `Document`, `XojPage`, `Layer`, `Element` subclasses (`Stroke`, `Text`, `Image`, `TexImage`). `DocumentHandler`/`DocumentListener` broadcast changes.
  - `view/` — rendering of model elements with Cairo (`DocumentView`, per-element views).
  - `gui/` — GTK widgets: `MainWindow`, `XournalView` (the page canvas), `PageView`, `Layout`, sidebars (`gui/sidebar/`, including the fork's `ProjectExplorerSidebar`), toolbars/menus, dialogs, and `inputdevices/` (pen/touch/mouse handlers, including `TouchInputHandler`).
  - `control/` — application logic. `Control` is the central object wiring document, settings, tools, undo, zoom, scroll, and the window together; `XournalMain` is startup. Subfolders hold tools, settings (`Settings.cpp` sets the default page template — graph paper in this fork), file I/O (`xojfile/`), jobs/scheduler, LaTeX, shape recognizer, zoom.
  - `control/actions/ActionDatabase` + `enums/Action.enum.h` + `ActionProperties.h`: every menu/toolbar command is a `GAction` keyed by the `Action` enum; adding a command means extending the enum (and the name map `enums/generated/Action.NameMap.generated.h`, produced by `enums/generateConvert.php` or edited by hand), adding an `ActionProperties` specialization, and referencing it in `ui/mainmenubar.xml` or the toolbar config.
  - `undo/` — undo/redo actions; model-modifying operations should push an `UndoAction`.
  - `plugin/` — Lua plugin API; bundled plugins live in `plugins/`.
- `src/exe/` — `main` and the Windows wrapper executable.
- `ui/` — Glade/GtkBuilder files (`main.glade`, `sidebar.ui`, dialogs), `mainmenubar.xml`, icons, and CSS.
- Fork identity: `APPLICATION_ID` in `src/config-dev.h.in` and `CONFIG_FOLDER_NAME` (`xournalpp-fork`) in `src/util/PathUtil.cpp`; keep these distinct from upstream.
