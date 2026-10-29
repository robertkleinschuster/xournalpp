#include "ProjectExplorerSidebar.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cairo.h>
#include <poppler.h>

#include "control/Control.h"
#include "control/RecentManager.h"
#include "control/settings/Settings.h"
#include "gui/sidebar/AbstractSidebarPage.h"
#include "gui/MainWindow.h"
#include "model/Document.h"
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

auto pathFromUtf8(const char* value) -> fs::path {
    std::u8string path;
    while (value && *value) {
        path.push_back(static_cast<char8_t>(*value++));
    }
    return fs::path(path);
}

auto makeRecentPreview(const fs::path& path) -> GdkPixbuf* {
    GdkPixbuf* pixbuf = nullptr;
    if (Util::hasXournalFileExt(path)) {
        XojPreviewExtractor extractor;
        if (extractor.readFile(path) == PREVIEW_RESULT_IMAGE_READ) {
            auto* loader = gdk_pixbuf_loader_new();
            GError* error = nullptr;
            gsize dataLength = 0;
            auto* previewData = extractor.getData(dataLength);
            if (gdk_pixbuf_loader_write(loader, previewData, dataLength, &error) &&
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
        auto uri = Util::toUri(path);
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
                auto* cr = cairo_create(surface);
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_paint(cr);
                const double scale = std::min(width / pageWidth, height / pageHeight);
                cairo_translate(cr, (width - pageWidth * scale) / 2, (height - pageHeight * scale) / 2);
                cairo_scale(cr, scale, scale);
                poppler_page_render(page, cr);
                cairo_destroy(cr);
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

struct GitResult {
    bool success;
    std::string output;
};

auto runGit(const fs::path& folder, std::initializer_list<std::string> args) -> GitResult {
    std::vector<std::string> ownedArgs{"git", "-C", utf8(folder)};
    ownedArgs.insert(ownedArgs.end(), args.begin(), args.end());
    std::vector<gchar*> argv;
    argv.reserve(ownedArgs.size() + 1);
    for (auto& arg: ownedArgs) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    gchar* stdoutText = nullptr;
    gchar* stderrText = nullptr;
    gint status = 0;
    GError* error = nullptr;
    auto** environment = g_get_environ();
    environment = g_environ_setenv(environment, "GIT_TERMINAL_PROMPT", "0", true);
    const gboolean spawned = g_spawn_sync(nullptr, argv.data(), environment, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                                          &stdoutText, &stderrText, &status, &error);
    g_strfreev(environment);
    std::string output = stdoutText ? stdoutText : "";
    if (stderrText && *stderrText) {
        if (!output.empty() && output.back() != '\n') {
            output += '\n';
        }
        output += stderrText;
    }
    const bool success = spawned && g_spawn_check_wait_status(status, nullptr);
    if (error) {
        output += error->message;
        g_error_free(error);
    }
    g_free(stdoutText);
    g_free(stderrText);
    return {success, std::move(output)};
}

}  // namespace

class ProjectExplorerSidebar final: public AbstractSidebarPage, public ProjectExplorerSidebarActions {
public:
    explicit ProjectExplorerSidebar(Control* control): AbstractSidebarPage(control) {
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        g_object_ref_sink(root);
        parent = GTK_WINDOW(control->getGtkWindow());
        auto* outer = root;
        gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

        auto* heading = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(heading), _("<big><b>Projects</b></big>"));
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(outer), heading, false, false, 0);

        auto* folderRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        folderEntry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(folderEntry, _("Project folder"));
        g_signal_connect(folderEntry, "activate",
                         G_CALLBACK(+[](GtkEntry*, gpointer d) { static_cast<ProjectExplorerSidebar*>(d)->navigateFromEntry(); }), this);
        gtk_box_pack_start(GTK_BOX(folderRow), GTK_WIDGET(folderEntry), true, true, 0);
        addButton(folderRow, _("Open"), +[](GtkButton*, gpointer d) {
                      static_cast<ProjectExplorerSidebar*>(d)->navigateFromEntry();
                  }, this);
        auto* browse = gtk_button_new_from_icon_name("folder-open-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(browse, _("Open project folder"));
        g_signal_connect(browse, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<ProjectExplorerSidebar*>(d)->chooseFolder(); }),
                         this);
        gtk_box_pack_start(GTK_BOX(folderRow), browse, false, false, 0);
        closeProjectButton = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(closeProjectButton, _("Close project"));
        g_signal_connect(closeProjectButton, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<ProjectExplorerSidebar*>(d)->closeProject(); }),
                         this);
        gtk_box_pack_start(GTK_BOX(folderRow), closeProjectButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(outer), folderRow, false, false, 0);

        auto* panes = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
        gtk_box_pack_start(GTK_BOX(outer), panes, true, true, 0);
        auto* recentPane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        auto* filesPane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_paned_pack1(GTK_PANED(panes), recentPane, true, false);
        gtk_paned_pack2(GTK_PANED(panes), filesPane, true, false);
        addHeading(recentPane, _("Recent documents"));
        addHeading(filesPane, _("Project files"));
        recentList = GTK_LIST_BOX(gtk_list_box_new());
        fileList = GTK_LIST_BOX(gtk_list_box_new());
        gtk_list_box_set_selection_mode(recentList, GTK_SELECTION_NONE);
        gtk_list_box_set_selection_mode(fileList, GTK_SELECTION_NONE);
        packScrolled(recentPane, GTK_WIDGET(recentList));
        packScrolled(filesPane, GTK_WIDGET(fileList));
        g_signal_connect(recentList, "row-activated", G_CALLBACK(openRow), this);
        g_signal_connect(fileList, "row-activated", G_CALLBACK(openRow), this);

        auto* gitRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gitStatus = GTK_LABEL(gtk_label_new(_("Git: checking repository...")));
        gtk_label_set_xalign(gitStatus, 0.0F);
        gtk_label_set_ellipsize(gitStatus, PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(gitRow), GTK_WIDGET(gitStatus), true, true, 0);
        addButton(gitRow, _("Refresh"), +[](GtkButton*, gpointer d) { static_cast<ProjectExplorerSidebar*>(d)->refresh(); }, this);
        gtk_box_pack_start(GTK_BOX(outer), gitRow, false, false, 0);

        auto* gitButtons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        addButton(gitButtons, _("Initialize Git"), +[](GtkButton*, gpointer d) {
                      static_cast<ProjectExplorerSidebar*>(d)->runGitAction({"init"});
                  }, this);
        addButton(gitButtons, _("Set remote..."), +[](GtkButton*, gpointer d) {
                      static_cast<ProjectExplorerSidebar*>(d)->setRemote();
                  }, this);
        addButton(gitButtons, _("Commit all..."), +[](GtkButton*, gpointer d) {
                      static_cast<ProjectExplorerSidebar*>(d)->commitAll();
                  }, this);
        addButton(gitButtons, _("Pull"), +[](GtkButton*, gpointer d) {
                      static_cast<ProjectExplorerSidebar*>(d)->runGitAction({"pull", "--rebase"});
                  }, this);
        addButton(gitButtons, _("Push"), +[](GtkButton*, gpointer d) {
                      static_cast<ProjectExplorerSidebar*>(d)->runGitAction({"push", "-u", "origin", "HEAD"});
                  }, this);
        gtk_box_pack_start(GTK_BOX(outer), gitButtons, false, false, 0);

        const auto projectConfig = Util::getConfigFile("last-project.txt");
        if (auto savedProject = Util::readString(projectConfig, false); savedProject && !savedProject->empty()) {
            projectRoot = pathFromUtf8(savedProject->c_str());
            std::error_code error;
            if (fs::is_directory(projectRoot, error)) {
                setFolder(projectRoot);
            } else {
                projectRoot.clear();
                fs::remove(projectConfig, error);
            }
        }
        updateProjectButtons();
        registerListener(control);
        refreshRecent();
    }

    ~ProjectExplorerSidebar() override { g_object_unref(root); }

    void enableSidebar() override { refresh(); }
    void disableSidebar() override {}
    void layout() override {}
    std::string getName() override { return _("Projects"); }
    std::string getIconName() override { return "folder-open-symbolic"; }
    bool hasData() override { return true; }
    GtkWidget* getWidget() override { return root; }
    void openProjectFolder() override { chooseFolder(); }
    void documentChanged(DocumentChangeType) override {
        const auto documentPath = control->getDocument()->getFilepath();
        if (!projectRoot.empty() && !documentPath.empty() &&
            !Util::isChildOrEquivalent(documentPath, projectRoot)) {
            leaveProjectForSingleFile();
        }
        refreshRecent();
        control->getWindow()->showDocumentScreen();
    }

