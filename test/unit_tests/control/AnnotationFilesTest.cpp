/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "control/AnnotationFiles.h"
#include "control/settings/Settings.h"

namespace fs = std::filesystem;

namespace {
class AnnotationFilesTest: public ::testing::Test {
protected:
    void SetUp() override {
        dir = fs::temp_directory_path() / u8"xournalpp-annotation-test ä";
        fs::remove_all(dir);
        fs::create_directories(dir);
        settings = std::make_unique<Settings>(dir / "settings.xml");
        settings->setDefaultPdfExportName(u8"%{name}_annotated");
        for (const auto* name: {"Blatt1.xopp", "Blatt1.pdf", "Blatt1_annotated.pdf", "Skript.pdf"}) {
            std::ofstream(dir / name) << "x";
        }
    }
    void TearDown() override { fs::remove_all(dir); }

    fs::path dir;
    std::unique_ptr<Settings> settings;
};
}  // namespace

TEST_F(AnnotationFilesTest, exportPathFollowsExportNameSetting) {
    EXPECT_EQ(AnnotationFiles::exportPathForFile(dir / "Blatt1.xopp", *settings), dir / "Blatt1_annotated.pdf");
}

TEST_F(AnnotationFilesTest, exportNeverOverwritesTheSourcePdf) {
    settings->setDefaultPdfExportName(u8"%{name}");
    EXPECT_TRUE(AnnotationFiles::exportPathForFile(dir / "Blatt1.xopp", *settings).empty());
}

TEST_F(AnnotationFilesTest, exportedPdfBelongsToItsDocument) {
    const auto document = AnnotationFiles::documentForExport(dir / "Blatt1_annotated.pdf", *settings);
    ASSERT_TRUE(document);
    EXPECT_EQ(fs::path(*document).filename(), "Blatt1.xopp");
}

TEST_F(AnnotationFilesTest, sourceAndUnrelatedPdfsAreNoExports) {
    EXPECT_FALSE(AnnotationFiles::documentForExport(dir / "Blatt1.pdf", *settings));
    EXPECT_FALSE(AnnotationFiles::documentForExport(dir / "Skript.pdf", *settings));
}
