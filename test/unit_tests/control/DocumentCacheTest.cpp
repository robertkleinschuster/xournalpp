/*
 * Xournal++ fork
 *
 * Tests for the cache of recently opened documents
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "control/DocumentCache.h"
#include "control/PdfCache.h"
#include "gui/XournalView.h"  // for KeptRendering
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/XojPage.h"
#include "util/PathUtil.h"

namespace fs = std::filesystem;

namespace {

class DocumentCacheTest: public ::testing::Test {
protected:
    void SetUp() override {
        dir = Util::getTmpDirSubfolder() / "document-cache-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    auto writeFile(const std::string& name, const std::string& content) -> fs::path {
        auto path = dir / name;
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    /// An open document loaded from path, with the given number of pages
    auto openDocument(const fs::path& path, size_t pages = 1) -> std::unique_ptr<Document> {
        auto doc = std::make_unique<Document>(&handler);
        doc->setFilepath(path);
        for (size_t i = 0; i < pages; i++) {
            doc->addPage(std::make_shared<XojPage>(595, 842));
        }
        return doc;
    }

    /// Opens path in cache (as Control does after loading) and keeps it when the next document is opened
    void openAndKeep(DocumentCache& cache, const fs::path& path, size_t pages = 1) {
        auto doc = openDocument(path, pages);
        cache.openedFromDisk(*doc);
        cache.keep(*doc, std::make_unique<KeptRendering>());
    }

    DocumentHandler handler;
    fs::path dir;
};

}  // namespace

TEST_F(DocumentCacheTest, reusesUnchangedDocument) {
    DocumentCache cache;
    const auto file = writeFile("ue4.xopp", "content");
    openAndKeep(cache, file, 3);

    auto entry = cache.take(file);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->doc->getPageCount(), 3U);
    EXPECT_EQ(entry->doc->getFilepath(), file);
    EXPECT_TRUE(entry->rendering);

    // Taken out: a second request loads it again
    EXPECT_FALSE(cache.take(file));
}

TEST_F(DocumentCacheTest, reloadsDocumentChangedOnDisk) {
    DocumentCache cache;
    const auto file = writeFile("ue4.xopp", "content");
    openAndKeep(cache, file);

    writeFile("ue4.xopp", "changed by git");  // other size, so the stamp differs regardless of the clock resolution
    EXPECT_FALSE(cache.take(file));
}

TEST_F(DocumentCacheTest, reloadsDocumentRemovedFromDisk) {
    DocumentCache cache;
    const auto file = writeFile("ue4.xopp", "content");
    openAndKeep(cache, file);

    fs::remove(file);
    EXPECT_FALSE(cache.take(file));
}

TEST_F(DocumentCacheTest, keepsOnlyDocumentsMatchingTheirFiles) {
    DocumentCache cache;
    const auto file = writeFile("ue4.xopp", "content");
    const auto other = writeFile("vo.xopp", "content");

    // Not known to match its file (e.g. a new document, or one loaded with errors)
    auto doc = openDocument(file);
    EXPECT_FALSE(cache.canKeep(*doc));
    cache.keep(*doc, nullptr);
    EXPECT_FALSE(cache.take(file));

    // The disk state belongs to another document
    auto otherDoc = openDocument(other);
    cache.openedFromDisk(*otherDoc);
    EXPECT_FALSE(cache.canKeep(*doc));

    // Without pages
    auto empty = openDocument(file, 0);
    cache.openedFromDisk(*empty);
    EXPECT_FALSE(cache.canKeep(*empty));
}

TEST_F(DocumentCacheTest, keepsTheMostRecentDocuments) {
    DocumentCache cache;
    std::vector<fs::path> files;
    for (size_t i = 0; i <= DocumentCache::MAX_ENTRIES; i++) {
        files.push_back(writeFile("doc" + std::to_string(i) + ".xopp", "content"));
        openAndKeep(cache, files.back());
    }
    // The oldest one was dropped, the others are still there
    EXPECT_FALSE(cache.take(files.front()));
    for (size_t i = 1; i < files.size(); i++) {
        EXPECT_TRUE(cache.take(files[i])) << i;
    }
}

TEST_F(DocumentCacheTest, keepingAgainReplacesTheOlderCopy) {
    DocumentCache cache;
    const auto file = writeFile("ue4.xopp", "content");
    openAndKeep(cache, file, 1);
    openAndKeep(cache, file, 2);

    auto entry = cache.take(file);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->doc->getPageCount(), 2U);
    EXPECT_FALSE(cache.take(file));
}
