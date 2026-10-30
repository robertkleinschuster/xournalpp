#include "AnnotationFiles.h"

#include <memory>
#include <string_view>
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
#include "util/GzInputStream.h"
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

namespace {

auto utf8(const fs::path& path) -> std::string {
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto pathFromUtf8(const std::string& value) -> fs::path {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

/// Export path following the export name setting
auto exportPathFromSetting(const Document& doc, const fs::path& xopp, const Settings& settings) -> fs::path {
    auto target = xopp.parent_path() /
                  doc.createSaveFilename(Document::PDF, settings.getDefaultSaveName(), settings.getDefaultPdfExportName());
    target += ".pdf";
    return target;
}

/// Rejects targets that would overwrite the document itself or the PDF it annotates
auto safeTarget(fs::path target, const fs::path& xopp, const fs::path& source) -> fs::path {
    if (target.empty() || samePath(target, xopp) || (!source.empty() && samePath(target, source))) {
        return {};
    }
    return target;
}

auto unescapeXml(std::string_view value) -> std::string {
    static const std::pair<std::string_view, char> entities[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string result;
    for (size_t i = 0; i < value.size();) {
        bool replaced = false;
        for (const auto& [entity, character]: entities) {
            if (value.substr(i, entity.size()) == entity) {
                result.push_back(character);
                i += entity.size();
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.push_back(value[i++]);
        }
    }
    return result;
}

}  // namespace

auto AnnotationFiles::recordedExportFilename(const fs::path& xopp) -> std::string {
    std::error_code error;
    if (!fs::is_regular_file(xopp, error)) {
        return {};
    }
    // The attribute is on the root element at the very beginning; no need to load the document
    std::string header;
    try {
        xoj::util::GzInputStream input(xopp);
        char buffer[4096];
        int read = 0;
        while (header.size() < 16384 && (read = input.read(buffer, sizeof(buffer))) > 0) {
            header.append(buffer, static_cast<size_t>(read));
            if (header.find('>', header.find("<xournal")) != std::string::npos) {
                break;
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    const auto tagStart = header.find("<xournal");
    const auto tagEnd = header.find('>', tagStart);
    if (tagStart == std::string::npos || tagEnd == std::string::npos) {
        return {};
    }
    const std::string_view tag(header.data() + tagStart, tagEnd - tagStart);
    constexpr std::string_view attribute = " fork-export=\"";
    const auto valueStart = tag.find(attribute);
    if (valueStart == std::string_view::npos) {
        return {};
    }
    const auto valueEnd = tag.find('"', valueStart + attribute.size());
    if (valueEnd == std::string_view::npos) {
        return {};
    }
    auto name = unescapeXml(tag.substr(valueStart + attribute.size(), valueEnd - valueStart - attribute.size()));
    // Only a plain file name next to the document is accepted
    return pathFromUtf8(name).filename() == pathFromUtf8(name) ? name : std::string();
}

auto AnnotationFiles::exportPath(const Document& doc, const Settings& settings) -> fs::path {
    const auto xopp = doc.getFilepath();
    if (xopp.empty() || !isXopp(xopp)) {
        return {};
    }
    const auto recorded = recordedExportFilename(xopp);
    auto target = recorded.empty() ? exportPathFromSetting(doc, xopp, settings) :
                                     xopp.parent_path() / pathFromUtf8(recorded);
    return safeTarget(std::move(target), xopp, doc.getPdfFilepath());
}

auto AnnotationFiles::exportPathForFile(const fs::path& xopp, const Settings& settings) -> fs::path {
    DocumentHandler handler;
    Document doc(&handler);
    doc.setFilepath(xopp);
    // Without loading the file the source PDF is unknown; never point at the PDF the document annotates
    auto source = xopp;
    source.replace_extension(".pdf");
    return safeTarget(exportPath(doc, settings), xopp, source);
}

auto AnnotationFiles::exportFilenameForSave(const Document& doc, const fs::path& target, const Settings& settings)
        -> std::string {
    if (auto recorded = recordedExportFilename(target); !recorded.empty()) {
        return recorded;
    }
    const auto path = safeTarget(exportPathFromSetting(doc, target, settings), target, doc.getPdfFilepath());
    return path.empty() ? std::string() : utf8(path.filename());
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
    control->setUserSavedHook([this] { exportIfOutdated([] {}); });
}

AnnotationExporter::~AnnotationExporter() {
    control->setBeforeCloseHook({});
    control->setUserSavedHook({});
}

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
