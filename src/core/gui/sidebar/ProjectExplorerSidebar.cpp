#include "ProjectExplorerSidebar.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "control/AnnotationFiles.h"
#include "control/Control.h"
#include "control/DocumentAutoSaver.h"
#include "control/ScrollHandler.h"  // for ScrollHandler
#include "control/settings/PageTemplateSettings.h"
#include "control/settings/Settings.h"
#include "control/tasks/TaskFiles.h"  // for isTaskFile
#include "control/xojfile/SaveHandler.h"
#include "gui/ContextMenuTrigger.h"
#include "gui/GladeSearchpath.h"
#include "gui/MainWindow.h"
#include "gui/sidebar/AbstractSidebarPage.h"
#include "gui/sidebar/ProjectGit.h"
#include "gui/sidebar/Sidebar.h"  // for Sidebar
#include "model/Document.h"
#include "undo/UndoRedoHandler.h"
#include "util/PathUtil.h"
#include "util/Util.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"

namespace fs = std::filesystem;

namespace {

enum Column : gint {
    COL_ICON,
    COL_NAME,
    COL_MARKUP,
    COL_PATH,
    COL_KIND,
    COL_SENSITIVE,
    COL_FOREGROUND,
    COL_TOOLTIP,
    COL_COUNT
};
/// KIND_NESTED: source or exported PDF shown below its .xopp document
/// KIND_DELETED(_DIRECTORY): a file or folder git tracks that was deleted in the working tree (removed by the next
/// sync)
enum Kind : gint {
    KIND_DIRECTORY,
    KIND_DOCUMENT,
    KIND_FILE,
    KIND_PLACEHOLDER,
    KIND_NESTED,
    KIND_DELETED,
    KIND_DELETED_DIRECTORY
};

auto isDeletedKind(gint kind) -> bool { return kind == KIND_DELETED || kind == KIND_DELETED_DIRECTORY; }
auto isFolderKind(gint kind) -> bool { return kind == KIND_DIRECTORY || kind == KIND_DELETED_DIRECTORY; }

constexpr size_t MAX_SEARCH_RESULTS = 1000;
constexpr int MAX_SEARCH_DEPTH = 16;
constexpr guint REFRESH_DELAY_MS = 250;
constexpr guint SAVE_DELAY_MS = 500;
constexpr guint GIT_REFRESH_DELAY_MS = 1000;
constexpr guint GIT_STATUS_INTERVAL_S = 60;
constexpr guint GIT_FETCH_INTERVAL_S = 300;

constexpr const char* COLOR_ADDED = "#2ea043";
constexpr const char* COLOR_MODIFIED = "#3584e4";
constexpr const char* COLOR_DELETED = "#8a8a8a";

auto colorFor(ProjectGit::FileState state) -> const char* {
    switch (state) {
        case ProjectGit::FileState::Added:
            return COLOR_ADDED;
        case ProjectGit::FileState::Modified:
            return COLOR_MODIFIED;
        default:
            return nullptr;
    }
}

auto trim(std::string value) -> std::string {
    while (!value.empty() && g_ascii_isspace(value.back())) {
        value.pop_back();
    }
    return value;
}

auto casefold(const std::string& value) -> std::string {
    gchar* folded = g_utf8_casefold(value.c_str(), -1);
    std::string result(folded);
    g_free(folded);
    return result;
}

auto escape(const std::string& value) -> std::string {
    gchar* escaped = g_markup_escape_text(value.c_str(), -1);
    std::string result(escaped);
    g_free(escaped);
    return result;
}

auto isIgnored(const fs::path& path) -> bool {
    return path.filename() == ".git" || AnnotationFiles::isRecoveryCopy(path) || AnnotationFiles::isCompanionPdf(path);
}

/// Keeps Xournal++ recovery copies out of the sync, in any folder of the repository
const std::vector<std::string> GIT_PATHSPEC = {"--", ".", ":(exclude,glob)**/.*.autosave.xopp", ":(exclude,glob)**/*~"};

auto withPathspec(std::vector<std::string> args) -> std::vector<std::string> {
    args.insert(args.end(), GIT_PATHSPEC.begin(), GIT_PATHSPEC.end());
    return args;
}

auto hasVisibleEntries(const fs::path& dir) -> bool {
    std::error_code error;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (!isIgnored(it->path())) {
            return true;
        }
    }
    return false;
}

auto kindFor(const fs::path& path, bool directory) -> Kind {
    if (directory) {
        return KIND_DIRECTORY;
    }
    return Util::hasXournalFileExt(path) || Util::hasPdfFileExt(path) ? KIND_DOCUMENT : KIND_FILE;
}

auto isImage(const fs::path& path) -> bool {
    auto ext = casefold(Util::toUtf8(path.extension()));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".svg" || ext == ".bmp";
}

/// Loads an icon shipped in ui/pixmaps, falling back to a themed icon if the file is missing.
auto loadPixmapIcon(Control* control, const char* file, const char* fallback) -> GIcon* {
    const auto path = control->getGladeSearchPath()->findFile("pixmaps", file);
    if (path.empty()) {
        return g_themed_icon_new(fallback);
    }
    // Fork: rendered once at the size of the tree's icons. A GFileIcon was read and rendered from the SVG again for
    // every visible row in every frame, which made scrolling the tree slow (19 ms per frame).
    constexpr int ICON_SIZE = 16;  // GTK_ICON_SIZE_MENU
    GtkWindow* window = control->getGtkWindow();
    const int size = ICON_SIZE * (window ? gtk_widget_get_scale_factor(GTK_WIDGET(window)) : 1);
    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_size(Util::toUtf8(path).c_str(), size, size, &error);
    if (!pixbuf) {
        g_warning("Could not load %s: %s", file, error ? error->message : "");
        g_clear_error(&error);
        return g_themed_icon_new(fallback);
    }
    return G_ICON(pixbuf);  // a GdkPixbuf is a GIcon
}

/// Key file group for a project, independent of characters in its path
auto projectGroup(const fs::path& project) -> std::string {
    gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, Util::toUtf8(project).c_str(), -1);
    std::string group = std::string("project-") + hash;
    g_free(hash);
    return group;
}

/// Returns the name with the first match of the (casefolded) needle in bold, or nullopt if it does not match.
auto highlightMatch(const std::string& name, const std::string& needle) -> std::optional<std::string> {
    const auto folded = casefold(name);
    const auto pos = folded.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    // Casefolding may change byte lengths; map via character offsets and only highlight if they line up.
    const auto* foldedStart = folded.c_str();
    const glong startChar = g_utf8_pointer_to_offset(foldedStart, foldedStart + pos);
    const glong endChar = g_utf8_pointer_to_offset(foldedStart, foldedStart + pos + needle.size());
    if (g_utf8_strlen(folded.c_str(), -1) != g_utf8_strlen(name.c_str(), -1)) {
        return escape(name);
    }
    const auto* nameStart = name.c_str();
    const auto begin = static_cast<size_t>(g_utf8_offset_to_pointer(nameStart, startChar) - nameStart);
    const auto end = static_cast<size_t>(g_utf8_offset_to_pointer(nameStart, endChar) - nameStart);
    return escape(name.substr(0, begin)) + "<b>" + escape(name.substr(begin, end - begin)) + "</b>" +
           escape(name.substr(end));
}

auto sameName(const std::string& a, const std::string& b) -> bool {
#ifdef _WIN32
    return casefold(a) == casefold(b);
#else
    return a == b;
#endif
}

auto compareRows(GtkTreeModel* model, GtkTreeIter* a, GtkTreeIter* b, gpointer) -> gint {
    gint kindA = 0;
    gint kindB = 0;
    gchar* nameA = nullptr;
    gchar* nameB = nullptr;
    gtk_tree_model_get(model, a, COL_KIND, &kindA, COL_NAME, &nameA, -1);
    gtk_tree_model_get(model, b, COL_KIND, &kindB, COL_NAME, &nameB, -1);
    gint result = 0;
    if (isFolderKind(kindA) != isFolderKind(kindB)) {
        result = isFolderKind(kindA) ? -1 : 1;
    } else {
        gchar* keyA = g_utf8_collate_key_for_filename(nameA ? nameA : "", -1);
        gchar* keyB = g_utf8_collate_key_for_filename(nameB ? nameB : "", -1);
        result = std::strcmp(keyA, keyB);
        g_free(keyA);
        g_free(keyB);
    }
    g_free(nameA);
    g_free(nameB);
    return result;
}

void addMenuItem(GtkWidget* menu, const char* label, std::function<void()> action, bool sensitive = true) {
    auto* item = gtk_menu_item_new_with_label(label);
    g_signal_connect_data(
            item, "activate",
            G_CALLBACK(+[](GtkMenuItem*, gpointer data) { (*static_cast<std::function<void()>*>(data))(); }),
            new std::function<void()>(std::move(action)),
            [](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, GConnectFlags(0));
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

void addMenuSeparator(GtkWidget* menu) { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); }

}  // namespace

