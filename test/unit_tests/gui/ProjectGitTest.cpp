/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * Fork: the project explorer's git support (libgit2) against real repositories with a local remote
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <git2.h>
#include <glib.h>
#include <gtest/gtest.h>

#include "gui/sidebar/ProjectGit.h"
#include "util/PathUtil.h"

namespace fs = std::filesystem;
using ProjectGit::FileState;
using ProjectGit::Outcome;

namespace {

void write(const fs::path& file, const std::string& content) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

auto read(const fs::path& file) -> std::string {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const ProjectGit::CredentialPrompt NO_PROMPT = [](const std::string&, const auto&) {
    return std::optional<ProjectGit::Credentials>();
};

class ProjectGitTest: public ::testing::Test {
protected:
    void SetUp() override {
        git_libgit2_init();
        root = fs::temp_directory_path() /
               ("xournalpp-git-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        fs::create_directories(root);
        remote = root / "remote.git";
        git_repository* repo = nullptr;
        ASSERT_EQ(git_repository_init(&repo, Util::toUtf8(remote).c_str(), true), 0);
        git_repository_free(repo);
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
        git_libgit2_shutdown();
    }

    /// A working repository with the remote as origin
    auto makeClone(const std::string& name) -> fs::path {
        const auto dir = root / name;
        git_repository* repo = nullptr;
        EXPECT_EQ(git_clone(&repo, Util::toUtf8(remote).c_str(), Util::toUtf8(dir).c_str(), nullptr), 0);
        git_repository_free(repo);
        return dir;
    }

    auto headMessage(const fs::path& dir) -> std::string {
        git_repository* repo = nullptr;
        git_repository_open(&repo, Util::toUtf8(dir).c_str());
        git_object* head = nullptr;
        std::string message;
        if (git_revparse_single(&head, repo, "HEAD") == 0) {
            message = git_commit_message(reinterpret_cast<git_commit*>(head));
            git_object_free(head);
        }
        git_repository_free(repo);
        return message;
    }

    auto repositoryState(const fs::path& dir) -> int {
        git_repository* repo = nullptr;
        git_repository_open(&repo, Util::toUtf8(dir).c_str());
        const int state = git_repository_state(repo);
        git_repository_free(repo);
        return state;
    }

    fs::path root;
    fs::path remote;
};

}  // namespace

TEST_F(ProjectGitTest, notARepository) {
    write(root / "plain" / "a.xopp", "a");
    const auto result = ProjectGit::status(root / "plain");
    EXPECT_FALSE(result.isRepo);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(ProjectGitTest, statusReportsAddedModifiedAndDeletedFiles) {
    const auto a = makeClone("a");
    write(a / "notes" / "Lecture 1.xopp", "v1");
    write(a / "old.pdf", "old");
    ASSERT_TRUE(ProjectGit::commitAll(a, "first").ok());

    write(a / "notes" / "Lecture 1.xopp", "v2");
    fs::remove(a / "old.pdf");
    write(a / "new.pdf", "new");
    write(a / ".Lecture 2.autosave.xopp", "recovery");  // never shown nor committed
    write(a / "notes" / "Lecture 1.xopp~", "backup");

    const auto result = ProjectGit::status(a);
    ASSERT_TRUE(result.isRepo);
    const auto& status = result.status;
    EXPECT_EQ(status.changedFiles, 3u);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(result.toplevel / "notes" / "Lecture 1.xopp")), FileState::Modified);
    EXPECT_EQ(status.files.at(ProjectGit::pathKey(result.toplevel / "new.pdf")), FileState::Added);
    ASSERT_EQ(status.deleted.size(), 1u);
    EXPECT_EQ(status.deleted[0].filename(), "old.pdf");
}

TEST_F(ProjectGitTest, statusAndCommitAreLimitedToTheProjectFolder) {
    const auto a = makeClone("a");
    write(a / "project" / "doc.xopp", "doc");
    write(a / "elsewhere.txt", "other");
    ASSERT_TRUE(ProjectGit::commitAll(a / "project", "project only").ok());

    const auto inProject = ProjectGit::status(a / "project");
    EXPECT_EQ(inProject.status.changedFiles, 0u);
    // The file outside the project folder was neither shown nor committed
    const auto whole = ProjectGit::status(a);
    EXPECT_EQ(whole.status.changedFiles, 1u);
    EXPECT_EQ(whole.status.files.at(ProjectGit::pathKey(whole.toplevel / "elsewhere.txt")), FileState::Added);
}

TEST_F(ProjectGitTest, pushSetsTheUpstreamAndAnotherCloneFastForwards) {
    const auto a = makeClone("a");
    write(a / "doc.xopp", "from a");
    ASSERT_TRUE(ProjectGit::commitAll(a, "a1").ok());
    const auto pushed = ProjectGit::push(a, NO_PROMPT);
    ASSERT_TRUE(pushed.ok()) << pushed.message;
    EXPECT_TRUE(ProjectGit::status(a).status.hasUpstream);

    const auto b = makeClone("b");
    write(a / "doc.xopp", "from a, again");
    ASSERT_TRUE(ProjectGit::commitAll(a, "a2").ok());
    ASSERT_TRUE(ProjectGit::push(a, NO_PROMPT).ok());

    EXPECT_TRUE(ProjectGit::fetch(b).ok());
    EXPECT_EQ(ProjectGit::status(b).status.behind, 1u);
    const auto pulled = ProjectGit::pullRebase(b, NO_PROMPT);
    ASSERT_TRUE(pulled.ok()) << pulled.message;
    EXPECT_EQ(read(b / "doc.xopp"), "from a, again");
    EXPECT_EQ(ProjectGit::status(b).status.changedFiles, 0u);
}

TEST_F(ProjectGitTest, divergedChangesAreRebased) {
    const auto a = makeClone("a");
    write(a / "shared.xopp", "base");
    ASSERT_TRUE(ProjectGit::commitAll(a, "base").ok());
    ASSERT_TRUE(ProjectGit::push(a, NO_PROMPT).ok());
    const auto b = makeClone("b");

    write(a / "from-a.xopp", "a");
    ASSERT_TRUE(ProjectGit::commitAll(a, "from a").ok());
    ASSERT_TRUE(ProjectGit::push(a, NO_PROMPT).ok());

    write(b / "from-b.xopp", "b");
    ASSERT_TRUE(ProjectGit::commitAll(b, "from b").ok());
    const auto pulled = ProjectGit::pullRebase(b, NO_PROMPT);
    ASSERT_TRUE(pulled.ok()) << pulled.message;
    EXPECT_EQ(read(b / "from-a.xopp"), "a");
    EXPECT_EQ(read(b / "from-b.xopp"), "b");
    EXPECT_EQ(headMessage(b), "from b");  // the local commit on top
    ASSERT_TRUE(ProjectGit::push(b, NO_PROMPT).ok());
    EXPECT_EQ(ProjectGit::status(b).status.ahead, 0u);
}

TEST_F(ProjectGitTest, aConflictLeavesEverythingAsBefore) {
    const auto a = makeClone("a");
    write(a / "shared.xopp", "base");
    ASSERT_TRUE(ProjectGit::commitAll(a, "base").ok());
    ASSERT_TRUE(ProjectGit::push(a, NO_PROMPT).ok());
    const auto b = makeClone("b");

    write(a / "shared.xopp", "a's version");
    ASSERT_TRUE(ProjectGit::commitAll(a, "a edits").ok());
    ASSERT_TRUE(ProjectGit::push(a, NO_PROMPT).ok());

    write(b / "shared.xopp", "b's version");
    ASSERT_TRUE(ProjectGit::commitAll(b, "b edits").ok());
    const auto pulled = ProjectGit::pullRebase(b, NO_PROMPT);
    EXPECT_EQ(pulled.outcome, Outcome::Conflict);
    EXPECT_EQ(read(b / "shared.xopp"), "b's version");
    EXPECT_EQ(headMessage(b), "b edits");
    EXPECT_EQ(repositoryState(b), GIT_REPOSITORY_STATE_NONE);  // no rebase left half done
    // Uploading is refused, the remote keeps a's version
    EXPECT_FALSE(ProjectGit::push(b, NO_PROMPT).ok());
}

TEST_F(ProjectGitTest, restoreDiscardsChanges) {
    const auto a = makeClone("a");
    write(a / "folder" / "doc.xopp", "committed");
    write(a / "folder" / "other.pdf", "committed too");
    ASSERT_TRUE(ProjectGit::commitAll(a, "base").ok());

    write(a / "folder" / "doc.xopp", "changed");
    ASSERT_TRUE(ProjectGit::restore(a, {a / "folder" / "doc.xopp"}).ok());
    EXPECT_EQ(read(a / "folder" / "doc.xopp"), "committed");

    // A deleted folder comes back with its files
    fs::remove_all(a / "folder");
    ASSERT_TRUE(ProjectGit::restore(a, {a / "folder"}).ok());
    EXPECT_EQ(read(a / "folder" / "other.pdf"), "committed too");
    EXPECT_EQ(ProjectGit::status(a).status.changedFiles, 0u);
}

TEST_F(ProjectGitTest, repositoriesUsingLfsAreNotSynced) {
    const auto a = makeClone("a");
    write(a / ".gitattributes", "*.pdf filter=lfs diff=lfs merge=lfs -text\n");
    ASSERT_TRUE(ProjectGit::commitAll(a, "lfs").ok());  // the attributes were not in the index yet
    write(a / "doc.pdf", "pdf");
    EXPECT_EQ(ProjectGit::commitAll(a, "pdf").outcome, Outcome::NotSupported);
}

TEST_F(ProjectGitTest, pathKeyNormalizesPaths) {
    EXPECT_EQ(ProjectGit::pathKey(fs::path("/repo/a/../b/")), ProjectGit::pathKey(fs::path("/repo/b")));
}

/// Manual check against a real remote that needs credentials: XOPP_GIT_FETCH_TEST_REPO=<clone with an https origin>.
/// The fetch must succeed with the credentials stored in the Windows credential manager, without asking.
TEST(ProjectGitRemoteTest, fetchesWithStoredCredentials) {
    const char* repo = g_getenv("XOPP_GIT_FETCH_TEST_REPO");
    if (!repo) {
        GTEST_SKIP() << "XOPP_GIT_FETCH_TEST_REPO not set";
    }
    const auto result = ProjectGit::fetch(Util::fromUtf8(repo));
    EXPECT_TRUE(result.ok()) << result.message;
}
