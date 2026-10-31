/*
 * Xournal++
 *
 * Minimal git support for the project explorer: running git asynchronously and parsing its status
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ProjectGit {

struct CommandResult {
    bool success = false;
    bool timedOut = false;
    std::string output;
    std::string error;
};

/// Whether a git executable could be found
bool isAvailable();

/**
 * Runs git with the given arguments in dir on a worker thread, without opening a console window.
 * The callback is invoked on the main thread, but only if owner is still alive by then.
 * After timeoutSeconds, git and everything it started (e.g. ssh waiting for a passphrase) is terminated.
 */
void run(const std::filesystem::path& dir, std::vector<std::string> args, std::weak_ptr<void> owner,
         std::function<void(CommandResult)> callback, unsigned timeoutSeconds = 600);

enum class FileState { Unchanged, Added, Modified };

struct Status {
    /// Changed files, keyed by pathKey() of their absolute path
    std::map<std::string, FileState> files;
    size_t changedFiles = 0;
    bool hasUpstream = false;
    size_t ahead = 0;
    size_t behind = 0;
};

/// Parses the output of `git status --porcelain=v1 -z --branch --untracked-files=all`
Status parseStatus(const std::string& output, const std::filesystem::path& toplevel);

/// Normalized key for comparing paths reported by git with paths in the tree
std::string pathKey(const std::filesystem::path& path);

}  // namespace ProjectGit
