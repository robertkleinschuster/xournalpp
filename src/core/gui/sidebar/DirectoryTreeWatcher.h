/*
 * Xournal++
 *
 * Fork: reports changes anywhere below a folder, with one recursive watch
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "filesystem.h"

/**
 * Watches a whole folder tree for created, deleted, renamed and written entries. On Windows one
 * ReadDirectoryChangesW call covers all subfolders, also collapsed and empty ones, and the watching thread sleeps
 * until the system reports a change: nothing polls. GLib's monitors only watch single folders.
 *
 * Elsewhere (or if the folder cannot be watched like this, e.g. on some network shares) isActive() is false and the
 * caller watches the folders itself.
 */
class DirectoryTreeWatcher final {
public:
    struct Change {
        fs::path path;  ///< absolute
        /// A name appeared or disappeared (created, deleted, renamed); false: the entry was written
        bool nameChanged;
    };
    /// Called on the GTK main thread. overflow: changes were lost (the system's buffer overflowed), rescan.
    using Callback = std::function<void(const std::vector<Change>& changes, bool overflow)>;

    DirectoryTreeWatcher(fs::path root, Callback callback);
    ~DirectoryTreeWatcher();
    DirectoryTreeWatcher(const DirectoryTreeWatcher&) = delete;
    DirectoryTreeWatcher& operator=(const DirectoryTreeWatcher&) = delete;

    auto isActive() const -> bool;
    auto getRoot() const -> const fs::path& { return root; }

    struct Impl;

private:
    fs::path root;
    std::unique_ptr<Impl> impl;
};
