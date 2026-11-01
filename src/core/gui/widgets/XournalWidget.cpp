#include "XournalWidget.h"

#include <algorithm>  // for max
#include <cmath>      // for NAN
#include <optional>   // for optional
#include <string>     // for to_string
#include <vector>     // for vector

#include <cairo.h>    // for cairo_restore, cairo_save
#include <gdk/gdk.h>  // for GdkRectangle, GdkWindowAttr
#include <pango/pangocairo.h>

#include "control/Control.h"                // for Control
#include "control/settings/Settings.h"      // for Settings
#include "control/tools/EditSelection.h"    // for EditSelection
#include "gui/Layout.h"                     // for Layout
#include "gui/LegacyRedrawable.h"           // for Redrawable
#include "gui/PageView.h"                   // for XojPageView
#include "gui/Shadow.h"                     // for Shadow
#include "gui/XournalView.h"                // for XournalView
#include "gui/inputdevices/InputContext.h"  // for InputContext
#include "gui/scroll/ScrollHandling.h"      // for ScrollHandling
#include "util/Color.h"                     // for cairo_set_source_rgbi
#include "util/Rectangle.h"                 // for Rectangle

#include "config-debug.h"  // for DEBUG_DRAW_WIDGET

/*
 * Declares:
 *      static void gtk_xournal_class_init(GtkXournalClass*);
 *      static void gtk_xournal_init(GtkXournal*);
 * Defines
 *      gtk_xournal_parent_class (pointer to GtkWidgetClass instance)
 *      GType gtk_xournal_get_type();
 */
G_DEFINE_TYPE_WITH_CODE(GtkXournal, gtk_xournal, GTK_TYPE_WIDGET, G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))

static void gtk_xournal_get_preferred_width(GtkWidget* widget, gint* minimal_width, gint* natural_width);
static void gtk_xournal_get_preferred_height(GtkWidget* widget, gint* minimal_height, gint* natural_height);
static void gtk_xournal_size_allocate(GtkWidget* widget, GtkAllocation* allocation);
static void gtk_xournal_realize(GtkWidget* widget);
static auto gtk_xournal_draw(GtkWidget* widget, cairo_t* cr) -> gboolean;
static void gtk_xournal_dispose(GObject* object);

static void gtk_xournal_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gtk_xournal_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

auto gtk_xournal_new(XournalView* view, InputContext* inputContext, GtkAdjustment* vadj, GtkAdjustment* hadj)
        -> GtkWidget* {
    GtkXournal* xoj = GTK_XOURNAL(g_object_new(gtk_xournal_get_type(), nullptr));
    xoj->view = view;
    xoj->scrollHandling = inputContext->getScrollHandling();
    xoj->layout = new Layout(view, inputContext->getScrollHandling());
    xoj->selection = nullptr;
    xoj->input = inputContext;

    // Scrollable interface
    xoj->vadjustment = GTK_ADJUSTMENT(g_object_ref(vadj));
    xoj->hadjustment = GTK_ADJUSTMENT(g_object_ref(hadj));
    xoj->vscroll_policy = GTK_SCROLL_NATURAL;
    xoj->hscroll_policy = GTK_SCROLL_NATURAL;


    xoj->input->connect(GTK_WIDGET(xoj));

    return GTK_WIDGET(xoj);
}

enum { PROP_0, PROP_HADJUSTMENT, PROP_VADJUSTMENT, PROP_HSCROLL_POLICY, PROP_VSCROLL_POLICY };

