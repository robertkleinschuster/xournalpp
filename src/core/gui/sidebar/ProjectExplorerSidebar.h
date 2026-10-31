#pragma once

#include <filesystem>
#include <memory>

class AbstractSidebarPage;
class Control;

class ProjectExplorerSidebarActions {
public:
    virtual ~ProjectExplorerSidebarActions() = default;
    virtual void openProjectFolder() = 0;
    /// The document last opened from the current project, or empty if there is none (or it no longer exists)
    virtual auto lastDocument() const -> std::filesystem::path = 0;
    /// The open project folder, empty if none
    virtual auto currentProject() const -> std::filesystem::path = 0;
    /// Opens a document like a click in the explorer (saving the current one first) at a page (0-based, -1 for none)
    virtual void openDocumentAt(const std::filesystem::path& document, int page) = 0;
};

std::unique_ptr<AbstractSidebarPage> createProjectExplorerSidebar(Control* control);
