/*
 * Xournal++
 *
 * Fork: tasks in todo.txt format (https://github.com/todotxt/todo.txt), compatible with apps like Simpletask
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <chrono>       // for year_month_day, minutes
#include <optional>     // for optional
#include <string>       // for string
#include <string_view>  // for string_view
#include <vector>       // for vector

namespace TodoTxt {

using Date = std::chrono::year_month_day;

/**
 * One line of a todo.txt file: "x 2026-11-02 2026-10-30 (A) Übungsblatt 3 +GET @uni due:2026-11-05 time:14:00".
 * Everything after the dates and priority is kept verbatim in description, including unknown tags, so a task can be
 * written back without changing what other apps put there.
 */
struct Task {
    bool done = false;
    char priority = 0;  ///< 'A'..'Z', 0 if none
    std::optional<Date> completed;
    std::optional<Date> created;
    std::string description;

    /// The value of a key:value tag, if present
    std::optional<std::string> tag(std::string_view key) const;
    /// due:YYYY-MM-DD
    std::optional<Date> due() const;
    /// time:HH:MM (the due time; kept apart from due: so other apps still understand the date)
    std::optional<std::chrono::minutes> time() const;
    /// t:YYYY-MM-DD, the task is hidden until then
    std::optional<Date> threshold() const;
    /// The +project and @context words, without the sign
    std::vector<std::string> projects() const;
    std::vector<std::string> contexts() const;
    /// The description without key:value tags, for display
    std::string title() const;
};

/// Parses a line; nullopt for blank lines
std::optional<Task> parse(std::string_view line);
std::string format(const Task& task);

std::optional<Date> parseDate(std::string_view text);
std::string formatDate(const Date& date);

/// Sets (or removes, if value is nullopt) a key:value tag, keeping the position of an existing one
void setTag(Task& task, std::string_view key, const std::optional<std::string>& value);

/// rec:1w, rec:+2d, ...: count and unit (d, w, m, y); strict (+) repeats from the due date instead of the completion
struct Recurrence {
    int count = 1;
    char unit = 'd';
    bool strict = false;
};
std::optional<Recurrence> parseRecurrence(std::string_view text);
Date addInterval(const Date& date, const Recurrence& interval);

/**
 * Marks the task done today. The priority moves into a pri: tag like in other todo.txt apps. For a recurring task,
 * returns the next occurrence, which the caller adds to the file.
 */
std::optional<Task> complete(Task& task, const Date& today);
/// Marks a completed task open again (restores the priority from pri:)
void reopen(Task& task);

/// When a task is shown, relative to today
enum class Group { Overdue, Today, Upcoming, NoDate, Later, Done };
Group groupOf(const Task& task, const Date& today);

/**
 * A task from what the user typed in the quick add field, understanding dates and times in German and English:
 * "heute", "morgen", "übermorgen", weekdays, "nächste Woche", "in 3 Tagen", "5.11.", "2026-11-05", "14:00",
 * "14 Uhr", and the English equivalents. The words used for the date are removed; +project and @context stay.
 */
Task quickAdd(std::string_view text, const Date& today);

}  // namespace TodoTxt
