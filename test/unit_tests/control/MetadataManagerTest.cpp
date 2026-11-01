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
#include <gtest/gtest.h>

#include "control/settings/MetadataManager.h"
#include "util/PathUtil.h"

#include "config-test.h"


TEST(Metadata, testRead) {
    auto entry = MetadataManager::loadMetadataFile(GET_TESTFILE(u8"metadata/1.metadata"));
    EXPECT_TRUE(entry);
    EXPECT_EQ(entry->time, 1);
    EXPECT_EQ(entry->page, 12);
    EXPECT_EQ(entry->zoom, 3.42);
    EXPECT_EQ(entry->path, fs::path(u8"/test/my/metadata.xopp"));
}
TEST(Metadata, testReadNaNZoom) {
    for (const auto& file: {u8"metadata/3.metadata", u8"metadata/4.metadata"}) {
        auto entry = MetadataManager::loadMetadataFile(GET_TESTFILE(file));
        ASSERT_TRUE(entry);
        EXPECT_EQ(entry->zoom, MetadataEntry().zoom);
        EXPECT_EQ(entry->page, 54);
        EXPECT_EQ(entry->path, fs::path(u8"/test/my/metadata.xopp"));
    }
}

TEST(Metadata, testWriteReadCycle) {
    auto entry = MetadataManager::loadMetadataFile(GET_TESTFILE(u8"metadata/aωkward ⒫ath/2.metadata"));
    EXPECT_TRUE(entry);
    EXPECT_EQ(entry->time, 2);
    EXPECT_EQ(entry->page, 12);
    EXPECT_EQ(entry->zoom, 3.42);
    EXPECT_EQ(entry->path, fs::path(u8"/test/my/metadatⓐ.xopp"));

    entry->time = 1234567;
    entry->page = 98765;
    entry->zoom = 56.734;

    auto tmp = Util::getTmpDirSubfolder() / "1234567.metadata";
    ASSERT_FALSE(fs::exists(tmp));
    MetadataManager::writeMetadataToFile(*entry, tmp);

    auto entry2 = MetadataManager::loadMetadataFile(tmp);
    ASSERT_TRUE(entry2);
    EXPECT_EQ(entry->path, entry2->path);
    EXPECT_EQ(entry->time, entry2->time);
    EXPECT_EQ(entry->page, entry2->page);
    EXPECT_EQ(entry->zoom, entry2->zoom);
    EXPECT_FALSE(entry2->hasViewPosition);  // older entries have only page and zoom
    fs::remove(tmp);
}

TEST(Metadata, testViewPositionCycle) {
    MetadataEntry entry;
    entry.path = fs::path(u8"/test/ue4.xopp");
    entry.page = 2;
    entry.zoom = 1.6;
    entry.hasViewPosition = true;
    entry.offsetX = 24.5;
    entry.offsetY = -12.25;
    entry.zoomFit = true;

    auto tmp = Util::getTmpDirSubfolder() / "7654321.metadata";
    MetadataManager::writeMetadataToFile(entry, tmp);
    auto read = MetadataManager::loadMetadataFile(tmp);
    fs::remove(tmp);

    ASSERT_TRUE(read);
    EXPECT_EQ(read->path, entry.path);
    EXPECT_EQ(read->page, 2);
    EXPECT_EQ(read->zoom, 1.6);
    EXPECT_TRUE(read->hasViewPosition);
    EXPECT_EQ(read->offsetX, 24.5);
    EXPECT_EQ(read->offsetY, -12.25);
    EXPECT_TRUE(read->zoomFit);
}
