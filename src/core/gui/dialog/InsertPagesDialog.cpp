#include "InsertPagesDialog.h"

#include <algorithm>  // for min
#include <memory>     // for unique_ptr
#include <string>     // for string

#include "pdf/base/XojPdfDocument.h"  // for XojPdfDocument
#include "util/PathUtil.h"            // for toUtf8
#include "util/i18n.h"                // for _

namespace {

constexpr int THUMBNAIL_WIDTH = 110;  ///< logical pixels
constexpr int MAX_THUMBNAIL_HEIGHT = 2 * THUMBNAIL_WIDTH;
/// Selected pages get a clear frame, the default pressed look is hard to see in the dark theme
constexpr const char* THUMBNAIL_CSS = "button.page-thumbnail:checked {"
                                      "  background-image: none;"
                                      "  background-color: alpha(@theme_selected_bg_color, 0.35);"
                                      "  box-shadow: inset 0 0 0 3px @theme_selected_bg_color;"
                                      "}";

class Dialog {
public:
    Dialog(GtkWindow* parent, const InsertPagesDialog::Request& request): request(request) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, THUMBNAIL_CSS, -1, nullptr);
        dialog = gtk_dialog_new_with_buttons(_("Insert Pages"), parent, GTK_DIALOG_MODAL, _("Cancel"),
                                             GTK_RESPONSE_CANCEL, nullptr);
        insertButton = gtk_dialog_add_button(GTK_DIALOG(dialog), _("Insert"), GTK_RESPONSE_ACCEPT);
        gtk_style_context_add_class(gtk_widget_get_style_context(insertButton), "suggested-action");
        gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 640);

        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(box), 12);

        // Source: the document's PDF or another one
        auto* sourceRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        ownSource = gtk_radio_button_new_with_label(nullptr, _("Pages of this document's PDF"));
        otherSource = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(ownSource), _("Another PDF:"));
        chooseButton = gtk_button_new_with_label(_("Choose PDF…"));
        gtk_box_pack_start(GTK_BOX(sourceRow), ownSource, false, false, 0);
        gtk_box_pack_start(GTK_BOX(sourceRow), otherSource, false, false, 0);
        gtk_box_pack_start(GTK_BOX(sourceRow), chooseButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(box), sourceRow, false, false, 0);

        // The pages: tap to select
        flow = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), true);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 3);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 12);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 6);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 6);
        gtk_widget_set_valign(flow, GTK_ALIGN_START);
        auto* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled), flow);
        gtk_widget_set_vexpand(scrolled, true);
        gtk_box_pack_start(GTK_BOX(box), scrolled, true, true, 0);

        message = gtk_label_new(nullptr);
        gtk_label_set_line_wrap(GTK_LABEL(message), true);
        gtk_style_context_add_class(gtk_widget_get_style_context(message), "dim-label");
        gtk_box_pack_start(GTK_BOX(box), message, false, false, 0);

        // Selection and position
        auto* bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto* all = gtk_button_new_with_label(_("All"));
        auto* none = gtk_button_new_with_label(_("None"));
        countLabel = gtk_label_new(nullptr);
        gtk_box_pack_start(GTK_BOX(bottom), all, false, false, 0);
        gtk_box_pack_start(GTK_BOX(bottom), none, false, false, 0);
        gtk_box_pack_start(GTK_BOX(bottom), countLabel, false, false, 8);
        atEnd = gtk_radio_button_new_with_label(nullptr, _("At the end"));
        afterCurrent =
                gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(atEnd), _("After the current page"));
        gtk_box_pack_end(GTK_BOX(bottom), atEnd, false, false, 0);
        gtk_box_pack_end(GTK_BOX(bottom), afterCurrent, false, false, 0);
        gtk_box_pack_start(GTK_BOX(box), bottom, false, false, 0);

        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, true, true, 0);

        g_signal_connect(all, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer self) { static_cast<Dialog*>(self)->selectAll(true); }),
                         this);
        g_signal_connect(none, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer self) { static_cast<Dialog*>(self)->selectAll(false); }),
                         this);
        g_signal_connect(chooseButton, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer self) { static_cast<Dialog*>(self)->chooseOtherPdf(); }),
                         this);
        g_signal_connect(otherSource, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer self) {
                             auto* d = static_cast<Dialog*>(self);
                             if (!gtk_toggle_button_get_active(button)) {
                                 d->showOwn();
                             } else if (d->otherPdf.empty()) {
                                 d->chooseOtherPdf();
                             } else {
                                 d->load(d->otherPdf, false);
                             }
                         }),
                         this);

        gtk_widget_show_all(dialog);
        if (request.ownPdf.empty()) {
            gtk_widget_set_sensitive(ownSource, false);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(afterCurrent), true);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(otherSource), true);  // asks for the PDF
        } else {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(atEnd), true);  // like "Append New PDF Pages"
            showOwn();
        }
    }

    ~Dialog() {
        stopThumbnails();
        gtk_widget_destroy(dialog);
        g_object_unref(css);
    }

    auto run() -> std::optional<InsertPagesDialog::Choice> {
        if (request.ownPdf.empty() && otherPdf.empty()) {
            return std::nullopt;  // no PDF chosen
        }
        if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
            return std::nullopt;
        }
        InsertPagesDialog::Choice choice;
        choice.otherPdf = showingOwn ? fs::path() : otherPdf;
        for (size_t i = 0; i < pages.size(); i++) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pages[i].button))) {
                choice.pages.push_back(i);
            }
        }
        choice.atEnd = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(atEnd));
        return choice;
    }

