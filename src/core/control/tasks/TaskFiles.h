/*
 * Xournal++
 *
 * Fork: reading and editing todo.txt files line by line
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <optional>  // for optional
#include <string>    // for string
#include <vector>    // for vector

#include "filesystem.h"  // for path

namespace TaskFiles {

/// Whether the file is a task file: todo.txt or *.todo.txt
bool isTaskFile(const fs::path& path);

/// The task files in root and its subfolders (without hidden folders such as .git), sorted by path
std::vector<fs::path> find(const fs::path& root);

/// The lines of a file (without line endings); nullopt if it cannot be read
std::optional<std::vector<std::string>> readLines(const fs::path& path);

/**
 * Replaces the first line equal to oldLine with newLines (none to delete it). The file is read again right before, so
 * changes from other apps or a Git pull are kept, and written with its line endings. Returns false if the line is no
 * longer there or the file cannot be written.
 */
bool replaceLine(const fs::path& path, const std::string& oldLine, const std::vector<std::string>& newLines);

/// Appends a line, creating the file if needed
bool appendLine(const fs::path& path, const std::string& line);

/// The task file for new tasks from a document: the nearest todo.txt up from its folder, else root/todo.txt
fs::path fileFor(const fs::path& document, const fs::path& root);

}  // namespace TaskFiles
