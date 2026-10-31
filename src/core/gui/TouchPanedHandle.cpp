#include "TouchPanedHandle.h"

namespace {

constexpr const char* STYLE_CLASS = "xopp-touch-paned";

// A wide separator with three grip dots in the middle, in colors that work with light and dark themes
constexpr const char* CSS = R"css(
paned.xopp-touch-paned > separator {
    min-width: 14px;
    background-color: alpha(currentColor, 0.06);
    background-image: radial-gradient(circle, alpha(currentColor, 0.6) 1.5px, transparent 2px),
                      radial-gradient(circle, alpha(currentColor, 0.6) 1.5px, transparent 2px),
                      radial-gradient(circle, alpha(currentColor, 0.6) 1.5px, transparent 2px);
    background-size: 6px 6px;
    background-repeat: no-repeat;
    background-position: center 47%, center 50%, center 53%;
}
paned.xopp-touch-paned > separator:hover {
    background-color: alpha(currentColor, 0.12);
}
)css";

struct Drag {
    GdkEventSequence* sequence = nullptr;
    double startX = 0;
    int startPosition = 0;
};

auto onTouch(GtkWidget* widget, GdkEventTouch* event, Drag* drag) -> gboolean {
    GtkPaned* paned = GTK_PANED(widget);
    switch (event->type) {
        case GDK_TOUCH_BEGIN: {
            if (drag->sequence) {
                return TRUE;  // one finger moves the separator
            }
            // Only touches on the separator itself start a drag; the children handle their own touches
            GdkWindow* handle = gtk_paned_get_handle_window(paned);
            if (event->window != handle) {
                return FALSE;
            }
            drag->sequence = event->sequence;
            drag->startX = event->x_root;
            drag->startPosition = gtk_paned_get_position(paned);
            return TRUE;
        }
        case GDK_TOUCH_UPDATE:
            if (event->sequence != drag->sequence) {
                return drag->sequence != nullptr;
            }
            gtk_paned_set_position(paned, drag->startPosition + static_cast<int>(event->x_root - drag->startX));
            return TRUE;
        case GDK_TOUCH_END:
        case GDK_TOUCH_CANCEL:
            if (event->sequence != drag->sequence) {
                return drag->sequence != nullptr;
            }
            drag->sequence = nullptr;
            return TRUE;
        default:
            return FALSE;
    }
}

}  // namespace

void TouchPanedHandle::install(GtkPaned* paned) {
    static GtkCssProvider* provider = [] {
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, CSS, -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        return css;
    }();
    (void)provider;

    gtk_paned_set_wide_handle(paned, true);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(paned)), STYLE_CLASS);
    gtk_widget_add_events(GTK_WIDGET(paned), GDK_TOUCH_MASK);
    // The separator has its own input window, which does not receive touch events by default
    g_signal_connect_after(paned, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer) {
                               if (GdkWindow* handle = gtk_paned_get_handle_window(GTK_PANED(widget))) {
                                   gdk_window_set_events(handle,
                                                         GdkEventMask(gdk_window_get_events(handle) | GDK_TOUCH_MASK));
                               }
                           }),
                           nullptr);
    auto* drag = new Drag;
    g_signal_connect_data(
            paned, "touch-event", G_CALLBACK(onTouch), drag,
            +[](gpointer data, GClosure*) { delete static_cast<Drag*>(data); }, GConnectFlags(0));
}
