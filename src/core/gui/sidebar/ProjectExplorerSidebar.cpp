#include "ProjectExplorerSidebar.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "control/Control.h"
#include "gui/MainWindow.h"
#include "gui/sidebar/AbstractSidebarPage.h"
#include "model/Document.h"
#include "util/PathUtil.h"
#include "util/Util.h"
#include "util/i18n.h"

namespace fs = std::filesystem;

namespace {

auto utf8(const fs::path& path) -> std::string {
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto pathFromUtf8(const char* value) -> fs::path {
    std::u8string path;
    while (value && *value) {
        path.push_back(static_cast<char8_t>(*value++));
    }
    return fs::path(path);
}

}  // namespace

class ProjectExplorerSidebar final: public AbstractSidebarPage, public ProjectExplorerSidebarActions {
public:
    explicit ProjectExplorerSidebar(Control* control): AbstractSidebarPage(control) {
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        g_object_ref_sink(root);
        parent = GTK_WINDOW(control->getGtkWindow());
        gtk_container_set_border_width(GTK_CONTAINER(root), 8);

        auto* heading = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(heading), _("<big><b>Project Explorer</b></big>"));
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(root), heading, false, false, 0);

        auto* folderRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        folderEntry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(folderEntry, _("Project folder"));
        g_signal_connect(folderEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             self->openProject(pathFromUtf8(gtk_entry_get_text(self->folderEntry)));
                         }), this);
        gtk_box_pack_start(GTK_BOX(folderRow), GTK_WIDGET(folderEntry), true, true, 0);

        auto* browse = gtk_button_new_from_icon_name("folder-open-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(browse, _("Open project folder"));
        g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<ProjectExplorerSidebar*>(data)->chooseFolder();
                         }), this);
        gtk_box_pack_start(GTK_BOX(folderRow), browse, false, false, 0);

        closeButton = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(closeButton, _("Close project folder"));
        g_signal_connect(closeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<ProjectExplorerSidebar*>(data)->closeProject();
                         }), this);
        gtk_box_pack_start(GTK_BOX(folderRow), closeButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(root), folderRow, false, false, 0);

        status = GTK_LABEL(gtk_label_new(_("No project folder open")));
        gtk_label_set_xalign(status, 0.0F);
        gtk_widget_set_halign(GTK_WIDGET(status), GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(status)), "dim-label");
        gtk_box_pack_start(GTK_BOX(root), GTK_WIDGET(status), false, false, 0);

        fileList = GTK_LIST_BOX(gtk_list_box_new());
        gtk_list_box_set_selection_mode(fileList, GTK_SELECTION_NONE);
        gtk_box_pack_start(GTK_BOX(root), createScrolledList(GTK_WIDGET(fileList)), true, true, 0);
        g_signal_connect(fileList, "row-activated", G_CALLBACK(openRow), this);

        const auto savedProject = Util::getConfigFile("last-project.txt");
        if (auto saved = Util::readString(savedProject, false); saved && !saved->empty()) {
            std::error_code error;
            if (fs::is_directory(pathFromUtf8(saved->c_str()), error)) {
                openProject(pathFromUtf8(saved->c_str()), false);
            } else {
                fs::remove(savedProject, error);
            }
        }
        registerListener(control);
        updateButtons();
        refreshFiles();
        gtk_widget_show_all(root);
    }

    ~ProjectExplorerSidebar() override { g_object_unref(root); }

    void enableSidebar() override { refreshFiles(); }
    void disableSidebar() override {}
    void layout() override {}
    std::string getName() override { return _("Project Explorer"); }
    std::string getIconName() override { return "folder-open-symbolic"; }
    bool hasData() override { return true; }
    GtkWidget* getWidget() override { return root; }
    void openProjectFolder() override { chooseFolder(); }

    void documentChanged(DocumentChangeType) override {
        const auto documentPath = control->getDocument()->getFilepath();
        if (!projectRoot.empty() && !documentPath.empty() &&
            !Util::isChildOrEquivalent(documentPath, projectRoot)) {
            folder.clear();
            gtk_entry_set_text(folderEntry, "");
            refreshFiles();
        }
    }