private:
    static void addHeading(GtkWidget* parent, const char* text) {
        auto* heading = gtk_label_new(nullptr);
        const std::string markup = "<b>" + std::string(text) + "</b>";
        gtk_label_set_markup(GTK_LABEL(heading), markup.c_str());
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(parent), heading, false, false, 0);
    }

    template <typename Callback>
    static void addButton(GtkWidget* parent, const char* label, Callback callback, gpointer data) {
        auto* button = gtk_button_new_with_label(label);
        g_signal_connect(button, "clicked", G_CALLBACK(callback), data);
        gtk_box_pack_start(GTK_BOX(parent), button, false, false, 0);
    }

    static void packScrolled(GtkWidget* parent, GtkWidget* child) {
        auto* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled), child);
        gtk_box_pack_start(GTK_BOX(parent), scrolled, true, true, 0);
    }

    static auto pathFromRow(GtkListBoxRow* row) -> fs::path* {
        return static_cast<fs::path*>(g_object_get_data(G_OBJECT(row), "project-path"));
    }

    static void openRow(GtkListBox* list, GtkListBoxRow* row, ProjectExplorerSidebar* self) {
        auto* path = pathFromRow(row);
        if (!path) {
            return;
        }
        std::error_code error;
        if (fs::is_directory(*path, error)) {
            self->setFolder(*path);
        } else if (fs::is_regular_file(*path, error)) {
            self->control->openFile(*path);
        }
    }

    static auto projectRootFor(const fs::path& folder) -> fs::path {
        const auto result = runGit(folder, {"rev-parse", "--show-toplevel"});
        if (!result.success) {
            return folder;
        }
        auto root = result.output;
        while (!root.empty() && (root.back() == '\n' || root.back() == '\r')) {
            root.pop_back();
        }
        return root.empty() ? folder : pathFromUtf8(root.c_str());
    }

    static void clearRows(GtkListBox* list) {
        GList* rows = gtk_container_get_children(GTK_CONTAINER(list));
        for (GList* item = rows; item; item = item->next) {
            gtk_widget_destroy(GTK_WIDGET(item->data));
        }
        g_list_free(rows);
    }

    auto createRow(const fs::path& path, const std::string& label, bool directory, bool recent) -> GtkWidget* {
        auto* row = gtk_list_box_row_new();
        auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(box), 6);
        if (directory) {
            auto* icon = gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_DND);
            gtk_box_pack_start(GTK_BOX(box), icon, false, false, 0);
        } else if (recent) {
            if (auto* preview = makeRecentPreview(path)) {
                auto* image = gtk_image_new_from_pixbuf(preview);
                gtk_widget_set_size_request(image, 120, 86);
                gtk_box_pack_start(GTK_BOX(box), image, false, false, 0);
                g_object_unref(preview);
            }
        }
        auto* text = gtk_label_new(nullptr);
        const std::string escapedName = g_markup_escape_text(label.c_str(), -1);
        std::string markup = "<b>" + escapedName + "</b>";
        if (!directory) {
            const std::string escapedPath = g_markup_escape_text(utf8(path.parent_path()).c_str(), -1);
            markup += "\n<small>" + escapedPath + "</small>";
        }
        gtk_label_set_markup(GTK_LABEL(text), markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(text), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_pack_start(GTK_BOX(box), text, true, true, 0);
        gtk_container_add(GTK_CONTAINER(row), box);
        g_object_set_data_full(G_OBJECT(row), "project-path", new fs::path(path),
                               [](gpointer p) { delete static_cast<fs::path*>(p); });
        return row;
    }

    void setFolder(const fs::path& folder) {
        std::error_code error;
        auto absolute = fs::weakly_canonical(folder, error);
        if (error || !fs::is_directory(absolute, error)) {
            showMessage(_("The selected folder could not be opened."));
            return;
        }
        if (!projectRoot.empty() && !Util::isChildOrEquivalent(absolute, projectRoot)) {
            showMessage(_("Folders outside the open project cannot be browsed."));
            return;
        }
        this->folder = std::move(absolute);
        gtk_entry_set_text(folderEntry, utf8(this->folder).c_str());
        refreshFiles();
        refreshGit();
    }

    void openProject(const fs::path& path) {
        std::error_code error;
        auto selected = fs::weakly_canonical(path, error);
        if (error || !fs::is_directory(selected, error)) {
            showMessage(_("The selected folder could not be opened."));
            return;
        }
        projectRoot = projectRootFor(selected);
        auto config = Util::getConfigFile("last-project.txt");
        std::ofstream output(config, std::ios::binary | std::ios::trunc);
        const auto saved = utf8(projectRoot);
        output.write(saved.data(), static_cast<std::streamsize>(saved.size()));
        if (!output) {
            showMessage(_("Could not save the selected project folder."));
            projectRoot.clear();
            return;
        }
        setFolder(projectRoot);
        updateProjectButtons();
    }

    void closeProject() {
        folder.clear();
        projectRoot.clear();
        gtk_entry_set_text(folderEntry, "");
        std::error_code error;
        fs::remove(Util::getConfigFile("last-project.txt"), error);
        updateProjectButtons();
        refresh();
    }

    void navigateFromEntry() { openProject(pathFromUtf8(gtk_entry_get_text(folderEntry))); }

    void chooseFolder() {
        auto* chooser = gtk_file_chooser_native_new(_("Choose project folder"), parent,
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

    void refresh() {
        refreshFiles();
        refreshGit();
        refreshRecent();
    }

    void refreshFiles() {
        clearRows(fileList);
        if (folder.empty()) {
            auto* message = gtk_label_new(_("No project open. Choose a folder to browse project files."));
            gtk_label_set_line_wrap(GTK_LABEL(message), true);
            gtk_widget_set_margin_start(message, 8);
            gtk_widget_set_margin_end(message, 8);
            gtk_widget_set_margin_top(message, 8);
            gtk_list_box_insert(fileList, message, -1);
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
                auto extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension == ".xopp" || extension == ".xoj" || extension == ".pdf") {
                    files.push_back(*it);
                }
            }
        }
        auto byName = [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); };
        std::sort(directories.begin(), directories.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        const auto parent = folder.parent_path();
        if (folder.has_parent_path() && folder != folder.root_path() &&
            (projectRoot.empty() || Util::isChildOrEquivalent(parent, projectRoot))) {
            gtk_list_box_insert(fileList, createRow(parent, "..", true, false), -1);
        }
        for (const auto& entry: directories) {
            gtk_list_box_insert(fileList, createRow(entry.path(), utf8(entry.path().filename()), true, false), -1);
        }
        for (const auto& entry: files) {
            gtk_list_box_insert(fileList, createRow(entry.path(), utf8(entry.path().filename()), false, false), -1);
        }
        gtk_widget_show_all(GTK_WIDGET(fileList));
    }

    void refreshRecent() {
        clearRows(recentList);
        auto recent = RecentManager::getRecentFiles();
        auto appendRecent = [this](const auto& items) {
            for (const auto& info: items) {
                const gchar* uri = gtk_recent_info_get_uri(info.get());
                auto path = uri ? Util::fromUri(uri) : std::nullopt;
                if (!path || !fs::exists(*path)) {
                    continue;
                }
                if (!folder.empty() && !projectRoot.empty() && !Util::isChildOrEquivalent(*path, projectRoot)) {
                    continue;
                }
                const gchar* name = gtk_recent_info_get_display_name(info.get());
                const std::string label = name ? name : utf8(path->filename());
                gtk_list_box_insert(recentList, createRow(*path, label, false, true), -1);
            }
        };
        appendRecent(recent.recentXoppFiles);
        appendRecent(recent.recentPdfFiles);
        gtk_widget_show_all(GTK_WIDGET(recentList));
    }

    void refreshGit() {
        if (folder.empty()) {
            gtk_label_set_text(gitStatus, _("No project open"));
            return;
        }
        const auto result = runGit(projectRoot.empty() ? folder : projectRoot, {"status", "--short", "--branch"});
        const std::string status = result.success ? result.output : _("No Git repository");
        gtk_label_set_text(gitStatus, status.c_str());
    }

    void runGitAction(std::initializer_list<std::string> args) {
        if (folder.empty()) {
            showMessage(_("Open a project folder before using Git."));
            return;
        }
        const auto result = runGit(projectRoot.empty() ? folder : projectRoot, args);
        if (!result.success) {
            showMessage(result.output.empty() ? _("Git command failed.") : result.output.c_str());
        }
        refreshGit();
    }

    void setRemote() {
        if (folder.empty()) {
            showMessage(_("Open a project folder before using Git."));
            return;
        }
        auto* dialog = gtk_dialog_new_with_buttons(_("Set Git remote"), parent, GTK_DIALOG_MODAL,
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Save"), GTK_RESPONSE_ACCEPT,
                                                   nullptr);
        auto* entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(entry, _("https://github.com/user/project.git"));
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), GTK_WIDGET(entry), false, false, 8);
        gtk_widget_show_all(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            const std::string url = gtk_entry_get_text(entry);
            const auto workingDirectory = projectRoot.empty() ? folder : projectRoot;
            const auto existing = runGit(workingDirectory, {"remote", "get-url", "origin"});
            const auto result = existing.success ? runGit(workingDirectory, {"remote", "set-url", "origin", url})
                                                 : runGit(workingDirectory, {"remote", "add", "origin", url});
            if (!result.success) {
                showMessage(result.output.c_str());
            }
        }
        gtk_widget_destroy(dialog);
        refreshGit();
    }

    void commitAll() {
        if (folder.empty()) {
            showMessage(_("Open a project folder before using Git."));
            return;
        }
        auto* dialog = gtk_dialog_new_with_buttons(_("Commit project changes"), parent, GTK_DIALOG_MODAL,
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Commit"), GTK_RESPONSE_ACCEPT,
                                                   nullptr);
        auto* entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(entry, _("Commit message"));
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), GTK_WIDGET(entry), false, false, 8);
        gtk_widget_show_all(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            const std::string message = gtk_entry_get_text(entry);
            if (message.empty()) {
                showMessage(_("Enter a commit message first."));
            } else {
                const auto workingDirectory = projectRoot.empty() ? folder : projectRoot;
                const auto add = runGit(workingDirectory, {"add", "--all"});
                const auto commit = add.success ? runGit(workingDirectory, {"commit", "-m", message}) : add;
                if (!commit.success) {
                    showMessage(commit.output.c_str());
                }
            }
        }
        gtk_widget_destroy(dialog);
        refreshGit();
    }

    void showMessage(const char* message) {
        auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                              "%s", message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    void leaveProjectForSingleFile() {
        folder.clear();
        gtk_entry_set_text(folderEntry, "");
        updateProjectButtons();
        refreshFiles();
        refreshGit();
        refreshRecent();
    }

    void updateProjectButtons() {
        if (closeProjectButton) {
            gtk_widget_set_sensitive(closeProjectButton, !projectRoot.empty());
        }
    }

    GtkWidget* root{};
    GtkWindow* parent{};
    GtkWidget* closeProjectButton{};
    GtkEntry* folderEntry{};
    GtkListBox* recentList{};
    GtkListBox* fileList{};
    GtkLabel* gitStatus{};
    fs::path folder;
    fs::path projectRoot;
};

auto createProjectExplorerSidebar(Control* control) -> std::unique_ptr<AbstractSidebarPage> {
    return std::make_unique<ProjectExplorerSidebar>(control);
}
