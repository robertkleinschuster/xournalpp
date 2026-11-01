#include "TasksSidebar.h"

#include <algorithm>  // for sort, find
#include <array>      // for array
#include <cstdint>    // for INT32_MAX
#include <memory>     // for unique_ptr
#include <optional>   // for optional
#include <set>        // for set
#include <string>     // for string
#include <tuple>      // for tuple
#include <utility>    // for move, exchange
#include <vector>     // for vector

#include <gtk/gtk.h>

#include "control/Control.h"                  // for Control
#include "control/tasks/TaskFiles.h"          // for TaskFiles
#include "control/tasks/TodoTxt.h"            // for TodoTxt
#include "gui/ContextMenuTrigger.h"           // for ContextMenuTrigger
#include "gui/TaskNotifier.h"                 // for TaskNotifier
#include "gui/sidebar/AbstractSidebarPage.h"  // for AbstractSidebarPage
#include "gui/sidebar/Sidebar.h"              // for Sidebar
#include "model/Document.h"                   // for Document
#include "util/PathUtil.h"                    // for toUtf8
#include "util/i18n.h"                        // for _

using namespace std::chrono;
using TodoTxt::Group;

namespace {

enum Column : gint { COL_DONE, COL_MARKUP, COL_INDEX, COL_IS_TASK, COL_COUNT };
constexpr gint HEADER = -1;

constexpr const char* COLOR_OVERDUE = "#e01b24";
constexpr guint RELOAD_DELAY_MS = 300;   ///< editors and Git write files in several steps
constexpr guint REMINDER_CHECK_S = 30;   ///< also catches reminders missed during sleep
constexpr auto SUMMARY_TIME = hours(8);  ///< the daily overview of the tasks due today is shown from then on
constexpr size_t MAX_LISTED = 5;         ///< tasks named in one notification

/// The groups in display order
constexpr std::array<Group, 6> GROUPS = {Group::Overdue, Group::Today, Group::Upcoming,
                                         Group::NoDate,  Group::Later, Group::Done};

auto groupName(Group group) -> const char* {
    switch (group) {
        case Group::Overdue:
            return _("Overdue");
        case Group::Today:
            return _("Today");
        case Group::Upcoming:
            return _("Upcoming");
        case Group::NoDate:
            return _("No date");
        case Group::Later:
            return _("Later");
        case Group::Done:
            return _("Completed");
    }
    return "";
}

auto today() -> TodoTxt::Date {
    GDateTime* now = g_date_time_new_now_local();
    TodoTxt::Date date{year(g_date_time_get_year(now)), month(static_cast<unsigned>(g_date_time_get_month(now))),
                       day(static_cast<unsigned>(g_date_time_get_day_of_month(now)))};
    g_date_time_unref(now);
    return date;
}

auto localNow() -> TodoTxt::LocalTime {
    GDateTime* now = g_date_time_new_now_local();
    TodoTxt::Date date{year(g_date_time_get_year(now)), month(static_cast<unsigned>(g_date_time_get_month(now))),
                       day(static_cast<unsigned>(g_date_time_get_day_of_month(now)))};
    const auto time = hours(g_date_time_get_hour(now)) + minutes(g_date_time_get_minute(now));
    g_date_time_unref(now);
    return local_days(date) + time;
}

/// The date of the last daily overview, kept in the config folder so it is shown once a day
auto summaryStateFile() -> std::string { return Util::toUtf8(Util::getConfigFile("tasks.ini")); }

auto lastSummaryDate() -> std::string {
    GKeyFile* file = g_key_file_new();
    std::string date;
    if (g_key_file_load_from_file(file, summaryStateFile().c_str(), G_KEY_FILE_NONE, nullptr)) {
        if (gchar* value = g_key_file_get_string(file, "Reminders", "lastSummary", nullptr)) {
            date = value;
            g_free(value);
        }
    }
    g_key_file_free(file);
    return date;
}

void storeSummaryDate(const std::string& date) {
    GKeyFile* file = g_key_file_new();
    g_key_file_load_from_file(file, summaryStateFile().c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_set_string(file, "Reminders", "lastSummary", date.c_str());
    g_key_file_save_to_file(file, summaryStateFile().c_str(), nullptr);
    g_key_file_free(file);
}

auto escape(const std::string& text) -> std::string {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result(escaped);
    g_free(escaped);
    return result;
}

/// "Today", "Tomorrow", "Fri, 7.11." or "7.11.2027", with the time if there is one
auto describeDue(const TodoTxt::Task& task, const TodoTxt::Date& now) -> std::string {
    auto due = task.due();
    if (!due) {
        return {};
    }
    static const std::array<const char*, 7> weekdays = {N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"),
                                                        N_("Fri"), N_("Sat"), N_("Sun")};
    const auto days = (sys_days(*due) - sys_days(now)).count();
    std::string text;
    if (days == 0) {
        text = _("Today");
    } else if (days == 1) {
        text = _("Tomorrow");
    } else if (days == -1) {
        text = _("Yesterday");
    } else {
        const unsigned weekday = year_month_weekday(sys_days(*due)).weekday().iso_encoding();
        text = std::string(_(weekdays[weekday - 1])) + ", " + std::to_string(static_cast<unsigned>(due->day())) + "." +
               std::to_string(static_cast<unsigned>(due->month())) + ".";
        if (due->year() != now.year()) {
            text += std::to_string(static_cast<int>(due->year()));
        }
    }
    if (auto time = task.time()) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), " %02d:%02d", static_cast<int>(duration_cast<hours>(*time).count()),
                      static_cast<int>(time->count() % 60));
        text += buffer;
    }
    return text;
}

