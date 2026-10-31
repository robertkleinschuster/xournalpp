/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "control/tasks/TaskFiles.h"
#include "control/tasks/TodoTxt.h"

namespace fs = std::filesystem;
using namespace std::chrono;
using TodoTxt::Date;

namespace {
auto date(int y, unsigned m, unsigned d) -> Date { return Date{year(y), month(m), day(d)}; }
auto readFile(const fs::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
const Date TODAY = date(2026, 10, 31);  // a Saturday
}  // namespace

TEST(TodoTxt, parsesAndFormatsLinesUnchanged) {
    for (const char* line: {"(A) 2026-10-30 Übungsblatt 3 +GET @uni due:2026-11-05 time:14:00",
                            "x 2026-11-02 2026-10-30 Skript lesen pri:B", "Einfach eine Aufgabe",
                            "2026-10-01 Mit Datum http://example.com/a:b", "x Erledigt ohne Datum"}) {
        auto task = TodoTxt::parse(line);
        ASSERT_TRUE(task) << line;
        EXPECT_EQ(TodoTxt::format(*task), line);
    }
    EXPECT_FALSE(TodoTxt::parse("   \r"));
}

TEST(TodoTxt, readsTheParts) {
    auto task = *TodoTxt::parse("(B) 2026-10-30 Blatt 3 abgeben +GET @uni due:2026-11-05 time:9:30 t:2026-11-01");
    EXPECT_FALSE(task.done);
    EXPECT_EQ(task.priority, 'B');
    EXPECT_EQ(task.created, date(2026, 10, 30));
    EXPECT_EQ(task.due(), date(2026, 11, 5));
    EXPECT_EQ(task.time(), hours(9) + minutes(30));
    EXPECT_EQ(task.threshold(), date(2026, 11, 1));
    EXPECT_EQ(task.projects(), std::vector<std::string>{"GET"});
    EXPECT_EQ(task.contexts(), std::vector<std::string>{"uni"});
    EXPECT_EQ(task.title(), "Blatt 3 abgeben +GET @uni");

    auto done = *TodoTxt::parse("x 2026-11-02 2026-10-30 Fertig");
    EXPECT_TRUE(done.done);
    EXPECT_EQ(done.completed, date(2026, 11, 2));
    EXPECT_EQ(done.created, date(2026, 10, 30));
    EXPECT_FALSE(TodoTxt::parse("Termin due:2026-02-30")->due());  // no such day
}

TEST(TodoTxt, setsAndRemovesTags) {
    auto task = *TodoTxt::parse("Lesen due:2026-11-05 +GET");
    TodoTxt::setTag(task, "due", "2026-11-06");
    EXPECT_EQ(task.description, "Lesen due:2026-11-06 +GET");
    TodoTxt::setTag(task, "due", std::nullopt);
    EXPECT_EQ(task.description, "Lesen +GET");
    TodoTxt::setTag(task, "time", "14:00");
    EXPECT_EQ(task.description, "Lesen +GET time:14:00");
    TodoTxt::setTag(task, "time", std::nullopt);
    EXPECT_EQ(task.description, "Lesen +GET");
}

TEST(TodoTxt, completesAndReopens) {
    auto task = *TodoTxt::parse("(A) 2026-10-30 Blatt abgeben due:2026-11-05");
    EXPECT_FALSE(TodoTxt::complete(task, TODAY));
    EXPECT_EQ(TodoTxt::format(task), "x 2026-10-31 2026-10-30 Blatt abgeben due:2026-11-05 pri:A");
    EXPECT_FALSE(TodoTxt::complete(task, TODAY));  // already done
    TodoTxt::reopen(task);
    EXPECT_EQ(TodoTxt::format(task), "(A) 2026-10-30 Blatt abgeben due:2026-11-05");
}

TEST(TodoTxt, recurringTasksCreateTheNextOne) {
    // From the completion date
    auto weekly = *TodoTxt::parse("2026-10-01 Vorlesung nachbereiten due:2026-10-29 rec:1w");
    auto next = TodoTxt::complete(weekly, TODAY);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->due(), date(2026, 11, 7));
    EXPECT_EQ(next->created, TODAY);
    EXPECT_FALSE(next->done);
    // Strict: from the due date, also keeping the start date's distance
    auto strict = *TodoTxt::parse("Abgabe due:2026-10-29 t:2026-10-27 rec:+1w");
    next = TodoTxt::complete(strict, TODAY);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->due(), date(2026, 11, 5));
    EXPECT_EQ(next->threshold(), date(2026, 11, 3));
    // Months keep the end of the month
    EXPECT_EQ(TodoTxt::addInterval(date(2026, 1, 31), {1, 'm', false}), date(2026, 2, 28));
    EXPECT_FALSE(TodoTxt::parseRecurrence("1x"));
    EXPECT_FALSE(TodoTxt::parseRecurrence("w"));
}

TEST(TodoTxt, groupsByDate) {
    using G = TodoTxt::Group;
    EXPECT_EQ(TodoTxt::groupOf(*TodoTxt::parse("a due:2026-10-30"), TODAY), G::Overdue);
    EXPECT_EQ(TodoTxt::groupOf(*TodoTxt::parse("a due:2026-10-31"), TODAY), G::Today);
    EXPECT_EQ(TodoTxt::groupOf(*TodoTxt::parse("a due:2026-11-01"), TODAY), G::Upcoming);
    EXPECT_EQ(TodoTxt::groupOf(*TodoTxt::parse("a"), TODAY), G::NoDate);
    EXPECT_EQ(TodoTxt::groupOf(*TodoTxt::parse("a t:2026-11-03 due:2026-11-05"), TODAY), G::Later);
    EXPECT_EQ(TodoTxt::groupOf(*TodoTxt::parse("x a due:2026-10-01"), TODAY), G::Done);
}

