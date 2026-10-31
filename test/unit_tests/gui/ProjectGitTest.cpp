/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <memory>
#include <string>

#include <glib.h>
#include <gtest/gtest.h>

#include "gui/sidebar/ProjectGit.h"

namespace fs = std::filesystem;
using ProjectGit::FileState;

namespace {
auto statusOutput(std::initializer_list<std::string> entries) -> std::string {
    std::string output;
    for (const auto& entry: entries) {
        output += entry;
        output.push_back('\0');
    }
    return output;
}
}  // namespace

TEST(ProjectGit, parsesBranchCounts) {
    const auto status = ProjectGit::parseStatus(statusOutput({"## main...origin/main [ahead 2, behind 13]"}), "/repo");
    EXPECT_TRUE(status.hasUpstream);
    EXPECT_EQ(status.ahead, 2U);
    EXPECT_EQ(status.behind, 13U);
    EXPECT_EQ(status.changedFiles, 0U);
}

TEST(ProjectGit, parsesBranchWithoutUpstream) {
    const auto status = ProjectGit::parseStatus(statusOutput({"## main"}), "/repo");
    EXPECT_FALSE(status.hasUpstream);
    EXPECT_EQ(status.ahead, 0U);
    EXPECT_EQ(status.behind, 0U);
}

TEST(ProjectGit, parsesFileStates) {
    const fs::path top = "/repo";
    const auto status = ProjectGit::parseStatus(
            statusOutput({"## main...origin/main", " M notes/Lecture 1.xopp", "?? new.pdf", "A  added.xopp",
                          "R  renamed.xopp", "old.xopp", " D deleted.xopp", "MM both.xopp"}),
            top);
    EXPECT_EQ(status.changedFiles, 6U);
    EXPECT_EQ(status.files.size(), 5U);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(top / "notes" / "Lecture 1.xopp")), FileState::Modified);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(top / "new.pdf")), FileState::Added);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(top / "added.xopp")), FileState::Added);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(top / "renamed.xopp")), FileState::Modified);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(top / "both.xopp")), FileState::Modified);
    EXPECT_FALSE(status.files.count(ProjectGit::pathKey(top / "old.xopp")));
    EXPECT_FALSE(status.files.count(ProjectGit::pathKey(top / "deleted.xopp")));
}

TEST(ProjectGit, pathKeyNormalizes) {
    EXPECT_EQ(ProjectGit::pathKey(fs::path("/repo/a/../b/")), ProjectGit::pathKey(fs::path("/repo/b")));
}

TEST(ProjectGit, runsGitAndDropsResultsOfExpiredOwners) {
    if (!ProjectGit::isAvailable()) {
        GTEST_SKIP() << "git is not installed";
    }
    auto owner = std::make_shared<int>(0);
    bool done = false;
    ProjectGit::CommandResult result;
    ProjectGit::run(fs::current_path(), {"--version"}, owner, [&](ProjectGit::CommandResult r) {
        result = std::move(r);
        done = true;
    });
    bool expiredCalled = false;
    {
        auto expired = std::make_shared<int>(0);
        ProjectGit::run(fs::current_path(), {"--version"}, expired, [&](auto) { expiredCalled = true; });
    }
    const gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!done && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, true);
    }
    // Give the second command time to finish as well
    const gint64 settle = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < settle) {
        g_main_context_iteration(nullptr, false);
    }
    ASSERT_TRUE(done);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.output.rfind("git version", 0), 0U) << result.output;
    EXPECT_FALSE(expiredCalled);
}

#ifdef _WIN32
TEST(ProjectGit, stopsHangingCommandsWithTheirChildren) {
    if (!ProjectGit::isAvailable()) {
        GTEST_SKIP() << "git is not installed";
    }
    // A shell alias runs sh and sleep as children of git, like ssh waiting for a passphrase. They hold the output
    // pipes, so the result only arrives if they are terminated as well.
    auto owner = std::make_shared<int>(0);
    bool done = false;
    ProjectGit::CommandResult result;
    const gint64 start = g_get_monotonic_time();
    ProjectGit::run(
            fs::current_path(), {"-c", "alias.hang=!sleep 60", "hang"}, owner,
            [&](ProjectGit::CommandResult r) {
                result = std::move(r);
                done = true;
            },
            2);
    const gint64 deadline = start + 20 * G_TIME_SPAN_SECOND;
    while (!done && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(10'000);
    }
    ASSERT_TRUE(done);
    EXPECT_TRUE(result.timedOut);
    EXPECT_FALSE(result.success);
    EXPECT_LT(g_get_monotonic_time() - start, 15 * G_TIME_SPAN_SECOND);
}
#endif

TEST(ProjectGit, quotesArgumentsWithSpacesAndQuotes) {
    if (!ProjectGit::isAvailable()) {
        GTEST_SKIP() << "git is not installed";
    }
    auto owner = std::make_shared<int>(0);
    bool done = false;
    ProjectGit::CommandResult result;
    const std::string value = R"(a "quoted" value with spaces\)";
    ProjectGit::run(fs::current_path(), {"-c", "test.value=" + value, "config", "test.value"}, owner,
                    [&](ProjectGit::CommandResult r) {
                        result = std::move(r);
                        done = true;
                    });
    const gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!done && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, true);
    }
    ASSERT_TRUE(done);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.output, value + "\n");
}

TEST(ProjectGit, branchNamesDoNotConfuseCounts) {
    auto status = ProjectGit::parseStatus(statusOutput({"## ahead...origin/ahead"}), "/repo");
    EXPECT_TRUE(status.hasUpstream);
    EXPECT_EQ(status.ahead, 0U);
    status = ProjectGit::parseStatus(statusOutput({"## behind 5...origin/behind 5 [ahead 3]"}), "/repo");
    EXPECT_EQ(status.ahead, 3U);
    EXPECT_EQ(status.behind, 0U);
}
