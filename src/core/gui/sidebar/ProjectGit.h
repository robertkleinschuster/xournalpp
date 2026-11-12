/*
 * Xournal++
 *
 * Fork: git support for the project explorer, built on libgit2 (no git installation needed)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>

namespace ProjectGit {

enum class FileState { Unchanged, Added, Modified };

struct Status {
    /// Changed files, keyed by pathKey() of their absolute path
    std::map<std::string, FileState> files;
    /// Files git tracks that were deleted in the working tree (absolute paths); they have no entry in files
    std::vector<std::filesystem::path> deleted;
    size_t changedFiles = 0;
    bool hasUpstream = false;
    size_t ahead = 0;
    size_t behind = 0;
};

struct RepoStatus {
    /// Whether the folder is part of a git repository; false also if it could not be read (see error)
    bool isRepo = false;
    std::filesystem::path toplevel;  ///< the repository's working directory
    Status status;
    std::string error;
};

enum class Outcome {
    Ok,
    Conflict,      ///< pull: the remote changes could not be combined with the local ones (nothing was changed)
    AuthFailed,    ///< no or wrong credentials
    NotSupported,  ///< e.g. a repository using Git LFS
    Failed,        ///< anything else, see the message
};

struct Result {
    Outcome outcome = Outcome::Failed;
    std::string message;  ///< details for the user (libgit2's error)
    auto ok() const -> bool { return outcome == Outcome::Ok; }
};

struct Credentials {
    std::string username;
    std::string password;  ///< a password or an access token
    bool remember = true;  ///< store them in the Windows credential manager once they worked
};

/**
 * Asks the user for credentials for url (on the main thread). previous: what was tried before and rejected, if
 * anything. Returns nothing if the user cancelled.
 */
using CredentialPrompt =
        std::function<std::optional<Credentials>(const std::string& url, const std::optional<Credentials>& previous)>;

/// Runs work on a worker thread and passes its result to callback on the main thread, if owner is still alive
template <typename T>
void inBackground(std::weak_ptr<void> owner, std::function<T()> work, std::function<void(T)> callback) {
    struct Job {
        std::weak_ptr<void> owner;
        std::function<void(T)> callback;
        std::optional<T> result;
    };
    std::thread([job = std::make_shared<Job>(Job{std::move(owner), std::move(callback), std::nullopt}),
                 work = std::move(work)]() mutable {
        job->result = work();
        g_idle_add_full(
                G_PRIORITY_DEFAULT,
                +[](gpointer data) -> gboolean {
                    auto& j = *static_cast<std::shared_ptr<Job>*>(data);
                    if (!j->owner.expired()) {
                        j->callback(std::move(*j->result));
                    }
                    return G_SOURCE_REMOVE;
                },
                new std::shared_ptr<Job>(job), +[](gpointer data) { delete static_cast<std::shared_ptr<Job>*>(data); });
    }).detach();
}

/// The repository containing dir and the status of the files below dir (not of the whole repository)
auto status(const std::filesystem::path& dir) -> RepoStatus;

/// Fetches from the upstream branch's remote, only with stored credentials (no prompt): for incoming commits
auto fetch(const std::filesystem::path& toplevel) -> Result;

/**
 * Commits all changes below dir (new, changed and deleted files; not Xournal++'s recovery copies), like
 * `git add -A -- . && git commit -- .` run in dir: changes staged elsewhere in the repository stay staged.
 */
auto commitAll(const std::filesystem::path& dir, const std::string& message) -> Result;

/// Like `git pull --rebase`: fetches the upstream branch and puts the local commits on top. Leaves everything as it
/// was on a conflict.
auto pullRebase(const std::filesystem::path& toplevel, const CredentialPrompt& prompt) -> Result;

/// Pushes the current branch to its upstream, or to the first remote, which then becomes its upstream
auto push(const std::filesystem::path& toplevel, const CredentialPrompt& prompt) -> Result;

/// Like `git restore --source=HEAD --staged --worktree -- paths`
auto restore(const std::filesystem::path& toplevel, const std::vector<std::filesystem::path>& paths) -> Result;

/// Normalized key for comparing paths reported by git with paths in the tree
std::string pathKey(const std::filesystem::path& path);

}  // namespace ProjectGit
