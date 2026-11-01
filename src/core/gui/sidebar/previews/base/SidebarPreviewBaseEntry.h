/*
 * Xournal++
 *
 * A preview entry in a sidebar
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>  // for unique_ptr
#include <mutex>   // for mutex

#include <cairo.h>    // for cairo_t, cairo_surface_t
#include <glib.h>     // for gboolean
#include <gtk/gtk.h>  // for GtkWidget

#include "model/PageRef.h"  // for PageRef
#include "util/raii/CairoWrappers.h"
#include "util/raii/GObjectSPtr.h"

class SidebarPreviewBase;
class ContextMenuTrigger;

typedef enum {
    /**
     * Render the whole page
     */
    RENDER_TYPE_PAGE_PREVIEW = 1,

    /**
     * Render only a layer
     */
    RENDER_TYPE_PAGE_LAYER,

    /**
     * Render the stack up to a layer
     */
    RENDER_TYPE_PAGE_LAYERSTACK
} PreviewRenderType;


class SidebarPreviewBaseEntry {
public:
    SidebarPreviewBaseEntry(SidebarPreviewBase* sidebar, const PageRef& page);
    virtual ~SidebarPreviewBaseEntry();

public:
    virtual GtkWidget* getWidget() const;
    virtual int getWidth() const;
    virtual int getHeight() const;

    virtual void setSelected(bool selected);

    /// Fork: opens the context menu as if requested at (x, y) in the widget's coordinates
    void requestContextMenu(double x, double y);

    virtual void repaint();
    virtual void updateSize();

    /**
     * @return What should be rendered
     */
    virtual PreviewRenderType getRenderType() const = 0;

private:
    static gboolean drawCallback(GtkWidget* widget, cairo_t* cr, SidebarPreviewBaseEntry* preview);

protected:
    virtual void mouseButtonPressCallback() = 0;

    virtual void drawLoadingPage();
    virtual void paint(cairo_t* cr);

protected:
    /**
     * If this page is currently selected
     */
    bool selected = false;

    int imageWidth;
    int imageHeight;
    int DPIscaling;  ///< 1, maybe 2 in HiDPI setups

    /**
     * The sidebar which displays the previews
     */
    SidebarPreviewBase* sidebar;

    /**
     * The page which is representated
     */
    PageRef page;

    /// Mutex protecting the buffer
    std::mutex drawingMutex{};

    /// Buffer because of performance reasons
    xoj::util::CairoSurfaceSPtr buffer;

    /// The main widget, containing the miniature
    xoj::util::WidgetSPtr button;

    /// Fork: declared after the button, so it is destroyed first
    std::unique_ptr<ContextMenuTrigger> contextMenu;

    friend class PreviewJob;
};