class ProjectExplorerSidebar final: public AbstractSidebarPage, public ProjectExplorerSidebarActions {
public:
    explicit ProjectExplorerSidebar(Control* control): AbstractSidebarPage(control) {
        parent = GTK_WINDOW(control->getGtkWindow());
        if (parent) {
            focusWindow = parent;
            g_object_add_weak_pointer(G_OBJECT(focusWindow), reinterpret_cast<gpointer*>(&focusWindow));
            auto onActive = +[](GObject* window, GParamSpec*, gpointer data) {
                if (gtk_window_is_active(GTK_WINDOW(window))) {
                    auto* self = static_cast<ProjectExplorerSidebar*>(data);
                    self->checkForMissedChanges();
                    self->catchUpGit();
                }
            };
            g_signal_connect(focusWindow, "notify::is-active", G_CALLBACK(onActive), this);
        }
        // The explorer lives as long as the main window, so it owns the IDE-like saving of documents
        autoSaver = std::make_unique<DocumentAutoSaver>(control);
        annotationExporter = std::make_unique<AnnotationExporter>(control);
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        g_object_ref_sink(root);

        searchEntry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), _("Search files"));
        g_signal_connect(searchEntry, "search-changed", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             self->query = casefold(gtk_entry_get_text(GTK_ENTRY(entry)));
                             self->refresh(false);
                         }),
                         this);
        g_signal_connect(searchEntry, "stop-search",
                         G_CALLBACK(+[](GtkSearchEntry* entry, gpointer) { gtk_entry_set_text(GTK_ENTRY(entry), ""); }),
                         nullptr);
        g_signal_connect(searchEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                             static_cast<ProjectExplorerSidebar*>(data)->openFirstDocument();
                         }),
                         this);
        g_signal_connect(searchEntry, "key-press-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
                             if (event->keyval != GDK_KEY_Down) {
                                 return FALSE;
                             }
                             gtk_widget_grab_focus(GTK_WIDGET(static_cast<ProjectExplorerSidebar*>(data)->tree));
                             return TRUE;
                         }),
                         this);
        searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        g_object_set(searchRow, "margin", 6, nullptr);
        gtk_box_pack_start(GTK_BOX(searchRow), GTK_WIDGET(searchEntry), true, true, 0);
        createSyncButton();
        gtk_box_pack_start(GTK_BOX(searchRow), syncButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(root), searchRow, false, false, 0);

        folderIcon = g_themed_icon_new("folder-symbolic");
        folderOpenIcon = g_themed_icon_new("folder-open-symbolic");
        imageIcon = g_themed_icon_new("image-x-generic-symbolic");
        fileIcon = g_themed_icon_new("text-x-generic-symbolic");
        xournalIcon = loadPixmapIcon(control, "project-explorer-xopp.svg", "x-office-document-symbolic");
        pdfIcon = loadPixmapIcon(control, "project-explorer-pdf.svg", "x-office-document-symbolic");
        annotatedPdfIcon = loadPixmapIcon(control, "project-explorer-pdf-annotated.svg", "x-office-document-symbolic");
        trashIcon = g_themed_icon_new("user-trash-symbolic");
        tasksIcon = g_themed_icon_new("task-due-symbolic");

        store = gtk_tree_store_new(COL_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                   G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);
        gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), COL_NAME, compareRows, nullptr, nullptr);
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), COL_NAME, GTK_SORT_ASCENDING);

        tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
        g_object_unref(store);
        gtk_tree_view_set_headers_visible(tree, false);
        gtk_tree_view_set_enable_search(tree, false);
        gtk_tree_view_set_activate_on_single_click(tree, true);
        gtk_tree_view_set_show_expanders(tree, true);
        gtk_tree_view_set_tooltip_column(tree, COL_TOOLTIP);

        auto* column = gtk_tree_view_column_new();
        auto* iconRenderer = gtk_cell_renderer_pixbuf_new();
        g_object_set(iconRenderer, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 4, "ypad", 5,
                     nullptr);  // Fork: rows tall enough to tap
        gtk_tree_view_column_pack_start(column, iconRenderer, false);
        gtk_tree_view_column_add_attribute(column, iconRenderer, "gicon", COL_ICON);
        gtk_tree_view_column_add_attribute(column, iconRenderer, "sensitive", COL_SENSITIVE);
        auto* textRenderer = gtk_cell_renderer_text_new();
        g_object_set(textRenderer, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 5, nullptr);
        gtk_tree_view_column_pack_start(column, textRenderer, true);
        gtk_tree_view_column_add_attribute(column, textRenderer, "markup", COL_MARKUP);
        gtk_tree_view_column_add_attribute(column, textRenderer, "sensitive", COL_SENSITIVE);
        gtk_tree_view_column_add_attribute(column, textRenderer, "foreground", COL_FOREGROUND);
        gtk_tree_view_append_column(tree, column);

        g_signal_connect(tree, "test-expand-row", G_CALLBACK(onTestExpandRow), this);
        g_signal_connect(tree, "row-expanded", G_CALLBACK(onRowExpanded), this);
        g_signal_connect(tree, "row-collapsed", G_CALLBACK(onRowCollapsed), this);
        g_signal_connect(tree, "row-activated", G_CALLBACK(onRowActivated), this);
        g_signal_connect(tree, "key-press-event", G_CALLBACK(onKeyPress), this);
        g_signal_connect(gtk_tree_view_get_selection(tree), "changed",
                         G_CALLBACK(+[](GtkTreeSelection*, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             if (self->updatingTree) {
                                 return;
                             }
                             if (auto selected = self->selectedPath(); !selected.empty()) {
                                 self->selectedFile = selected;
                                 self->scheduleSave();
                             }
                         }),
                         this);

        contextMenu = std::make_unique<ContextMenuTrigger>(
                GTK_WIDGET(tree), [this](const ContextMenuTrigger::Request& request) { showContextMenu(request); });

        scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(tree));
        gtk_box_pack_start(GTK_BOX(root), scrolled, true, true, 0);

        emptyView = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_valign(emptyView, GTK_ALIGN_CENTER);
        gtk_container_set_border_width(GTK_CONTAINER(emptyView), 16);
        auto* emptyLabel = gtk_label_new(_("No folder opened"));
        gtk_style_context_add_class(gtk_widget_get_style_context(emptyLabel), "dim-label");
        gtk_box_pack_start(GTK_BOX(emptyView), emptyLabel, false, false, 0);
        auto* openButton = gtk_button_new_with_label(_("Open Folder"));
        gtk_style_context_add_class(gtk_widget_get_style_context(openButton), "suggested-action");
        g_signal_connect(openButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<ProjectExplorerSidebar*>(data)->chooseFolder();
                         }),
                         this);
        gtk_box_pack_start(GTK_BOX(emptyView), openButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(root), emptyView, true, true, 0);

        gtk_widget_show_all(root);
        gtk_widget_hide(syncButton);
        // Reset to nullptr by GLib when the widgets are destroyed, see the destructor
        g_object_add_weak_pointer(G_OBJECT(syncButton), reinterpret_cast<gpointer*>(&syncButton));
        g_object_add_weak_pointer(G_OBJECT(tree), reinterpret_cast<gpointer*>(&tree));
        g_object_add_weak_pointer(G_OBJECT(searchEntry), reinterpret_cast<gpointer*>(&searchEntry));

        loadState();
        registerListener(control);
        buildTree();
        if (!selectedFile.empty()) {
            reveal(Util::fromUtf8(selectedFile.c_str()), true);
        }

        refreshGit();
        gitStatusTimer = g_timeout_add_seconds(
                GIT_STATUS_INTERVAL_S,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<ProjectExplorerSidebar*>(data);
                    // Fork: no git processes while the window is in the background (saves battery); caught up on
                    // activation
                    if (self->isInBackground()) {
                        self->gitStatusMissed = true;
                    } else {
                        self->refreshGit();
                    }
                    return G_SOURCE_CONTINUE;
                },
                this);
        gitFetchTimer = g_timeout_add_seconds(
                GIT_FETCH_INTERVAL_S,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<ProjectExplorerSidebar*>(data);
                    if (self->isInBackground()) {
                        self->gitFetchMissed = true;
                    } else {
                        self->fetchRemote();
                    }
                    return G_SOURCE_CONTINUE;
                },
                this);
    }

    ~ProjectExplorerSidebar() override {
        cancelSearch();
        if (saveSource) {
            g_source_remove(saveSource);
            saveState();
        }
        if (refreshSource) {
            g_source_remove(refreshSource);
        }
        if (scrollSource) {
            g_source_remove(scrollSource);
        }
        for (guint source: {gitRefreshSource, gitStatusTimer, gitFetchTimer}) {
            if (source) {
                g_source_remove(source);
            }
        }
        clearMonitors();
        // On shutdown the main window destroys these widgets before the sidebar is deleted
        if (focusWindow) {
            g_signal_handlers_disconnect_by_data(focusWindow, this);
            g_object_remove_weak_pointer(G_OBJECT(focusWindow), reinterpret_cast<gpointer*>(&focusWindow));
        }
        if (syncButton) {
            g_signal_handlers_disconnect_by_data(syncButton, this);
            g_object_remove_weak_pointer(G_OBJECT(syncButton), reinterpret_cast<gpointer*>(&syncButton));
        }
        if (tree) {
            g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(tree), this);
            g_signal_handlers_disconnect_by_data(tree, this);
            g_object_remove_weak_pointer(G_OBJECT(tree), reinterpret_cast<gpointer*>(&tree));
        }
        if (searchEntry) {
            g_signal_handlers_disconnect_by_data(searchEntry, this);
            g_object_remove_weak_pointer(G_OBJECT(searchEntry), reinterpret_cast<gpointer*>(&searchEntry));
        }
        contextMenu.reset();
        g_object_unref(root);
        for (auto* icon: {folderIcon, folderOpenIcon, imageIcon, fileIcon, xournalIcon, pdfIcon, annotatedPdfIcon,
                          trashIcon, tasksIcon}) {
            g_object_unref(icon);
        }
    }

    void enableSidebar() override {
        // Rebuilding the whole tree is expensive and also happens whenever a document is opened, so only do it
        // when coming back from another tab
        if (refreshOnEnable) {
            refreshOnEnable = false;
            refresh(true);
        }
        refreshGit();
    }
    void disableSidebar() override { refreshOnEnable = true; }
    void layout() override {}
    std::string getName() override { return _("Project Explorer"); }
    std::string getIconName() override { return "folder-open-symbolic"; }
    bool hasData() override { return true; }
    GtkWidget* getWidget() override { return root; }
    void openProjectFolder() override { chooseFolder(); }

    auto currentProject() const -> fs::path override { return projectRoot; }

    void openDocumentAt(const fs::path& document, int page) override {
        std::error_code error;
        if (fs::equivalent(document, currentDocumentPath(), error)) {
            if (page >= 0) {
                control->getScrollHandler()->scrollToPage(static_cast<size_t>(page));
            }
            return;
        }
        autoSaver->saveNow([this, document, page](bool saved) {
            if (saved) {
                control->openFile(document, [](bool) {}, page);
            } else {
                reveal(currentDocumentPath(), true);
            }
        });
    }

    auto lastDocument() const -> fs::path override {
        if (lastDocumentFile.empty() || projectRoot.empty()) {
            return {};
        }
        const auto path = Util::fromUtf8(lastDocumentFile.c_str());
        std::error_code error;
        return fs::is_regular_file(path, error) && Util::isChildOrEquivalent(path, projectRoot) ? path : fs::path();
    }

    void documentChanged(DocumentChangeType type) override {
        if (type != DOCUMENT_CHANGE_COMPLETE || projectRoot.empty()) {
            return;
        }
        const auto documentPath = currentDocumentPath();
        std::error_code error;
        const auto absolute = documentPath.empty() ? fs::path() : fs::weakly_canonical(documentPath, error);
        if (absolute.empty() || error || !Util::isChildOrEquivalent(absolute, projectRoot)) {
            return;
        }
        lastDocumentFile = Util::toUtf8(absolute);
        saveState();
        if (!searching) {
            reveal(absolute, true);
        }
    }

