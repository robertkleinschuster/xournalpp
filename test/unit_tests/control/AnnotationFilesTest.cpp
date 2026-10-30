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
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/XojPage.h"

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

namespace {
/// Saves an empty document to xopp the way SaveJob does, recording the export name
void saveDocument(const fs::path& xopp, const Settings& settings) {
    DocumentHandler handler;
    Document doc(&handler);
    doc.addPage(std::make_shared<XojPage>(595.0, 842.0));
    doc.setFilepath(xopp);
    SaveHandler saver;
    saver.prepareSave(&doc, xopp);
    saver.setExportFilename(AnnotationFiles::exportFilenameForSave(doc, xopp, settings));
    saver.saveTo(xopp);
    ASSERT_TRUE(saver.getErrorMessage().empty()) << saver.getErrorMessage();
}
}  // namespace

TEST_F(AnnotationFilesTest, recordedExportNameSurvivesSettingChange) {
    const auto xopp = dir / "Notes & Co.xopp";
    saveDocument(xopp, *settings);
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(xopp), "Notes & Co_annotated.pdf");

    settings->setDefaultPdfExportName(u8"%{name} (export)");
    EXPECT_EQ(AnnotationFiles::exportPathForFile(xopp, *settings), dir / "Notes & Co_annotated.pdf");
    // Saving again keeps the recorded name instead of switching to the new setting
    saveDocument(xopp, *settings);
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(xopp), "Notes & Co_annotated.pdf");

    std::ofstream(dir / "Notes & Co_annotated.pdf") << "x";
    const auto document = AnnotationFiles::documentForExport(dir / "Notes & Co_annotated.pdf", *settings);
    ASSERT_TRUE(document);
    EXPECT_EQ(fs::path(*document).filename(), xopp.filename());
}

TEST_F(AnnotationFilesTest, documentWithRecordedExportNameStillLoads) {
    const auto xopp = dir / "Loadable.xopp";
    saveDocument(xopp, *settings);
    std::unique_ptr<Document> loaded;
    ASSERT_NO_THROW(loaded = LoadHandler{}.loadDocument(xopp));
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->getPageCount(), 1U);
}

TEST_F(AnnotationFilesTest, sourceAndUnrelatedPdfsAreNoExports) {
    EXPECT_FALSE(AnnotationFiles::documentForExport(dir / "Blatt1.pdf", *settings));
    EXPECT_FALSE(AnnotationFiles::documentForExport(dir / "Skript.pdf", *settings));
}
