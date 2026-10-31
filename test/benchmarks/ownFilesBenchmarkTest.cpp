/*
 * Xournal++
 *
 * Benchmark of loading, saving and exporting real documents, e.g. a copy of one's own notes.
 * Set XOPP_BENCHMARK_FILES to a ';'-separated list of .xopp files; the test is skipped otherwise.
 * The documents are only read; their copies are written to the temporary folder.
 *
 * @license GNU GPLv2 or later
 */

#include <iostream>
#include <sstream>
#include <string>

#include <glib.h>
#include <gtest/gtest.h>

#include "control/jobs/ProgressListener.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "pdf/base/XojPdfExport.h"
#include "pdf/base/XojPdfExportFactory.h"
#include "util/PathUtil.h"

#include "filesystem.h"

namespace {
auto ms(gint64 us) -> double { return static_cast<double>(us) / 1000.0; }
}  // namespace

TEST(OwnFilesBenchmark, loadSaveExport) {
    const char* list = g_getenv("XOPP_BENCHMARK_FILES");
    if (!list) {
        GTEST_SKIP() << "XOPP_BENCHMARK_FILES is not set";
    }
    std::stringstream files(list);
    std::string file;
    const auto dir = Util::getTmpDirSubfolder();
    while (std::getline(files, file, ';')) {
        if (file.empty()) {
            continue;
        }
        const fs::path path = fs::path(std::u8string(file.begin(), file.end()));
        constexpr int RUNS = 5;

        gint64 load = 0;
        std::unique_ptr<Document> doc;
        for (int i = 0; i < RUNS; i++) {
            const auto start = g_get_monotonic_time();
            doc = LoadHandler{}.loadDocument(path);
            load += g_get_monotonic_time() - start;
            ASSERT_TRUE(doc) << path;
        }

        const auto target = dir / u8"own-benchmark.xopp";
        gint64 prepare = 0, write = 0;
        for (int i = 0; i < RUNS; i++) {
            SaveHandler handler;
            const auto start = g_get_monotonic_time();
            handler.prepareSave(doc.get(), target);
            const auto prepared = g_get_monotonic_time();
            handler.saveTo(target);
            write += g_get_monotonic_time() - prepared;
            prepare += prepared - start;
            ASSERT_TRUE(handler.getErrorMessage().empty()) << handler.getErrorMessage();
        }

        gint64 exportTime = 0;
        const auto pdf = dir / u8"own-benchmark.pdf";
        for (int i = 0; i < RUNS; i++) {
            DummyProgressListener listener;
            auto exporter = XojPdfExportFactory::createExport(doc.get(), &listener);
            const auto start = g_get_monotonic_time();
            ASSERT_TRUE(exporter->createPdf(pdf, false)) << exporter->getLastError();
            exportTime += g_get_monotonic_time() - start;
        }

        std::cout << path.filename().string() << ": " << doc->getPageCount() << " pages, load " << ms(load / RUNS)
                  << " ms, prepareSave " << ms(prepare / RUNS) << " ms, saveTo " << ms(write / RUNS) << " ms ("
                  << fs::file_size(target) << " B), PDF export " << ms(exportTime / RUNS) << " ms ("
                  << fs::file_size(pdf) << " B)\n";
        std::error_code error;
        fs::remove(target, error);
        fs::remove(pdf, error);
    }
}