/// The tags edited with their own fields in the edit dialog
auto isEditedTag(const std::string& word) -> bool {
    for (const char* key: {"due:", "time:", "rec:"}) {
        if (word.rfind(key, 0) == 0) {
            return true;
        }
    }
    return false;
}

auto splitWords(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> words;
    gchar** parts = g_strsplit_set(text.c_str(), " \t", -1);
    for (gchar** part = parts; *part; part++) {
        if (**part) {
            words.emplace_back(*part);
        }
    }
    g_strfreev(parts);
    return words;
}

struct Entry {
    fs::path file;
    std::string line;  ///< as in the file, to find it again when editing
    TodoTxt::Task task;
};

class TasksSidebar final: public AbstractSidebarPage, public TasksSidebarActions {
public:
    TasksSidebar(Control* control, TasksSidebarContext context):
            AbstractSidebarPage(control), context(std::move(context)) {
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        // Quick add, with the link to a page when started from "New Task for This Page"
        auto* addBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(addBox), 6);
        linkBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        linkLabel = gtk_label_new(nullptr);
        gtk_label_set_ellipsize(GTK_LABEL(linkLabel), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_xalign(GTK_LABEL(linkLabel), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(linkLabel), "dim-label");
        gtk_box_pack_start(GTK_BOX(linkBar), linkLabel, true, true, 0);
        auto* unlink = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(unlink), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(unlink, _("Remove link"));
        g_signal_connect(unlink, "clicked", G_CALLBACK(+[](GtkButton*, gpointer self) {
                             static_cast<TasksSidebar*>(self)->setPendingLink(std::nullopt);
                         }),
                         this);
        gtk_box_pack_start(GTK_BOX(linkBar), unlink, false, false, 0);
        gtk_box_pack_start(GTK_BOX(addBox), linkBar, false, false, 0);

        auto* entryRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("New task, e.g. “Sheet 3 tomorrow 14:00 +GET”"));
        g_signal_connect(
                entry, "activate",
                G_CALLBACK(+[](GtkEntry*, gpointer self) { static_cast<TasksSidebar*>(self)->addFromEntry(); }), this);
        gtk_box_pack_start(GTK_BOX(entryRow), entry, true, true, 0);
        auto* addButton = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(addButton, _("Add task"));
        g_signal_connect(
                addButton, "clicked",
                G_CALLBACK(+[](GtkButton*, gpointer self) { static_cast<TasksSidebar*>(self)->addFromEntry(); }), this);
        gtk_box_pack_start(GTK_BOX(entryRow), addButton, false, false, 0);
        gtk_box_pack_start(GTK_BOX(addBox), entryRow, false, false, 0);
        gtk_box_pack_start(GTK_BOX(root), addBox, false, false, 0);

        // The tasks, grouped by date
        store = gtk_tree_store_new(COL_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);
        tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
        gtk_tree_view_set_headers_visible(tree, false);
        gtk_tree_view_set_activate_on_single_click(tree, true);
        gtk_tree_view_set_show_expanders(tree, false);
        gtk_tree_view_set_level_indentation(tree, 0);

