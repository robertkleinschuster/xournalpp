#include "AnnotationFiles.h"

#include <cstring>
#include <fstream>
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
        // PdfExportJob only reports back on errors (and only while there is a window), so report success here.
        // Calling callAfterRun() twice would run afterRun() twice.
        if (this->errorMsg.empty() || !control->getWindow()) {
            callAfterRun();
        }
    }

    void afterRun() override {
        PdfExportJob::afterRun();  // shows an error, if any
        if (!done) {
            return;
        }
        // Continue after the blocking job has unblocked the UI, which is queued just like this callback
        g_idle_add(+[](gpointer data) -> gboolean {
            std::unique_ptr<std::function<void()>> callback(static_cast<std::function<void()>*>(data));
            (*callback)();
            return G_SOURCE_REMOVE;
        }, new std::function<void()>(std::exchange(done, {})));
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
                  doc.createSaveFilename(Document::PDF, settings.getDefaultSaveName(),
                                         settings.getDefaultPdfExportName());
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

/// Whether files can be created in dir; checked by actually creating (and removing) a probe file
auto isWritableDirectory(const fs::path& dir) -> bool {
    const auto probe = dir / ".xournalpp-write-test";
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    fs::remove(probe, error);
    return true;
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

namespace {
/**
 * Reads an attribute of the first element starting with tagStart from the beginning of a .xopp file, without loading
 * the document. Only the first maxBytes (decompressed) are searched.
 */
auto readXoppAttribute(const fs::path& xopp, std::string_view tagStart, std::string_view attribute, size_t maxBytes)
        -> std::string {
    std::error_code error;
    if (!fs::is_regular_file(xopp, error)) {
        return {};
    }
    std::string header;
    size_t tagPos = std::string::npos;
    try {
        xoj::util::GzInputStream input(xopp);
        char buffer[4096];
        int read = 0;
        while (header.size() < maxBytes && (read = input.read(buffer, sizeof(buffer))) > 0) {
            header.append(buffer, static_cast<size_t>(read));
            tagPos = header.find(tagStart);
            if (tagPos != std::string::npos && header.find('>', tagPos) != std::string::npos) {
                break;
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    tagPos = header.find(tagStart);
    const auto tagEnd = tagPos == std::string::npos ? std::string::npos : header.find('>', tagPos);
    if (tagEnd == std::string::npos) {
        return {};
    }
    const std::string_view tag(header.data() + tagPos, tagEnd - tagPos);
    const std::string prefix = " " + std::string(attribute) + "=\"";
    const auto valueStart = tag.find(prefix);
    if (valueStart == std::string_view::npos) {
        return {};
    }
    const auto valueEnd = tag.find('"', valueStart + prefix.size());
    if (valueEnd == std::string_view::npos) {
        return {};
    }
    return unescapeXml(tag.substr(valueStart + prefix.size(), valueEnd - valueStart - prefix.size()));
}

auto sameFileName(const fs::path& a, const fs::path& b) -> bool {
#ifdef _WIN32
    gchar* foldedA = g_utf8_casefold(utf8(a.filename()).c_str(), -1);
    gchar* foldedB = g_utf8_casefold(utf8(b.filename()).c_str(), -1);
    const bool same = std::strcmp(foldedA, foldedB) == 0;
    g_free(foldedA);
    g_free(foldedB);
    return same;
#else
    return a.filename() == b.filename();
#endif
}
}  // namespace

auto AnnotationFiles::recordedSourcePdf(const fs::path& xopp) -> fs::path {
    // The first PDF background names the file, relative to the document or absolute. The preview image comes first.
    const auto name = readXoppAttribute(xopp, "<background type=\"pdf\"", "filename", 1024 * 1024);
    if (name.empty()) {
        return {};
    }
    const auto path = pathFromUtf8(name);
    return path.is_absolute() ? path : xopp.parent_path() / path;
}

auto AnnotationFiles::documentForSource(const fs::path& pdf) -> std::optional<fs::path> {
    std::error_code error;
    for (fs::directory_iterator it(pdf.parent_path(), error), end; !error && it != end; it.increment(error)) {
        if (!isXopp(it->path())) {
            continue;
        }
        // Only documents next to their PDF, as created when annotating; compare names to not depend on drive letters
        const auto source = recordedSourcePdf(it->path());
        if (!source.empty() && sameFileName(source, pdf) && samePath(source.parent_path(), pdf.parent_path())) {
            return it->path();
        }
    }
    return std::nullopt;
}

auto AnnotationFiles::recordedExportFilename(const fs::path& xopp) -> std::string {
    // The attribute is on the root element at the very beginning
    auto name = readXoppAttribute(xopp, "<xournal", "fork-export", 16384);
    if (name.empty()) {
        return {};
    }
    // The value comes from a file that may have been shared or edited by hand: only accept a plain .pdf file name
    // next to the document, so an export can never overwrite another kind of file
    const auto path = pathFromUtf8(name);
    const bool plainPdfName = path.filename() == path && name != "." && name != ".." && Util::hasPdfFileExt(path);
    return plainPdfName ? name : std::string();
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
    // Never point at the PDF the document annotates: the recorded background, or by name for older documents
    auto source = recordedSourcePdf(xopp);
    if (source.empty()) {
        source = xopp;
        source.replace_extension(".pdf");
    }
    return safeTarget(exportPath(doc, settings), xopp, source);
}

auto AnnotationFiles::exportFilenameForSave(const Document& doc, const fs::path& target, const Settings& settings)
        -> std::string {
    if (auto recorded = recordedExportFilename(target); !recorded.empty()) {
        if (!safeTarget(target.parent_path() / pathFromUtf8(recorded), target, doc.getPdfFilepath()).empty()) {
            return recorded;
        }
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
    auto target = pdf.parent_path() /
                  doc->createSaveFilename(Document::XOPP, control->getSettings()->getDefaultSaveName());
    doc->unlock_shared();
    target += ".xopp";

    std::error_code error;
    if (pdf.empty() || hasFile || fs::exists(target, error) || !isWritableDirectory(target.parent_path())) {
        // E.g. a PDF on a read-only share or in a temporary folder: keep asking for a location on first save
        return;
    }
    doc->lock();
    doc->setFilepath(target);
    doc->unlock();
    control->save();
}

AnnotationExporter::AnnotationExporter(Control* control): control(control) {
    control->setBeforeCloseHook([this](std::function<void()> proceed) { exportIfOutdated(std::move(proceed)); });
    control->setUserSavedHook([this](bool changed) {
        // Saving rewrites the document even without changes; a full export would then only cost time
        if (!changed && hasExport()) {
            return;
        }
        exportIfOutdated([] {});
    });
}

auto AnnotationExporter::hasExport() const -> bool {
    Document* doc = control->getDocument();
    doc->lock_shared();
    const auto target = AnnotationFiles::exportPath(*doc, *control->getSettings());
    doc->unlock_shared();
    std::error_code error;
    return !target.empty() && fs::exists(target, error);
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
    // Start the export once the main loop is idle: when called from a save callback, the save job still blocks the UI
    // and its unblock is already queued. Creating the export job (which blocks the UI itself) now would make that
    // unblock re-enable editing while the export reads the document.
    struct Pending {
        Control* control;
        fs::path target;
        std::function<void()> done;
    };
    g_idle_add(+[](gpointer data) -> gboolean {
        std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
        auto* job = new AutoPdfExportJob(pending->control, pending->target, std::move(pending->done));
        pending->control->getScheduler()->addJob(job, JOB_PRIORITY_NONE);
        job->unref();
        return G_SOURCE_REMOVE;
    }, new Pending{control, target, std::move(done)});
}
