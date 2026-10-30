/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/PageType.h"
#include "model/XojPage.h"

namespace fs = std::filesystem;

/// The project explorer creates new documents by saving a fresh Document with one default page
TEST(NewDocumentFile, emptyDocumentWithGraphPageReloads) {
    const fs::path path = fs::temp_directory_path() / u8"xournalpp-test-new Dokument ä.xopp";
    fs::remove(path);

    DocumentHandler handler;
    {
        Document document(&handler);
        auto page = std::make_shared<XojPage>(595.275591, 841.889764);
        page->setBackgroundType(PageType(PageTypeFormat::Graph));
        document.addPage(std::move(page));
        SaveHandler saver;
        saver.prepareSave(&document, path);
        saver.saveTo(path);
        ASSERT_TRUE(saver.getErrorMessage().empty()) << saver.getErrorMessage();
    }

    struct RemoveOnExit {
        ~RemoveOnExit() { fs::remove(file); }
        fs::path file;
    } guard{path};

    std::unique_ptr<Document> reloaded;
    ASSERT_NO_THROW(reloaded = LoadHandler{}.loadDocument(path));
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->getPageCount(), 1U);
    EXPECT_EQ(reloaded->getPage(0)->getBackgroundType().format, PageTypeFormat::Graph);
}
