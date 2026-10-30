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
};

std::unique_ptr<AbstractSidebarPage> createProjectExplorerSidebar(Control* control);
