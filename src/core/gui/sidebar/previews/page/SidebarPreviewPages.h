/*
 * Xournal++
 *
 * Previews of the pages in the document
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <memory>   // for unique_ptr
#include <string>   // for string
#include <tuple>    // for tuple
#include <vector>   // for vector

#include <glib.h>     // for gulong
#include <gtk/gtk.h>  // for GtkWidget

#include "gui/IconNameHelper.h"                            // for IconNameHe...
#include "gui/sidebar/previews/base/SidebarPreviewBase.h"  // for SidebarPre...
#include "util/raii/GObjectSPtr.h"

class Control;
class PageReorderGesture;
class GladeGui;


class SidebarPreviewPages: public SidebarPreviewBase {
public:
    SidebarPreviewPages(Control* control);
    ~SidebarPreviewPages() override;

    /// Fork: holding a finger on a miniature starts dragging it (PageReorderGesture); lifted without moving, it
    /// opens the context menu
    auto touchHoldOpensMenu() const -> bool override { return false; }

public:
    void enableSidebar() override;

    /**
     * @overwrite
     */
    std::string getName() override;

    /**
     * @overwrite
     */
    std::string getIconName() override;

    /**
     * Update the preview images
     * @overwrite
     */
    void updatePreviews() override;

public:
    // DocumentListener interface (only the part which is not handled by SidebarPreviewBase)
    void pageSizeChanged(size_t page) override;
    void pageChanged(size_t page) override;
    void pageSelected(size_t page) override;
    void pageInserted(size_t page) override;
    void pageDeleted(size_t page) override;

private:
    /**
     * Unselect the last selected page, if any
     */
    void unselectPage();

    /**
     * Updates the indices of the pages
     */
    void updateIndices();

private:
    IconNameHelper iconNameHelper;

    /// Fork: drag and drop of the miniatures to reorder pages
    std::unique_ptr<PageReorderGesture> reorder;
};
