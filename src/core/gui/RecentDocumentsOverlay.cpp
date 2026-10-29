#include "RecentDocumentsOverlay.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <cairo.h>
#include <poppler.h>

#include "control/Control.h"
#include "control/RecentManager.h"
#include "gui/MainWindow.h"
#include "util/PathUtil.h"
#include "util/Util.h"
#include "util/XojPreviewExtractor.h"
#include "util/i18n.h"

namespace fs = std::filesystem;

namespace {

auto utf8(const fs::path& path) -> std::string {
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto makePreview(const fs::path& path) -> GdkPixbuf* {
    GdkPixbuf* pixbuf = nullptr;
    if (Util::hasXournalFileExt(path)) {
        XojPreviewExtractor extractor;
        if (extractor.readFile(path) == PREVIEW_RESULT_IMAGE_READ) {
            auto* loader = gdk_pixbuf_loader_new();
            GError* error = nullptr;
            gsize length = 0;
            auto* data = extractor.getData(length);
            if (gdk_pixbuf_loader_write(loader, data, length, &error) &&
                gdk_pixbuf_loader_close(loader, &error)) {
                if (auto* loaded = gdk_pixbuf_loader_get_pixbuf(loader)) {
                    pixbuf = GDK_PIXBUF(g_object_ref(loaded));
                }
            }
            if (error) {
                g_error_free(error);
            }
            g_object_unref(loader);
        }
    } else if (Util::hasPdfFileExt(path)) {
        const auto uri = Util::toUri(path);
        GError* error = nullptr;
        auto* document = uri ? poppler_document_new_from_file(uri->c_str(), nullptr, &error) : nullptr;
        auto* page = document ? poppler_document_get_page(document, 0) : nullptr;
        if (page) {
            double pageWidth = 0;
            double pageHeight = 0;
            poppler_page_get_size(page, &pageWidth, &pageHeight);
            constexpr int width = 120;
            constexpr int height = 86;
            if (pageWidth > 0 && pageHeight > 0) {
                auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
                auto* context = cairo_create(surface);
                cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
                cairo_paint(context);
                const double scale = std::min(width / pageWidth, height / pageHeight);
                cairo_translate(context, (width - pageWidth * scale) / 2, (height - pageHeight * scale) / 2);
                cairo_scale(context, scale, scale);
                poppler_page_render(page, context);
                cairo_destroy(context);
                pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
                cairo_surface_destroy(surface);
            }
            g_object_unref(page);
        }
        if (document) {
            g_object_unref(document);
        }
        if (error) {
            g_error_free(error);
        }
    }
    if (!pixbuf) {
        return nullptr;
    }
    auto* scaled = gdk_pixbuf_scale_simple(pixbuf, 120, 86, GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);
    return scaled;
}

auto pathFromRecent(GtkRecentInfo* info) -> std::optional<fs::path> {
    const gchar* uri = gtk_recent_info_get_uri(info);
    auto path = uri ? Util::fromUri(uri) : std::nullopt;
    if (!path || !fs::is_regular_file(*path)) {
        return std::nullopt;
    }
    return path;
}

auto makeRecentRow(Control* control, GtkRecentInfo* info) -> GtkWidget* {
    auto path = pathFromRecent(info);
    if (!path) {
        return nullptr;
    }

    auto* row = gtk_list_box_row_new();
    auto* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    if (auto* preview = makePreview(*path)) {
        auto* image = gtk_image_new_from_pixbuf(preview);
        gtk_widget_set_size_request(image, 120, 86);
        gtk_box_pack_start(GTK_BOX(content), image, false, false, 0);
        g_object_unref(preview);
    }

    auto* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    const gchar* displayName = gtk_recent_info_get_display_name(info);
    const std::string titleText = displayName ? std::string(displayName) : utf8(path->filename());
    auto* title = gtk_label_new(titleText.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(labels), title, false, false, 0);
    const std::string locationText = utf8(path->parent_path());
    auto* location = gtk_label_new(locationText.c_str());
    gtk_label_set_xalign(GTK_LABEL(location), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(location), PANGO_ELLIPSIZE_MIDDLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(location), "dim-label");
    gtk_box_pack_start(GTK_BOX(labels), location, false, false, 0);
    gtk_box_pack_start(GTK_BOX(content), labels, true, true, 0);
    gtk_container_add(GTK_CONTAINER(row), content);
    g_object_set_data_full(G_OBJECT(row), "recent-path", new fs::path(*path),
                           [](gpointer data) { delete static_cast<fs::path*>(data); });
    g_object_set_data(G_OBJECT(row), "control", control);
    return row;
}

void recentActivated(GtkListBox*, GtkListBoxRow* row, gpointer) {
    auto* path = static_cast<fs::path*>(g_object_get_data(G_OBJECT(row), "recent-path"));
    auto* control = static_cast<Control*>(g_object_get_data(G_OBJECT(row), "control"));
    if (path && control) {
        control->openFile(*path);
    }
}

}  // namespace

GtkWidget* createRecentDocumentsOverlay(Control* control) {
    auto* overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, true);
    gtk_widget_set_vexpand(overlay, true);

