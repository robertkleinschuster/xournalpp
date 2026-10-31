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

TEST_F(AnnotationFilesTest, rewrittenDocumentIsReadAgain) {
    // The header is remembered per file; a rewrite of the same size right away must not return the old value
    std::ofstream(dir / "Edited.xopp") << "<?xml version=\"1.0\"?>\n<xournal fork-export=\"a.pdf\">";
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(dir / "Edited.xopp"), "a.pdf");
    std::ofstream(dir / "Edited.xopp") << "<?xml version=\"1.0\"?>\n<xournal fork-export=\"b.pdf\">";
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(dir / "Edited.xopp"), "b.pdf");
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

TEST_F(AnnotationFilesTest, documentFollowsItsRenamedPdf) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Lecture.pdf");
    const auto xopp = dir / "Lecture.xopp";
    {
        DocumentHandler handler;
        Document doc(&handler);
        ASSERT_TRUE(doc.readPdf(dir / "Lecture.pdf", true, false));
        doc.setFilepath(xopp);
        SaveHandler saver;
        saver.prepareSave(&doc, xopp);
        saver.setExportFilename("Lecture_annotated.pdf");
        saver.saveTo(xopp);
        ASSERT_TRUE(saver.getErrorMessage().empty()) << saver.getErrorMessage();
    }
    fs::rename(dir / "Lecture.pdf", dir / "Lecture 3.pdf");
    EXPECT_EQ(AnnotationFiles::relinkSourcePdf(xopp, dir / "Lecture 3.pdf", *settings), "");

    EXPECT_EQ(AnnotationFiles::recordedSourcePdf(xopp).filename(), "Lecture 3.pdf");
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(xopp), "Lecture_annotated.pdf");
    LoadHandler loader;
    auto doc = loader.loadDocument(xopp);
    ASSERT_TRUE(doc);
    EXPECT_TRUE(loader.getMissingPdfFilename().empty());
    EXPECT_GT(doc->getPdfPageCount(), 0U);
}

namespace {
/// Saves a document annotating pdf to xopp, recording exportName, like the fork does when annotating a PDF
void saveAnnotation(const fs::path& xopp, const fs::path& pdf, const std::string& exportName) {
    DocumentHandler handler;
    Document doc(&handler);
    ASSERT_TRUE(doc.readPdf(pdf, true, false));
    doc.setFilepath(xopp);
    SaveHandler saver;
    saver.prepareSave(&doc, xopp);
    saver.setExportFilename(exportName);
    saver.saveTo(xopp);
    ASSERT_TRUE(saver.getErrorMessage().empty()) << saver.getErrorMessage();
}

/// Performs the moves of a group and records the new names, as the project explorer does
void applyGroup(const AnnotationFiles::DocumentGroup& group, const Settings& settings) {
    for (const auto& [from, to]: group.moves) {
        fs::rename(from, to);
    }
    if (group.newSource || group.newExportName) {
        ASSERT_EQ(AnnotationFiles::relinkDocument(group.newXopp, group.newSource, group.newExportName, settings), "");
    }
}

/// The document loads with its PDF background
void expectLoadsWithPdf(const fs::path& xopp) {
    LoadHandler loader;
    auto doc = loader.loadDocument(xopp);
    ASSERT_TRUE(doc);
    EXPECT_TRUE(loader.getMissingPdfFilename().empty()) << loader.getMissingPdfFilename();
    EXPECT_GT(doc->getPdfPageCount(), 0U);
}
}  // namespace

TEST_F(AnnotationFilesTest, renamingADocumentRenamesItsFiles) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "VO8.pdf");
    std::ofstream(dir / "VO8_annotated.pdf") << "x";
    saveAnnotation(dir / "VO8.xopp", dir / "VO8.pdf", "VO8_annotated.pdf");

    const auto newXopp = dir / "VO8 Induktion.xopp";
    const auto group = AnnotationFiles::groupForRename(dir / "VO8.xopp", newXopp, *settings);
    EXPECT_EQ(group.moves.size(), 3U);
    EXPECT_FALSE(AnnotationFiles::groupConflict(group));
    applyGroup(group, *settings);

    EXPECT_TRUE(fs::exists(dir / "VO8 Induktion.pdf"));
    EXPECT_TRUE(fs::exists(dir / "VO8 Induktion_annotated.pdf"));
    EXPECT_FALSE(fs::exists(dir / "VO8.pdf"));
    EXPECT_EQ(AnnotationFiles::recordedSourcePdf(newXopp).filename(), "VO8 Induktion.pdf");
    EXPECT_EQ(AnnotationFiles::exportPathForFile(newXopp, *settings).filename(), "VO8 Induktion_annotated.pdf");
    expectLoadsWithPdf(newXopp);
}

