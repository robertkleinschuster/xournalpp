#pragma once

#include <memory>

class AbstractSidebarPage;
class Control;

std::unique_ptr<AbstractSidebarPage> createProjectExplorerSidebar(Control* control);