static void gtk_xournal_class_init(GtkXournalClass* cptr) {
    auto* widget_class = reinterpret_cast<GtkWidgetClass*>(cptr);

    widget_class->realize = gtk_xournal_realize;
    widget_class->get_preferred_width = gtk_xournal_get_preferred_width;
    widget_class->get_preferred_height = gtk_xournal_get_preferred_height;
    widget_class->size_allocate = gtk_xournal_size_allocate;

    widget_class->draw = gtk_xournal_draw;

#ifdef DEBUG_DRAW_WIDGET
    widget_class->queue_draw_region = +[](GtkWidget* w, const cairo_region_t* reg) {
        cairo_rectangle_int_t r;
        cairo_region_get_extents(reg, &r);
        auto width = gtk_widget_get_allocated_width(w);
        auto height = gtk_widget_get_allocated_height(w);

        printf("   * queue_draw_region: %d x %d + (%d ; %d) out of %d x %d\n", r.width, r.height, r.x, r.y, width,
               height);
        GTK_WIDGET_CLASS(gtk_xournal_parent_class)->queue_draw_region(w, reg);
    };
#endif

    G_OBJECT_CLASS(cptr)->dispose = gtk_xournal_dispose;

    // Scrollable interface
    G_OBJECT_CLASS(cptr)->set_property = gtk_xournal_set_property;
    G_OBJECT_CLASS(cptr)->get_property = gtk_xournal_get_property;
    g_object_class_override_property(G_OBJECT_CLASS(cptr), PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(G_OBJECT_CLASS(cptr), PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(G_OBJECT_CLASS(cptr), PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(G_OBJECT_CLASS(cptr), PROP_VSCROLL_POLICY, "vscroll-policy");
}

auto gtk_xournal_get_visible_area(GtkWidget* widget, const XojPageView* p) -> xoj::util::Rectangle<double>* {
    if (!p || !p->isVisible()) {
        return nullptr;
    }

    g_return_val_if_fail(widget != nullptr, nullptr);
    g_return_val_if_fail(GTK_IS_XOURNAL(widget), nullptr);

    GtkXournal* xournal = GTK_XOURNAL(widget);

    GtkAdjustment* vadj = xournal->scrollHandling->getVertical();
    GtkAdjustment* hadj = xournal->scrollHandling->getHorizontal();

    GdkRectangle r2;
    r2.x = static_cast<int>(gtk_adjustment_get_value(hadj));
    r2.y = static_cast<int>(gtk_adjustment_get_value(vadj));
    r2.width = static_cast<int>(gtk_adjustment_get_page_size(hadj));
    r2.height = static_cast<int>(gtk_adjustment_get_page_size(vadj));

    GdkRectangle r1;
    auto pos = p->getPixelPosition();
    r1.x = pos.x;
    r1.y = pos.y;
    r1.width = p->getDisplayWidth();
    r1.height = p->getDisplayHeight();

    GdkRectangle r3 = {0, 0, 0, 0};
    gdk_rectangle_intersect(&r1, &r2, &r3);

    if (r3.width == 0 && r3.height == 0) {
        return nullptr;
    }

    r3.x -= r1.x;
    r3.y -= r1.y;

    double zoom = xournal->view->getZoom();

    if (r3.x < 0 || r3.y < 0) {
        g_warning("XournalWidget:gtk_xournal_get_visible_area: intersection rectangle coordinates are negative which "
                  "should never happen");
    }

    return new xoj::util::Rectangle<double>(std::max(r3.x, 0) / zoom, std::max(r3.y, 0) / zoom, r3.width / zoom,
                                            r3.height / zoom);
}

auto gtk_xournal_get_layout(GtkWidget* widget) -> Layout* {
    g_return_val_if_fail(widget != nullptr, nullptr);
    g_return_val_if_fail(GTK_IS_XOURNAL(widget), nullptr);

    GtkXournal* xournal = GTK_XOURNAL(widget);
    return xournal->layout;
}

static void gtk_xournal_init(GtkXournal* xournal) {
    GtkWidget* widget = GTK_WIDGET(xournal);

    gtk_widget_set_can_focus(widget, true);
}

static void gtk_xournal_get_preferred_width(GtkWidget* widget, gint* minimal_width, gint* natural_width) {
    g_return_if_fail(GTK_IS_XOURNAL(widget));
    GtkXournal* xournal = GTK_XOURNAL(widget);
    g_return_if_fail(xournal->layout);
    *minimal_width = *natural_width = xournal->layout->getMinimalPixelWidth();
}

static void gtk_xournal_get_preferred_height(GtkWidget* widget, gint* minimal_height, gint* natural_height) {
    g_return_if_fail(GTK_IS_XOURNAL(widget));
    GtkXournal* xournal = GTK_XOURNAL(widget);
    g_return_if_fail(xournal->layout);
    *minimal_height = *natural_height = xournal->layout->getMinimalPixelHeight();
}

/**
 * This method is called while scrolling or after the XournalWidget size has changed
 */
static void gtk_xournal_size_allocate(GtkWidget* widget, GtkAllocation* allocation) {
    g_return_if_fail(widget != nullptr);
    g_return_if_fail(GTK_IS_XOURNAL(widget));
    g_return_if_fail(allocation != nullptr);

    gtk_widget_set_allocation(widget, allocation);

    if (gtk_widget_get_realized(widget)) {
        gdk_window_move_resize(gtk_widget_get_window(widget), allocation->x, allocation->y, allocation->width,
                               allocation->height);
    }

    GtkXournal* xournal = GTK_XOURNAL(widget);

    gtk_adjustment_set_page_size(xournal->hadjustment, allocation->width);
    gtk_adjustment_set_page_size(xournal->vadjustment, allocation->height);

    gtk_xournal_get_layout(widget)->recomputeCenteringPadding(allocation->width, allocation->height);
}

static void gtk_xournal_realize(GtkWidget* widget) {
    GdkWindowAttr attributes;

    g_return_if_fail(widget != nullptr);
    g_return_if_fail(GTK_IS_XOURNAL(widget));

    gtk_widget_set_realized(widget, true);

    gtk_widget_set_hexpand(widget, true);
    gtk_widget_set_vexpand(widget, true);

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    attributes.window_type = GDK_WINDOW_CHILD;
    attributes.x = allocation.x;
    attributes.y = allocation.y;
    attributes.width = allocation.width;
    attributes.height = allocation.height;

    attributes.wclass = GDK_INPUT_OUTPUT;
    attributes.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK;

    gint attributes_mask = GDK_WA_X | GDK_WA_Y;

    GdkWindow* win = gdk_window_new(gtk_widget_get_parent_window(widget), &attributes, attributes_mask);
    gtk_widget_set_window(widget, win);
    gtk_widget_register_window(widget, win);
}

static void gtk_xournal_draw_shadow(GtkXournal* xournal, cairo_t* cr, int left, int top, int width, int height,
                                    bool selected) {
    Settings* settings = xournal->view->getControl()->getSettings();
    bool showShadow = settings->isShowPageShadow();
    if (selected) {
        if (showShadow) {
            Shadow::drawShadow(cr, left - 2, top - 2, width + 4, height + 4);
        }

        // Draw border
        Util::cairo_set_source_rgbi(cr, settings->getBorderColor());
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

        cairo_rectangle(cr, left - 1, top - 1, width + 2, height + 2);
        cairo_stroke(cr);
    } else if (showShadow) {
        Shadow::drawShadow(cr, left, top, width, height);
    }
}

/**
 * Fork: the scroll position the content is drawn at, rounded to whole device pixels. Touch and touchpad scrolling
 * leave fractional positions, at which every page was blended with bilinear filtering: slower and blurry.
 */
static auto gtk_xournal_draw_origin(GtkXournal* xournal) -> xoj::util::Point<double> {
    const double scale = gtk_widget_get_scale_factor(GTK_WIDGET(xournal));
    return {std::round(gtk_adjustment_get_value(xournal->hadjustment) * scale) / scale,
            std::round(gtk_adjustment_get_value(xournal->vadjustment) * scale) / scale};
}

void gtk_xournal_repaint_area(GtkWidget* widget, int x1, int y1, int x2, int y2) {
    g_return_if_fail(widget != nullptr);
    g_return_if_fail(GTK_IS_XOURNAL(widget));

    Range rg(x1, y1, x2, y2);
    if (!rg.isValid()) {
        return;
    }

    const auto origin = gtk_xournal_draw_origin(GTK_XOURNAL(widget));
    Range visible(xoj::util::Rectangle<double>(origin.x, origin.y,
                                               gtk_adjustment_get_page_size(GTK_XOURNAL(widget)->hadjustment),
                                               gtk_adjustment_get_page_size(GTK_XOURNAL(widget)->vadjustment)));

    rg = visible.intersect(rg);
    if (rg.empty()) {
        return;
    }

    rg.translate(-visible.minX, -visible.minY);
    int minX = floor_cast<int>(rg.minX);
    int minY = floor_cast<int>(rg.minY);
    int width = ceil_cast<int>(rg.maxX) - minX;
    int height = ceil_cast<int>(rg.maxY) - minY;

    gtk_widget_queue_draw_area(widget, minX, minY, width, height);
}

/// Fork: the page number as a rounded badge, centered in the gap below the page
static void gtk_xournal_draw_page_number(GtkWidget* widget, cairo_t* cr, size_t index, size_t count, int left, int top,
                                         int width, int height, Color background) {
    const auto label = std::to_string(index + 1) + " / " + std::to_string(count);
    // The widget's layout uses the UI font at the screen's DPI
    PangoLayout* layout = gtk_widget_create_pango_layout(widget, label.c_str());
    PangoFontDescription* font =
            pango_font_description_copy(pango_context_get_font_description(pango_layout_get_context(layout)));
    pango_font_description_set_size(font, 10 * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int textWidth = 0, textHeight = 0;
    pango_layout_get_pixel_size(layout, &textWidth, &textHeight);

    const double badgeHeight = std::min(textHeight + 2.0, Layout::PAGE_GAP - 16.0);
    const double badgeWidth = textWidth + 1.2 * badgeHeight;
    const double x = left + (width - badgeWidth) / 2.0;
    const double y = top + height + (Layout::PAGE_GAP - badgeHeight) / 2.0 + 1;  // a bit lower, below the shadow
    const double r = badgeHeight / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + badgeWidth - r, y + r, r, -M_PI / 2, M_PI / 2);
    cairo_arc(cr, x + r, y + r, r, M_PI / 2, 3 * M_PI / 2);
    cairo_close_path(cr);

    // A dark badge with light text on a light background and the other way round
    const double luminance = (0.299 * background.red + 0.587 * background.green + 0.114 * background.blue) / 255.0;
    const bool darkBackground = luminance < 0.5;
    if (darkBackground) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
    } else {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.42);
    }
    cairo_fill(cr);

    const double text = darkBackground ? 0.1 : 1.0;
    cairo_set_source_rgb(cr, text, text, text);
    cairo_move_to(cr, x + (badgeWidth - textWidth) / 2.0, y + (badgeHeight - textHeight) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static auto gtk_xournal_draw(GtkWidget* widget, cairo_t* cr) -> gboolean {
    g_return_val_if_fail(widget != nullptr, false);
    g_return_val_if_fail(GTK_IS_XOURNAL(widget), false);


#ifdef DEBUG_DRAW_WIDGET
    {
        double x1 = NAN, x2 = NAN, y1 = NAN, y2 = NAN;
        cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
        printf("$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n\n"
               "      DRAW  %d x %d + (%d ; %d)\n\n"
               "$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$&&&&&&&&&&&&&&&&&&&&&&\n",
               round_cast<int>(x2 - x1), round_cast<int>(y2 - y1), round_cast<int>(x1), round_cast<int>(y1));
    }
#endif

    GtkXournal* xournal = GTK_XOURNAL(widget);

    const auto origin = gtk_xournal_draw_origin(xournal);
    cairo_translate(cr, -origin.x, -origin.y);

    Range clip;
    cairo_clip_extents(cr, &clip.minX, &clip.minY, &clip.maxX, &clip.maxY);

    // Draw background
    Settings* settings = xournal->view->getControl()->getSettings();
    Util::cairo_set_source_rgbi(cr, settings->getBackgroundColor());
    cairo_paint(cr);

    // Add a padding for the shadow of the pages (Fork: and the page numbers below them)
    clip.addPadding(Layout::PAGE_GAP);

    const auto& views = xournal->view->getViewPages();
    // Store the pages to release the layout mutex ASAP
    std::vector<std::pair<size_t, xoj::util::Point<int>>> pages;
    xournal->layout->forEachEntriesIntersectingRange(
            clip, [&](size_t index, const Range&, xoj::util::Point<int> pos) { pages.emplace_back(index, pos); });

    for (auto [index, pos]: pages) {
        const auto& pv = views[index];
        int pw = pv->getDisplayWidth();
        int ph = pv->getDisplayHeight();

        gtk_xournal_draw_shadow(xournal, cr, pos.x, pos.y, pw, ph, pv->isSelected());

        cairo_save(cr);
        cairo_translate(cr, pos.x, pos.y);

        pv->paintPage(cr, nullptr);
        cairo_restore(cr);

        gtk_xournal_draw_page_number(widget, cr, index, views.size(), pos.x, pos.y, pw, ph,
                                     settings->getBackgroundColor());
    }

    if (xournal->selection) {
        cairo_save(cr);
        double zoom = xournal->view->getZoom();

        auto pos = xournal->selection->getView()->getPixelPosition();
        cairo_translate(cr, pos.x, pos.y);

        xournal->selection->paint(cr, zoom);
        cairo_restore(cr);
    }

    std::optional<Recolor> recolor = settings->getRecolorParameters().recolorizeMainView ?
                                             std::make_optional(settings->getRecolorParameters().recolor) :
                                             std::nullopt;

    if (recolor) {
        recolor->recolorCurrentCairoRegion(cr);
    }

    return true;
}

static void gtk_xournal_dispose(GObject* object) {
    g_return_if_fail(object != nullptr);
    g_return_if_fail(GTK_IS_XOURNAL(object));
    GtkXournal* xournal = GTK_XOURNAL(object);

    g_object_unref(xournal->vadjustment);
    xournal->vadjustment = nullptr;
    g_object_unref(xournal->hadjustment);
    xournal->hadjustment = nullptr;

    delete xournal->selection;
    xournal->selection = nullptr;

    delete xournal->layout;
    xournal->layout = nullptr;

    delete xournal->input;
    xournal->input = nullptr;

    G_OBJECT_CLASS(gtk_xournal_parent_class)->dispose(object);
}

static void gtk_xournal_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
    GtkXournal* xournal = GTK_XOURNAL(object);

    switch (prop_id) {
        case PROP_HADJUSTMENT:
            xournal->hadjustment = GTK_ADJUSTMENT(g_value_get_object(value));
            break;
        case PROP_VADJUSTMENT:
            xournal->vadjustment = GTK_ADJUSTMENT(g_value_get_object(value));
            break;
        case PROP_HSCROLL_POLICY:
            if (xournal->hscroll_policy != g_value_get_enum(value)) {
                xournal->hscroll_policy = static_cast<GtkScrollablePolicy>(g_value_get_enum(value));
                gtk_widget_queue_resize(GTK_WIDGET(xournal));
                g_object_notify_by_pspec(object, pspec);
            }
            break;
        case PROP_VSCROLL_POLICY:
            if (xournal->vscroll_policy != g_value_get_enum(value)) {
                xournal->vscroll_policy = static_cast<GtkScrollablePolicy>(g_value_get_enum(value));
                gtk_widget_queue_resize(GTK_WIDGET(xournal));
                g_object_notify_by_pspec(object, pspec);
            }
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}
static void gtk_xournal_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
    GtkXournal* xournal = GTK_XOURNAL(object);

    switch (prop_id) {
        case PROP_HADJUSTMENT:
            g_value_set_object(value, xournal->hadjustment);
            break;
        case PROP_VADJUSTMENT:
            g_value_set_object(value, xournal->vadjustment);
            break;
        case PROP_HSCROLL_POLICY:
            g_value_set_enum(value, xournal->hscroll_policy);
            break;
        case PROP_VSCROLL_POLICY:
            g_value_set_enum(value, xournal->vscroll_policy);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}
