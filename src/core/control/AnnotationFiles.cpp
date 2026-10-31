#include "AnnotationFiles.h"

#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <glib.h>
#include <glib/gstdio.h>

#include "control/Control.h"
#include "control/jobs/ProgressListener.h"
#include "control/settings/Settings.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Image.h"
#include "model/Layer.h"
#include "model/XojPage.h"
#include "pdf/base/XojPdfExport.h"
#include "pdf/base/XojPdfExportFactory.h"
#include "undo/UndoRedoHandler.h"
#include "util/GzInputStream.h"
#include "util/PathUtil.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"

#ifdef _WIN32
#include <windows.h>  // last: its macros clash with names in the headers above
#endif

namespace fs = std::filesystem;

namespace {

auto isXopp(const fs::path& path) -> bool {
    return path.extension() == ".xopp" && !AnnotationFiles::isRecoveryCopy(path);
}

auto samePath(const fs::path& a, const fs::path& b) -> bool {
    std::error_code error;
    return fs::equivalent(a, b, error) || a.lexically_normal() == b.lexically_normal();
}

/// A copy of the document that the PDF export can read on another thread while the original is edited
struct ExportSnapshot {
    DocumentHandler handler;  // declared first: the document refers to it until it is destroyed
    std::unique_ptr<Document> doc = std::make_unique<Document>(&handler);
    fs::path target;
    std::string error;
};

/// Copies pages and elements of doc (which the caller has locked) so that nothing is shared with the original
auto createSnapshot(Document& doc, fs::path target) -> std::unique_ptr<ExportSnapshot> {
    auto snapshot = std::make_unique<ExportSnapshot>();
    snapshot->target = std::move(target);
    snapshot->doc->setFilepath(doc.getFilepath());
    snapshot->doc->setPdfAttributes(doc.getPdfFilepath(), doc.isAttachPdf());
    std::vector<PageRef> pages;
    pages.reserve(doc.getPageCount());
    for (size_t i = 0; i < doc.getPageCount(); i++) {
        const auto& original = doc.getPage(i);
        PageRef copy(original->clone());
        const auto& originalLayers = original->getLayers();
        const auto& layers = copy->getLayers();
        for (size_t l = 0; l < layers.size() && l < originalLayers.size(); l++) {
            layers[l]->setVisible(originalLayers[l]->isVisible());  // Layer::clone() does not keep it
            for (const auto& element: layers[l]->getElements()) {
                if (element->getType() == ELEMENT_IMAGE) {
                    // A clone shares the decoded surface with the original, which the UI keeps drawing
                    auto* image = static_cast<Image*>(element.get());
                    image->setImage(std::string(image->getBinaryData()));
                }
            }
        }
        pages.push_back(std::move(copy));
    }
    snapshot->doc->addPages(pages.begin(), pages.end());
    return snapshot;
}

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
    auto target = xopp.parent_path() / doc.createSaveFilename(Document::PDF, settings.getDefaultSaveName(),
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
 * The first element starting with tagStart (up to its '>') from the beginning of a .xopp file, without loading the
 * document; empty if it is not within the first maxBytes (decompressed).
 */
auto readXoppTag(const fs::path& xopp, std::string_view tagStart, size_t maxBytes) -> std::string {
    std::string header;
    size_t tagPos = std::string::npos;
    try {
        xoj::util::GzInputStream input(xopp);
        char buffer[4096];
        int read = 0;
        while (header.size() < maxBytes && (read = input.read(buffer, sizeof(buffer))) > 0) {
            // Only search the new part (and the end of the previous one, which may hold the start of the tag)
            const size_t searchFrom = header.size() < tagStart.size() ? 0 : header.size() - tagStart.size();
            header.append(buffer, static_cast<size_t>(read));
            if (tagPos == std::string::npos) {
                tagPos = header.find(tagStart, searchFrom);
            }
            if (tagPos != std::string::npos && header.find('>', tagPos) != std::string::npos) {
                break;
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    const auto tagEnd = tagPos == std::string::npos ? std::string::npos : header.find('>', tagPos);
    if (tagEnd == std::string::npos) {
        return {};
    }
    return header.substr(tagPos, tagEnd - tagPos);
}

/**
 * readXoppTag(), remembered per file as long as it is not modified: the project explorer asks for every document of a
 * folder whenever it shows it, and reading a document without PDF background decompresses up to maxBytes.
 */
auto cachedXoppTag(const fs::path& xopp, std::string_view tagStart, size_t maxBytes) -> std::string {
    const auto fileStamp = AnnotationFiles::fileStamp(xopp);
    if (!fileStamp) {
        return {};
    }
    const auto stamp = std::make_pair(fileStamp->modified, fileStamp->size);
#ifdef _WIN32
    FILETIME nowTime;
    GetSystemTimeAsFileTime(&nowTime);
    const uint64_t now = (uint64_t(nowTime.dwHighDateTime) << 32) | nowTime.dwLowDateTime;
    constexpr uint64_t SECOND = 10'000'000;  // in 100 ns
#else
    const uint64_t now = uint64_t(g_get_real_time()) * 1000;
    constexpr uint64_t SECOND = 1'000'000'000;
#endif
    // The clock that stamps files ticks coarsely (about 16 ms on Windows): a file written just now may be written
    // again with the same stamp, so only remember files that have not changed for a while
    const bool settled = now > stamp.first + 2 * SECOND;
    struct Entry {
        std::pair<uint64_t, uint64_t> stamp;
        std::string tag;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Entry> cache;
    auto key = utf8(xopp.lexically_normal());
    key += '|';
    key += tagStart;
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end() && it->second.stamp == stamp) {
            return it->second.tag;
        }
    }
    auto tag = readXoppTag(xopp, tagStart, maxBytes);
    if (!settled) {
        return tag;
    }
    std::lock_guard lock(mutex);
    if (cache.size() > 4096) {
        cache.clear();  // e.g. after browsing many folders; the entries are cheap to rebuild
    }
    cache[std::move(key)] = Entry{stamp, tag};
    return tag;
}

/**
 * Reads an attribute of the first element starting with tagStart from the beginning of a .xopp file, without loading
 * the document. Only the first maxBytes (decompressed) are searched.
 */
auto readXoppAttribute(const fs::path& xopp, std::string_view tagStart, std::string_view attribute, size_t maxBytes)
        -> std::string {
    const auto tag = cachedXoppTag(xopp, tagStart, maxBytes);
    if (tag.empty()) {
        return {};
    }
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

auto AnnotationFiles::fileStamp(const fs::path& path) -> std::optional<FileStamp> {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return std::nullopt;
    }
    return FileStamp{(uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
                     (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
#else
    GStatBuf info;
    if (g_stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
    }
    return FileStamp{uint64_t(info.st_mtim.tv_sec) * 1'000'000'000 + uint64_t(info.st_mtim.tv_nsec),
                     uint64_t(info.st_size)};
#endif
}

auto AnnotationFiles::isRecoveryCopy(const fs::path& path) -> bool {
    const auto name = utf8(path.filename());
    constexpr std::string_view autosaveSuffix = ".autosave.xopp";
    return (name.size() > autosaveSuffix.size() && name.front() == '.' &&
            name.compare(name.size() - autosaveSuffix.size(), autosaveSuffix.size(), autosaveSuffix) == 0) ||
           (!name.empty() && name.back() == '~');
}

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
    auto target =
            recorded.empty() ? exportPathFromSetting(doc, xopp, settings) : xopp.parent_path() / pathFromUtf8(recorded);
    return safeTarget(std::move(target), xopp, doc.getPdfFilepath());
}

auto AnnotationFiles::isExportUpToDate(const fs::path& xopp, const fs::path& pdf) -> bool {
    if (xopp.empty() || pdf.empty()) {
        return false;
    }
    std::error_code error;
    const auto saved = fs::last_write_time(xopp, error);
    if (error) {
        return false;
    }
    const auto exported = fs::last_write_time(pdf, error);
    return !error && exported >= saved;
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

auto AnnotationFiles::relinkSourcePdf(const fs::path& xopp, const fs::path& pdf, const Settings& settings)
        -> std::string {
    return relinkDocument(xopp, pdf, std::nullopt, settings);
}

auto AnnotationFiles::relinkDocument(const fs::path& xopp, const std::optional<fs::path>& pdf,
                                     const std::optional<std::string>& exportFilename, const Settings& settings)
        -> std::string {
    // Loading does not need the PDF: a missing one is only recorded, which is replaced here anyway
    std::unique_ptr<Document> doc;
    try {
        doc = LoadHandler().loadDocument(xopp);
    } catch (const std::exception& e) {
        return e.what();
    }
    // Without a path it was not loaded completely (or is an old Xournal file): saving it would lose content
    if (!doc || doc->getFilepath().empty()) {
        return "The document could not be loaded completely.";
    }
    const auto exported =
            exportFilename ? xopp.parent_path() / pathFromUtf8(*exportFilename) : exportPathForFile(xopp, settings);
    const bool exportUpToDate = isExportUpToDate(xopp, exported);
    if (pdf) {
        doc->setPdfAttributes(*pdf, doc->isAttachPdf());
    }
    SaveHandler saver;
    saver.prepareSave(doc.get(), xopp);
    saver.setExportFilename(exportFilename ? *exportFilename : recordedExportFilename(xopp));
    saver.saveTo(xopp);
    if (saver.getErrorMessage().empty() && exportUpToDate) {
        std::error_code error;  // the annotations did not change
        fs::last_write_time(exported, fs::file_time_type::clock::now(), error);
    }
    return saver.getErrorMessage();
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

auto AnnotationFiles::hasAnnotations(Document& doc) -> bool {
    doc.lock_shared();
    bool annotated = doc.getPageCount() != doc.getPdfPageCount();
    for (size_t i = 0; !annotated && i < doc.getPageCount(); i++) {
        const auto page = doc.getPage(i);
        // Reordered or duplicated pages change the exported PDF as well
        annotated = !page->getBackgroundType().isPdfPage() || page->getPdfPageNr() != i || page->isAnnotated();
    }
    doc.unlock_shared();
    return annotated;
}

void AnnotationFiles::createDocumentFileForPdf(Control* control) {
    Document* doc = control->getDocument();
    doc->lock_shared();
    const auto pdf = doc->getPdfFilepath();
    const bool hasFile = !doc->getFilepath().empty();
    auto target =
            pdf.parent_path() / doc->createSaveFilename(Document::XOPP, control->getSettings()->getDefaultSaveName());
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
    if (running) {
        waiting.emplace_back([this, done = std::move(done)]() mutable { exportIfOutdated(std::move(done)); });
        return;
    }
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
    if (!AnnotationFiles::hasAnnotations(*doc) && !fs::exists(target, error)) {
        // Nothing to export yet, e.g. right after editing was enabled; an existing export is still updated
        done();
        return;
    }
    const auto saved = fs::last_write_time(xopp, error);
    if (error) {
        done();
        return;
    }
    if (const auto exported = fs::last_write_time(target, error); !error && exported >= saved) {
        done();
        return;
    }
    // Fork: export a copy of the document on its own thread, so the UI is not blocked and editing can continue.
    // A second request waits for the running export and then checks again whether the PDF is outdated.
    running = true;
    doc->lock_shared();
    auto snapshot = createSnapshot(*doc, target);
    doc->unlock_shared();

    struct Finished {
        std::weak_ptr<void> alive;
        AnnotationExporter* self;
        std::unique_ptr<ExportSnapshot> snapshot;
        std::function<void()> done;
    };
    auto* finished = new Finished{lifetime, this, std::move(snapshot), std::move(done)};
    std::thread([finished] {
        DummyProgressListener listener;
        auto exporter = XojPdfExportFactory::createExport(finished->snapshot->doc.get(), &listener);
        if (!exporter->createPdf(finished->snapshot->target, false)) {
            finished->snapshot->error = exporter->getLastError();
        }
        g_idle_add(
                +[](gpointer data) -> gboolean {
                    std::unique_ptr<Finished> f(static_cast<Finished*>(data));
                    if (!f->alive.expired()) {
                        f->self->exportFinished(f->snapshot->error, std::move(f->done));
                    }
                    return G_SOURCE_REMOVE;
                },
                finished);
    }).detach();
}

void AnnotationExporter::exportFinished(const std::string& error, std::function<void()> done) {
    running = false;
    if (!error.empty()) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), FS(_F("Error exporting PDF: {1}") % error));
    }
    done();
    for (auto& next: std::exchange(waiting, {})) {
        next();
    }
}
