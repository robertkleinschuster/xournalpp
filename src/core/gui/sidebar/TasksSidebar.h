/*
 * Xournal++
 *
 * Fork: the "Tasks" sidebar tab, showing the todo.txt tasks of the project folder
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>     // for size_t
#include <functional>  // for function
#include <memory>      // for unique_ptr

#include "filesystem.h"  // for path

class AbstractSidebarPage;
class Control;

/// What the task sidebar needs from the project explorer
struct TasksSidebarContext {
    /// The open project folder, empty if none
    std::function<fs::path()> projectRoot;
    /// Opens a document (saving the current one first) and shows the page (0-based, -1 for none)
    std::function<void(const fs::path& document, int page)> openDocument;
};

class TasksSidebarActions {
public:
    virtual ~TasksSidebarActions() = default;
    /// Starts a new task linked to the page (0-based) of the document: focuses the quick add field
    virtual void addTaskForPage(const fs::path& document, size_t page) = 0;
};

std::unique_ptr<AbstractSidebarPage> createTasksSidebar(Control* control, TasksSidebarContext context);
