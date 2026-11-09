#include "SidebarLayout.h"

#include <algorithm>  // for max
#include <vector>     // for vector

#include <gtk/gtk.h>  // for GTK_FIXED, gtk_fixed_move

#include "util/gtk4_helper.h"
#include "util/safe_casts.h"  // for as_unsigned

#include "SidebarPreviewBase.h"       // for SidebarPreviewBase
#include "SidebarPreviewBaseEntry.h"  // for SidebarPreviewBaseEntry

/// Fork: one column, each miniature centered; they are scaled to the width of the sidebar instead of wrapping into
/// rows (see SidebarPreviewBase::newWidth())
void SidebarLayout::layout(SidebarPreviewBase* sidebar) {
    const int sidebarWidth = gtk_widget_get_width(sidebar->scrollableBox.get());
    GtkFixed* w = sidebar->miniaturesContainer.get();

    int y = 0;
    int width = 0;
    for (auto& p: sidebar->previews) {
        width = std::max(width, p->getWidth());
    }
    for (auto& p: sidebar->previews) {
        const int x = std::max(0, (std::max(width, sidebarWidth) - p->getWidth()) / 2);
        gtk_fixed_move(w, p->getWidget(), x, y);
        y += p->getHeight();
    }

    gtk_widget_set_size_request(GTK_WIDGET(w), std::min(width, std::max(sidebarWidth, 1)), y);
    gtk_widget_show_all(GTK_WIDGET(w));
}
