#include "ProjectExplorerSidebar.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

enum Column : gint { COL_ICON, COL_NAME, COL_MARKUP, COL_PATH, COL_KIND, COL_SENSITIVE, COL_COUNT };
enum Kind : gint { KIND_DIRECTORY, KIND_DOCUMENT, KIND_FILE, KIND_PLACEHOLDER };

constexpr size_t MAX_SEARCH_RESULTS = 1000;
constexpr int MAX_SEARCH_DEPTH = 16;
constexpr guint REFRESH_DELAY_MS = 250;

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

auto isIgnored(const fs::path& path) -> bool { return path.filename() == ".git"; }

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

auto iconFor(const fs::path& path, Kind kind) -> const char* {
    if (kind == KIND_DIRECTORY) {
        return "folder-symbolic";
    }
    if (kind == KIND_DOCUMENT) {
        return "x-office-document-symbolic";
    }
    auto ext = casefold(utf8(path.extension()));
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".svg" || ext == ".bmp") {
        return "image-x-generic-symbolic";
    }
    return "text-x-generic-symbolic";
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
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        g_object_ref_sink(root);

        searchEntry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), _("Search files"));
        g_object_set(searchEntry, "margin", 6, nullptr);
        g_signal_connect(searchEntry, "search-changed", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                             auto* self = static_cast<ProjectExplorerSidebar*>(data);
                             self->query = casefold(gtk_entry_get_text(GTK_ENTRY(entry)));
                             self->refresh(false);
                         }), this);
        g_signal_connect(searchEntry, "stop-search", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer) {
                             gtk_entry_set_text(GTK_ENTRY(entry), "");
                         }), nullptr);
        g_signal_connect(searchEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                             static_cast<ProjectExplorerSidebar*>(data)->openFirstDocument();
                         }), this);
        g_signal_connect(searchEntry, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
                             if (event->keyval != GDK_KEY_Down) {
                                 return FALSE;
                             }
                             gtk_widget_grab_focus(GTK_WIDGET(static_cast<ProjectExplorerSidebar*>(data)->tree));
                             return TRUE;
                         }), this);
        gtk_box_pack_start(GTK_BOX(root), GTK_WIDGET(searchEntry), false, false, 0);

        store = gtk_tree_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                   G_TYPE_BOOLEAN);
        gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), COL_NAME, compareRows, nullptr, nullptr);
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), COL_NAME, GTK_SORT_ASCENDING);

        tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
        g_object_unref(store);
        gtk_tree_view_set_headers_visible(tree, false);
        gtk_tree_view_set_enable_search(tree, false);
        gtk_tree_view_set_activate_on_single_click(tree, true);
        gtk_tree_view_set_show_expanders(tree, true);

        auto* column = gtk_tree_view_column_new();
        auto* iconRenderer = gtk_cell_renderer_pixbuf_new();
        g_object_set(iconRenderer, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 4, "ypad", 3, nullptr);
        gtk_tree_view_column_pack_start(column, iconRenderer, false);
        gtk_tree_view_column_add_attribute(column, iconRenderer, "icon-name", COL_ICON);
        gtk_tree_view_column_add_attribute(column, iconRenderer, "sensitive", COL_SENSITIVE);
        auto* textRenderer = gtk_cell_renderer_text_new();
        g_object_set(textRenderer, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 3, nullptr);
        gtk_tree_view_column_pack_start(column, textRenderer, true);
        gtk_tree_view_column_add_attribute(column, textRenderer, "markup", COL_MARKUP);
        gtk_tree_view_column_add_attribute(column, textRenderer, "sensitive", COL_SENSITIVE);
        gtk_tree_view_append_column(tree, column);

        g_signal_connect(tree, "test-expand-row", G_CALLBACK(onTestExpandRow), this);
        g_signal_connect(tree, "row-expanded", G_CALLBACK(onRowExpanded), this);
        g_signal_connect(tree, "row-collapsed", G_CALLBACK(onRowCollapsed), this);
        g_signal_connect(tree, "row-activated", G_CALLBACK(onRowActivated), this);
        g_signal_connect(tree, "button-press-event", G_CALLBACK(onButtonPress), this);

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
                         }), this);
        gtk_box_pack_start(GTK_BOX(emptyView), openButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(root), emptyView, true, true, 0);

        gtk_widget_show_all(root);

        const auto savedProject = Util::getConfigFile("last-project.txt");
        if (auto saved = Util::readString(savedProject, false); saved && !saved->empty()) {
            std::error_code error;
            if (auto savedPath = fs::weakly_canonical(pathFromUtf8(saved->c_str()), error);
                !error && fs::is_directory(savedPath, error)) {
                projectRoot = savedPath;
            } else {
                fs::remove(savedProject, error);
            }
        }
        registerListener(control);
        buildTree();
    }

    ~ProjectExplorerSidebar() override {
        if (refreshSource) {
            g_source_remove(refreshSource);
        }
        if (scrollSource) {
            g_source_remove(scrollSource);
        }
        clearMonitors();
        g_signal_handlers_disconnect_by_data(tree, this);
        g_signal_handlers_disconnect_by_data(searchEntry, this);
        g_object_unref(longPress);
        g_object_unref(root);
    }

    void enableSidebar() override { refresh(true); }
    void disableSidebar() override {}
    void layout() override {}
    std::string getName() override { return _("Project Explorer"); }
    std::string getIconName() override { return "folder-open-symbolic"; }
    bool hasData() override { return true; }
    GtkWidget* getWidget() override { return root; }
    void openProjectFolder() override { chooseFolder(); }

    void documentChanged(DocumentChangeType type) override {
        if (type != DOCUMENT_CHANGE_COMPLETE || projectRoot.empty() || searching) {
            return;
        }
        const auto documentPath = control->getDocument()->getFilepath();
        if (!documentPath.empty()) {
            reveal(documentPath, true);
        }
    }

