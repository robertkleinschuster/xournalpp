/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @license GNU GPLv2 or later
 */

#include <filesystem>
#include <fstream>

#include <config-test.h>
#include <gtest/gtest.h>

#include "control/AnnotationFiles.h"
#include "control/settings/Settings.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Point.h"
#include "model/Stroke.h"
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

TEST_F(AnnotationFilesTest, untrustedRecordedExportNamesAreRejected) {
    // An edited or shared document must not make the export overwrite other files
    for (const auto* value: {"Notes.xopp", "thesis.docx", "..", "sub/dir.pdf", "C:/other.pdf"}) {
        std::ofstream(dir / "Edited.xopp") << "<?xml version=\"1.0\"?>\n<xournal fork-export=\"" << value << "\">";
        EXPECT_EQ(AnnotationFiles::recordedExportFilename(dir / "Edited.xopp"), "") << value;
    }
    std::ofstream(dir / "Edited.xopp") << "<?xml version=\"1.0\"?>\n<xournal fork-export=\"Fine &amp; ok.pdf\">";
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(dir / "Edited.xopp"), "Fine & ok.pdf");
}

TEST_F(AnnotationFilesTest, renamedDocumentIsFoundByItsSourcePdf) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Lecture.pdf");
    DocumentHandler handler;
    Document doc(&handler);
    ASSERT_TRUE(doc.readPdf(dir / "Lecture.pdf", true, false));
    const auto xopp = dir / "Renamed notes.xopp";
    doc.setFilepath(xopp);
    SaveHandler saver;
    saver.prepareSave(&doc, xopp);
    saver.saveTo(xopp);
    ASSERT_TRUE(saver.getErrorMessage().empty()) << saver.getErrorMessage();

    EXPECT_EQ(AnnotationFiles::recordedSourcePdf(xopp).filename(), "Lecture.pdf");
    const auto found = AnnotationFiles::documentForSource(dir / "Lecture.pdf");
    ASSERT_TRUE(found);
    EXPECT_EQ(fs::path(*found).filename(), xopp.filename());
    EXPECT_FALSE(AnnotationFiles::documentForSource(dir / "Skript.pdf"));
}

TEST_F(AnnotationFilesTest, recoveryCopiesAreNotTheDocumentOfAPdf) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Lecture.pdf");
    DocumentHandler handler;
    Document doc(&handler);
    ASSERT_TRUE(doc.readPdf(dir / "Lecture.pdf", true, false));
    // Like the autosave of a document and a save that was interrupted, both annotating the PDF
    for (const auto* name: {".Lecture.autosave.xopp", "Lecture.xopp~"}) {
        SaveHandler saver;
        saver.prepareSave(&doc, dir / "Lecture.xopp");
        saver.saveTo(dir / "Saving.xopp");
        ASSERT_TRUE(saver.getErrorMessage().empty()) << saver.getErrorMessage();
        fs::rename(dir / "Saving.xopp", dir / name);
    }
    EXPECT_FALSE(AnnotationFiles::documentForSource(dir / "Lecture.pdf"));
}

TEST_F(AnnotationFilesTest, onlyChangedDocumentsHaveAnnotations) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Lecture.pdf");
    DocumentHandler handler;
    Document doc(&handler);
    ASSERT_TRUE(doc.readPdf(dir / "Lecture.pdf", true, false));
    ASSERT_GT(doc.getPageCount(), 0U);
    EXPECT_FALSE(AnnotationFiles::hasAnnotations(doc));

    auto stroke = std::make_unique<Stroke>();
    stroke->addPoint(Point(10, 10));
    stroke->addPoint(Point(20, 20));
    doc.getPage(0)->getSelectedLayer()->addElement(std::move(stroke));
    EXPECT_TRUE(AnnotationFiles::hasAnnotations(doc));
}

TEST_F(AnnotationFilesTest, addedPagesCountAsAnnotations) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Lecture.pdf");
    DocumentHandler handler;
    Document doc(&handler);
    ASSERT_TRUE(doc.readPdf(dir / "Lecture.pdf", true, false));
    doc.addPage(std::make_shared<XojPage>(100.0, 100.0));
    EXPECT_TRUE(AnnotationFiles::hasAnnotations(doc));
}

TEST_F(AnnotationFilesTest, reorderedPagesCountAsAnnotations) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Lecture.pdf");
    DocumentHandler handler;
    Document doc(&handler);
    ASSERT_TRUE(doc.readPdf(dir / "Lecture.pdf", true, false));
    ASSERT_GE(doc.getPageCount(), 2U);
    EXPECT_FALSE(AnnotationFiles::hasAnnotations(doc));

    // Same page count, only PDF backgrounds, no strokes: the order still differs from the PDF
    auto first = doc.getPage(0);
    doc.deletePage(0);
    doc.insertPage(first, 1);
    EXPECT_TRUE(AnnotationFiles::hasAnnotations(doc));
}