private:
    // --- Tree model helpers ---

    auto model() const -> GtkTreeModel* { return GTK_TREE_MODEL(store); }

    auto iconFor(const fs::path& path, Kind kind) const -> GIcon* {
        if (kind == KIND_DIRECTORY) {
            return folderIcon;
        }
        if (Util::hasXournalFileExt(path)) {
            return xournalIcon;
        }
        if (Util::hasPdfFileExt(path)) {
            return pdfIcon;
        }
        if (TaskFiles::isTaskFile(path)) {
            return tasksIcon;
        }
        return isImage(path) ? imageIcon : fileIcon;
    }

    auto kindAt(GtkTreeIter* iter) const -> gint {
        gint kind = KIND_PLACEHOLDER;
        gtk_tree_model_get(model(), iter, COL_KIND, &kind, -1);
        return kind;
    }

    auto pathAt(GtkTreeIter* iter) const -> std::string {
        gchar* value = nullptr;
        gtk_tree_model_get(model(), iter, COL_PATH, &value, -1);
        std::string result = value ? value : "";
        g_free(value);
        return result;
    }

    auto appendNode(GtkTreeIter* parentIter, const fs::path& path, const std::string& name, Kind kind,
                    const std::string& markup, bool lazy) -> GtkTreeIter {
        GtkTreeIter iter;
        gtk_tree_store_insert_with_values(
                store, &iter, parentIter, -1, COL_ICON, iconFor(path, kind), COL_NAME, name.c_str(), COL_MARKUP,
                markup.c_str(), COL_PATH, Util::toUtf8(path).c_str(), COL_KIND, kind, COL_SENSITIVE,
                kind != KIND_FILE || TaskFiles::isTaskFile(path), COL_FOREGROUND, gitColorFor(path, kind), -1);
        if (kind == KIND_DIRECTORY && lazy && (hasVisibleEntries(path) || !deletedEntriesIn(path).empty())) {
            GtkTreeIter placeholder;
            gtk_tree_store_insert_with_values(store, &placeholder, &iter, -1, COL_NAME, "", COL_KIND, KIND_PLACEHOLDER,
                                              COL_SENSITIVE, false, -1);
        }
        return iter;
    }

    void appendInfoRow(GtkTreeIter* parentIter, const char* text) {
        GtkTreeIter iter;
        const auto markup = "<i>" + escape(text) + "</i>";
        gtk_tree_store_insert_with_values(store, &iter, parentIter, -1, COL_NAME, "", COL_MARKUP, markup.c_str(),
                                          COL_KIND, KIND_PLACEHOLDER, COL_SENSITIVE, false, -1);
    }

    /// Replaces the placeholder child of a lazily loaded directory with its actual entries.
    void populate(GtkTreeIter* iter) {
        GtkTreeIter placeholder;
        if (!gtk_tree_model_iter_children(model(), &placeholder, iter) || kindAt(&placeholder) != KIND_PLACEHOLDER) {
            return;
        }
        const auto dir = Util::fromUtf8(pathAt(iter).c_str());
        std::vector<fs::path> directories;
        std::vector<fs::path> files;
        std::error_code error;
        for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
            if (isIgnored(it->path())) {
                continue;
            }
            std::error_code itemError;
            if (it->is_directory(itemError)) {
                directories.push_back(it->path());
            } else if (it->is_regular_file(itemError)) {
                files.push_back(it->path());
            }
        }
        for (const auto& path: directories) {
            const auto name = Util::toUtf8(path.filename());
            appendNode(iter, path, name, KIND_DIRECTORY, escape(name), true);
        }
        // Nest the source PDF and the exported PDF below their .xopp document, like file nesting in IDEs
        // The icon tells them apart: plain PDF for the source, PDF with handwriting for the export
        struct NestedFile {
            fs::path path;
            GIcon* icon;
            const char* tooltip;
        };
        std::map<std::string, std::vector<NestedFile>> nested;
        std::set<std::string> nestedFiles;
        for (const auto& path: files) {
            if (path.extension() != ".xopp") {
                continue;
            }
            // The PDF the document annotates (still right after renaming the document), or by name for older ones
            auto source = AnnotationFiles::recordedSourcePdf(path);
            if (source.empty() || source.parent_path() != dir) {
                source = path;
                source.replace_extension(".pdf");
            }
            const auto exported = AnnotationFiles::exportPathForFile(path, *control->getSettings());
            for (const auto& candidate: {NestedFile{source, pdfIcon, _("Source PDF")},
                                         NestedFile{exported, annotatedPdfIcon, _("Exported PDF with annotations")}}) {
                std::error_code fileError;
                if (!candidate.path.empty() && fs::is_regular_file(candidate.path, fileError) &&
                    nestedFiles.insert(Util::toUtf8(candidate.path)).second) {
                    nested[Util::toUtf8(path)].push_back(candidate);
                }
            }
        }
        for (const auto& path: files) {
            if (nestedFiles.count(Util::toUtf8(path))) {
                continue;
            }
            const auto name = Util::toUtf8(path.filename());
            auto fileIter = appendNode(iter, path, name, kindFor(path, false), escape(name), true);
            if (auto children = nested.find(Util::toUtf8(path)); children != nested.end()) {
                for (const auto& child: children->second) {
                    const auto childName = Util::toUtf8(child.path.filename());
                    auto childIter =
                            appendNode(&fileIter, child.path, childName, KIND_NESTED, escape(childName), false);
                    gtk_tree_store_set(store, &childIter, COL_ICON, child.icon, COL_TOOLTIP,
                                       escape(child.tooltip).c_str(), -1);
                }
            }
        }
        for (const auto& [path, directory]: deletedEntriesIn(dir)) {
            appendDeletedNode(iter, path, directory);
        }
        gtk_tree_store_remove(store, &placeholder);
        watch(dir);
    }

    // --- Deleted entries ---

    /**
     * The entries directly in dir that git tracks but that were deleted in the working tree: deleted files, and
     * folders that no longer exist and contained tracked files (bool: whether it is a folder)
     */
    auto deletedEntriesIn(const fs::path& dir) const -> std::vector<std::pair<fs::path, bool>> {
        std::vector<std::pair<fs::path, bool>> entries;
        if (!gitRepo || gitStatus.deleted.empty()) {
            return entries;
        }
        const auto dirKey = ProjectGit::pathKey(dir);
        std::set<std::string> folders;
        for (const auto& deleted: gitStatus.deleted) {
            const auto key = ProjectGit::pathKey(deleted);
            if (key.size() <= dirKey.size() + 1 || key.compare(0, dirKey.size(), dirKey) != 0 ||
                (key[dirKey.size()] != '\\' && key[dirKey.size()] != '/')) {
                continue;
            }
            if (ProjectGit::pathKey(deleted.parent_path()) == dirKey) {
                entries.emplace_back(deleted, false);
                continue;
            }
            // Deeper: the first missing folder below dir stands for it
            const auto relative = deleted.lexically_relative(dir);
            if (relative.empty() || relative.begin() == relative.end()) {
                continue;
            }
            const auto folder = dir / *relative.begin();
            std::error_code error;
            if (!fs::exists(folder, error) && folders.insert(ProjectGit::pathKey(folder)).second) {
                entries.emplace_back(folder, true);
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second :
                                          Util::toUtf8(a.first.filename()) < Util::toUtf8(b.first.filename());
        });
        return entries;
    }

    /// A deleted entry: struck through, gray and with a trash icon, unlike the dimmed unsupported files
    void appendDeletedNode(GtkTreeIter* parentIter, const fs::path& path, bool directory) {
        const auto name = Util::toUtf8(path.filename());
        GtkTreeIter iter;
        gtk_tree_store_insert_with_values(
                store, &iter, parentIter, -1, COL_ICON, trashIcon, COL_NAME, name.c_str(), COL_MARKUP,
                ("<s>" + escape(name) + "</s>").c_str(), COL_PATH, Util::toUtf8(path).c_str(), COL_KIND,
                directory ? KIND_DELETED_DIRECTORY : KIND_DELETED, COL_SENSITIVE, true, COL_FOREGROUND, COLOR_DELETED,
                COL_TOOLTIP,
                escape(directory ? _("Deleted folder – removed from the repository with the next sync") :
                                   _("Deleted – removed from the repository with the next sync"))
                        .c_str(),
                -1);
    }

    /// Brings a deleted file or folder back from the last commit
    void restoreEntry(const fs::path& path) {
        if (gitToplevel.empty()) {
            return;
        }
        ProjectGit::run(gitToplevel, {"restore", "--source=HEAD", "--staged", "--worktree", "--", Util::toUtf8(path)},
                        lifetime, [this, path](auto restored) {
                            if (!restored.success) {
                                showError(_("The entry could not be restored."), restored.error);
                                return;
                            }
                            refreshGit([this, path] {
                                refresh(true);
                                showEntry(path);
                            });
                        });
    }

    void expandIter(GtkTreeIter* iter) {
        GtkTreePath* path = gtk_tree_model_get_path(model(), iter);
        gtk_tree_view_expand_row(tree, path, false);
        gtk_tree_path_free(path);
    }

    // --- Building ---

    void buildTree() {
        cancelSearch();
        searching = !query.empty();
        updatingTree = true;
        clearMonitors();
        gtk_tree_store_clear(store);
        updateView();
        if (!projectRoot.empty()) {
            auto rootName = Util::toUtf8(projectRoot.filename());
            if (rootName.empty()) {
                rootName = Util::toUtf8(projectRoot);
            }
            auto rootIter = appendNode(nullptr, projectRoot, rootName, KIND_DIRECTORY,
                                       "<b>" + escape(rootName) + "</b>", !searching);
            if (searching) {
                fillSearchResults(&rootIter);
                gtk_tree_view_expand_all(tree);
            } else {
                expandIter(&rootIter);
            }
        }
        updatingTree = false;
        shownListing = listingSignature();
    }

    /// The entries of all watched (i.e. shown) folders, to tell whether a monitor event changed what the tree shows
    auto listingSignature() const -> std::string {
        std::string signature;
        for (const auto& entry: monitors) {
            const auto& folder = entry.first;
            std::vector<std::string> names;
            std::error_code error;
            for (fs::directory_iterator it(Util::fromUtf8(folder.c_str()), error), end; !error && it != end;
                 it.increment(error)) {
                if (!isIgnored(it->path())) {
                    std::error_code typeError;
                    names.push_back(Util::toUtf8(it->path().filename()) + (it->is_directory(typeError) ? "/" : ""));
                }
            }
            std::sort(names.begin(), names.end());
            signature += folder;
            signature += '\n';
            for (const auto& name: names) {
                signature += name;
                signature += '\n';
            }
        }
        return signature;
    }

    struct SearchMatch {
        fs::path path;
        bool directory;
        std::string markup;
    };

    /// Searches the project folder on a worker thread (it may be large), then shows the matches below the root row
    void fillSearchResults(GtkTreeIter* rootIter) {
        cancelSearch();
        searchCancelled = std::make_shared<std::atomic<bool>>(false);
        appendInfoRow(rootIter, _("Searching…"));

        struct Search {
            std::weak_ptr<int> alive;
            ProjectExplorerSidebar* self;
            std::shared_ptr<std::atomic<bool>> cancelled;
            fs::path root;
            std::string query;
            std::vector<SearchMatch> matches;
        };
        auto* search = new Search{lifetime, this, searchCancelled, projectRoot, query, {}};
        std::thread([search] {
            std::error_code error;
            for (fs::recursive_directory_iterator
                         it(search->root, fs::directory_options::skip_permission_denied, error),
                 end;
                 !error && it != end && !*search->cancelled; it.increment(error)) {
                const auto& path = it->path();
                std::error_code itemError;
                const bool directory = it->is_directory(itemError);
                if (isIgnored(path)) {
                    if (directory) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it.depth() >= MAX_SEARCH_DEPTH) {
                    it.disable_recursion_pending();
                }
                if (!directory && !it->is_regular_file(itemError)) {
                    continue;
                }
                if (auto markup = highlightMatch(Util::toUtf8(path.filename()), search->query)) {
                    search->matches.push_back({path, directory, std::move(*markup)});
                    if (search->matches.size() >= MAX_SEARCH_RESULTS) {
                        break;
                    }
                }
            }
            g_idle_add(
                    +[](gpointer data) -> gboolean {
                        std::unique_ptr<Search> search(static_cast<Search*>(data));
                        if (!search->alive.expired() && !*search->cancelled) {
                            search->self->showSearchResults(search->matches);
                        }
                        return G_SOURCE_REMOVE;
                    },
                    search);
        }).detach();
    }

    void cancelSearch() {
        if (searchCancelled) {
            *searchCancelled = true;
            searchCancelled.reset();
        }
    }

    void showSearchResults(const std::vector<SearchMatch>& matches) {
        searchCancelled.reset();
        GtkTreeIter rootIter;
        if (!searching || !gtk_tree_model_get_iter_first(model(), &rootIter)) {
            return;
        }
        updatingTree = true;
        GtkTreeIter child;
        while (gtk_tree_model_iter_children(model(), &child, &rootIter)) {
            gtk_tree_store_remove(store, &child);  // the "Searching…" row
        }
        std::map<std::string, GtkTreeIter> directories{{Util::toUtf8(projectRoot), rootIter}};
        for (const auto& match: matches) {
            auto* parentIter = ensureDirectory(match.path.parent_path(), directories);
            auto iter = appendNode(parentIter, match.path, Util::toUtf8(match.path.filename()),
                                   kindFor(match.path, match.directory), match.markup, false);
            if (match.directory) {
                directories[Util::toUtf8(match.path)] = iter;
            }
        }
        if (matches.empty()) {
            appendInfoRow(&rootIter, _("No matching files"));
        }
        gtk_tree_view_expand_all(tree);
        updatingTree = false;
    }

    auto ensureDirectory(const fs::path& dir, std::map<std::string, GtkTreeIter>& directories) -> GtkTreeIter* {
        const auto key = Util::toUtf8(dir);
        if (auto found = directories.find(key); found != directories.end()) {
            return &found->second;
        }
        if (dir == dir.parent_path()) {
            return &directories.begin()->second;
        }
        auto* parentIter = ensureDirectory(dir.parent_path(), directories);
        const auto name = Util::toUtf8(dir.filename());
        auto iter = appendNode(parentIter, dir, name, KIND_DIRECTORY, escape(name), false);
        return &directories.emplace(key, iter).first->second;
    }

    /// Rebuilds the tree, keeping expansion state and selection. Keeps the scroll position unless the
    /// selection should be brought into view instead (e.g. after the search query changed).
    void refresh(bool keepScroll) {
        auto* adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tree));
        pendingScroll = gtk_adjustment_get_value(adjustment);
        const auto selected = selectedPath();
        buildTree();
        if (!selected.empty()) {
            reveal(Util::fromUtf8(selected.c_str()), !keepScroll);
        }
        if (keepScroll && !scrollSource) {
            scrollSource = g_idle_add(
                    +[](gpointer data) -> gboolean {
                        auto* self = static_cast<ProjectExplorerSidebar*>(data);
                        self->scrollSource = 0;
                        gtk_adjustment_set_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(self->tree)),
                                                 self->pendingScroll);
                        return G_SOURCE_REMOVE;
                    },
                    this);
        }
    }

    void scheduleRefresh() {
        if (refreshSource) {
            return;
        }
        refreshSource = g_timeout_add(
                REFRESH_DELAY_MS,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<ProjectExplorerSidebar*>(data);
                    self->refreshSource = 0;
                    // Fork: saving replaces the document file (via "<name>.xopp~"), which the monitors report as
                    // changes; only rebuild the tree if the shown entries actually changed
                    if (!self->searching && self->listingSignature() == self->shownListing) {
                        return G_SOURCE_REMOVE;
                    }
                    self->refresh(true);
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    void updateView() {
        const bool hasProject = !projectRoot.empty();
        gtk_widget_set_visible(searchRow, hasProject);
        gtk_widget_set_visible(scrolled, hasProject);
        gtk_widget_set_visible(emptyView, !hasProject);
    }

    // --- Git: status colors and sync ---

    void createSyncButton() {
        syncButton = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(syncButton), GTK_RELIEF_NONE);
        auto* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        syncIcon = gtk_image_new_from_icon_name("mail-send-receive-symbolic", GTK_ICON_SIZE_BUTTON);
        syncSpinner = gtk_spinner_new();
        syncLabel = gtk_label_new(nullptr);
        gtk_box_pack_start(GTK_BOX(content), syncIcon, false, false, 0);
        gtk_box_pack_start(GTK_BOX(content), syncSpinner, false, false, 0);
        gtk_box_pack_start(GTK_BOX(content), syncLabel, false, false, 0);
        gtk_container_add(GTK_CONTAINER(syncButton), content);
        g_signal_connect(syncButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<ProjectExplorerSidebar*>(data)->startSync();
                         }),
                         this);
    }

    void updateSyncButton() {
        gtk_widget_set_visible(syncButton, gitRepo && !projectRoot.empty());
        if (!gitRepo) {
            return;
        }
        gtk_widget_set_sensitive(syncButton, !syncing);
        gtk_widget_set_visible(syncIcon, !syncing);
        gtk_widget_set_visible(syncSpinner, syncing);
        if (syncing) {
            gtk_spinner_start(GTK_SPINNER(syncSpinner));
        } else {
            gtk_spinner_stop(GTK_SPINNER(syncSpinner));
        }

        const auto incoming = gitStatus.behind;
        const auto outgoing = gitStatus.ahead + gitStatus.changedFiles;
        std::string counts;
        if (incoming > 0) {
            counts += "↓" + std::to_string(incoming);
        }
        if (outgoing > 0) {
            counts += (counts.empty() ? "↑" : " ↑") + std::to_string(outgoing);
        }
        gtk_label_set_text(GTK_LABEL(syncLabel), counts.c_str());
        gtk_widget_set_visible(syncLabel, !counts.empty());

        std::string tooltip;
        if (syncing) {
            tooltip = _("Syncing…");
        } else if (incoming == 0 && outgoing == 0) {
            tooltip = _("Sync: everything is up to date");
        } else {
            tooltip = _("Sync: commit, pull and push");
            if (incoming > 0) {
                tooltip += "\n" + FS(_F("{1} incoming commits") % incoming);
            }
            if (gitStatus.changedFiles > 0) {
                tooltip += "\n" + FS(_F("{1} changed files") % gitStatus.changedFiles);
            }
            if (gitStatus.ahead > 0) {
                tooltip += "\n" + FS(_F("{1} unpushed commits") % gitStatus.ahead);
            }
        }
        gtk_widget_set_tooltip_text(syncButton, tooltip.c_str());
    }

    auto gitColorFor(const fs::path& path, Kind kind) const -> const char* {
        if (!gitRepo) {
            return nullptr;
        }
        const auto key = ProjectGit::pathKey(path);
        const auto& states = kind == KIND_DIRECTORY ? directoryStates : gitStatus.files;
        const auto found = states.find(key);
        return found == states.end() ? nullptr : colorFor(found->second);
    }

    /// Recomputes folder colors and recolors all rows in the tree
    void updateGitDecorations() {
        directoryStates.clear();
        if (gitRepo) {
            const auto rootKey = ProjectGit::pathKey(projectRoot);
            auto changes = gitStatus.files;
            for (const auto& deleted: gitStatus.deleted) {
                changes.emplace(ProjectGit::pathKey(deleted), ProjectGit::FileState::Modified);
            }
            for (const auto& [key, state]: changes) {
                if (key.size() <= rootKey.size() || key.compare(0, rootKey.size(), rootKey) != 0) {
                    continue;
                }
                // Mark all parent folders below the project root; modified wins over added
                for (auto dir = Util::fromUtf8(key.c_str()).parent_path();; dir = dir.parent_path()) {
                    const auto dirKey = ProjectGit::pathKey(dir);
                    if (dirKey.size() <= rootKey.size()) {
                        break;
                    }
                    auto& dirState = directoryStates[dirKey];
                    if (dirState != ProjectGit::FileState::Modified) {
                        dirState = state;
                    }
                }
            }
        }
        std::function<void(GtkTreeIter*)> recolor = [&](GtkTreeIter* parentIter) {
            GtkTreeIter child;
            for (bool valid = gtk_tree_model_iter_children(model(), &child, parentIter); valid;
                 valid = gtk_tree_model_iter_next(model(), &child)) {
                const auto kind = kindAt(&child);
                if (kind == KIND_PLACEHOLDER) {
                    continue;
                }
                gtk_tree_store_set(store, &child, COL_FOREGROUND,
                                   gitColorFor(Util::fromUtf8(pathAt(&child).c_str()), static_cast<Kind>(kind)), -1);
                recolor(&child);
            }
        };
        recolor(nullptr);
        updateSyncButton();
    }

    void setGitRepo(bool repo) {
        const bool becameRepo = repo && !gitRepo;
        gitRepo = repo;
        if (!repo) {
            gitStatus = {};
            gitToplevel.clear();
        }
        updateGitDecorations();
        if (becameRepo) {
            fetchRemote();
        }
    }

    void scheduleGitRefresh() {
        if (gitRefreshSource) {
            return;
        }
        gitRefreshSource = g_timeout_add(
                GIT_REFRESH_DELAY_MS,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<ProjectExplorerSidebar*>(data);
                    self->gitRefreshSource = 0;
                    self->refreshGit();
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    /// Reads the repository root and status of the project folder in the background
    void refreshGit(std::function<void()> done = {}) {
        if (projectRoot.empty() || !ProjectGit::isAvailable()) {
            setGitRepo(false);
            if (done) {
                done();
            }
            return;
        }
        // Background refreshes wait while another refresh or a sync is running; a sync refreshes itself (with done)
        if ((gitBusy || syncing) && !done) {
            gitRefreshPending = true;
            return;
        }
        gitBusy = true;
        const auto root = projectRoot;
        ProjectGit::run(root, {"rev-parse", "--show-toplevel"}, lifetime, [this, root, done](auto result) {
            if (root != projectRoot || !result.success) {
                finishGitRefresh(root == projectRoot && !result.success ? std::optional<bool>(false) : std::nullopt,
                                 done);
                return;
            }
            std::error_code error;
            const auto top = fs::weakly_canonical(Util::fromUtf8(trim(result.output).c_str()), error);
            // Only the project folder (cwd and pathspec "."), which may be part of a larger repository. Porcelain
            // paths are still relative to the repository root. --no-optional-locks keeps status from taking the
            // index lock, which would make a concurrent commit fail.
            ProjectGit::run(root,
                            withPathspec({"--no-optional-locks", "status", "--porcelain=v1", "-z", "--branch",
                                          "--untracked-files=all"}),
                            lifetime, [this, root, top, done](auto status) {
                                if (root == projectRoot && status.success) {
                                    gitToplevel = top;
                                    auto previousDeleted = std::move(gitStatus.deleted);
                                    gitStatus = ProjectGit::parseStatus(status.output, top);
                                    const bool deletedChanged = previousDeleted != gitStatus.deleted;
                                    finishGitRefresh(true, done);
                                    if (deletedChanged && !searching) {
                                        refresh(true);  // show or remove the rows of deleted entries
                                    }
                                } else {
                                    finishGitRefresh(std::nullopt, done);
                                }
                            });
        });
    }

    void finishGitRefresh(std::optional<bool> repo, const std::function<void()>& done) {
        gitBusy = false;
        if (repo) {
            setGitRepo(*repo);
        }
        if (done) {
            done();
        }
        if (gitRefreshPending) {
            gitRefreshPending = false;
            refreshGit();
        }
    }

    /// Fetches remote changes without any credential prompt, to show incoming commits
    void fetchRemote() {
        if (!gitRepo || syncing || fetchRunning || !gitStatus.hasUpstream) {
            return;
        }
        fetchRunning = true;
        ProjectGit::run(
                gitToplevel, {"-c", "credential.interactive=false", "fetch", "--quiet"}, lifetime,
                [this](auto) {
                    fetchRunning = false;
                    if (std::exchange(syncAfterFetch, false)) {
                        beginSync();
                    } else {
                        refreshGit();
                    }
                },
                120);  // e.g. ssh waiting for a passphrase nobody can enter
    }

    void startSync() {
        if (syncing || !gitRepo) {
            return;
        }
        syncing = true;
        autoSaver->setPaused(true);  // the pull may rewrite the open document
        updateSyncButton();
        // A running fetch holds ref locks the pull needs; continue when it is done
        if (fetchRunning) {
            syncAfterFetch = true;
            return;
        }
        beginSync();
    }

    void beginSync() {
        // Changes in the open document have to be on disk to be committed
        const auto documentPath = control->getDocument()->getFilepath();
        if (!documentPath.empty() && Util::isChildOrEquivalent(documentPath, gitToplevel) &&
            control->getUndoRedoHandler()->isChanged()) {
            std::weak_ptr<void> alive = lifetime;
            control->save([this, alive](bool saved) {
                if (alive.expired()) {
                    return;
                }
                if (saved) {
                    syncCommit();
                } else {
                    finishSync(_("The document could not be saved."), {});
                }
            });
        } else {
            syncCommit();
        }
    }

    void syncCommit() {
        // Upload an up-to-date annotated PDF along with the document
        annotationExporter->exportIfOutdated([this, alive = std::weak_ptr<void>(lifetime)] {
            if (!alive.expired()) {
                syncCommitExported();
            }
        });
    }

    void syncCommitExported() {
        // Refresh first, so that changes saved just now are included
        refreshGit([this] {
            if (!gitRepo) {
                finishSync(_("The project folder is no longer a git repository."), {});
                return;
            }
            if (gitStatus.changedFiles == 0) {
                syncPull();
                return;
            }
            // Only the project folder: other changes in a larger repository are not part of this project
            ProjectGit::run(projectRoot, withPathspec({"add", "-A"}), lifetime, [this](auto added) {
                if (!added.success) {
                    finishSync(_("Your changes could not be committed."), added.error);
                    return;
                }
                // Only the project folder ("--" "." from it): changes staged elsewhere in a larger repository stay
                ProjectGit::run(projectRoot, {"commit", "-m", commitMessage(), "--", "."}, lifetime,
                                [this](auto committed) {
                                    if (!committed.success) {
                                        finishSync(_("Your changes could not be committed."),
                                                   committed.error.empty() ? committed.output : committed.error);
                                        return;
                                    }
                                    syncPull();
                                });
            });
        });
    }

    auto commitMessage() const -> std::string {
        GDateTime* now = g_date_time_new_now_local();
        gchar* date = g_date_time_format(now, "%Y-%m-%d %H:%M");
        std::string message = std::string("Sync from ") + g_get_host_name() + " (" + date + ")";
        g_free(date);
        g_date_time_unref(now);
        return message;
    }

    void syncPull() {
        if (!gitStatus.hasUpstream) {
            syncPush();
            return;
        }
        const auto documentPath = control->getDocument()->getFilepath();
        const auto before = documentPath.empty() ? std::nullopt : AnnotationFiles::fileStamp(documentPath);
        ProjectGit::run(gitToplevel, {"pull", "--rebase"}, lifetime, [this, documentPath, before](auto pulled) {
            if (!pulled.success) {
                // A conflict needs a git tool; other failures (no network, no access) only need to be reported
                const bool conflict = pulled.output.find("CONFLICT") != std::string::npos ||
                                      pulled.error.find("CONFLICT") != std::string::npos ||
                                      pulled.error.find("could not apply") != std::string::npos;
                // Leave the repository as before the pull (if a rebase started at all); the local commit is kept
                ProjectGit::run(gitToplevel, {"rebase", "--abort"}, lifetime, [this, pulled, conflict](auto) {
                    finishSync(conflict ? _("Remote changes could not be merged with yours. Your changes are "
                                            "committed locally, but not uploaded. Please resolve the conflict with a "
                                            "git tool.") :
                                          _("Remote changes could not be downloaded or applied. Your changes are "
                                            "committed locally, but not uploaded."),
                               pulled.error);
                });
                return;
            }
            reloadIfChanged(documentPath, before);
            syncPush();
        });
    }

    /// Reloads the open document if the pull changed it on disk (it has no unsaved changes at this point)
    void reloadIfChanged(const fs::path& documentPath, std::optional<AnnotationFiles::FileStamp> before) {
        if (documentPath.empty() || control->getDocument()->getFilepath() != documentPath ||
            AnnotationFiles::fileStamp(documentPath) == before) {
            return;
        }
        auto reload = [this, documentPath] {
            const auto page = static_cast<int>(control->getCurrentPageNo());
            control->openFileWithoutSavingTheCurrentDocument(documentPath, false, page, [](bool) {});
        };
        if (!control->getUndoRedoHandler()->isChanged()) {
            reload();
            return;
        }
        // Edited during the sync: saving now would silently overwrite the version just pulled
        enum { RELOAD = 1, KEEP };
        XojMsgBox::askQuestion(
                parent, _("The open document was updated by the sync while you were editing it."),
                _("Reload it to get the updated version (your changes since the sync are discarded), or keep your "
                  "version (it replaces the updated one when you save)."),
                {{_("Reload"), RELOAD}, {_("Keep my version"), KEEP}},
                [reload, alive = std::weak_ptr<void>(lifetime)](int response) {
                    if (!alive.expired() && response == RELOAD) {
                        reload();
                    }
                });
    }

    void syncPush() {
        if (gitStatus.hasUpstream) {
            ProjectGit::run(gitToplevel, {"push"}, lifetime, [this](auto pushed) {
                finishSync(pushed.success ? nullptr : _("Your changes could not be uploaded."), pushed.error);
            });
            return;
        }
        // Without an upstream branch, publish to the first remote if there is one
        ProjectGit::run(gitToplevel, {"remote"}, lifetime, [this](auto remotes) {
            const auto remote = trim(remotes.output.substr(0, remotes.output.find('\n')));
            if (!remotes.success || remote.empty()) {
                finishSync(nullptr, {});
                return;
            }
            ProjectGit::run(gitToplevel, {"push", "-u", remote, "HEAD"}, lifetime, [this](auto pushed) {
                finishSync(pushed.success ? nullptr : _("Your changes could not be uploaded."), pushed.error);
            });
        });
    }

    void finishSync(const char* message, const std::string& details) {
        syncing = false;
        autoSaver->setPaused(false);
        refreshGit();
        updateSyncButton();
        if (message) {
            auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
                                                  message);
            if (const auto text = trim(details); !text.empty()) {
                gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", text.c_str());
            }
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
    }

    // --- File system monitoring ---

    void watch(const fs::path& dir) {
        const auto key = Util::toUtf8(dir);
        if (monitors.count(key)) {
            return;
        }
        GFile* file = g_file_new_for_path(key.c_str());
        GFileMonitor* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
        g_object_unref(file);
        if (!monitor) {
            return;
        }
        g_signal_connect(monitor, "changed",
                         G_CALLBACK(+[](GFileMonitor*, GFile* file, GFile*, GFileMonitorEvent event, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             gchar* name = g_file_get_basename(file);
                             // Neither .git nor the backups Xournal++ writes while saving are shown in the tree
                             const bool ignored = name && (std::strcmp(name, ".git") == 0 ||
                                                           AnnotationFiles::isRecoveryCopy(Util::fromUtf8(name)));
                             g_free(name);
                             if (ignored) {
                                 // Still changes the folder's modification time: do not take it for a missed change
                                 self->rememberFolderTime(file);
                                 return;
                             }
                             switch (event) {
                                 case G_FILE_MONITOR_EVENT_CREATED:
                                 case G_FILE_MONITOR_EVENT_DELETED:
                                 case G_FILE_MONITOR_EVENT_MOVED_IN:
                                 case G_FILE_MONITOR_EVENT_MOVED_OUT:
                                 case G_FILE_MONITOR_EVENT_RENAMED:
                                     self->scheduleRefresh();
                                     self->scheduleGitRefresh();
                                     break;
                                 case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
                                     self->scheduleGitRefresh();
                                     break;
                                 default:
                                     break;
                             }
                         }),
                         this);
        monitors.emplace(key, monitor);
        std::error_code error;
        folderTimes[key] = fs::last_write_time(dir, error);
    }

    void rememberFolderTime(GFile* changed) {
        GFile* folder = g_file_get_parent(changed);
        if (!folder) {
            return;
        }
        gchar* path = g_file_get_path(folder);
        g_object_unref(folder);
        if (path) {
            if (auto found = folderTimes.find(path); found != folderTimes.end()) {
                std::error_code error;
                found->second = fs::last_write_time(Util::fromUtf8(path), error);
            }
            g_free(path);
        }
    }

    /// File monitors can miss changes, e.g. on network or cloud folders: when the window is activated again, refresh
    /// if a shown folder was modified meanwhile. Comparing the folder times is cheap, unlike rebuilding the tree.
    void checkForMissedChanges() {
        if (projectRoot.empty()) {
            return;
        }
        for (const auto& [key, time]: folderTimes) {
            std::error_code error;
            if (fs::last_write_time(Util::fromUtf8(key.c_str()), error) != time || error) {
                scheduleRefresh();
                break;
            }
        }
        scheduleGitRefresh();  // e.g. commits made outside
    }

    void clearMonitors() {
        for (auto& [path, monitor]: monitors) {
            g_signal_handlers_disconnect_by_data(monitor, this);
            g_file_monitor_cancel(monitor);
            g_object_unref(monitor);
        }
        monitors.clear();
        folderTimes.clear();
    }

    // --- Selection and navigation ---

    /// The selected entry: its path, kind and, for a nested PDF, the document it belongs to
    struct Target {
        fs::path path;
        gint kind = KIND_PLACEHOLDER;
        fs::path owner;
    };

    auto selectedTarget() const -> Target {
        GtkTreeIter iter;
        if (!tree || !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tree), nullptr, &iter)) {
            return {};
        }
        Target target{Util::fromUtf8(pathAt(&iter).c_str()), kindAt(&iter), {}};
        if (target.kind == KIND_NESTED) {
            target.owner = owningDocument(&iter);
        }
        return target;
    }

    /// Whether the entry can be renamed or deleted (not the project folder itself)
    auto canModify(const Target& target) const -> bool {
        return target.kind != KIND_PLACEHOLDER && !isDeletedKind(target.kind) && !target.path.empty() &&
               target.path != projectRoot;
    }

    auto selectedPath() -> std::string {
        GtkTreeIter iter;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tree), nullptr, &iter) ||
            kindAt(&iter) == KIND_PLACEHOLDER) {
            return {};
        }
        return pathAt(&iter);
    }

    /// Expands the tree down to the given path and selects it.
    void reveal(const fs::path& target, bool scroll) {
        std::error_code error;
        const auto absolute = fs::weakly_canonical(target, error);
        if (error || projectRoot.empty() || !Util::isChildOrEquivalent(absolute, projectRoot)) {
            return;
        }
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter_first(model(), &iter)) {
            return;
        }
        for (const auto& part: absolute.lexically_relative(projectRoot)) {
            if (part == ".") {
                continue;
            }
            expandIter(&iter);
            const auto name = Util::toUtf8(part);
            GtkTreeIter child;
            bool found = false;
            for (bool valid = gtk_tree_model_iter_children(model(), &child, &iter); valid;
                 valid = gtk_tree_model_iter_next(model(), &child)) {
                if (kindAt(&child) == KIND_PLACEHOLDER) {
                    continue;
                }
                gchar* childName = nullptr;
                gtk_tree_model_get(model(), &child, COL_NAME, &childName, -1);
                found = childName && sameName(childName, name);
                g_free(childName);
                if (found) {
                    break;
                }
            }
            if (!found) {
                return;
            }
            iter = child;
        }
        GtkTreePath* path = gtk_tree_model_get_path(model(), &iter);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), path);
        if (scroll) {
            gtk_tree_view_scroll_to_cell(tree, path, nullptr, false, 0, 0);
        }
        gtk_tree_path_free(path);
    }

    void openFirstDocument() {
        std::function<bool(GtkTreeIter*)> visit = [&](GtkTreeIter* parentIter) {
            GtkTreeIter child;
            for (bool valid = gtk_tree_model_iter_children(model(), &child, parentIter); valid;
                 valid = gtk_tree_model_iter_next(model(), &child)) {
                if (kindAt(&child) == KIND_DOCUMENT) {
                    openDocument(Util::fromUtf8(pathAt(&child).c_str()));
                    return true;
                }
                if (visit(&child)) {
                    return true;
                }
            }
            return false;
        };
        visit(nullptr);
    }

    // --- Signal handlers ---

    static auto onTestExpandRow(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*, ProjectExplorerSidebar* self)
            -> gboolean {
        self->populate(iter);
        return false;
    }

    static void onRowExpanded(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*, ProjectExplorerSidebar* self) {
        if (self->kindAt(iter) == KIND_DIRECTORY) {
            gtk_tree_store_set(self->store, iter, COL_ICON, self->folderOpenIcon, -1);
        }
        if (self->searching) {
            return;
        }
        if (self->expandedPaths.insert(self->pathAt(iter)).second) {
            self->scheduleSave();
        }
        // Restore previously expanded subdirectories
        std::vector<GtkTreePath*> toExpand;
        GtkTreeIter child;
        for (bool valid = gtk_tree_model_iter_children(self->model(), &child, iter); valid;
             valid = gtk_tree_model_iter_next(self->model(), &child)) {
            // Folders and documents with nested PDFs
            if (gtk_tree_model_iter_has_child(self->model(), &child) &&
                self->expandedPaths.count(self->pathAt(&child))) {
                toExpand.push_back(gtk_tree_model_get_path(self->model(), &child));
            }
        }
        for (auto* path: toExpand) {
            gtk_tree_view_expand_row(self->tree, path, false);
            gtk_tree_path_free(path);
        }
    }

    static void onRowCollapsed(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*, ProjectExplorerSidebar* self) {
        if (self->kindAt(iter) == KIND_DIRECTORY) {
            gtk_tree_store_set(self->store, iter, COL_ICON, self->folderIcon, -1);
        }
        if (!self->searching && !self->updatingTree && self->expandedPaths.erase(self->pathAt(iter))) {
            self->scheduleSave();
        }
    }

    static void onRowActivated(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn*, ProjectExplorerSidebar* self) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(self->model(), &iter, path)) {
            return;
        }
        switch (self->kindAt(&iter)) {
            case KIND_DIRECTORY:
                if (gtk_tree_view_row_expanded(view, path)) {
                    gtk_tree_view_collapse_row(view, path);
                } else {
                    gtk_tree_view_expand_row(view, path, false);
                }
                break;
            case KIND_DOCUMENT:
                self->openDocument(Util::fromUtf8(self->pathAt(&iter).c_str()));
                break;
            case KIND_NESTED:
                // The document row above opens the PDFs for editing; the nested entries show them as they are
                self->openReadOnly(Util::fromUtf8(self->pathAt(&iter).c_str()));
                break;
            case KIND_FILE:
                // Fork: a todo.txt shows its tasks in the task tab
                if (TaskFiles::isTaskFile(Util::fromUtf8(self->pathAt(&iter).c_str()))) {
                    self->control->getSidebar()->selectTasks();
                }
                break;
            default:
                break;
        }
    }

    auto owningDocument(GtkTreeIter* nested) const -> fs::path {
        GtkTreeIter parentIter;
        if (!gtk_tree_model_iter_parent(model(), &parentIter, nested)) {
            return {};
        }
        return Util::fromUtf8(pathAt(&parentIter).c_str());
    }

    // --- Context menu ---

    void showContextMenu(const ContextMenuTrigger::Request& request) {
        auto at = request;
        gint kind = KIND_PLACEHOLDER;
        std::string target;
        fs::path document;  // what "Open" opens: the entry itself, or the document a nested PDF belongs to
        if (GtkTreePath* path = ContextMenuTrigger::rowOf(tree, at)) {
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model(), &iter, path)) {
                kind = kindAt(&iter);
                target = pathAt(&iter);
                document = kind == KIND_NESTED ? owningDocument(&iter) : Util::fromUtf8(target.c_str());
            }
            if (kind != KIND_PLACEHOLDER) {
                gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), path);
            }
            gtk_tree_path_free(path);
        }

        auto* menu = gtk_menu_new();
        if (isDeletedKind(kind)) {
            addMenuItem(menu, _("Restore"), [this, target] { restoreEntry(Util::fromUtf8(target.c_str())); });
            addMenuItem(menu, _("Copy Path"), [target] {
                gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), target.c_str(), -1);
            });
            popupMenu(menu, at);
            return;
        }
        if (kind == KIND_DOCUMENT || kind == KIND_NESTED) {
            addMenuItem(menu, _("Open"), [this, document] { openDocument(document); });
            if (Util::hasPdfFileExt(Util::fromUtf8(target.c_str()))) {
                // Read-only viewing, e.g. of the exported PDF, without creating annotations
                addMenuItem(menu, _("Open Read-Only"),
                            [this, target] { openReadOnly(Util::fromUtf8(target.c_str())); });
                addMenuItem(menu, _("Open in PDF Viewer"), [this, target] { openWithDefaultApp(target); });
            }
            addMenuSeparator(menu);
        }
        if (!projectRoot.empty()) {
            const auto dir = targetDirectory(target, kind);
            addMenuItem(menu, _("New Document"), [this, dir] { createDocument(dir); });
            addMenuItem(menu, _("New Folder"), [this, dir] { createFolder(dir); });
            addMenuSeparator(menu);
        }
        const bool isRoot = !target.empty() && Util::fromUtf8(target.c_str()) == projectRoot;
        if (kind != KIND_PLACEHOLDER && !isRoot) {
            addMenuItem(menu, _("Rename…"), [this, target, kind, document] {
                renameEntry(Util::fromUtf8(target.c_str()), kind, document);
            });
            addMenuItem(menu, _("Move to…"), [this, target, kind, document] {
                moveEntry(Util::fromUtf8(target.c_str()), kind, kind == KIND_NESTED ? document : fs::path());
            });
            addMenuItem(menu, _("Delete"),
                        [this, target, kind] { deleteEntry(Util::fromUtf8(target.c_str()), kind == KIND_DIRECTORY); });
            if (auto changed = changedFilesOf(Util::fromUtf8(target.c_str()), kind); !changed.empty()) {
                addMenuItem(menu, _("Discard Changes…"), [this, changed] { discardChanges(changed); });
            }
            addMenuSeparator(menu);
        }
        if (kind != KIND_PLACEHOLDER) {
            addMenuItem(menu, _("Show in File Manager"), [this, target, kind] {
                const auto path = Util::fromUtf8(target.c_str());
                showInFileManager(kind == KIND_DIRECTORY ? path : path.parent_path());
            });
            addMenuItem(menu, _("Copy Path"), [target] {
                gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), target.c_str(), -1);
            });
            addMenuSeparator(menu);
        }
        if (!searching) {
            addMenuItem(menu, _("Collapse All"), [this] { collapseAll(); });
        }
        addMenuItem(menu, _("Refresh"), [this] { refresh(true); });
        addMenuSeparator(menu);
        addMenuItem(menu, _("Open Folder…"), [this] { chooseFolder(); });
        addMenuItem(menu, _("Close Folder"), [this] { closeProject(); }, !projectRoot.empty());

        popupMenu(menu, at);
    }

    void popupMenu(GtkWidget* menu, const ContextMenuTrigger::Request& at) {
        ContextMenuTrigger::popup(GTK_MENU(menu), GTK_WIDGET(tree), at, true);
    }

    void collapseAll() {
        expandedPaths.clear();
        scheduleSave();
        gtk_tree_view_collapse_all(tree);
        GtkTreeIter rootIter;
        if (gtk_tree_model_get_iter_first(model(), &rootIter)) {
            expandIter(&rootIter);
        }
    }

    void openWithDefaultApp(const std::string& file) {
        if (auto uri = Util::toUri(Util::fromUtf8(file.c_str()))) {
            GError* error = nullptr;
            if (!g_app_info_launch_default_for_uri(uri->c_str(), nullptr, &error)) {
                showError(_("The file could not be opened."), error->message);
                g_error_free(error);
            }
        }
    }

    void showInFileManager(const fs::path& dir) {
        if (auto uri = Util::toUri(dir)) {
            GError* error = nullptr;
            if (!g_app_info_launch_default_for_uri(uri->c_str(), nullptr, &error)) {
                g_warning("Could not open folder: %s", error->message);
                g_error_free(error);
            }
        }
    }

    // --- Project handling ---

    void openProject(const fs::path& selected, bool persist = true) {
        std::error_code error;
        auto absolute = fs::weakly_canonical(selected, error);
        if (error || !fs::is_directory(absolute, error)) {
            showMessage(_("The selected folder could not be opened."));
            return;
        }
        if (absolute != projectRoot) {
            projectRoot = absolute;
            loadProjectState();
            setGitRepo(false);
            refreshGit();
        }
        if (persist) {
            saveState();
        }
        resetSearchAndBuild();
        const auto documentPath = control->getDocument()->getFilepath();
        if (!documentPath.empty() && Util::isChildOrEquivalent(documentPath, projectRoot)) {
            reveal(documentPath, true);
        } else if (!selectedFile.empty()) {
            reveal(Util::fromUtf8(selectedFile.c_str()), true);
        }
    }

    void closeProject() {
        saveState();
        projectRoot.clear();
        loadProjectState();
        setGitRepo(false);
        saveState();
        resetSearchAndBuild();
    }

    // --- Persistent state (project-explorer.ini in the config folder) ---

    auto stateFile() const -> std::string { return Util::toUtf8(Util::getConfigFile("project-explorer.ini")); }

    auto loadKeyFile() const -> GKeyFile* {
        GKeyFile* keyFile = g_key_file_new();
        g_key_file_load_from_file(keyFile, stateFile().c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
        return keyFile;
    }

    auto readString(GKeyFile* keyFile, const std::string& group, const char* key) const -> std::string {
        gchar* value = g_key_file_get_string(keyFile, group.c_str(), key, nullptr);
        std::string result = value ? value : "";
        g_free(value);
        return result;
    }

    void loadState() {
        GKeyFile* keyFile = loadKeyFile();
        auto project = readString(keyFile, "General", "project");
        g_key_file_free(keyFile);

        // Migrate the project folder from the previous single-line state file
        const auto legacyFile = Util::getConfigFile("last-project.txt");
        std::error_code error;
        if (project.empty()) {
            if (auto legacy = Util::readString(legacyFile, false)) {
                project = *legacy;
            }
        }
        fs::remove(legacyFile, error);

        if (!project.empty()) {
            auto path = fs::weakly_canonical(Util::fromUtf8(project.c_str()), error);
            if (!error && fs::is_directory(path, error)) {
                projectRoot = path;
            }
        }
        loadProjectState();
    }

    /// Restores expanded folders, selection and last document of the current project.
    void loadProjectState() {
        expandedPaths.clear();
        selectedFile.clear();
        lastDocumentFile.clear();
        if (projectRoot.empty()) {
            return;
        }
        GKeyFile* keyFile = loadKeyFile();
        const auto group = projectGroup(projectRoot);
        gsize count = 0;
        if (gchar** expanded = g_key_file_get_string_list(keyFile, group.c_str(), "expanded", &count, nullptr)) {
            for (gsize i = 0; i < count; i++) {
                expandedPaths.insert(expanded[i]);
            }
            g_strfreev(expanded);
        }
        selectedFile = readString(keyFile, group, "selected");
        lastDocumentFile = readString(keyFile, group, "document");
        g_key_file_free(keyFile);
    }

    void saveState() {
        GKeyFile* keyFile = loadKeyFile();
        if (projectRoot.empty()) {
            g_key_file_remove_key(keyFile, "General", "project", nullptr);
        } else {
            const auto group = projectGroup(projectRoot);
            g_key_file_set_string(keyFile, "General", "project", Util::toUtf8(projectRoot).c_str());
            g_key_file_set_string(keyFile, group.c_str(), "root", Util::toUtf8(projectRoot).c_str());
            std::vector<const gchar*> expanded;
            for (const auto& path: expandedPaths) {
                expanded.push_back(path.c_str());
            }
            g_key_file_set_string_list(keyFile, group.c_str(), "expanded", expanded.data(), expanded.size());
            g_key_file_set_string(keyFile, group.c_str(), "selected", selectedFile.c_str());
            g_key_file_set_string(keyFile, group.c_str(), "document", lastDocumentFile.c_str());
        }
        GError* error = nullptr;
        if (!g_key_file_save_to_file(keyFile, stateFile().c_str(), &error)) {
            g_warning("Could not save project explorer state: %s", error->message);
            g_error_free(error);
        }
        g_key_file_free(keyFile);
    }

    void scheduleSave() {
        if (saveSource || projectRoot.empty()) {
            return;
        }
        saveSource = g_timeout_add(
                SAVE_DELAY_MS,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<ProjectExplorerSidebar*>(data);
                    self->saveSource = 0;
                    self->saveState();
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    void resetSearchAndBuild() {
        query.clear();
        gtk_entry_set_text(GTK_ENTRY(searchEntry), "");
        buildTree();
    }

    void chooseFolder() {
        auto* chooser = gtk_file_chooser_native_new(_("Open project folder"), parent,
                                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("Open"), _("Cancel"));
        const auto initialFolder = projectRoot.empty() ? fs::path(g_get_home_dir()) : projectRoot;
        if (auto uri = Util::toUri(initialFolder)) {
            gtk_file_chooser_set_current_folder_uri(GTK_FILE_CHOOSER(chooser), uri->c_str());
        }
        g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* dialog, gint response, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             if (response == GTK_RESPONSE_ACCEPT) {
                                 GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
                                 if (file) {
                                     if (gchar* path = g_file_get_path(file)) {
                                         self->openProject(Util::fromUtf8(path));
                                         g_free(path);
                                     }
                                     g_object_unref(file);
                                 }
                             }
                             g_object_unref(dialog);
                         }),
                         this);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }

    // --- File operations ---

    /// The folder new entries are created in: the folder itself, the parent of a file, or the project root
    auto targetDirectory(const std::string& target, gint kind) const -> fs::path {
        if (target.empty() || kind == KIND_PLACEHOLDER) {
            return projectRoot;
        }
        const auto path = Util::fromUtf8(target.c_str());
        return kind == KIND_DIRECTORY ? path : path.parent_path();
    }

    static auto uniquePath(const fs::path& dir, const std::string& base, const std::string& extension) -> fs::path {
        std::error_code error;
        auto candidate = dir / Util::fromUtf8((base + extension).c_str());
        for (int i = 2; fs::exists(candidate, error); i++) {
            candidate = dir / Util::fromUtf8((base + " " + std::to_string(i) + extension).c_str());
        }
        return candidate;
    }

    /**
     * Asks for a name in a small dialog. The part before the extension is preselected, as in JetBrains IDEs.
     * Returns nullopt if the dialog was cancelled.
     */
    auto askForName(const char* title, const char* acceptLabel, const std::string& initial)
            -> std::optional<std::string> {
        auto* dialog = gtk_dialog_new_with_buttons(
                title, parent, static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                _("Cancel"), GTK_RESPONSE_CANCEL, acceptLabel, GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        gtk_window_set_resizable(GTK_WINDOW(dialog), false);
        auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(area), 12);
        gtk_box_set_spacing(GTK_BOX(area), 6);
        auto* label = gtk_label_new(_("Name"));
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_box_pack_start(GTK_BOX(area), label, false, false, 0);
        auto* entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), initial.c_str());
        gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
        gtk_box_pack_start(GTK_BOX(area), entry, false, false, 0);
        gtk_widget_show_all(area);

        const auto stem = Util::toUtf8(Util::fromUtf8(initial.c_str()).stem());
        gtk_widget_grab_focus(entry);
        gtk_editable_select_region(GTK_EDITABLE(entry), 0, static_cast<gint>(g_utf8_strlen(stem.c_str(), -1)));

        std::optional<std::string> result;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            gchar* stripped = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
            result = std::string(stripped);
            g_free(stripped);
        }
        gtk_widget_destroy(dialog);
        return result;
    }

    /// Returns an error message if name cannot be used for a new entry next to the others in dir
    static auto checkName(const std::string& name, const fs::path& target, const fs::path& existing = {})
            -> std::optional<std::pair<const char*, std::string>> {
        if (name.empty() || name == "." || name == ".." || name.find_first_of("<>:\"/\\|?*") != std::string::npos) {
            return std::pair{_("This name is not allowed."),
                             std::string(_("Names cannot contain any of these characters: < > : \" / \\ | ? *"))};
        }
        std::error_code error;
        // A case-only rename refers to the same file on Windows, so it does not count as a conflict
        if (fs::exists(target, error) && (existing.empty() || !fs::equivalent(existing, target, error))) {
            return std::pair{_("An entry with this name already exists."), name};
        }
        return std::nullopt;
    }

    /// Makes sure the new or renamed entry is visible and selected
    void showEntry(const fs::path& path) {
        if (searching) {
            query.clear();
            gtk_entry_set_text(GTK_ENTRY(searchEntry), "");
        }
        refresh(true);
        reveal(path, true);
    }

    void createDocument(const fs::path& dir) {
        // Suggest the default file name from the preferences, e.g. "2026-10-30-Note-14-05"
        Document unnamed(control);
        const auto stem = unnamed.createSaveFilename(Document::XOPP, control->getSettings()->getDefaultSaveName());
        std::string name = Util::toUtf8(uniquePath(dir, Util::toUtf8(stem), ".xopp").filename());
        fs::path path;
        while (true) {
            auto entered = askForName(_("New Document"), _("Create"), name);
            if (!entered) {
                return;
            }
            name = *entered;
            // Every new document is a Xournal++ file, whatever else the name looks like
            if (!Util::hasXournalFileExt(Util::fromUtf8(name.c_str()))) {
                name += ".xopp";
            }
            path = dir / Util::fromUtf8(name.c_str());
            if (auto problem = checkName(name, path)) {
                showError(problem->first, problem->second);
                continue;
            }
            break;
        }
        // Build an empty document with the default page template and write it like a normal save
        Document document(control);
        // The export name is derived from the document's file name
        document.setFilepath(path);
        control->addDefaultPage(std::nullopt, &document);
        SaveHandler handler;
        handler.prepareSave(&document, path);
        handler.setExportFilename(AnnotationFiles::exportFilenameForSave(document, path, *control->getSettings()));
        handler.saveTo(path);
        if (!handler.getErrorMessage().empty()) {
            showError(_("The document could not be created."), handler.getErrorMessage());
            return;
        }
        showEntry(path);
        openDocument(path);
    }

    void createFolder(const fs::path& dir) {
        std::string name = Util::toUtf8(uniquePath(dir, _("New Folder"), "").filename());
        while (true) {
            auto entered = askForName(_("New Folder"), _("Create"), name);
            if (!entered) {
                return;
            }
            name = *entered;
            const auto path = dir / Util::fromUtf8(name.c_str());
            if (auto problem = checkName(name, path)) {
                showError(problem->first, problem->second);
                continue;
            }
            std::error_code error;
            if (!fs::create_directory(path, error)) {
                showError(_("The folder could not be created."), error.message());
                return;
            }
            showEntry(path);
            return;
        }
    }

    /// owner: for a nested PDF (kind KIND_NESTED), the document it belongs to
    void renameEntry(const fs::path& oldPath, gint kind, const fs::path& owner = {}) {
        const bool document = kind == KIND_DOCUMENT || kind == KIND_NESTED;
        std::string name = Util::toUtf8(oldPath.filename());
        while (true) {
            auto entered = askForName(_("Rename"), _("Rename"), name);
            if (!entered || *entered == Util::toUtf8(oldPath.filename())) {
                return;
            }
            name = *entered;
            // Keep the document type if the extension was left out
            if (document && !Util::fromUtf8(name.c_str()).has_extension()) {
                name += Util::toUtf8(oldPath.extension());
            }
            const auto newPath = oldPath.parent_path() / Util::fromUtf8(name.c_str());
            if (auto problem = checkName(name, newPath, oldPath)) {
                showError(problem->first, problem->second);
                continue;
            }
            // Fork: a document is renamed together with its source PDF (if named like it) and exported PDF
            if (kind == KIND_DOCUMENT && isXoppFile(oldPath)) {
                if (!isXoppFile(newPath)) {
                    showError(_("A document keeps its file extension."), Util::toUtf8(oldPath.extension()));
                    continue;
                }
                auto group = AnnotationFiles::groupForRename(oldPath, newPath, *control->getSettings());
                if (auto problem = AnnotationFiles::groupConflict(group)) {
                    showError(_("An entry with this name already exists."), *problem);
                    continue;
                }
                renameDocumentGroup(oldPath, std::move(group));
                return;
            }
            // Fork: renaming the exported PDF keeps it the export of its document
            if (kind == KIND_NESTED && !owner.empty() && Util::hasPdfFileExt(oldPath) &&
                isSameFile(AnnotationFiles::exportPathForFile(owner, *control->getSettings()), oldPath)) {
                renameExport(owner, oldPath, newPath);
                return;
            }
            // Fork: the document annotating a renamed PDF keeps using it as background
            const auto annotating =
                    Util::hasPdfFileExt(oldPath) ? AnnotationFiles::documentForSource(oldPath) : std::nullopt;
            std::error_code error;
            fs::rename(oldPath, newPath, error);
            if (error) {
                showError(_("The entry could not be renamed."), error.message());
                return;
            }
            movedEntry(oldPath, newPath);
            if (annotating) {
                relinkSourcePdf(*annotating, newPath);
            }
            showEntry(newPath);
            return;
        }
    }

    static auto isSameFile(const fs::path& a, const fs::path& b) -> bool {
        std::error_code error;
        return a == b || fs::equivalent(a, b, error);
    }

    static auto isXoppFile(const fs::path& path) -> bool {
        gchar* folded = g_utf8_casefold(Util::toUtf8(path.extension()).c_str(), -1);
        const bool xopp = std::strcmp(folded, ".xopp") == 0;
        g_free(folded);
        return xopp;
    }

    /// Renames the files (in this order). If one fails, the renames done so far are undone. Returns the done renames,
    /// or nullopt on failure.
    auto renameAll(const std::vector<std::pair<fs::path, fs::path>>& moves)
            -> std::optional<std::vector<std::pair<fs::path, fs::path>>> {
        std::vector<std::pair<fs::path, fs::path>> done;
        for (const auto& [from, to]: moves) {
            std::error_code error;
            fs::rename(from, to, error);
            if (error) {
                const auto message = Util::toUtf8(from.filename()) + ": " + error.message();
                undoRenames(done);
                showError(_("The entry could not be renamed."), message);
                return std::nullopt;
            }
            done.emplace_back(from, to);
        }
        for (const auto& [from, to]: done) {
            movedEntry(from, to);
        }
        return done;
    }

    /// Renames back, in reverse order; reports files that could not be renamed back
    void undoRenames(const std::vector<std::pair<fs::path, fs::path>>& done) {
        std::string failed;
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code error;
            fs::rename(it->second, it->first, error);
            if (error) {
                failed += Util::toUtf8(it->second.filename()) + " → " + Util::toUtf8(it->first.filename()) + ": " +
                          error.message() + "\n";
            }
        }
        if (!failed.empty()) {
            showError(_("Some files could not be renamed back."), failed);
        }
    }


    /**
     * Renames (or moves) a document together with its files, so that everything stays consistent; the document records
     * the new names and paths. If recording them fails, the renames are undone. Returns whether it worked.
     */
    auto applyDocumentGroup(const AnnotationFiles::DocumentGroup& group) -> bool {
        auto done = renameAll(group.moves);
        if (!done) {
            return false;
        }
        if (group.newSource || group.newExportName) {
            if (auto problem = AnnotationFiles::relinkDocument(group.newXopp, group.newSource, group.newExportName,
                                                               *control->getSettings());
                !problem.empty()) {
                undoRenames(*done);
                for (const auto& [from, to]: *done) {
                    movedEntry(to, from);
                }
                showError(FC(_F("“{1}” could not be updated to use the renamed PDF.") %
                             Util::toUtf8(group.newXopp.filename())),
                          problem);
                return false;
            }
        }
        showEntry(group.newXopp);
        return true;
    }

    void renameDocumentGroup(const fs::path& oldXopp, AnnotationFiles::DocumentGroup group) {
        std::vector<fs::path> affected;
        for (const auto& move: group.moves) {
            affected.push_back(move.first);
        }
        withOpenDocumentClosed(affected, [this, group, oldXopp](const fs::path& open) -> std::optional<fs::path> {
            if (!applyDocumentGroup(group)) {
                return std::nullopt;
            }
            return isSameFile(open, oldXopp) ? group.newXopp : open;
        });
    }

    /// Whether the open document or its background PDF is one of the paths or inside one of them
    auto affectsOpenDocument(const std::vector<fs::path>& paths) const -> bool {
        Document* document = control->getDocument();
        document->lock_shared();
        const auto documentPath = document->getFilepath();
        const auto pdfPath = document->getPdfFilepath();
        document->unlock_shared();
        for (const auto& path: paths) {
            if ((!documentPath.empty() && Util::isChildOrEquivalent(documentPath, path)) ||
                (!pdfPath.empty() && Util::isChildOrEquivalent(pdfPath, path))) {
                return true;
            }
        }
        return false;
    }

    /**
     * Runs a file operation on paths. The open document's files cannot be renamed, moved or deleted while it is open
     * (its PDF is in use): if they are involved, the document is saved (with a regular save, so nothing is discarded),
     * closed, and afterwards opened again at the same page. perform gets the open document's path and returns where
     * to open it again (its new path; empty for a new document), or nullopt if the operation failed (it is opened from
     * where it was). With discardChanges, unsaved changes are dropped instead of saved.
     */
    void withOpenDocumentClosed(const std::vector<fs::path>& paths,
                                std::function<std::optional<fs::path>(const fs::path& open)> perform,
                                bool discardChanges = false) {
        if (!affectsOpenDocument(paths)) {
            perform({});
            refresh(true);
            return;
        }
        const auto open = control->getDocument()->getFilepath();
        const auto page = static_cast<int>(control->getCurrentPageNo());
        std::weak_ptr<void> alive = lifetime;
        auto closeAndPerform = [this, alive, perform, open, page]() {
            control->close(
                    [this, alive, perform, open, page](bool closed) {
                        if (alive.expired() || !closed) {
                            return;
                        }
                        control->releaseDocumentFiles();
                        const auto reopen = perform(open).value_or(open);
                        std::error_code error;
                        if (!reopen.empty() && fs::exists(reopen, error)) {
                            control->openFileWithoutSavingTheCurrentDocument(reopen, false, page, [](bool) {});
                        } else {
                            control->newFile();
                        }
                        refresh(true);
                    },
                    true, false, true);
        };
        if (discardChanges || !control->getUndoRedoHandler()->isChanged()) {
            closeAndPerform();
            return;
        }
        control->save([alive, closeAndPerform](bool saved) {
            if (!alive.expired() && saved) {
                closeAndPerform();
            }
        });
    }

    // --- Moving ---


    /**
     * Asks for a folder of the project to move path to. Folders that cannot be the target (the folder itself, folders
     * inside it, the folder it is in) are not offered.
     */
    auto askForTargetFolder(const fs::path& path) -> std::optional<fs::path> {
        auto* dialog = gtk_dialog_new_with_buttons(FC(_F("Move “{1}”") % Util::toUtf8(path.filename())), parent,
                                                   GTK_DIALOG_MODAL, _("Cancel"), GTK_RESPONSE_CANCEL, _("Move"),
                                                   GTK_RESPONSE_ACCEPT, nullptr);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 520);
        auto* folders = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING);  // name, path
        std::error_code error;
        const auto current = path.parent_path();
        std::function<void(const fs::path&, GtkTreeIter*, int)> add = [&](const fs::path& dir, GtkTreeIter* parentIter,
                                                                          int depth) {
            std::vector<fs::path> children;
            for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
                std::error_code typeError;
                const auto name = Util::toUtf8(it->path().filename());
                if (it->is_directory(typeError) && !isIgnored(it->path()) && !name.empty() && name.front() != '.' &&
                    !Util::isChildOrEquivalent(it->path(), path)) {
                    children.push_back(it->path());
                }
            }
            error.clear();
            std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
                gchar* keyA = g_utf8_collate_key_for_filename(Util::toUtf8(a.filename()).c_str(), -1);
                gchar* keyB = g_utf8_collate_key_for_filename(Util::toUtf8(b.filename()).c_str(), -1);
                const bool less = std::strcmp(keyA, keyB) < 0;
                g_free(keyA);
                g_free(keyB);
                return less;
            });
            for (const auto& child: children) {
                GtkTreeIter iter;
                gtk_tree_store_insert_with_values(folders, &iter, parentIter, -1, 0,
                                                  Util::toUtf8(child.filename()).c_str(), 1,
                                                  Util::toUtf8(child).c_str(), -1);
                if (depth < 8) {
                    add(child, &iter, depth + 1);
                }
            }
        };
        GtkTreeIter rootIter;
        gtk_tree_store_insert_with_values(folders, &rootIter, nullptr, -1, 0,
                                          Util::toUtf8(projectRoot.filename()).c_str(), 1,
                                          Util::toUtf8(projectRoot).c_str(), -1);
        add(projectRoot, &rootIter, 1);

        auto* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(folders));
        g_object_unref(folders);
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), false);
        auto* column = gtk_tree_view_column_new();
        auto* iconRenderer = gtk_cell_renderer_pixbuf_new();
        g_object_set(iconRenderer, "icon-name", "folder-symbolic", "ypad", 8, nullptr);
        gtk_tree_view_column_pack_start(column, iconRenderer, false);
        auto* textRenderer = gtk_cell_renderer_text_new();
        g_object_set(textRenderer, "ypad", 8, nullptr);  // large enough to tap
        gtk_tree_view_column_pack_start(column, textRenderer, true);
        gtk_tree_view_column_add_attribute(column, textRenderer, "text", 0);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
        GtkTreePath* first = gtk_tree_path_new_first();
        gtk_tree_view_expand_row(GTK_TREE_VIEW(view), first, false);
        gtk_tree_path_free(first);
        auto* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled), view);
        gtk_widget_set_vexpand(scrolled, true);
        auto* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), 8);
        gtk_box_pack_start(GTK_BOX(content), scrolled, true, true, 0);

        // Only a folder other than the current one can be chosen
        auto* moveButton = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        gtk_style_context_add_class(gtk_widget_get_style_context(moveButton), "suggested-action");
        gtk_widget_set_sensitive(moveButton, false);
        struct Choice {
            GtkWidget* button;
            fs::path current;
            fs::path chosen;
        } choice{moveButton, current, {}};
        g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), "changed",
                         G_CALLBACK(+[](GtkTreeSelection* selection, Choice* choice) {
                             GtkTreeModel* model = nullptr;
                             GtkTreeIter iter;
                             choice->chosen.clear();
                             if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
                                 gchar* value = nullptr;
                                 gtk_tree_model_get(model, &iter, 1, &value, -1);
                                 choice->chosen = Util::fromUtf8(value ? value : "");
                                 g_free(value);
                             }
                             gtk_widget_set_sensitive(
                                     choice->button,
                                     !choice->chosen.empty() && !isSameFile(choice->chosen, choice->current));
                         }),
                         &choice);
        g_signal_connect(view, "row-activated",
                         G_CALLBACK(+[](GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*, GtkWidget* dialog) {
                             gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
                         }),
                         dialog);

        gtk_widget_show_all(dialog);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        auto chosen = choice.chosen;
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_ACCEPT || chosen.empty() || isSameFile(chosen, current)) {
            return std::nullopt;
        }
        return chosen;
    }

    /// Moves an entry to another folder of the project; a document moves with its files like when renaming
    void moveEntry(const fs::path& path, gint kind, const fs::path& owner) {
        const auto target = askForTargetFolder(path);
        if (!target) {
            return;
        }
        if (kind == KIND_DOCUMENT && isXoppFile(path)) {
            auto group = AnnotationFiles::groupForMove(path, *target, *control->getSettings());
            if (auto problem = AnnotationFiles::groupConflict(group)) {
                showError(_("An entry with this name already exists."), *problem);
                return;
            }
            renameDocumentGroup(path, std::move(group));
            return;
        }
        const auto newPath = *target / path.filename();
        std::error_code error;
        if (fs::exists(newPath, error)) {
            showError(_("An entry with this name already exists."), Util::toUtf8(newPath.filename()));
            return;
        }
        // A PDF keeps being the background of the document annotating it, wherever it is moved
        const auto annotating = Util::hasPdfFileExt(path) ? (kind == KIND_NESTED && !owner.empty() ?
                                                                     std::optional<fs::path>(owner) :
                                                                     AnnotationFiles::documentForSource(path)) :
                                                            std::nullopt;
        withOpenDocumentClosed(
                {path}, [this, path, newPath, annotating](const fs::path& open) -> std::optional<fs::path> {
                    if (!renameAll({{path, newPath}})) {
                        return std::nullopt;
                    }
                    if (annotating && isSameFile(AnnotationFiles::recordedSourcePdf(*annotating), path)) {
                        if (auto problem =
                                    AnnotationFiles::relinkSourcePdf(*annotating, newPath, *control->getSettings());
                            !problem.empty()) {
                            showError(FC(_F("“{1}” could not be updated to use the renamed PDF.") %
                                         Util::toUtf8(annotating->filename())),
                                      problem);
                        }
                    }
                    showEntry(newPath);
                    // The open document was moved with a folder: open it from there
                    if (!open.empty() && Util::isChildOrEquivalent(open, path)) {
                        return newPath / open.lexically_relative(path);
                    }
                    return open;
                });
    }

    /// Renames the exported PDF of a document, which records the new name so it keeps exporting there
    void renameExport(const fs::path& xopp, const fs::path& oldPdf, const fs::path& newPdf) {
        auto relink = [this, xopp, oldPdf, newPdf]() {
            if (!renameAll({{oldPdf, newPdf}})) {
                return;
            }
            if (auto problem = AnnotationFiles::relinkDocument(xopp, std::nullopt, Util::toUtf8(newPdf.filename()),
                                                               *control->getSettings());
                !problem.empty()) {
                showError(FC(_F("“{1}” could not be updated to use the renamed PDF.") % Util::toUtf8(xopp.filename())),
                          problem);
            }
            showEntry(newPdf);
        };
        // The document records the name in its file: save an open document first, so that rewriting its file does not
        // happen behind unsaved changes (the next save reads the recorded name from the file)
        if (isSameFile(control->getDocument()->getFilepath(), xopp) && control->getUndoRedoHandler()->isChanged()) {
            std::weak_ptr<void> alive = lifetime;
            control->save([alive, relink](bool saved) {
                if (!alive.expired() && saved) {
                    relink();
                }
            });
            return;
        }
        relink();
    }

    void relinkSourcePdf(const fs::path& xopp, const fs::path& pdf) {
        Document* document = control->getDocument();
        if (Util::isChildOrEquivalent(document->getFilepath(), xopp)) {
            // Open: point it at the new name and save it, so the file records it as well
            document->lock();
            document->setPdfAttributes(pdf, document->isAttachPdf());
            document->unlock();
            if (control->isAutoSaveAllowed()) {
                control->save([](bool) {});
            }
            return;
        }
        if (auto problem = AnnotationFiles::relinkSourcePdf(xopp, pdf, *control->getSettings()); !problem.empty()) {
            showError(FC(_F("“{1}” could not be updated to use the renamed PDF.") % Util::toUtf8(xopp.filename())),
                      problem);
        }
    }

    /// Updates all remembered paths (open document, selection, expanded folders) after a rename
    void movedEntry(const fs::path& oldPath, const fs::path& newPath) {
        const auto oldPrefix = Util::toUtf8(oldPath);
        auto rebase = [&](const std::string& value) -> std::optional<std::string> {
            if (value == oldPrefix) {
                return Util::toUtf8(newPath);
            }
            if (value.size() > oldPrefix.size() && value.compare(0, oldPrefix.size(), oldPrefix) == 0 &&
                (value[oldPrefix.size()] == '/' || value[oldPrefix.size()] == '\\')) {
                return Util::toUtf8(newPath) + value.substr(oldPrefix.size());
            }
            return std::nullopt;
        };
        std::set<std::string> expanded;
        for (const auto& path: expandedPaths) {
            expanded.insert(rebase(path).value_or(path));
        }
        expandedPaths = std::move(expanded);
        selectedFile = rebase(selectedFile).value_or(selectedFile);
        lastDocumentFile = rebase(lastDocumentFile).value_or(lastDocumentFile);

        Document* document = control->getDocument();
        if (auto moved = rebase(Util::toUtf8(document->getFilepath()))) {
            document->lock();
            document->setFilepath(Util::fromUtf8(moved->c_str()));
            document->unlock();
            control->updateWindowTitle();
        }
        scheduleSave();
    }

    void deleteEntry(const fs::path& path, bool directory) {
        const auto name = Util::toUtf8(path.filename());
        // A document with its PDF and exported PDF: they can go along
        const auto group = !directory && isXoppFile(path) ? AnnotationFiles::groupFiles(path, *control->getSettings()) :
                                                            std::vector<fs::path>{path};
        enum { DELETE_ONE = 1, DELETE_GROUP };
        auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                              FC(_F("Delete “{1}”?") % name));
        std::string details = directory ? _("The folder and everything in it will be moved to the trash.") :
                                          _("The file will be moved to the trash.");
        if (group.size() > 1) {
            details += std::string("\n\n") + _("Belonging to the document:");
            for (size_t i = 1; i < group.size(); i++) {
                details += "\n• " + Util::toUtf8(group[i].filename());
            }
        }
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", details.c_str());
        gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
        auto* deleteButton = gtk_dialog_add_button(GTK_DIALOG(dialog),
                                                   group.size() > 1 ? _("Only the Document") : _("Delete"), DELETE_ONE);
        if (group.size() > 1) {
            deleteButton = gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete All"), DELETE_GROUP);
        }
        gtk_style_context_add_class(gtk_widget_get_style_context(deleteButton), "destructive-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != DELETE_ONE && response != DELETE_GROUP) {
            return;
        }
        const auto paths = response == DELETE_GROUP ? group : std::vector<fs::path>{path};

        // The user deletes the open document on purpose: its unsaved changes are discarded instead of saving them
        // (which would write the deleted file again). If only its PDF goes, the annotations are kept.
        const auto open = control->getDocument()->getFilepath();
        const bool deletesDocument = !open.empty() && Util::isChildOrEquivalent(open, path);
        withOpenDocumentClosed(
                paths,
                [this, paths, deletesDocument](const fs::path& open) -> std::optional<fs::path> {
                    for (const auto& file: paths) {
                        if (!trash(file)) {
                            break;  // the error was shown
                        }
                    }
                    return deletesDocument ? fs::path() : open;
                },
                deletesDocument);
    }

    // --- Discarding changes ---

    /**
     * The files git tracks that changed since the last commit: of a document, those of its group (the document, the
     * PDF it owns, its export); otherwise the file itself. New, untracked files are not included: discarding them
     * would mean deleting them.
     */
    auto changedFilesOf(const fs::path& path, gint kind) const -> std::vector<fs::path> {
        std::vector<fs::path> changed;
        if (!gitRepo || kind == KIND_DIRECTORY || isDeletedKind(kind) || kind == KIND_PLACEHOLDER) {
            return changed;
        }
        const auto candidates = kind == KIND_DOCUMENT && isXoppFile(path) ?
                                        AnnotationFiles::groupFiles(path, *control->getSettings()) :
                                        std::vector<fs::path>{path};
        for (const auto& file: candidates) {
            auto state = gitStatus.files.find(ProjectGit::pathKey(file));
            if (state != gitStatus.files.end() && state->second == ProjectGit::FileState::Modified) {
                changed.push_back(file);
            }
        }
        return changed;
    }

    /// Resets the files to the last commit, after asking. The open document is closed first, without saving.
    void discardChanges(const std::vector<fs::path>& files) {
        std::string names;
        for (const auto& file: files) {
            names += (names.empty() ? "• " : "\n• ") + Util::toUtf8(file.filename());
        }
        auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                              _("Discard the changes since the last sync?"));
        gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog), "%s",
                (names + "\n\n" + _("The files are reset to the state of the last sync. This cannot be undone."))
                        .c_str());
        gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
        auto* discard = gtk_dialog_add_button(GTK_DIALOG(dialog), _("Discard Changes"), GTK_RESPONSE_ACCEPT);
        gtk_style_context_add_class(gtk_widget_get_style_context(discard), "destructive-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_ACCEPT || gitToplevel.empty()) {
            return;
        }

        std::vector<std::string> args = {"restore", "--source=HEAD", "--staged", "--worktree", "--"};
        for (const auto& file: files) {
            args.push_back(Util::toUtf8(file));
        }
        auto restore = [this, args](std::function<void()> done) {
            ProjectGit::run(gitToplevel, args, lifetime, [this, done](auto result) {
                if (!result.success) {
                    showError(_("The changes could not be discarded."), result.error);
                }
                refreshGit([this, done] {
                    refresh(true);
                    done();
                });
            });
        };
        if (!affectsOpenDocument(files)) {
            restore([] {});
            return;
        }
        // The open document goes back to the committed state: close it without saving, reopen it afterwards
        const auto open = control->getDocument()->getFilepath();
        const auto page = static_cast<int>(control->getCurrentPageNo());
        std::weak_ptr<void> alive = lifetime;
        control->close(
                [this, alive, restore, open, page](bool closed) {
                    if (alive.expired() || !closed) {
                        return;
                    }
                    control->releaseDocumentFiles();  // git can only replace the PDF once it is no longer open
                    restore([this, open, page] {
                        std::error_code error;
                        if (!open.empty() && fs::exists(open, error)) {
                            control->openFileWithoutSavingTheCurrentDocument(open, false, page, [](bool) {});
                        } else {
                            control->newFile();
                        }
                    });
                },
                true, false, true);
    }

    auto trash(const fs::path& path) -> bool {
        GFile* file = g_file_new_for_path(Util::toUtf8(path).c_str());
        GError* error = nullptr;
        const bool trashed = g_file_trash(file, nullptr, &error);
        g_object_unref(file);
        if (!trashed) {
            showError(_("The entry could not be moved to the trash."), error->message);
            g_error_free(error);
        }
        return trashed;
    }

    static auto onKeyPress(GtkWidget*, GdkEventKey* event, ProjectExplorerSidebar* self) -> gboolean {
        if (event->keyval != GDK_KEY_F2 && event->keyval != GDK_KEY_Delete) {
            return false;
        }
        GtkTreeIter iter;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(self->tree), nullptr, &iter)) {
            return false;
        }
        const auto target = self->selectedTarget();
        if (!self->canModify(target)) {
            return false;
        }
        if (event->keyval == GDK_KEY_F2) {
            self->renameEntry(target.path, target.kind, target.owner);
        } else {
            self->deleteEntry(target.path, target.kind == KIND_DIRECTORY);
        }
        return true;
    }

    void showError(const char* message, const std::string& details) {
        auto* dialog =
                gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);
        if (!details.empty()) {
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", details.c_str());
        }
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    /// The file of the open document. A PDF viewed read-only has no document file yet: the PDF itself
    auto currentDocumentPath() const -> fs::path {
        auto documentPath = control->getDocument()->getFilepath();
        return documentPath.empty() ? control->getDocument()->getPdfFilepath() : documentPath;
    }

    /// Opens a document like an IDE switches files: pending changes of the current one are saved first. If that fails
    /// (the error was shown), the current document stays open and selected, so its changes are not lost
    void openDocument(fs::path path) {
        autoSaver->saveNow([this, path = std::move(path)](bool saved) {
            if (saved) {
                control->openFile(path);
            } else {
                reveal(currentDocumentPath(), true);
            }
        });
    }

    void openReadOnly(fs::path pdf) {
        autoSaver->saveNow([this, pdf = std::move(pdf)](bool saved) {
            if (saved) {
                control->openPdfReadOnly(pdf);
            } else {
                reveal(currentDocumentPath(), true);
            }
        });
    }

    void showMessage(const char* message) {
        auto* dialog =
                gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    std::unique_ptr<DocumentAutoSaver> autoSaver;
    std::unique_ptr<AnnotationExporter> annotationExporter;
    GtkWidget* root{};
    GtkWindow* parent{};
    GtkSearchEntry* searchEntry{};
    GtkTreeStore* store{};
    GtkTreeView* tree{};
    GtkWidget* scrolled{};
    GtkWidget* emptyView{};
    GtkWidget* searchRow{};
    GtkWidget* syncButton{};
    GtkWidget* syncIcon{};
    GtkWidget* syncSpinner{};
    GtkWidget* syncLabel{};
    /// Expires when the explorer is destroyed, so late git results are dropped
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    /// Set when the running search is outdated (the query changed or the explorer is gone)
    std::shared_ptr<std::atomic<bool>> searchCancelled;
    bool gitRepo = false;
    bool gitBusy = false;
    bool gitRefreshPending = false;
    bool syncing = false;
    std::string shownListing;  ///< listingSignature() of the tree as last built
    bool fetchRunning = false;
    bool syncAfterFetch = false;
    fs::path gitToplevel;
    ProjectGit::Status gitStatus;
    std::map<std::string, ProjectGit::FileState> directoryStates;
    guint gitRefreshSource = 0;
    guint gitStatusTimer = 0;
    guint gitFetchTimer = 0;
    bool gitStatusMissed = false;  ///< a periodic refresh was skipped while the window was in the background
    bool gitFetchMissed = false;

    auto isInBackground() const -> bool { return focusWindow && !gtk_window_is_active(focusWindow); }

    void catchUpGit() {
        if (std::exchange(gitFetchMissed, false)) {
            gitStatusMissed = false;
            fetchRemote();  // refreshes the status afterwards
            if (!fetchRunning) {
                refreshGit();  // no upstream to fetch from
            }
        } else if (std::exchange(gitStatusMissed, false)) {
            refreshGit();
        }
    }
    std::unique_ptr<ContextMenuTrigger> contextMenu;
    fs::path projectRoot;
    std::string query;
    bool searching = false;
    bool updatingTree = false;
    std::set<std::string> expandedPaths;
    std::string selectedFile;
    std::string lastDocumentFile;
    GIcon* folderIcon{};
    GIcon* folderOpenIcon{};
    GIcon* imageIcon{};
    GIcon* fileIcon{};
    GIcon* xournalIcon{};
    GIcon* pdfIcon{};
    GIcon* annotatedPdfIcon{};
    GIcon* trashIcon{};
    GIcon* tasksIcon{};  ///< todo.txt files  ///< deleted entries
    guint saveSource = 0;
    std::map<std::string, GFileMonitor*> monitors;
    std::map<std::string, fs::file_time_type> folderTimes;  ///< modification times of the watched folders
    GtkWindow* focusWindow{};                               ///< weak pointer, see checkForMissedChanges()
    guint refreshSource = 0;
    /// Whether another sidebar tab was shown since the tree was built (the file monitors keep it current otherwise)
    bool refreshOnEnable = false;
    guint scrollSource = 0;
    double pendingScroll = 0;
};

auto createProjectExplorerSidebar(Control* control) -> std::unique_ptr<AbstractSidebarPage> {
    return std::make_unique<ProjectExplorerSidebar>(control);
}