private:
    // --- Tree model helpers ---

    auto model() const -> GtkTreeModel* { return GTK_TREE_MODEL(store); }

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
                                          COL_KIND, kind, COL_SENSITIVE, kind != KIND_FILE, -1);
        if (kind == KIND_DIRECTORY && lazy && hasVisibleEntries(path)) {
            GtkTreeIter placeholder;
            gtk_tree_store_insert_with_values(store, &placeholder, &iter, -1, COL_NAME, "", COL_KIND,
                                              KIND_PLACEHOLDER, COL_SENSITIVE, false, -1);
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
        std::error_code error;
        for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
            if (isIgnored(it->path())) {
                continue;
            }
            std::error_code itemError;
            const bool directory = it->is_directory(itemError);
            if (!directory && !it->is_regular_file(itemError)) {
                continue;
            }
            const auto name = utf8(it->path().filename());
            appendNode(iter, it->path(), name, kindFor(it->path(), directory), escape(name), true);
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
            auto rootIter =
                    appendNode(nullptr, projectRoot, rootName, KIND_DIRECTORY, "<b>" + escape(rootName) + "</b>", !searching);
            if (searching) {
                fillSearchResults(&rootIter);
                gtk_tree_view_expand_all(tree);
            } else {
                expandIter(&rootIter);
            }
        }
        updatingTree = false;
    }

    void fillSearchResults(GtkTreeIter* rootIter) {
        std::map<std::string, GtkTreeIter> directories{{utf8(projectRoot), *rootIter}};
        size_t results = 0;
        std::error_code error;
        for (fs::recursive_directory_iterator it(projectRoot, fs::directory_options::skip_permission_denied, error),
             end;
             !error && it != end; it.increment(error)) {
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
            const auto name = utf8(path.filename());
            const auto markup = highlightMatch(name, query);
            if (!markup) {
                continue;
            }
            auto* parentIter = ensureDirectory(path.parent_path(), directories);
            auto iter = appendNode(parentIter, path, name, kindFor(path, directory), *markup, false);
            if (directory) {
                directories[utf8(path)] = iter;
            }
            if (++results >= MAX_SEARCH_RESULTS) {
                break;
            }
        }
        if (results == 0) {
            appendInfoRow(rootIter, _("No matching files"));
        }
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
            scrollSource = g_idle_add(+[](gpointer data) -> gboolean {
                auto* self = static_cast<ProjectExplorerSidebar*>(data);
                self->scrollSource = 0;
                gtk_adjustment_set_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(self->tree)),
                                         self->pendingScroll);
                return G_SOURCE_REMOVE;
            }, this);
        }
    }

    void scheduleRefresh() {
        if (refreshSource) {
            return;
        }
        refreshSource = g_timeout_add(REFRESH_DELAY_MS, +[](gpointer data) -> gboolean {
            auto* self = static_cast<ProjectExplorerSidebar*>(data);
            self->refreshSource = 0;
            self->refresh(true);
            return G_SOURCE_REMOVE;
        }, this);
    }

    void updateView() {
        const bool hasProject = !projectRoot.empty();
        gtk_widget_set_visible(GTK_WIDGET(searchEntry), hasProject);
        gtk_widget_set_visible(scrolled, hasProject);
        gtk_widget_set_visible(emptyView, !hasProject);
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
                         G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, gpointer data) {
                             switch (event) {
                                 case G_FILE_MONITOR_EVENT_CREATED:
                                 case G_FILE_MONITOR_EVENT_DELETED:
                                 case G_FILE_MONITOR_EVENT_MOVED_IN:
                                 case G_FILE_MONITOR_EVENT_MOVED_OUT:
                                 case G_FILE_MONITOR_EVENT_RENAMED:
                                     static_cast<ProjectExplorerSidebar*>(data)->scheduleRefresh();
                                     break;
                                 default:
                                     break;
                             }
                         }), this);
        monitors.emplace(key, monitor);
    }

    void clearMonitors() {
        for (auto& [path, monitor]: monitors) {
            g_signal_handlers_disconnect_by_data(monitor, this);
            g_file_monitor_cancel(monitor);
            g_object_unref(monitor);
        }
        monitors.clear();
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
                    control->openFile(pathFromUtf8(pathAt(&child).c_str()));
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
        gtk_tree_store_set(self->store, iter, COL_ICON, "folder-open-symbolic", -1);
        if (self->searching) {
            return;
        }
        self->expandedPaths.insert(self->pathAt(iter));
        // Restore previously expanded subdirectories
        std::vector<GtkTreePath*> toExpand;
        GtkTreeIter child;
        for (bool valid = gtk_tree_model_iter_children(self->model(), &child, iter); valid;
             valid = gtk_tree_model_iter_next(self->model(), &child)) {
            if (self->kindAt(&child) == KIND_DIRECTORY && self->expandedPaths.count(self->pathAt(&child))) {
                toExpand.push_back(gtk_tree_model_get_path(self->model(), &child));
            }
        }
        for (auto* path: toExpand) {
            gtk_tree_view_expand_row(self->tree, path, false);
            gtk_tree_path_free(path);
        }
    }

    static void onRowCollapsed(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*, ProjectExplorerSidebar* self) {
        gtk_tree_store_set(self->store, iter, COL_ICON, "folder-symbolic", -1);
        if (!self->searching && !self->updatingTree) {
            self->expandedPaths.erase(self->pathAt(iter));
        }
    }

    static void onRowActivated(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn*,
                               ProjectExplorerSidebar* self) {
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
                self->control->openFile(pathFromUtf8(self->pathAt(&iter).c_str()));
                break;
            default:
                break;
        }
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
        gtk_tree_view_convert_widget_to_bin_window_coords(self->tree, static_cast<gint>(x), static_cast<gint>(y),
                                                          &binX, &binY);
        // Claim the touch sequence so the release does not also activate the row
        gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
        self->showContextMenu(binX, binY, nullptr);
    }

    // --- Context menu ---

    void showContextMenu(gint x, gint y, GdkEvent* event) {
        GtkTreePath* path = nullptr;
        gint kind = KIND_PLACEHOLDER;
        std::string target;
        if (gtk_tree_view_get_path_at_pos(tree, x, y, &path, nullptr, nullptr, nullptr)) {
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model(), &iter, path)) {
                kind = kindAt(&iter);
                target = pathAt(&iter);
            }
            if (kind != KIND_PLACEHOLDER) {
                gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), path);
            }
            gtk_tree_path_free(path);
        }

        auto* menu = gtk_menu_new();
        if (kind == KIND_DOCUMENT) {
            addMenuItem(menu, _("Open"), [this, target] { control->openFile(pathFromUtf8(target.c_str())); });
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
                             g_idle_add(+[](gpointer widget) -> gboolean {
                                 gtk_widget_destroy(GTK_WIDGET(widget));
                                 return G_SOURCE_REMOVE;
                             }, shell);
                         }), nullptr);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
    }

    void collapseAll() {
        expandedPaths.clear();
        gtk_tree_view_collapse_all(tree);
        GtkTreeIter rootIter;
        if (gtk_tree_model_get_iter_first(model(), &rootIter)) {
            expandIter(&rootIter);
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
            expandedPaths.clear();
        }
        projectRoot = absolute;
        if (persist) {
            const auto config = Util::getConfigFile("last-project.txt");
            std::ofstream output(config, std::ios::binary | std::ios::trunc);
            const auto saved = utf8(projectRoot);
            output.write(saved.data(), static_cast<std::streamsize>(saved.size()));
        }
        resetSearchAndBuild();
        const auto documentPath = control->getDocument()->getFilepath();
        if (!documentPath.empty()) {
            reveal(documentPath, true);
        }
    }

    void closeProject() {
        projectRoot.clear();
        expandedPaths.clear();
        std::error_code error;
        fs::remove(Util::getConfigFile("last-project.txt"), error);
        resetSearchAndBuild();
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
                         }), this);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }

    void showMessage(const char* message) {
        auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                              "%s", message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    GtkWidget* root{};
    GtkWindow* parent{};
    GtkSearchEntry* searchEntry{};
    GtkTreeStore* store{};
    GtkTreeView* tree{};
    GtkWidget* scrolled{};
    GtkWidget* emptyView{};
    GtkGesture* longPress{};
    fs::path projectRoot;
    std::string query;
    bool searching = false;
    bool updatingTree = false;
    std::set<std::string> expandedPaths;
    std::map<std::string, GFileMonitor*> monitors;
    guint refreshSource = 0;
    guint scrollSource = 0;
    double pendingScroll = 0;
};

auto createProjectExplorerSidebar(Control* control) -> std::unique_ptr<AbstractSidebarPage> {
    return std::make_unique<ProjectExplorerSidebar>(control);
}