TEST_F(AnnotationFilesTest, renamingADocumentKeepsADifferentlyNamedPdf) {
    // e.g. the PDF from the course keeps its name; the export name follows even without an exported PDF
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Kurs.pdf");
    saveAnnotation(dir / "Notes.xopp", dir / "Kurs.pdf", "Notes_annotated.pdf");

    const auto group = AnnotationFiles::groupForRename(dir / "Notes.xopp", dir / "Mitschrift.xopp", *settings);
    EXPECT_EQ(group.moves.size(), 1U);
    EXPECT_FALSE(group.newSource);
    ASSERT_TRUE(group.newExportName);
    EXPECT_EQ(*group.newExportName, "Mitschrift_annotated.pdf");
    applyGroup(group, *settings);

    EXPECT_TRUE(fs::exists(dir / "Kurs.pdf"));
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(dir / "Mitschrift.xopp"), "Mitschrift_annotated.pdf");
    expectLoadsWithPdf(dir / "Mitschrift.xopp");
}

TEST_F(AnnotationFilesTest, renamesThatWouldOverwriteFilesAreConflicts) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "VO8.pdf");
    saveAnnotation(dir / "VO8.xopp", dir / "VO8.pdf", "VO8_annotated.pdf");
    // The document and its PDF would both become "Foo.pdf"
    EXPECT_TRUE(AnnotationFiles::groupConflict(
            AnnotationFiles::groupForRename(dir / "VO8.xopp", dir / "Foo.pdf", *settings)));
    // The source PDF would replace an existing file
    std::ofstream(dir / "Other.pdf") << "x";
    EXPECT_EQ(AnnotationFiles::groupConflict(
                      AnnotationFiles::groupForRename(dir / "VO8.xopp", dir / "Other.xopp", *settings)),
              std::optional<std::string>("Other.pdf"));
}

TEST_F(AnnotationFilesTest, movingADocumentMovesItsFiles) {
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "VO8.pdf");
    std::ofstream(dir / "VO8_annotated.pdf") << "x";
    saveAnnotation(dir / "VO8.xopp", dir / "VO8.pdf", "VO8_annotated.pdf");
    fs::create_directory(dir / "Archiv");

    const auto group = AnnotationFiles::groupForMove(dir / "VO8.xopp", dir / "Archiv", *settings);
    EXPECT_EQ(group.moves.size(), 3U);
    EXPECT_FALSE(AnnotationFiles::groupConflict(group));
    applyGroup(group, *settings);

    EXPECT_TRUE(fs::exists(dir / "Archiv" / "VO8.pdf"));
    EXPECT_TRUE(fs::exists(dir / "Archiv" / "VO8_annotated.pdf"));
    EXPECT_EQ(AnnotationFiles::recordedExportFilename(dir / "Archiv" / "VO8.xopp"), "VO8_annotated.pdf");
    expectLoadsWithPdf(dir / "Archiv" / "VO8.xopp");
    const auto found = AnnotationFiles::documentForSource(dir / "Archiv" / "VO8.pdf");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->filename(), "VO8.xopp");
}

TEST_F(AnnotationFilesTest, movingADocumentKeepsAPdfItDoesNotOwn) {
    // A differently named PDF stays; the moved document refers to it from its new folder
    fs::copy_file(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf"), dir / "Kurs.pdf");
    saveAnnotation(dir / "Notes.xopp", dir / "Kurs.pdf", "Notes_annotated.pdf");
    fs::create_directories(dir / "Woche 1");

    const auto group = AnnotationFiles::groupForMove(dir / "Notes.xopp", dir / "Woche 1", *settings);
    EXPECT_EQ(group.moves.size(), 1U);
    ASSERT_TRUE(group.newSource);
    applyGroup(group, *settings);

    EXPECT_TRUE(fs::exists(dir / "Kurs.pdf"));
    expectLoadsWithPdf(dir / "Woche 1" / "Notes.xopp");
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
