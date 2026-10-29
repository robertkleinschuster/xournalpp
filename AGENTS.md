# Xournal++ Fork Work Log

## Repository

- GitHub fork: https://github.com/robertkleinschuster/xournalpp
- Local checkout: `C:\Users\rober\xournalpp`
- Current branch: `master`
- Upstream project: https://github.com/xournalpp/xournalpp
- The user's use case is a Windows convertible for university, primarily Xournal++ and Git-versioned PDF/Xournal++ documents.
- A project browser with Git integration is a planned feature and has not been implemented yet.

## Work completed

- Created the GitHub fork and cloned it locally.
- Inspected the current source. Scrollbar visibility controls already exist under the View settings (`cbHideHorizontalScrollbar` and `cbHideVerticalScrollbar`). The Touchscreen settings also already contain a switch to disable GTK's built-in inertial scrolling.
- Added touch-state reset when the main window loses activation. This clears stale touchscreen sequences that can remain after switching to another app and interfere with subsequent single-finger scrolling or pinch zoom.
- Gave the fork a distinct GTK application ID (`com.github.robertkleinschuster.xournalppfork`) and configuration directory (`xournalpp-fork`) so its app identity and settings are separate from upstream.
- Added a blue `F` badge to the Xournal++ SVG application icon. The Windows executable icon is generated from this SVG by the Windows build.
- Added `.github/workflows/fork-windows-build.yml`, a manually triggered workflow that builds the Windows installer and publishes a portable Windows package. The portable package is intended to run next to the upstream installation without replacing it.
- Committed and pushed the implementation as `3a8937d75` (`Add fork identity and reset stale touch gestures`).

## Build and validation status

- Local Windows compilation has not been possible: CMake, Ninja, and MSYS2 are not installed in this environment.
- `git diff --check` was attempted, but the checkout has pre-existing inaccessible/missing tracked files under `po/`; the Git working tree reports those translations as deleted. Do not stage or commit the `po/` deletions. The source edits themselves were committed separately.
- GitHub Actions Windows workflow run: https://github.com/robertkleinschuster/xournalpp/actions/runs/40941963968
- The run was queued at the time this file was written. Check its result before claiming that the fork builds or downloading its portable artifact.
- No device-level touchscreen testing has been performed. The reset on window deactivation is a targeted fix for stale touch sequences, not yet a verified resolution on the user's device.

## Next steps

1. Check the Windows workflow result and fix any build failures.
2. If it succeeds, download the portable artifact and explain how to run it alongside the upstream installation.
3. Test the touch behavior on the user's convertible if available.
4. Continue the project-browser and Git-integration design/implementation after the user confirms or refines that feature.