    auto* backdrop = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(backdrop), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(backdrop, false);
    gtk_widget_set_hexpand(backdrop, true);
    gtk_widget_set_vexpand(backdrop, true);
    gtk_style_context_add_class(gtk_widget_get_style_context(backdrop), "xopp-start-backdrop");
    g_signal_connect(backdrop, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Control*>(data)->getWindow()->hideStartOverlay();
                     }), control);
    gtk_container_add(GTK_CONTAINER(overlay), backdrop);

    auto* card = gtk_event_box_new();
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 560, 520);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "xopp-start-card");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), card);

    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    gtk_container_add(GTK_CONTAINER(card), content);

    auto* titleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), _("<big><b>Recent Documents</b></big>"));
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(titleRow), title, true, true, 0);
    auto* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(close, _("Close start screen"));
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Control*>(data)->getWindow()->hideStartOverlay();
                     }), control);
    gtk_box_pack_start(GTK_BOX(titleRow), close, false, false, 0);
    gtk_box_pack_start(GTK_BOX(content), titleRow, false, false, 0);

    auto* list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);
    auto* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled, 520, 360);
    gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(list));
    gtk_box_pack_start(GTK_BOX(content), scrolled, true, true, 0);
    g_signal_connect(list, "row-activated", G_CALLBACK(recentActivated), nullptr);

    const auto recent = RecentManager::getRecentFiles();
    size_t count = 0;
    constexpr size_t maxRows = 6;
    auto append = [&](const auto& items) {
        for (const auto& info: items) {
            if (count >= maxRows) {
                return;
            }
            if (auto* row = makeRecentRow(control, info.get())) {
                gtk_list_box_insert(list, row, -1);
                ++count;
            }
        }
    };
    append(recent.recentXoppFiles);
    append(recent.recentPdfFiles);
    if (count == 0) {
        auto* empty = gtk_label_new(_("Your recent documents will appear here."));
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_list_box_insert(list, empty, -1);
    }

    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    auto* newDocument = gtk_button_new_with_label(_("Continue with blank page"));
    g_signal_connect(newDocument, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Control*>(data)->getWindow()->hideStartOverlay();
                     }), control);
    gtk_box_pack_start(GTK_BOX(actions), newDocument, false, false, 0);
    auto* openDocument = gtk_button_new_with_label(_("Open document..."));
    g_signal_connect(openDocument, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Control*>(data)->askToOpenFile();
                     }), control);
    gtk_box_pack_start(GTK_BOX(actions), openDocument, false, false, 0);
    gtk_box_pack_start(GTK_BOX(content), actions, false, false, 0);

    auto* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
            provider,
            ".xopp-start-backdrop { background-color: rgba(0, 0, 0, 0.42); border: none; padding: 0; }\n"
            ".xopp-start-card { background-color: @theme_bg_color; border-radius: 8px; }\n",
            -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(backdrop), GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_style_context_add_provider(gtk_widget_get_style_context(card), GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    gtk_widget_show_all(overlay);
    return overlay;
}
