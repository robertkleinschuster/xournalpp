# Xournal++ Fork Work Log

## Repository

- Fork: https://github.com/robertkleinschuster/xournalpp
- Upstream: https://github.com/xournalpp/xournalpp
- Local checkout: C:\Users\rober\xournalpp
- Branch: master
- Use case: Xournal++ on a Windows convertible for university.

## Fork-specific behavior

- Keep the fork identity and configuration directory separate from upstream so both installations can run side by side.
- Preserve the touch-sequence reset on window deactivation. It prevents stale touch state from breaking touchscreen scroll and pinch zoom.
- Use graph paper as the default for new documents.
- Keep UI changes focused: a project-folder browser in the existing sidebar and persistent document tabs. The startup recent-documents overlay has been removed. Do not add Git actions or replace the normal document view.

## Windows build

- The local MSYS2/UCRT64 build is configured in build/.
- Run windows-setup/build-local.ps1 for an incremental build, package, and side-by-side installation.
- Use -SkipInstall if the fork is running; install-fork.ps1 refuses to overwrite files locked by a running process.
- The manually triggered CI workflow is .github/workflows/fork-windows-build.yml.

## Repository cautions

- Fork-specific UI strings have German translations appended to po/de.po (the file is CRLF in the working copy; validate with `msgfmt --check`). Never stage translation-file deletions reported by git status.
- Device-level touchscreen testing has not been performed.
