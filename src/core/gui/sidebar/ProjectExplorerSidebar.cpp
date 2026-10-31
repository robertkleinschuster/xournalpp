#include "ProjectExplorerSidebar.h"

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
#include "control/settings/PageTemplateSettings.h"
#include "control/settings/Settings.h"
#include "control/xojfile/SaveHandler.h"
#include "gui/GladeSearchpath.h"
#include "gui/MainWindow.h"
#include "gui/sidebar/AbstractSidebarPage.h"
#include "gui/sidebar/ProjectGit.h"
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
enum Kind : gint { KIND_DIRECTORY, KIND_DOCUMENT, KIND_FILE, KIND_PLACEHOLDER, KIND_NESTED };

constexpr size_t MAX_SEARCH_RESULTS = 1000;
constexpr int MAX_SEARCH_DEPTH = 16;
constexpr guint REFRESH_DELAY_MS = 250;
constexpr guint SAVE_DELAY_MS = 500;
constexpr guint GIT_REFRESH_DELAY_MS = 1000;
constexpr guint GIT_STATUS_INTERVAL_S = 60;
constexpr guint GIT_FETCH_INTERVAL_S = 300;

constexpr const char* COLOR_ADDED = "#2ea043";
constexpr const char* COLOR_MODIFIED = "#3584e4";

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
    return path.filename() == ".git" || AnnotationFiles::isRecoveryCopy(path);
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
    auto ext = casefold(utf8(path.extension()));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".svg" || ext == ".bmp";
}

/// Loads an icon shipped in ui/pixmaps, falling back to a themed icon if the file is missing.
auto loadPixmapIcon(Control* control, const char* file, const char* fallback) -> GIcon* {
    const auto path = control->getGladeSearchPath()->findFile("pixmaps", file);
    if (path.empty()) {
        return g_themed_icon_new(fallback);
    }
    GFile* gfile = g_file_new_for_path(utf8(path).c_str());
    GIcon* icon = g_file_icon_new(gfile);
    g_object_unref(gfile);
    return icon;
}