        toggleColumn = gtk_tree_view_column_new();
        auto* toggle = gtk_cell_renderer_toggle_new();
        g_object_set(toggle, "xpad", 8, "ypad", 8, "indicator-size", 20, nullptr);
        gtk_tree_view_column_pack_start(toggleColumn, toggle, false);
        gtk_tree_view_column_add_attribute(toggleColumn, toggle, "active", COL_DONE);
        gtk_tree_view_column_add_attribute(toggleColumn, toggle, "visible", COL_IS_TASK);
        g_signal_connect(toggle, "toggled", G_CALLBACK(+[](GtkCellRendererToggle*, gchar* path, gpointer self) {
                             static_cast<TasksSidebar*>(self)->toggled(path);
                         }),
                         this);
        gtk_tree_view_append_column(tree, toggleColumn);

        auto* textColumn = gtk_tree_view_column_new();
        auto* text = gtk_cell_renderer_text_new();
        g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 6, nullptr);
        gtk_tree_view_column_pack_start(textColumn, text, true);
        gtk_tree_view_column_add_attribute(textColumn, text, "markup", COL_MARKUP);
        gtk_tree_view_column_set_expand(textColumn, true);
        gtk_tree_view_append_column(tree, textColumn);

        g_signal_connect(tree, "row-activated",
                         G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn* column, gpointer self) {
                             static_cast<TasksSidebar*>(self)->activated(path, column);
                         }),
                         this);
        contextMenu = std::make_unique<ContextMenuTrigger>(
                GTK_WIDGET(tree), [this](const ContextMenuTrigger::Request& request) { showMenu(request); });

        scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(tree));
        gtk_box_pack_start(GTK_BOX(root), scrolled, true, true, 0);

        emptyLabel = gtk_label_new(nullptr);
        gtk_label_set_line_wrap(GTK_LABEL(emptyLabel), true);
        gtk_label_set_justify(GTK_LABEL(emptyLabel), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(emptyLabel), "dim-label");
        gtk_widget_set_margin_start(emptyLabel, 16);
        gtk_widget_set_margin_end(emptyLabel, 16);
        gtk_box_pack_start(GTK_BOX(root), emptyLabel, true, true, 0);

        gtk_widget_show_all(root);
        gtk_widget_hide(linkBar);
        g_object_ref_sink(root);
        reload();

        // Reminders, also while the tab is hidden
        notifier = std::make_unique<TaskNotifier>([this] {
            gtk_window_present(this->control->getGtkWindow());
            this->control->getSidebar()->selectTasks();
        });
        lastCheck = localNow();
        reminderSource = g_timeout_add_seconds(
                REMINDER_CHECK_S,
                +[](gpointer self) -> gboolean {
                    static_cast<TasksSidebar*>(self)->checkReminders();
                    return G_SOURCE_CONTINUE;
                },
                this);
        // The daily overview right after the start, once the window is there
        startupSource = g_timeout_add_seconds(
                5,
                +[](gpointer self) -> gboolean {
                    auto* sidebar = static_cast<TasksSidebar*>(self);
                    sidebar->startupSource = 0;
                    sidebar->checkReminders();
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    ~TasksSidebar() override {
        stopMonitors();
        for (guint source: {reloadSource, reminderSource, startupSource}) {
            if (source) {
                g_source_remove(source);
            }
        }
        contextMenu.reset();
        g_object_unref(store);
        g_object_unref(root);
    }

    // --- AbstractSidebarPage ---

    void enableSidebar() override {
        enabled = true;
        reload();  // also finds task files created meanwhile
    }
    void disableSidebar() override { enabled = false; }
    void layout() override {}
    std::string getName() override { return _("Tasks"); }
    std::string getIconName() override { return "task-due-symbolic"; }
    bool hasData() override { return true; }
    GtkWidget* getWidget() override { return root; }

    // --- TasksSidebarActions ---

    void addTaskForPage(const fs::path& document, size_t page) override {
        setPendingLink(PendingLink{document, static_cast<int>(page)});
        gtk_widget_grab_focus(entry);
    }

private:
    struct PendingLink {
        fs::path document;
        int page = -1;
    };

    // --- Loading ---

    void reload() {
        entries.clear();
        stopMonitors();
        const auto project = context.projectRoot();
        loadedProject = project;
        loadedDay = today();
        if (!project.empty()) {
            for (const auto& file: TaskFiles::find(project)) {
                watch(file);
                auto lines = TaskFiles::readLines(file);
                if (!lines) {
                    continue;
                }
                for (const auto& line: *lines) {
                    if (line.rfind('#', 0) == 0) {
                        continue;  // comment
                    }
                    if (auto task = TodoTxt::parse(line)) {
                        entries.push_back({file, line, std::move(*task)});
                    }
                }
            }
            // New task files in the project folder itself
            watch(project);
        }
        rebuild();
    }

    void watch(const fs::path& path) {
        GFile* file = g_file_new_for_path(Util::toUtf8(path).c_str());
        GFileMonitor* monitor = g_file_monitor(file, G_FILE_MONITOR_NONE, nullptr, nullptr);
        g_object_unref(file);
        if (!monitor) {
            return;
        }
        g_signal_connect(monitor, "changed",
                         G_CALLBACK(+[](GFileMonitor*, GFile* changed, GFile*, GFileMonitorEvent, gpointer self) {
                             char* path = g_file_get_path(changed);
                             const bool relevant = path && TaskFiles::isTaskFile(Util::fromUtf8(path));
                             g_free(path);
                             if (relevant) {
                                 static_cast<TasksSidebar*>(self)->scheduleReload();
                             }
                         }),
                         this);
        monitors.push_back(monitor);
    }

    void stopMonitors() {
        for (auto* monitor: monitors) {
            g_signal_handlers_disconnect_by_data(monitor, this);
            g_file_monitor_cancel(monitor);
            g_object_unref(monitor);
        }
        monitors.clear();
    }

    void scheduleReload() {
        if (reloadSource) {
            g_source_remove(reloadSource);
        }
        reloadSource = g_timeout_add(
                RELOAD_DELAY_MS,
                +[](gpointer self) -> gboolean {
                    auto* sidebar = static_cast<TasksSidebar*>(self);
                    sidebar->reloadSource = 0;
                    sidebar->reload();
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    // --- Reminders ---

    /// Shows the reminders due since the last check, and once a day an overview of the tasks due today
    void checkReminders() {
        if (context.projectRoot() != loadedProject || today() != loadedDay) {
            // Another project was opened while the tab was hidden, or a new day moves tasks to "Today"
            reload();
        }
        const auto now = localNow();
        std::vector<const Entry*> due;
        for (const auto& entry: entries) {
            for (const auto& at: TodoTxt::reminders(entry.task)) {
                if (at > lastCheck && at <= now) {
                    due.push_back(&entry);
                    break;
                }
            }
        }
        lastCheck = now;
        if (!due.empty()) {
            notify(due.size() == 1 ? due.front()->task.title() : FS(_F("{1} task reminders") % due.size()), due);
        }
        showDailySummary(now);
    }

    void showDailySummary(const TodoTxt::LocalTime& now) {
        const auto date = TodoTxt::formatDate(TodoTxt::Date(floor<days>(now)));
        if (now - floor<days>(now) < SUMMARY_TIME || loadedProject.empty() || lastSummaryDate() == date) {
            return;
        }
        storeSummaryDate(date);
        const auto day = today();
        std::vector<const Entry*> todays;
        size_t overdue = 0;
        for (const auto& entry: entries) {
            const auto group = TodoTxt::groupOf(entry.task, day);
            if (group == Group::Overdue || group == Group::Today) {
                todays.push_back(&entry);
                overdue += group == Group::Overdue ? 1 : 0;
            }
        }
        if (todays.empty()) {
            return;
        }
        const size_t dueToday = todays.size() - overdue;
        const auto count = [](const char* singular, const char* plural, size_t n) {
            return PlaceholderString(ngettext(singular, plural, static_cast<unsigned long>(n)));
        };
        const std::string title =
                overdue == 0 ?
                        FS(count("{1} task due today", "{1} tasks due today", dueToday) % dueToday) :
                dueToday == 0 ?
                        FS(count("{1} task overdue", "{1} tasks overdue", overdue) % overdue) :
                        FS(count("{1} task due today, {2} overdue", "{1} tasks due today, {2} overdue", dueToday) %
                           dueToday % overdue);
        notify(title, todays);
    }

    /// One notification naming the tasks with their due dates
    void notify(const std::string& title, const std::vector<const Entry*>& tasks) {
        const auto day = today();
        std::string text;
        for (size_t i = 0; i < tasks.size() && i < MAX_LISTED; i++) {
            const auto& task = tasks[i]->task;
            std::string line = tasks.size() == 1 ? describeDue(task, day) : task.title();
            if (tasks.size() > 1) {
                if (auto due = describeDue(task, day); !due.empty()) {
                    line += " (" + due + ")";
                }
            }
            text += (text.empty() ? "" : "\n") + line;
        }
        if (tasks.size() > MAX_LISTED) {
            text += "\n" + FS(_F("and {1} more") % (tasks.size() - MAX_LISTED));
        }
        notifier->show(title, text);
    }

    // --- Display ---

    void rebuild() {
        // Keep which groups the user collapsed (the completed ones start collapsed)
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter)) {
            collapsed.clear();
            do {
                gint group = 0;
                gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, COL_INDEX, &group, -1);
                GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &iter);
                if (!gtk_tree_view_row_expanded(tree, path)) {
                    collapsed.insert(static_cast<int>(-group - 2));
                }
                gtk_tree_path_free(path);
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter));
        }
        gtk_tree_store_clear(store);

        const auto now = today();
        std::vector<size_t> order(entries.size());
        for (size_t i = 0; i < order.size(); i++) {
            order[i] = i;
        }
        const auto sortKey = [this](size_t i) {
            const auto& task = entries[i].task;
            const auto due = task.due();
            return std::tuple(due ? static_cast<long long>(sys_days(*due).time_since_epoch().count()) : INT64_MAX,
                              task.time() ? task.time()->count() : 24 * 60, task.priority ? task.priority : 'Z' + 1,
                              task.title());
        };
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sortKey(a) < sortKey(b); });

        for (Group group: GROUPS) {
            std::vector<size_t> members;
            for (size_t i: order) {
                if (TodoTxt::groupOf(entries[i].task, now) == group) {
                    members.push_back(i);
                }
            }
            if (members.empty()) {
                continue;
            }
            if (group == Group::Done) {
                // The most recently completed first
                std::stable_sort(members.begin(), members.end(), [this](size_t a, size_t b) {
                    return entries[a].task.completed.value_or(TodoTxt::Date{}) >
                           entries[b].task.completed.value_or(TodoTxt::Date{});
                });
            }
            GtkTreeIter header;
            const auto title = std::string("<b>") + escape(groupName(group)) + "</b>  <span alpha=\"60%\">" +
                               std::to_string(members.size()) + "</span>";
            // Headers store their group as a negative index: -2 - group
            gtk_tree_store_insert_with_values(store, &header, nullptr, -1, COL_DONE, FALSE, COL_MARKUP, title.c_str(),
                                              COL_INDEX, -2 - static_cast<gint>(group), COL_IS_TASK, FALSE, -1);
            for (size_t i: members) {
                GtkTreeIter row;
                gtk_tree_store_insert_with_values(store, &row, &header, -1, COL_DONE, entries[i].task.done, COL_MARKUP,
                                                  markup(entries[i], now).c_str(), COL_INDEX, static_cast<gint>(i),
                                                  COL_IS_TASK, TRUE, -1);
            }
            GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &header);
            const bool startCollapsed = collapsed.count(static_cast<int>(group)) ||
                                        (group == Group::Done && !seenGroups.count(static_cast<int>(group)));
            if (!startCollapsed) {
                gtk_tree_view_expand_row(tree, path, false);
            }
            gtk_tree_path_free(path);
            seenGroups.insert(static_cast<int>(group));
        }

        // Nothing stays selected: a tap checks a task off or opens it (GTK_SELECTION_NONE would also stop the
        // activation by a single tap)
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(tree));

        const bool noProject = context.projectRoot().empty();
        gtk_label_set_text(GTK_LABEL(emptyLabel),
                           noProject ? _("Open a project folder in the project explorer to manage its tasks.") :
                                       _("No tasks yet. They are stored in todo.txt files in the project folder."));
        gtk_widget_set_visible(emptyLabel, entries.empty());
        gtk_widget_set_visible(scrolled, !entries.empty());
        gtk_widget_set_sensitive(entry, !noProject);
    }

    auto markup(const Entry& entry, const TodoTxt::Date& now) const -> std::string {
        const auto& task = entry.task;
        std::string title = escape(task.title());
        if (task.priority) {
            title = std::string("<b>(") + task.priority + ")</b> " + title;
        }
        if (task.done) {
            title = "<s>" + title + "</s>";
        }
        std::vector<std::string> details;
        if (auto due = describeDue(task, now); !due.empty()) {
            const bool overdue = TodoTxt::groupOf(task, now) == Group::Overdue;
            details.push_back(overdue ? std::string("<span foreground=\"") + COLOR_OVERDUE + "\">" + escape(due) +
                                                "</span>" :
                                        escape(due));
        }
        if (task.tag("rec")) {
            details.emplace_back("↻");
        }
        if (auto doc = task.tag("doc")) {
            if (auto link = TaskFiles::resolveDocumentLink(entry.file, *doc)) {
                auto name = Util::toUtf8(link->document.stem());
                if (link->page >= 0) {
                    name += FS(_F(", p. {1}") % (link->page + 1));
                }
                details.push_back("📄 " + escape(name));
            }
        }
        // The file, if the project has several (e.g. one per course)
        const auto project = context.projectRoot();
        if (entry.file.parent_path() != project) {
            details.push_back(escape(Util::toUtf8(entry.file.parent_path().lexically_relative(project))));
        }
        std::string result = title;
        if (!details.empty()) {
            std::string joined;
            for (const auto& detail: details) {
                joined += (joined.empty() ? "" : " · ") + detail;
            }
            result += "\n<small><span alpha=\"70%\">" + joined + "</span></small>";
        }
        return result;
    }

    // --- Editing ---

    /// Replaces the task's line in its file; reloads in any case, as the file may have changed meanwhile
    void replace(const Entry& entry, const std::vector<std::string>& newLines) {
        if (!TaskFiles::replaceLine(entry.file, entry.line, newLines)) {
            showError(_("The task could not be changed. The file may have been edited meanwhile."));
        }
        reload();
    }

    auto entryAt(GtkTreePath* path) -> const Entry* {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path)) {
            return nullptr;
        }
        gint index = HEADER;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, COL_INDEX, &index, -1);
        return index >= 0 && static_cast<size_t>(index) < entries.size() ? &entries[static_cast<size_t>(index)] :
                                                                           nullptr;
    }

    void toggled(const gchar* pathString) {
        GtkTreePath* path = gtk_tree_path_new_from_string(pathString);
        const Entry* found = entryAt(path);
        gtk_tree_path_free(path);
        if (!found) {
            return;
        }
        Entry entry = *found;
        std::vector<std::string> lines;
        if (entry.task.done) {
            TodoTxt::reopen(entry.task);
            lines.push_back(TodoTxt::format(entry.task));
        } else {
            auto next = TodoTxt::complete(entry.task, today());
            lines.push_back(TodoTxt::format(entry.task));
            if (next) {
                lines.push_back(TodoTxt::format(*next));
            }
        }
        replace(entry, lines);
    }

    void activated(GtkTreePath* path, GtkTreeViewColumn* column) {
        if (column == toggleColumn) {
            return;  // the check box handles it
        }
        const Entry* entry = entryAt(path);
        if (!entry) {
            // A group header: show or hide its tasks
            if (gtk_tree_view_row_expanded(tree, path)) {
                gtk_tree_view_collapse_row(tree, path);
            } else {
                gtk_tree_view_expand_row(tree, path, false);
            }
            return;
        }
        // A copy: the dialog runs a main loop, in which a changed file reloads the entries
        const Entry copy = *entry;
        if (!openLinkedDocument(copy)) {
            edit(copy);
        }
    }

    auto openLinkedDocument(const Entry& entry) -> bool {
        auto doc = entry.task.tag("doc");
        if (!doc) {
            return false;
        }
        auto link = TaskFiles::resolveDocumentLink(entry.file, *doc);
        std::error_code error;
        if (!link || !fs::exists(link->document, error)) {
            showError(_("The linked document no longer exists."));
            return true;
        }
        context.openDocument(link->document, link->page);
        return true;
    }

    void addFromEntry() {
        const std::string text = gtk_entry_get_text(GTK_ENTRY(entry));
        const auto project = context.projectRoot();
        if (text.find_first_not_of(" \t") == std::string::npos || project.empty()) {
            return;
        }
        auto task = TodoTxt::quickAdd(text, today());
        const auto document = pendingLink ? pendingLink->document : control->getDocument()->getFilepath();
        const auto file = TaskFiles::fileFor(document, project);
        if (pendingLink) {
            TodoTxt::setTag(task, "doc", TaskFiles::documentLink(file, pendingLink->document, pendingLink->page));
        }
        if (!TaskFiles::appendLine(file, TodoTxt::format(task))) {
            showError(_("The task could not be saved."));
            return;
        }
        gtk_entry_set_text(GTK_ENTRY(entry), "");
        setPendingLink(std::nullopt);
        reload();
    }

    void setPendingLink(std::optional<PendingLink> link) {
        pendingLink = std::move(link);
        if (pendingLink) {
            auto name = Util::toUtf8(pendingLink->document.stem());
            if (pendingLink->page >= 0) {
                name += FS(_F(", p. {1}") % (pendingLink->page + 1));
            }
            gtk_label_set_text(GTK_LABEL(linkLabel), FC(_F("Linked to {1}") % name));
        }
        gtk_widget_set_visible(linkBar, pendingLink.has_value());
    }

    // --- Context menu ---

    void showMenu(const ContextMenuTrigger::Request& request) {
        auto at = request;
        GtkTreePath* path = ContextMenuTrigger::rowOf(tree, at);
        if (!path) {
            return;
        }
        const Entry* found = entryAt(path);
        gtk_tree_path_free(path);
        if (!found) {
            return;
        }
        const Entry entry = *found;
        auto* menu = gtk_menu_new();
        addMenuItem(menu, _("Edit…"), [this, entry] { edit(entry); });
        if (!entry.task.done) {
            addMenuItem(menu, _("Postpone to Tomorrow"), [this, entry] {
                Entry changed = entry;
                TodoTxt::setTag(changed.task, "due", TodoTxt::formatDate(sys_days(today()) + days(1)));
                replace(entry, {TodoTxt::format(changed.task)});
            });
        }
        if (entry.task.tag("doc")) {
            addMenuItem(menu, _("Open Document"), [this, entry] { openLinkedDocument(entry); });
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        addMenuItem(menu, _("Delete"), [this, entry] {
            if (confirm(FS(_F("Delete the task “{1}”?") % entry.task.title()))) {
                replace(entry, {});
            }
        });
        ContextMenuTrigger::popup(GTK_MENU(menu), GTK_WIDGET(tree), at, true);
    }

    template <class Fn>
    void addMenuItem(GtkWidget* menu, const char* label, Fn action) {
        auto* item = gtk_menu_item_new_with_label(label);
        auto* callback = new std::function<void()>(std::move(action));
        g_signal_connect_data(
                item, "activate",
                G_CALLBACK(+[](GtkMenuItem*, gpointer data) { (*static_cast<std::function<void()>*>(data))(); }),
                callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); },
                GConnectFlags(0));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    // --- Edit dialog ---

    /// Takes a copy, as the entries may be reloaded while the dialog is open
    void edit(const Entry entry) {
        const auto& task = entry.task;
        auto* dialog =
                gtk_dialog_new_with_buttons(_("Edit Task"), control->getGtkWindow(), GTK_DIALOG_MODAL, _("Cancel"),
                                            GTK_RESPONSE_CANCEL, _("Save"), GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
        auto* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
        int row = 0;
        auto addRow = [&](const char* label, GtkWidget* widget) {
            auto* caption = gtk_label_new(label);
            gtk_label_set_xalign(GTK_LABEL(caption), 0);
            gtk_widget_set_valign(caption, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), caption, 0, row, 1, 1);
            gtk_widget_set_hexpand(widget, true);
            gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);
        };

        // The text without the tags that have their own fields
        std::string text;
        for (const auto& word: splitWords(task.description)) {
            if (!isEditedTag(word)) {
                text += (text.empty() ? "" : " ") + word;
            }
        }
        auto* textEntry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(textEntry), text.c_str());
        gtk_entry_set_activates_default(GTK_ENTRY(textEntry), true);
        addRow(_("Task"), textEntry);

        auto* hasDue = gtk_check_button_new_with_label(_("Due date"));
        auto* calendar = gtk_calendar_new();
        const auto due = task.due().value_or(today());
        gtk_calendar_select_month(GTK_CALENDAR(calendar), static_cast<guint>(due.month()) - 1,
                                  static_cast<guint>(static_cast<int>(due.year())));
        gtk_calendar_select_day(GTK_CALENDAR(calendar), static_cast<guint>(due.day()));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hasDue), task.due().has_value());
        auto* dueBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_pack_start(GTK_BOX(dueBox), hasDue, false, false, 0);
        gtk_box_pack_start(GTK_BOX(dueBox), calendar, false, false, 0);
        gtk_widget_set_sensitive(calendar, task.due().has_value());
        g_signal_connect(hasDue, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer calendar) {
                             gtk_widget_set_sensitive(GTK_WIDGET(calendar), gtk_toggle_button_get_active(button));
                         }),
                         calendar);
        addRow(_("Due"), dueBox);

        auto* timeEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(timeEntry), _("e.g. 14:00"));
        if (auto time = task.tag("time")) {
            gtk_entry_set_text(GTK_ENTRY(timeEntry), time->c_str());
        }
        gtk_entry_set_activates_default(GTK_ENTRY(timeEntry), true);
        addRow(_("Time"), timeEntry);

        // Repeat: the common intervals, and whatever another app wrote
        struct Choice {
            const char* label;
            std::optional<std::string> rec;
        };
        // Strict (+) from the due date, so e.g. a weekly lecture stays on its weekday when done early or late
        std::vector<Choice> choices = {
                {_("Does not repeat"), std::nullopt}, {_("Daily"), "1d"},    {_("Weekly"), "+1w"},
                {_("Every 2 weeks"), "+2w"},          {_("Monthly"), "+1m"}, {_("Yearly"), "+1y"}};
        const auto currentRec = task.tag("rec");
        auto* repeat = gtk_combo_box_text_new();
        int active = 0;
        for (size_t i = 0; i < choices.size(); i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(repeat), choices[i].label);
            if (choices[i].rec == currentRec) {
                active = static_cast<int>(i);
            }
        }
        if (currentRec && active == 0) {
            choices.push_back({nullptr, currentRec});
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(repeat), ("rec:" + *currentRec).c_str());
            active = static_cast<int>(choices.size() - 1);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(repeat), active);
        addRow(_("Repeat"), repeat);

        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, true, true, 0);
        gtk_widget_show_all(dialog);

        while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            const std::string newText = gtk_entry_get_text(GTK_ENTRY(textEntry));
            const std::string timeText = gtk_entry_get_text(GTK_ENTRY(timeEntry));
            std::optional<std::string> time;
            if (timeText.find_first_not_of(" ") != std::string::npos) {
                auto parsed = TodoTxt::quickAdd(timeText, today()).tag("time");
                if (!parsed) {
                    showError(_("Please enter the time as HH:MM, e.g. 14:00."));
                    continue;
                }
                time = parsed;
            }
            if (newText.find_first_not_of(" ") == std::string::npos) {
                showError(_("Please enter a task."));
                continue;
            }
            Entry changed = entry;
            changed.task.description = newText;
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hasDue))) {
                guint y = 0;
                guint m = 0;
                guint d = 0;
                gtk_calendar_get_date(GTK_CALENDAR(calendar), &y, &m, &d);
                TodoTxt::setTag(changed.task, "due",
                                TodoTxt::formatDate(TodoTxt::Date{year(static_cast<int>(y)), month(m + 1), day(d)}));
            } else if (time) {
                TodoTxt::setTag(changed.task, "due", TodoTxt::formatDate(today()));  // a time needs a day
            }
            TodoTxt::setTag(changed.task, "time", time);
            TodoTxt::setTag(changed.task, "rec",
                            choices[static_cast<size_t>(gtk_combo_box_get_active(GTK_COMBO_BOX(repeat)))].rec);
            gtk_widget_destroy(dialog);
            if (TodoTxt::format(changed.task) != entry.line) {
                replace(entry, {TodoTxt::format(changed.task)});
            }
            return;
        }
        gtk_widget_destroy(dialog);
    }

    // --- Messages ---

    void showError(const char* message) {
        auto* dialog = gtk_message_dialog_new(control->getGtkWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_CLOSE, "%s", message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    auto confirm(const std::string& question) -> bool {
        auto* dialog = gtk_message_dialog_new(control->getGtkWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                              GTK_BUTTONS_NONE, "%s", question.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL, _("Delete"), GTK_RESPONSE_ACCEPT,
                               nullptr);
        const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
        gtk_widget_destroy(dialog);
        return accepted;
    }

    TasksSidebarContext context;
    std::vector<Entry> entries;
    fs::path loadedProject;
    TodoTxt::Date loadedDay;
    std::unique_ptr<TaskNotifier> notifier;
    TodoTxt::LocalTime lastCheck;  ///< reminders up to then were shown (or were before the start)
    guint reminderSource = 0;
    guint startupSource = 0;
    std::vector<GFileMonitor*> monitors;
    guint reloadSource = 0;
    bool enabled = false;
    std::set<int> collapsed;   ///< groups the user collapsed
    std::set<int> seenGroups;  ///< groups shown before, so the completed ones only start collapsed
    std::optional<PendingLink> pendingLink;

    GtkWidget* root{};
    GtkWidget* entry{};
    GtkWidget* linkBar{};
    GtkWidget* linkLabel{};
    GtkWidget* scrolled{};
    GtkWidget* emptyLabel{};
    GtkTreeView* tree{};
    GtkTreeViewColumn* toggleColumn{};
    GtkTreeStore* store{};
    std::unique_ptr<ContextMenuTrigger> contextMenu;
};

}  // namespace

auto createTasksSidebar(Control* control, TasksSidebarContext context) -> std::unique_ptr<AbstractSidebarPage> {
    return std::make_unique<TasksSidebar>(control, std::move(context));
}
