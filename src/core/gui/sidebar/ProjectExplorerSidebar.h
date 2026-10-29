#pragma once

#include <memory>

class AbstractSidebarPage;
class Control;

class ProjectExplorerSidebarActions {
public:
    virtual ~ProjectExplorerSidebarActions() = default;
    virtual void openProjectFolder() = 0;
};

std::unique_ptr<AbstractSidebarPage> createProjectExplorerSidebar(Control* control);