private:
    struct Page {
        GtkWidget* button;
        GtkWidget* image;
    };

    void showOwn() { load(request.ownPdf, true); }

    void chooseOtherPdf() {
        auto* chooser = gtk_file_chooser_native_new(_("Choose PDF"), GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    _("_Open"), _("_Cancel"));
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, _("PDF files"));
        gtk_file_filter_add_pattern(filter, "*.pdf");
        gtk_file_filter_add_pattern(filter, "*.PDF");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
        const auto folder = !otherPdf.empty() ? otherPdf.parent_path() : request.ownPdf.parent_path();
        if (!folder.empty()) {
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), Util::toUtf8(folder).c_str());
        }
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
            gchar* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
            otherPdf = Util::fromUtf8(file);
            g_free(file);
        }
        g_object_unref(chooser);
        if (otherPdf.empty()) {
            // Nothing chosen: back to the document's PDF, if there is one
            if (!request.ownPdf.empty()) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ownSource), true);
            }
            return;
        }
        gtk_button_set_label(GTK_BUTTON(chooseButton), Util::toUtf8(otherPdf.filename()).c_str());
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(otherSource))) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(otherSource), true);  // loads it
        } else {
            load(otherPdf, false);
        }
    }

    void load(const fs::path& path, bool own) {
        stopThumbnails();
        pages.clear();
        gtk_container_foreach(
                GTK_CONTAINER(flow), +[](GtkWidget* child, gpointer) { gtk_widget_destroy(child); }, nullptr);
        showingOwn = own;
        pdf = std::make_unique<XojPdfDocument>();
        GError* error = nullptr;
        if (!pdf->load(path, "", &error)) {
            gtk_label_set_text(GTK_LABEL(message),
                               FC(_F("The PDF could not be read: {1}") % (error ? error->message : "")));
            if (error) {
                g_error_free(error);
            }
            pdf.reset();
            updateCount();
            return;
        }
        gtk_label_set_text(GTK_LABEL(message),
                           own ? _("Pages already in the document are marked; the others are selected.") :
                                 _("Tap the pages to insert. They are stored with the document in a combined PDF."));
        for (size_t i = 0; i < pdf->getPageCount(); i++) {
            const bool inDocument = own && request.pagesInDocument.count(i);
            auto* button = gtk_toggle_button_new();
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            auto* style = gtk_widget_get_style_context(button);
            gtk_style_context_add_class(style, "page-thumbnail");
            gtk_style_context_add_provider(style, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            auto page = pdf->getPage(i);
            const int height =
                    page && page->getWidth() > 0 ?
                            std::min(MAX_THUMBNAIL_HEIGHT,
                                     static_cast<int>(THUMBNAIL_WIDTH * page->getHeight() / page->getWidth())) :
                            THUMBNAIL_WIDTH;
            auto* image = gtk_image_new();
            gtk_widget_set_size_request(image, THUMBNAIL_WIDTH, height);
            gtk_box_pack_start(GTK_BOX(content), image, false, false, 0);
            std::string caption = std::to_string(i + 1);
            if (inDocument) {
                caption += std::string(" · ") + _("in the document");
            }
            auto* label = gtk_label_new(caption.c_str());
            if (inDocument) {
                gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
            }
            gtk_box_pack_start(GTK_BOX(content), label, false, false, 0);
            gtk_container_add(GTK_CONTAINER(button), content);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), own && !inDocument);
            g_signal_connect(button, "toggled", G_CALLBACK(+[](GtkToggleButton*, gpointer self) {
                                 static_cast<Dialog*>(self)->updateCount();
                             }),
                             this);
            gtk_flow_box_insert(GTK_FLOW_BOX(flow), button, -1);
            pages.push_back({button, image});
        }
        gtk_widget_show_all(flow);
        updateCount();
        // Render the thumbnails one by one while the dialog stays responsive
        nextThumbnail = 0;
        thumbnailSource = g_idle_add(
                +[](gpointer self) -> gboolean { return static_cast<Dialog*>(self)->renderNextThumbnail(); }, this);
    }

    auto renderNextThumbnail() -> gboolean {
        if (!pdf || nextThumbnail >= pages.size()) {
            thumbnailSource = 0;
            return G_SOURCE_REMOVE;
        }
        const size_t index = nextThumbnail++;
        auto page = pdf->getPage(index);
        if (!page || page->getWidth() <= 0) {
            return G_SOURCE_CONTINUE;
        }
        const double scale = THUMBNAIL_WIDTH / page->getWidth();
        const int height = std::min(MAX_THUMBNAIL_HEIGHT, static_cast<int>(page->getHeight() * scale));
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, THUMBNAIL_WIDTH, height);
        cairo_t* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        page->render(cr);
        cairo_destroy(cr);
        gtk_image_set_from_surface(GTK_IMAGE(pages[index].image), surface);
        cairo_surface_destroy(surface);
        return G_SOURCE_CONTINUE;
    }

    void stopThumbnails() {
        if (thumbnailSource) {
            g_source_remove(thumbnailSource);
            thumbnailSource = 0;
        }
    }

    void selectAll(bool selected) {
        for (const auto& page: pages) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page.button), selected);
        }
    }

    void updateCount() {
        size_t count = 0;
        for (const auto& page: pages) {
            count += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page.button)) ? 1 : 0;
        }
        gtk_label_set_text(GTK_LABEL(countLabel),
                           FC(PlaceholderString(ngettext("{1} page selected", "{1} pages selected",
                                                         static_cast<unsigned long>(count))) %
                              count));
        gtk_widget_set_sensitive(insertButton, count > 0);
    }

    const InsertPagesDialog::Request& request;
    fs::path otherPdf;
    bool showingOwn = true;
    std::unique_ptr<XojPdfDocument> pdf;  ///< its own instance: poppler documents must not be shared across threads
    std::vector<Page> pages;
    size_t nextThumbnail = 0;
    guint thumbnailSource = 0;

    GtkCssProvider* css{};
    GtkWidget* dialog{};
    GtkWidget* insertButton{};
    GtkWidget* ownSource{};
    GtkWidget* otherSource{};
    GtkWidget* chooseButton{};
    GtkWidget* flow{};
    GtkWidget* message{};
    GtkWidget* countLabel{};
    GtkWidget* atEnd{};
    GtkWidget* afterCurrent{};
};

}  // namespace

auto InsertPagesDialog::run(GtkWindow* parent, const Request& request) -> std::optional<Choice> {
    Dialog dialog(parent, request);
    return dialog.run();
}