TEST(TodoTxt, quickAddUnderstandsDatesAndTimes) {
    auto task = TodoTxt::quickAdd("Übungsblatt 3 abgeben morgen 14:00 +GET", TODAY);
    EXPECT_EQ(task.title(), "Übungsblatt 3 abgeben +GET");
    EXPECT_EQ(task.due(), date(2026, 11, 1));
    EXPECT_EQ(task.time(), hours(14));
    EXPECT_EQ(task.created, TODAY);

    EXPECT_EQ(TodoTxt::quickAdd("Skript lesen bis Montag", TODAY).due(), date(2026, 11, 2));
    EXPECT_EQ(TodoTxt::quickAdd("Skript lesen bis Montag", TODAY).title(), "Skript lesen");
    EXPECT_EQ(TodoTxt::quickAdd("Lernen Samstag", TODAY).due(), date(2026, 11, 7));  // today is Saturday: next week
    EXPECT_EQ(TodoTxt::quickAdd("Labor übermorgen", TODAY).due(), date(2026, 11, 2));
    EXPECT_EQ(TodoTxt::quickAdd("Prüfung am 5.11.", TODAY).due(), date(2026, 11, 5));
    EXPECT_EQ(TodoTxt::quickAdd("Prüfung am 5.11.", TODAY).title(), "Prüfung");
    EXPECT_EQ(TodoTxt::quickAdd("Neujahr 1.1.", TODAY).due(), date(2027, 1, 1));
    EXPECT_EQ(TodoTxt::quickAdd("Anmelden in 3 Tagen", TODAY).due(), date(2026, 11, 3));
    EXPECT_EQ(TodoTxt::quickAdd("Projekt nächste Woche", TODAY).due(), date(2026, 11, 7));
    EXPECT_EQ(TodoTxt::quickAdd("Call tomorrow at 9:30", TODAY).time(), hours(9) + minutes(30));
    EXPECT_EQ(TodoTxt::quickAdd("Call tomorrow at 9:30", TODAY).title(), "Call");
    EXPECT_EQ(TodoTxt::quickAdd("Tutorium um 16 Uhr", TODAY).due(), TODAY);
    EXPECT_EQ(TodoTxt::quickAdd("Tutorium um 16 Uhr", TODAY).time(), hours(16));
    EXPECT_EQ(TodoTxt::quickAdd("Abgabe 2026-12-01", TODAY).due(), date(2026, 12, 1));
    EXPECT_EQ(TodoTxt::quickAdd("(A) Wichtig", TODAY).priority, 'A');
    // Numbers that are no dates stay in the text
    EXPECT_EQ(TodoTxt::quickAdd("Aufgabe 3.5 rechnen", TODAY).title(), "Aufgabe 3.5 rechnen");
    EXPECT_FALSE(TodoTxt::quickAdd("Aufgabe 3.5 rechnen", TODAY).due());
}

TEST(TaskFiles, editsLinesAndKeepsTheRest) {
    const auto dir = fs::temp_directory_path() / u8"xournalpp-tasks-test ä";
    fs::remove_all(dir);
    fs::create_directories(dir / "GET" / "VO");
    fs::create_directories(dir / ".git");
    const auto file = dir / "todo.txt";
    std::ofstream(file, std::ios::binary) << "# Kommentar\r\nErste\r\nZweite due:2026-11-05\r\n";
    std::ofstream(dir / "GET" / "Mathe.todo.txt", std::ios::binary) << "Rechnen";
    std::ofstream(dir / ".git" / "todo.txt", std::ios::binary) << "intern";
    std::ofstream(dir / "notes.txt", std::ios::binary) << "keine Aufgaben";

    const auto files = TaskFiles::find(dir);
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0].filename(), "Mathe.todo.txt");
    EXPECT_EQ(files[1], file);

    EXPECT_TRUE(TaskFiles::replaceLine(file, "Erste", {"x 2026-10-31 Erste"}));
    EXPECT_EQ(readFile(file), "# Kommentar\r\nx 2026-10-31 Erste\r\nZweite due:2026-11-05\r\n");
    EXPECT_FALSE(TaskFiles::replaceLine(file, "Gibt es nicht", {}));
    EXPECT_TRUE(TaskFiles::replaceLine(file, "Zweite due:2026-11-05", {}));
    EXPECT_TRUE(TaskFiles::appendLine(file, "Dritte"));
    EXPECT_EQ(readFile(file), "# Kommentar\r\nx 2026-10-31 Erste\r\nDritte\r\n");

    // New file, and a file without final line break
    EXPECT_TRUE(TaskFiles::appendLine(dir / "GET" / "VO" / "todo.txt", "Neu"));
    EXPECT_EQ(readFile(dir / "GET" / "VO" / "todo.txt"), "Neu\n");
    EXPECT_TRUE(TaskFiles::appendLine(dir / "GET" / "Mathe.todo.txt", "Mehr"));
    EXPECT_EQ(readFile(dir / "GET" / "Mathe.todo.txt"), "Rechnen\nMehr\n");

    EXPECT_EQ(TaskFiles::fileFor(dir / "GET" / "VO" / "VO1.xopp", dir), dir / "GET" / "VO" / "todo.txt");
    EXPECT_EQ(TaskFiles::fileFor(dir / "GET" / "VO1.xopp", dir), file);
    EXPECT_EQ(TaskFiles::fileFor({}, dir), file);
    fs::remove_all(dir);
}
