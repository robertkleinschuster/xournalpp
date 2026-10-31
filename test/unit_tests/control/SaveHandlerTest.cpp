/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#include <memory>

#include <gtest/gtest.h>

#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/XojPage.h"
#include "util/PathUtil.h"

#include "filesystem.h"

namespace {
class SaveHandlerTest: public ::testing::Test {
protected:
    void SetUp() override {
        dir = Util::getTmpDirSubfolder("save-handler-test");
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    /// Saves a document with the given number of pages and returns the error message
    auto save(const fs::path& file, size_t pages) -> std::string {
        DocumentHandler handler;
        Document doc(&handler);
        for (size_t i = 0; i < pages; i++) {
            doc.addPage(std::make_shared<XojPage>(595.0, 842.0));
        }
        SaveHandler saver;
        saver.prepareSave(&doc, file);
        saver.saveTo(file);
        return saver.getErrorMessage();
    }

    static auto pageCount(const fs::path& file) -> size_t { return LoadHandler{}.loadDocument(file)->getPageCount(); }

    fs::path dir;
};
}  // namespace

TEST_F(SaveHandlerTest, replacesAnExistingFile) {
    const auto file = dir / "doc.xopp";
    ASSERT_EQ(save(file, 1), "");
    ASSERT_EQ(save(file, 3), "");

    EXPECT_EQ(pageCount(file), 3);
    EXPECT_FALSE(fs::exists(fs::path{file} += "~"));
}

TEST_F(SaveHandlerTest, keepsThePreviousFileIfItCannotBeReplaced) {
    // A directory cannot be replaced by the new file
    const auto file = dir / "doc.xopp";
    fs::create_directories(file / "content");

    EXPECT_NE(save(file, 1), "");
    EXPECT_TRUE(fs::exists(file / "content"));
    EXPECT_FALSE(fs::exists(fs::path{file} += "~"));
}
