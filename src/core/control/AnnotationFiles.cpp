#include "AnnotationFiles.h"

#include <memory>
#include <utility>

#include <glib.h>

#include "control/Control.h"
#include "control/jobs/PdfExportJob.h"
#include "control/jobs/Scheduler.h"
#include "control/jobs/XournalScheduler.h"
#include "control/settings/Settings.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "undo/UndoRedoHandler.h"
#include "util/PathUtil.h"

namespace fs = std::filesystem;

namespace {

auto isXopp(const fs::path& path) -> bool { return path.extension() == ".xopp"; }

auto samePath(const fs::path& a, const fs::path& b) -> bool {
    std::error_code error;
    return fs::equivalent(a, b, error) || a.lexically_normal() == b.lexically_normal();
}

/// The regular PDF export, with a fixed target instead of the file dialog. Calls done when finished.
class AutoPdfExportJob final: public PdfExportJob {
public:
    AutoPdfExportJob(Control* control, fs::path target, std::function<void()> done):
            PdfExportJob(control), done(std::move(done)) {
        this->filepath = std::move(target);
    }

    void run() override {
        PdfExportJob::run();
        // PdfExportJob only reports back on errors
        callAfterRun();
    }

    void afterRun() override {
        PdfExportJob::afterRun();  // shows an error, if any
        // Continue after the blocking job has unblocked the UI, which is queued just like this callback
        g_idle_add(+[](gpointer data) -> gboolean {
            std::unique_ptr<std::function<void()>> callback(static_cast<std::function<void()>*>(data));
            (*callback)();
            return G_SOURCE_REMOVE;
        }, new std::function<void()>(std::move(done)));
    }

private:
    ~AutoPdfExportJob() override = default;

    std::function<void()> done;
};

}  // namespace

auto AnnotationFiles::exportPath(const Document& doc, const Settings& settings) -> fs::path {
    const auto xopp = doc.getFilepath();
    if (xopp.empty() || !isXopp(xopp)) {
        return {};
    }
    auto target = xopp.parent_path() /
                  doc.createSaveFilename(Document::PDF, settings.getDefaultSaveName(), settings.getDefaultPdfExportName());
    target += ".pdf";
    const auto source = doc.getPdfFilepath();
    if (samePath(target, xopp) || (!source.empty() && samePath(target, source))) {
        return {};
    }
    return target;
}

auto AnnotationFiles::exportPathForFile(const fs::path& xopp, const Settings& settings) -> fs::path {
    DocumentHandler handler;
    Document doc(&handler);
    doc.setFilepath(xopp);
    auto target = exportPath(doc, settings);
    // Without loading the file the source PDF is unknown; never point at the PDF the document annotates
    auto source = xopp;
    source.replace_extension(".pdf");
    return samePath(target, source) ? fs::path() : target;
}

auto AnnotationFiles::documentForExport(const fs::path& pdf, const Settings& settings) -> std::optional<fs::path> {
    std::error_code error;
    for (fs::directory_iterator it(pdf.parent_path(), error), end; !error && it != end; it.increment(error)) {
        if (isXopp(it->path()) && samePath(exportPathForFile(it->path(), settings), pdf)) {
            return it->path();
        }
    }
    return std::nullopt;
}

void AnnotationFiles::createDocumentFileForPdf(Control* control) {
    Document* doc = control->getDocument();
    doc->lock_shared();
    const auto pdf = doc->getPdfFilepath();
    const bool hasFile = !doc->getFilepath().empty();
    auto target = pdf.parent_path() / doc->createSaveFilename(Document::XOPP, control->getSettings()->getDefaultSaveName());
    doc->unlock_shared();
    target += ".xopp";

    std::error_code error;
    if (pdf.empty() || hasFile || fs::exists(target, error)) {
        return;
    }
    doc->lock();
    doc->setFilepath(target);
    doc->unlock();
    control->save();
}

AnnotationExporter::AnnotationExporter(Control* control): control(control) {
    control->setBeforeCloseHook([this](std::function<void()> proceed) { exportIfOutdated(std::move(proceed)); });
}

AnnotationExporter::~AnnotationExporter() { control->setBeforeCloseHook({}); }

void AnnotationExporter::exportIfOutdated(std::function<void()> done) {
    Document* doc = control->getDocument();
    auto* undoRedo = control->getUndoRedoHandler();
    if (!doc || !undoRedo || undoRedo->isChanged()) {
        // Unsaved changes: the file on disk is not what would be exported
        done();
        return;
    }
    doc->lock_shared();
    const auto xopp = doc->getFilepath();
    const auto target = AnnotationFiles::exportPath(*doc, *control->getSettings());
    doc->unlock_shared();
    if (target.empty()) {
        done();
        return;
    }
    std::error_code error;
    const auto saved = fs::last_write_time(xopp, error);
    if (error) {
        done();
        return;
    }
    if (const auto exported = fs::last_write_time(target, error); !error && exported >= saved) {
        done();
        return;
    }
    auto* job = new AutoPdfExportJob(control, target, std::move(done));
    control->getScheduler()->addJob(job, JOB_PRIORITY_NONE);
    job->unref();
}
