/*
 * Xournal++
 *
 * Fixed input benchmark test of the file loading process
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#include <iostream>
#include <memory>

#include <config-test.h>
#include <glib-2.0/glib.h>
#include <gtest/gtest.h>

#include "control/jobs/ProgressListener.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Layer.h"
#include "model/PageRef.h"
#include "model/PageType.h"
#include "model/XojPage.h"
#include "pdf/base/XojPdfExport.h"
#include "pdf/base/XojPdfExportFactory.h"
#include "util/OutputStream.h"
#include "util/PathUtil.h"

#include "filesystem.h"


static void benchLoadFile(const fs::path& filename, int iterations) {
    const auto start = g_get_monotonic_time();
    for (int i = 0; i < iterations; ++i) {
        LoadHandler{}.loadDocument(filename);
    }
    const auto stop = g_get_monotonic_time();
    std::cout << "Loaded " << filename << ' ' << iterations << " times in " << (stop - start) / 1000 << "ms.\n";
}

TEST(FileLoadBenchmark, benchmarkHandwrittenText) {
    benchLoadFile(GET_TESTFILE(u8"benchmark/handwritten-text.xopp"), 25);
}

TEST(FileLoadBenchmark, benchmarkTypedText) { benchLoadFile(GET_TESTFILE(u8"benchmark/typed-text.xopp"), 5'000); }

TEST(FileLoadBenchmark, benchmarkLatex) { benchLoadFile(GET_TESTFILE(u8"benchmark/latex.xopp"), 50); }

static auto createTemporaryFile(void (*buildDoc)(Document&), const fs::path& filename) -> fs::path {
    // Build file
    DocumentHandler dh;
    Document doc{&dh};
    buildDoc(doc);

    // Save it to a temporary path
    SaveHandler sh;
    auto tmp_path = Util::getTmpDirSubfolder() / filename;
    sh.prepareSave(&doc, tmp_path);
    sh.saveTo(tmp_path);

    return tmp_path;
}

TEST(FileLoadBenchmark, benchmarkEmpty) {
    // Create empty file (containing only one obligatory page)
    const auto tmp_path = createTemporaryFile(
            [](Document& doc) -> void {
                const PageRef page = std::make_shared<XojPage>(50, 50);
                doc.addPage(page);
            },
            u8"empty.xopp");

    // Benchmark loading time
    benchLoadFile(tmp_path, 100'000);

    // Clean up test file
    fs::remove(tmp_path);
}

TEST(FileLoadBenchmark, benchmarkManyPages) {
    // Create a 500-page file
    const auto tmp_path = createTemporaryFile(
            [](Document& doc) -> void {
                for (int i = 0; i < 500; ++i) {
                    const PageRef page = std::make_shared<XojPage>(50, 50);
                    doc.addPage(page);
                }
            },
            u8"many-pages.xopp");

    // Benchmark loading time
    benchLoadFile(tmp_path, 1000);

    // Clean up test file
    fs::remove(tmp_path);
}

static void benchSaveFile(const fs::path& filename, int iterations) {
    auto doc = LoadHandler{}.loadDocument(filename);
    ASSERT_TRUE(doc);
    const auto target = Util::getTmpDirSubfolder() / u8"save-benchmark.xopp";
    gint64 prepare = 0;
    gint64 write = 0;
    for (int i = 0; i < iterations; ++i) {
        SaveHandler handler;
        const auto start = g_get_monotonic_time();
        handler.prepareSave(doc.get(), target);
        const auto prepared = g_get_monotonic_time();
        handler.saveTo(target);
        const auto stop = g_get_monotonic_time();
        prepare += prepared - start;
        write += stop - prepared;
    }
    std::cout << "Saved " << filename << ' ' << iterations << " times: prepareSave " << prepare / 1000 << "ms, saveTo "
              << write / 1000 << "ms (" << fs::file_size(target) << " bytes).\n";
    fs::remove(target);
}

TEST(FileSaveBenchmark, benchmarkHandwrittenText) {
    benchSaveFile(GET_TESTFILE(u8"benchmark/handwritten-text.xopp"), 10);
}

namespace {
/// Discards all output, to measure the XML formatting without compression and disk access
class NullOutputStream: public OutputStream {
public:
    void write(const char*, size_t len) override { bytes += len; }
    void close() override {}
    size_t bytes = 0;
};
}  // namespace

TEST(FileSaveBenchmark, benchmarkHandwrittenTextFormattingOnly) {
    auto doc = LoadHandler{}.loadDocument(GET_TESTFILE(u8"benchmark/handwritten-text.xopp"));
    ASSERT_TRUE(doc);
    const auto target = Util::getTmpDirSubfolder() / u8"save-benchmark.xopp";
    gint64 total = 0;
    size_t bytes = 0;
    for (int i = 0; i < 10; ++i) {
        SaveHandler handler;
        handler.prepareSave(doc.get(), target);
        NullOutputStream out;
        const auto start = g_get_monotonic_time();
        handler.saveTo(&out, target);
        total += g_get_monotonic_time() - start;
        bytes = out.bytes;
    }
    std::cout << "Formatted handwritten-text 10 times in " << total / 1000 << "ms (" << bytes
              << " uncompressed bytes).\n";
}


static void exportPdf(const Document& doc, const fs::path& target, const char* label) {
    DummyProgressListener listener;
    auto exporter = XojPdfExportFactory::createExport(&doc, &listener);
    const auto start = g_get_monotonic_time();
    ASSERT_TRUE(exporter->createPdf(target, false)) << exporter->getLastError();
    std::cout << label << ": " << doc.getPageCount() << " pages in " << (g_get_monotonic_time() - start) / 1000
              << "ms (" << fs::file_size(target) << " bytes).\n";
}

TEST(PdfExportBenchmark, benchmarkHandwrittenTextAndPdfBackground) {
    auto handwritten = LoadHandler{}.loadDocument(GET_TESTFILE(u8"benchmark/handwritten-text.xopp"));
    ASSERT_TRUE(handwritten);
    const auto dir = Util::getTmpDirSubfolder();
    const auto source = dir / u8"export-benchmark-source.pdf";

    // Handwriting only: rendered with cairo. The result serves as source PDF for the next case.
    exportPdf(*handwritten, source, "Cairo export of handwriting");

    // The same pages as PDF background with the same handwriting on top: the annotation workflow (QPDF)
    auto annotated = LoadHandler{}.loadDocument(GET_TESTFILE(u8"benchmark/handwritten-text.xopp"));
    ASSERT_TRUE(annotated->readPdf(source, false, true));
    for (size_t i = 0; i < annotated->getPageCount(); ++i) {
        annotated->getPage(i)->setBackgroundPdfPageNr(i);
        annotated->getPage(i)->setBackgroundType(PageType(PageTypeFormat::Pdf));
    }
    exportPdf(*annotated, dir / u8"export-benchmark-annotated.pdf", "QPDF export of annotated PDF");

    std::error_code error;  // the source may still be open by the PDF document
    fs::remove(source, error);
    if (const char* keep = g_getenv("XOPP_BENCHMARK_KEEP_EXPORT")) {
        // Keep the result for inspection, e.g. with qpdf --check
        fs::copy_file(dir / u8"export-benchmark-annotated.pdf", fs::path(keep), fs::copy_options::overwrite_existing);
    }
    fs::remove(dir / u8"export-benchmark-annotated.pdf", error);
}