private:
    static auto createScrolledList(GtkWidget* child) -> GtkWidget* {
        auto* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled), child);
        return scrolled;
    }

    static auto pathFromRow(GtkListBoxRow* row) -> fs::path* {
        return static_cast<fs::path*>(g_object_get_data(G_OBJECT(row), "project-path"));
    }

    static void openRow(GtkListBox*, GtkListBoxRow* row, ProjectExplorerSidebar* self) {
        auto* path = pathFromRow(row);
        if (!path) {
            return;
        }
        std::error_code error;
        if (fs::is_directory(*path, error)) {
            self->setFolder(*path);
        } else if (fs::is_regular_file(*path, error) &&
                   (Util::hasXournalFileExt(*path) || Util::hasPdfFileExt(*path))) {
            self->control->openFile(*path);
        }
    }

    auto createRow(const fs::path& path, const std::string& label, bool directory) -> GtkWidget* {
        auto* row = gtk_list_box_row_new();
        auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 6);
        auto* icon = gtk_image_new_from_icon_name(directory ? "folder" : "text-x-generic", GTK_ICON_SIZE_DND);
        gtk_box_pack_start(GTK_BOX(box), icon, false, false, 0);
        auto* text = gtk_label_new(nullptr);
        const std::string escapedName = g_markup_escape_text(label.c_str(), -1);
        const std::string markup = "<b>" + escapedName + "</b>";
        gtk_label_set_markup(GTK_LABEL(text), markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(text), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_pack_start(GTK_BOX(box), text, true, true, 0);
        gtk_container_add(GTK_CONTAINER(row), box);
        g_object_set_data_full(G_OBJECT(row), "project-path", new fs::path(path),
                               [](gpointer data) { delete static_cast<fs::path*>(data); });
        return row;
    }

    void openProject(const fs::path& selected, bool persist = true) {
        std::error_code error;
        auto absolute = fs::weakly_canonical(selected, error);
        if (error || !fs::is_directory(absolute, error)) {
            showMessage(_("The selected folder could not be opened."));
            return;
        }
        projectRoot = absolute;
        folder = absolute;
        gtk_entry_set_text(folderEntry, utf8(folder).c_str());
        if (persist) {
            const auto config = Util::getConfigFile("last-project.txt");
            std::ofstream output(config, std::ios::binary | std::ios::trunc);
            const auto saved = utf8(projectRoot);
            output.write(saved.data(), static_cast<std::streamsize>(saved.size()));
        }
        updateButtons();
        refreshFiles();
    }

    void setFolder(const fs::path& selected) {
        std::error_code error;
        auto absolute = fs::weakly_canonical(selected, error);
        if (error || !fs::is_directory(absolute, error) ||
            !Util::isChildOrEquivalent(absolute, projectRoot)) {
            return;
        }
        folder = absolute;
        gtk_entry_set_text(folderEntry, utf8(folder).c_str());
        refreshFiles();
    }

    void chooseFolder() {
        auto* chooser = gtk_file_chooser_native_new(_("Open project folder"), parent,
                                                   GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("Open"), _("Cancel"));
        const auto initialFolder = folder.empty() ?
                                           (projectRoot.empty() ? fs::path(g_get_home_dir()) : projectRoot) :
                                           folder;
        if (auto uri = Util::toUri(initialFolder)) {
            gtk_file_chooser_set_current_folder_uri(GTK_FILE_CHOOSER(chooser), uri->c_str());
        }
        g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* dialog, gint response, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             if (response == GTK_RESPONSE_ACCEPT) {
                                 GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
                                 if (file) {
                                     if (gchar* path = g_file_get_path(file)) {
                                         self->openProject(pathFromUtf8(path));
                                         g_free(path);
                                     }
                                     g_object_unref(file);
                                 }
                             }
                             g_object_unref(dialog);
                         }), this);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }

    void closeProject() {
        folder.clear();
        projectRoot.clear();
        gtk_entry_set_text(folderEntry, "");
        std::error_code error;
        fs::remove(Util::getConfigFile("last-project.txt"), error);
        updateButtons();
        refreshFiles();
    }

    void refreshFiles() {
        GList* rows = gtk_container_get_children(GTK_CONTAINER(fileList));
        for (GList* item = rows; item; item = item->next) {
            gtk_widget_destroy(GTK_WIDGET(item->data));
        }
        g_list_free(rows);

        if (folder.empty()) {
            gtk_label_set_text(status, _("No project folder open"));
            gtk_widget_show_all(GTK_WIDGET(fileList));
            return;
        }

        std::vector<fs::directory_entry> directories;
        std::vector<fs::directory_entry> files;
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (it->path().filename() == ".git") {
                continue;
            }
            std::error_code itemError;
            if (it->is_directory(itemError)) {
                directories.push_back(*it);
            } else if (it->is_regular_file(itemError)) {
                files.push_back(*it);
            }
        }
        const auto byName = [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); };
        std::sort(directories.begin(), directories.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        if (error) {
            gtk_label_set_text(status, _("Folder could not be read"));
        } else if (directories.empty() && files.empty()) {
            gtk_label_set_text(status, _("Folder is empty"));
        } else {
            const auto count = directories.size() + files.size();
            const auto countText = std::to_string(count) + " items";
            gtk_label_set_text(status, countText.c_str());
        }
        const auto parentPath = folder.parent_path();
        if (folder != projectRoot && Util::isChildOrEquivalent(parentPath, projectRoot)) {
            gtk_list_box_insert(fileList, createRow(parentPath, "..", true), -1);
        }
        for (const auto& entry: directories) {
            gtk_list_box_insert(fileList, createRow(entry.path(), utf8(entry.path().filename()), true), -1);
        }
        for (const auto& entry: files) {
            gtk_list_box_insert(fileList, createRow(entry.path(), utf8(entry.path().filename()), false), -1);
        }
        gtk_widget_show_all(GTK_WIDGET(fileList));
    }

    void updateButtons() {
        if (closeButton) {
            gtk_widget_set_sensitive(closeButton, !projectRoot.empty());
        }
    }

    void showMessage(const char* message) {
        auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                              "%s", message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    GtkWidget* root{};
    GtkWindow* parent{};
    GtkWidget* closeButton{};
    GtkEntry* folderEntry{};
    GtkLabel* status{};
    GtkListBox* fileList{};
    fs::path folder;
    fs::path projectRoot;
};

auto createProjectExplorerSidebar(Control* control) -> std::unique_ptr<AbstractSidebarPage> {
    return std::make_unique<ProjectExplorerSidebar>(control);
}