/// Key file group for a project, independent of characters in its path
auto projectGroup(const fs::path& project) -> std::string {
    gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, utf8(project).c_str(), -1);
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
    if ((kindA == KIND_DIRECTORY) != (kindB == KIND_DIRECTORY)) {
        result = kindA == KIND_DIRECTORY ? -1 : 1;
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
                    static_cast<ProjectExplorerSidebar*>(data)->checkForMissedChanges();
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
        g_object_set(iconRenderer, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 4, "ypad", 3, nullptr);
        gtk_tree_view_column_pack_start(column, iconRenderer, false);
        gtk_tree_view_column_add_attribute(column, iconRenderer, "gicon", COL_ICON);
        gtk_tree_view_column_add_attribute(column, iconRenderer, "sensitive", COL_SENSITIVE);
        auto* textRenderer = gtk_cell_renderer_text_new();
        g_object_set(textRenderer, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 3, nullptr);
        gtk_tree_view_column_pack_start(column, textRenderer, true);
        gtk_tree_view_column_add_attribute(column, textRenderer, "markup", COL_MARKUP);
        gtk_tree_view_column_add_attribute(column, textRenderer, "sensitive", COL_SENSITIVE);
        gtk_tree_view_column_add_attribute(column, textRenderer, "foreground", COL_FOREGROUND);
        gtk_tree_view_append_column(tree, column);

        g_signal_connect(tree, "test-expand-row", G_CALLBACK(onTestExpandRow), this);
        g_signal_connect(tree, "row-expanded", G_CALLBACK(onRowExpanded), this);
        g_signal_connect(tree, "row-collapsed", G_CALLBACK(onRowCollapsed), this);
        g_signal_connect(tree, "row-activated", G_CALLBACK(onRowActivated), this);
        g_signal_connect(tree, "button-press-event", G_CALLBACK(onButtonPress), this);
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

        longPress = gtk_gesture_long_press_new(GTK_WIDGET(tree));
        gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), true);
        g_signal_connect(longPress, "pressed", G_CALLBACK(onLongPress), this);

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
            reveal(pathFromUtf8(selectedFile.c_str()), true);
        }

        refreshGit();
        gitStatusTimer = g_timeout_add_seconds(
                GIT_STATUS_INTERVAL_S,
                +[](gpointer data) -> gboolean {
                    static_cast<ProjectExplorerSidebar*>(data)->refreshGit();
                    return G_SOURCE_CONTINUE;
                },
                this);
        gitFetchTimer = g_timeout_add_seconds(
                GIT_FETCH_INTERVAL_S,
                +[](gpointer data) -> gboolean {
                    static_cast<ProjectExplorerSidebar*>(data)->fetchRemote();
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
        g_object_unref(longPress);
        g_object_unref(root);
        for (auto* icon: {folderIcon, folderOpenIcon, imageIcon, fileIcon, xournalIcon, pdfIcon, annotatedPdfIcon}) {
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

    auto lastDocument() const -> fs::path override {
        if (lastDocumentFile.empty() || projectRoot.empty()) {
            return {};
        }
        const auto path = pathFromUtf8(lastDocumentFile.c_str());
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
        lastDocumentFile = utf8(absolute);
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
        gtk_tree_store_insert_with_values(store, &iter, parentIter, -1, COL_ICON, iconFor(path, kind), COL_NAME,
                                          name.c_str(), COL_MARKUP, markup.c_str(), COL_PATH, utf8(path).c_str(),
                                          COL_KIND, kind, COL_SENSITIVE, kind != KIND_FILE, COL_FOREGROUND,
                                          gitColorFor(path, kind), -1);
        if (kind == KIND_DIRECTORY && lazy && hasVisibleEntries(path)) {
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
        const auto dir = pathFromUtf8(pathAt(iter).c_str());
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
            const auto name = utf8(path.filename());
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
                    nestedFiles.insert(utf8(candidate.path)).second) {
                    nested[utf8(path)].push_back(candidate);
                }
            }
        }
        for (const auto& path: files) {
            if (nestedFiles.count(utf8(path))) {
                continue;
            }
            const auto name = utf8(path.filename());
            auto fileIter = appendNode(iter, path, name, kindFor(path, false), escape(name), true);
            if (auto children = nested.find(utf8(path)); children != nested.end()) {
                for (const auto& child: children->second) {
                    const auto childName = utf8(child.path.filename());
                    auto childIter =
                            appendNode(&fileIter, child.path, childName, KIND_NESTED, escape(childName), false);
                    gtk_tree_store_set(store, &childIter, COL_ICON, child.icon, COL_TOOLTIP,
                                       escape(child.tooltip).c_str(), -1);
                }
            }
        }
        gtk_tree_store_remove(store, &placeholder);
        watch(dir);
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
            auto rootName = utf8(projectRoot.filename());
            if (rootName.empty()) {
                rootName = utf8(projectRoot);
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
                if (auto markup = highlightMatch(utf8(path.filename()), search->query)) {
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
        std::map<std::string, GtkTreeIter> directories{{utf8(projectRoot), rootIter}};
        for (const auto& match: matches) {
            auto* parentIter = ensureDirectory(match.path.parent_path(), directories);
            auto iter = appendNode(parentIter, match.path, utf8(match.path.filename()),
                                   kindFor(match.path, match.directory), match.markup, false);
            if (match.directory) {
                directories[utf8(match.path)] = iter;
            }
        }
        if (matches.empty()) {
            appendInfoRow(&rootIter, _("No matching files"));
        }
        gtk_tree_view_expand_all(tree);
        updatingTree = false;
    }

    auto ensureDirectory(const fs::path& dir, std::map<std::string, GtkTreeIter>& directories) -> GtkTreeIter* {
        const auto key = utf8(dir);
        if (auto found = directories.find(key); found != directories.end()) {
            return &found->second;
        }
        if (dir == dir.parent_path()) {
            return &directories.begin()->second;
        }
        auto* parentIter = ensureDirectory(dir.parent_path(), directories);
        const auto name = utf8(dir.filename());
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
            reveal(pathFromUtf8(selected.c_str()), !keepScroll);
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
            for (const auto& [key, state]: gitStatus.files) {
                if (key.size() <= rootKey.size() || key.compare(0, rootKey.size(), rootKey) != 0) {
                    continue;
                }
                // Mark all parent folders below the project root; modified wins over added
                for (auto dir = pathFromUtf8(key.c_str()).parent_path();; dir = dir.parent_path()) {
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
                                   gitColorFor(pathFromUtf8(pathAt(&child).c_str()), static_cast<Kind>(kind)), -1);
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
            const auto top = fs::weakly_canonical(pathFromUtf8(trim(result.output).c_str()), error);
            // Only the project folder (cwd and pathspec "."), which may be part of a larger repository. Porcelain
            // paths are still relative to the repository root. --no-optional-locks keeps status from taking the
            // index lock, which would make a concurrent commit fail.
            ProjectGit::run(root,
                            withPathspec({"--no-optional-locks", "status", "--porcelain=v1", "-z", "--branch",
                                          "--untracked-files=all"}),
                            lifetime, [this, root, top, done](auto status) {
                                if (root == projectRoot && status.success) {
                                    gitToplevel = top;
                                    gitStatus = ProjectGit::parseStatus(status.output, top);
                                    finishGitRefresh(true, done);
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
                ProjectGit::run(gitToplevel, {"commit", "-m", commitMessage()}, lifetime, [this](auto committed) {
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
        std::error_code error;
        const auto before = documentPath.empty() ? fs::file_time_type() : fs::last_write_time(documentPath, error);
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
    void reloadIfChanged(const fs::path& documentPath, fs::file_time_type before) {
        std::error_code error;
        if (documentPath.empty() || control->getDocument()->getFilepath() != documentPath ||
            fs::last_write_time(documentPath, error) == before || error) {
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
        const auto key = utf8(dir);
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
                                                           AnnotationFiles::isRecoveryCopy(pathFromUtf8(name)));
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
                found->second = fs::last_write_time(pathFromUtf8(path), error);
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
            if (fs::last_write_time(pathFromUtf8(key.c_str()), error) != time || error) {
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
            const auto name = utf8(part);
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
                    openDocument(pathFromUtf8(pathAt(&child).c_str()));
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
                self->openDocument(pathFromUtf8(self->pathAt(&iter).c_str()));
                break;
            case KIND_NESTED:
                // The document row above opens the PDFs for editing; the nested entries show them as they are
                self->openReadOnly(pathFromUtf8(self->pathAt(&iter).c_str()));
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
        return pathFromUtf8(pathAt(&parentIter).c_str());
    }

    static auto onButtonPress(GtkWidget*, GdkEventButton* event, ProjectExplorerSidebar* self) -> gboolean {
        if (!gdk_event_triggers_context_menu(reinterpret_cast<GdkEvent*>(event)) ||
            event->window != gtk_tree_view_get_bin_window(self->tree)) {
            return false;
        }
        self->showContextMenu(static_cast<gint>(event->x), static_cast<gint>(event->y),
                              reinterpret_cast<GdkEvent*>(event));
        return true;
    }

    static void onLongPress(GtkGesture* gesture, gdouble x, gdouble y, ProjectExplorerSidebar* self) {
        gint binX = 0;
        gint binY = 0;
        gtk_tree_view_convert_widget_to_bin_window_coords(self->tree, static_cast<gint>(x), static_cast<gint>(y), &binX,
                                                          &binY);
        // Claim the touch sequence so the release does not also activate the row
        gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
        self->showContextMenu(binX, binY, nullptr);
    }

    // --- Context menu ---

    void showContextMenu(gint x, gint y, GdkEvent* event) {
        GtkTreePath* path = nullptr;
        gint kind = KIND_PLACEHOLDER;
        std::string target;
        fs::path document;  // what "Open" opens: the entry itself, or the document a nested PDF belongs to
        if (gtk_tree_view_get_path_at_pos(tree, x, y, &path, nullptr, nullptr, nullptr)) {
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model(), &iter, path)) {
                kind = kindAt(&iter);
                target = pathAt(&iter);
                document = kind == KIND_NESTED ? owningDocument(&iter) : pathFromUtf8(target.c_str());
            }
            if (kind != KIND_PLACEHOLDER) {
                gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), path);
            }
            gtk_tree_path_free(path);
        }

        auto* menu = gtk_menu_new();
        if (kind == KIND_DOCUMENT || kind == KIND_NESTED) {
            addMenuItem(menu, _("Open"), [this, document] { openDocument(document); });
            if (Util::hasPdfFileExt(pathFromUtf8(target.c_str()))) {
                // Read-only viewing, e.g. of the exported PDF, without creating annotations
                addMenuItem(menu, _("Open Read-Only"), [this, target] { openReadOnly(pathFromUtf8(target.c_str())); });
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
        const bool isRoot = !target.empty() && pathFromUtf8(target.c_str()) == projectRoot;
        if (kind != KIND_PLACEHOLDER && !isRoot) {
            addMenuItem(menu, _("Rename…"),
                        [this, target, kind] { renameEntry(pathFromUtf8(target.c_str()), kind == KIND_DOCUMENT); });
            addMenuItem(menu, _("Delete"),
                        [this, target, kind] { deleteEntry(pathFromUtf8(target.c_str()), kind == KIND_DIRECTORY); });
            addMenuSeparator(menu);
        }
        if (kind != KIND_PLACEHOLDER) {
            addMenuItem(menu, _("Show in File Manager"), [this, target, kind] {
                const auto path = pathFromUtf8(target.c_str());
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

        gtk_widget_show_all(menu);
        gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(tree), nullptr);
        // Items are activated after "deactivate", so destroy the menu only once the main loop is idle again
        g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* shell, gpointer) {
                             g_idle_add(
                                     +[](gpointer widget) -> gboolean {
                                         gtk_widget_destroy(GTK_WIDGET(widget));
                                         return G_SOURCE_REMOVE;
                                     },
                                     shell);
                         }),
                         nullptr);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
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
        if (auto uri = Util::toUri(pathFromUtf8(file.c_str()))) {
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
            reveal(pathFromUtf8(selectedFile.c_str()), true);
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

    auto stateFile() const -> std::string { return utf8(Util::getConfigFile("project-explorer.ini")); }

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
            auto path = fs::weakly_canonical(pathFromUtf8(project.c_str()), error);
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
            g_key_file_set_string(keyFile, "General", "project", utf8(projectRoot).c_str());
            g_key_file_set_string(keyFile, group.c_str(), "root", utf8(projectRoot).c_str());
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
                                         self->openProject(pathFromUtf8(path));
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
        const auto path = pathFromUtf8(target.c_str());
        return kind == KIND_DIRECTORY ? path : path.parent_path();
    }

    static auto uniquePath(const fs::path& dir, const std::string& base, const std::string& extension) -> fs::path {
        std::error_code error;
        auto candidate = dir / pathFromUtf8((base + extension).c_str());
        for (int i = 2; fs::exists(candidate, error); i++) {
            candidate = dir / pathFromUtf8((base + " " + std::to_string(i) + extension).c_str());
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

        const auto stem = utf8(pathFromUtf8(initial.c_str()).stem());
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
        std::string name = utf8(uniquePath(dir, utf8(stem), ".xopp").filename());
        fs::path path;
        while (true) {
            auto entered = askForName(_("New Document"), _("Create"), name);
            if (!entered) {
                return;
            }
            name = *entered;
            // Every new document is a Xournal++ file, whatever else the name looks like
            if (!Util::hasXournalFileExt(pathFromUtf8(name.c_str()))) {
                name += ".xopp";
            }
            path = dir / pathFromUtf8(name.c_str());
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
        std::string name = utf8(uniquePath(dir, _("New Folder"), "").filename());
        while (true) {
            auto entered = askForName(_("New Folder"), _("Create"), name);
            if (!entered) {
                return;
            }
            name = *entered;
            const auto path = dir / pathFromUtf8(name.c_str());
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

    void renameEntry(const fs::path& oldPath, bool document) {
        std::string name = utf8(oldPath.filename());
        while (true) {
            auto entered = askForName(_("Rename"), _("Rename"), name);
            if (!entered || *entered == utf8(oldPath.filename())) {
                return;
            }
            name = *entered;
            // Keep the document type if the extension was left out
            if (document && !pathFromUtf8(name.c_str()).has_extension()) {
                name += utf8(oldPath.extension());
            }
            const auto newPath = oldPath.parent_path() / pathFromUtf8(name.c_str());
            if (auto problem = checkName(name, newPath, oldPath)) {
                showError(problem->first, problem->second);
                continue;
            }
            std::error_code error;
            fs::rename(oldPath, newPath, error);
            if (error) {
                showError(_("The entry could not be renamed."), error.message());
                return;
            }
            movedEntry(oldPath, newPath);
            showEntry(newPath);
            return;
        }
    }

    /// Updates all remembered paths (open document, selection, expanded folders) after a rename
    void movedEntry(const fs::path& oldPath, const fs::path& newPath) {
        const auto oldPrefix = utf8(oldPath);
        auto rebase = [&](const std::string& value) -> std::optional<std::string> {
            if (value == oldPrefix) {
                return utf8(newPath);
            }
            if (value.size() > oldPrefix.size() && value.compare(0, oldPrefix.size(), oldPrefix) == 0 &&
                (value[oldPrefix.size()] == '/' || value[oldPrefix.size()] == '\\')) {
                return utf8(newPath) + value.substr(oldPrefix.size());
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
        if (auto moved = rebase(utf8(document->getFilepath()))) {
            document->lock();
            document->setFilepath(pathFromUtf8(moved->c_str()));
            document->unlock();
            control->updateWindowTitle();
        }
        scheduleSave();
    }

    void deleteEntry(const fs::path& path, bool directory) {
        const auto name = utf8(path.filename());
        auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                              FC(_F("Delete “{1}”?") % name));
        gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog), "%s",
                directory ? _("The folder and everything in it will be moved to the trash.") :
                            _("The file will be moved to the trash."));
        gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
        auto* deleteButton = gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete"), GTK_RESPONSE_ACCEPT);
        gtk_style_context_add_class(gtk_widget_get_style_context(deleteButton), "destructive-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_ACCEPT) {
            return;
        }

        GFile* file = g_file_new_for_path(utf8(path).c_str());
        GError* error = nullptr;
        const bool trashed = g_file_trash(file, nullptr, &error);
        g_object_unref(file);
        if (!trashed) {
            showError(_("The entry could not be moved to the trash."), error->message);
            g_error_free(error);
            return;
        }
        // The open document no longer exists on disk: replace it with a new, empty one
        const auto documentPath = control->getDocument()->getFilepath();
        if (!documentPath.empty() && Util::isChildOrEquivalent(documentPath, path)) {
            control->newFile();
        }
        refresh(true);
    }

    static auto onKeyPress(GtkWidget*, GdkEventKey* event, ProjectExplorerSidebar* self) -> gboolean {
        if (event->keyval != GDK_KEY_F2 && event->keyval != GDK_KEY_Delete) {
            return false;
        }
        GtkTreeIter iter;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(self->tree), nullptr, &iter)) {
            return false;
        }
        const auto kind = self->kindAt(&iter);
        const auto path = pathFromUtf8(self->pathAt(&iter).c_str());
        if (kind == KIND_PLACEHOLDER || path == self->projectRoot) {
            return false;
        }
        if (event->keyval == GDK_KEY_F2) {
            self->renameEntry(path, kind == KIND_DOCUMENT);
        } else {
            self->deleteEntry(path, kind == KIND_DIRECTORY);
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
    bool fetchRunning = false;
    bool syncAfterFetch = false;
    fs::path gitToplevel;
    ProjectGit::Status gitStatus;
    std::map<std::string, ProjectGit::FileState> directoryStates;
    guint gitRefreshSource = 0;
    guint gitStatusTimer = 0;
    guint gitFetchTimer = 0;
    GtkGesture* longPress{};
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
