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
#include "pdf/base/PdfCombiner.h"
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

/// A copy of the document that the PDF export can read on another thread while the original is edited or closed
struct ExportSnapshot {
    DocumentHandler handler;  // declared first: the document refers to it until it is destroyed
    std::unique_ptr<Document> doc = std::make_unique<Document>(&handler);
    fs::path target;
    /// When the exported .xopp file was saved, to recognize a repeated request for the same version
    fs::file_time_type saved;
    std::string error;
};

namespace {

auto isXopp(const fs::path& path) -> bool {
    return path.extension() == ".xopp" && !AnnotationFiles::isRecoveryCopy(path);
}

auto samePath(const fs::path& a, const fs::path& b) -> bool {
    std::error_code error;
    return fs::equivalent(a, b, error) || a.lexically_normal() == b.lexically_normal();
}

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
    auto key = Util::toUtf8(xopp.lexically_normal());
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
    gchar* foldedA = g_utf8_casefold(Util::toUtf8(a.filename()).c_str(), -1);
    gchar* foldedB = g_utf8_casefold(Util::toUtf8(b.filename()).c_str(), -1);
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
    const auto name = Util::toUtf8(path.filename());
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
    const auto path = Util::fromUtf8(name);
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
    const auto path = Util::fromUtf8(name);
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
                                     xopp.parent_path() / Util::fromUtf8(recorded);
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
        if (!safeTarget(target.parent_path() / Util::fromUtf8(recorded), target, doc.getPdfFilepath()).empty()) {
            return recorded;
        }
    }
    const auto path = safeTarget(exportPathFromSetting(doc, target, settings), target, doc.getPdfFilepath());
    return path.empty() ? std::string() : Util::toUtf8(path.filename());
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
            exportFilename ? xopp.parent_path() / Util::fromUtf8(*exportFilename) : exportPathForFile(xopp, settings);
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

auto AnnotationFiles::groupForRename(const fs::path& oldXopp, const fs::path& newXopp, const Settings& settings)
        -> DocumentGroup {
    DocumentGroup group;
    group.newXopp = newXopp;
    group.moves.emplace_back(oldXopp, newXopp);
    const auto dir = oldXopp.parent_path();
    const auto oldStem = Util::toUtf8(oldXopp.stem());
    const auto newStem = Util::toUtf8(newXopp.stem());
    std::error_code error;
    if (const auto source = recordedSourcePdf(oldXopp);
        !source.empty() && fs::exists(source, error) && ownsSourcePdf(oldXopp, source)) {
        // "VO8.pdf" becomes "VO9.pdf", the companion ".VO8.pages.pdf" becomes ".VO9.pages.pdf"
        const auto sourceName = Util::toUtf8(source.filename());
        const size_t prefix = sourceName.front() == '.' ? 1 : 0;
        group.newSource = dir / Util::fromUtf8(sourceName.substr(0, prefix) + newStem +
                                               sourceName.substr(prefix + oldStem.size()));
        group.moves.emplace_back(source, *group.newSource);
    }
    const auto exported = exportPathForFile(oldXopp, settings);
    const auto exportName = Util::toUtf8(exported.filename());
    if (!exported.empty() && exportName.rfind(oldStem, 0) == 0) {
        group.newExportName = newStem + exportName.substr(oldStem.size());
        if (fs::exists(exported, error)) {
            group.moves.emplace_back(exported, dir / Util::fromUtf8(*group.newExportName));
        }
    }
    return group;
}

auto AnnotationFiles::groupForMove(const fs::path& xopp, const fs::path& dir, const Settings& settings)
        -> DocumentGroup {
    DocumentGroup group;
    group.newXopp = dir / xopp.filename();
    group.moves.emplace_back(xopp, group.newXopp);
    std::error_code error;
    if (const auto source = recordedSourcePdf(xopp); !source.empty() && fs::exists(source, error)) {
        if (ownsSourcePdf(xopp, source)) {
            group.newSource = dir / source.filename();
            group.moves.emplace_back(source, *group.newSource);
        } else {
            group.newSource = source;  // stays, but its path relative to the document changes
        }
    }
    if (const auto exported = exportPathForFile(xopp, settings); !exported.empty() && fs::exists(exported, error)) {
        group.moves.emplace_back(exported, dir / exported.filename());
    }
    return group;
}

namespace {
constexpr std::string_view COMPANION_SUFFIX = ".pages";
}

auto AnnotationFiles::companionPdf(const fs::path& xopp) -> fs::path {
    return xopp.parent_path() /
           Util::fromUtf8("." + Util::toUtf8(xopp.stem()) + std::string(COMPANION_SUFFIX) + ".pdf");
}

auto AnnotationFiles::isCompanionPdf(const fs::path& path) -> bool {
    const auto name = Util::toUtf8(path.filename());
    return name.size() > 1 && name.front() == '.' && Util::hasPdfFileExt(path) &&
           name.find(std::string(COMPANION_SUFFIX)) != std::string::npos;
}

auto AnnotationFiles::ownsSourcePdf(const fs::path& xopp, const fs::path& pdf) -> bool {
    if (!samePath(pdf.parent_path(), xopp.parent_path())) {
        return false;
    }
    const auto stem = Util::toUtf8(xopp.stem());
    const auto pdfStem = Util::toUtf8(pdf.stem());
    if (pdfStem == stem) {
        return true;
    }
    // The companion ".VO8.pages.pdf", or "VO8 (combined).pdf" of an earlier version: the document's own copies
    const bool companion = pdfStem.rfind("." + stem + std::string(COMPANION_SUFFIX), 0) == 0;
    return (companion || pdfStem.rfind(stem, 0) == 0) && PdfCombiner::isCombined(pdf);
}

auto AnnotationFiles::groupFiles(const fs::path& xopp, const Settings& settings) -> std::vector<fs::path> {
    std::vector<fs::path> files{xopp};
    std::error_code error;
    if (const auto source = recordedSourcePdf(xopp);
        !source.empty() && fs::exists(source, error) && ownsSourcePdf(xopp, source)) {
        files.push_back(source);
    }
    if (const auto exported = exportPathForFile(xopp, settings); !exported.empty() && fs::exists(exported, error)) {
        files.push_back(exported);
    }
    return files;
}

auto AnnotationFiles::groupConflict(const DocumentGroup& group) -> std::optional<std::string> {
    std::vector<fs::path> targets;
    std::error_code error;
    for (const auto& [from, to]: group.moves) {
        for (const auto& other: targets) {
            if (samePath(other, to) || Util::toUtf8(other.filename()) == Util::toUtf8(to.filename())) {
                return Util::toUtf8(to.filename());
            }
        }
        targets.push_back(to);
        if (!samePath(from, to) && fs::exists(to, error)) {
            return Util::toUtf8(to.filename());
        }
    }
    return std::nullopt;
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
    control->setBeforeCloseHook([this](std::function<void()> proceed, bool waitForExport) {
        if (waitForExport) {
            exportIfOutdated(std::move(proceed));
        } else {
            // Switching documents: the export works on a copy and finishes in the background
            exportIfOutdated({});
            proceed();
        }
    });
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
    queueIfOutdated();
    if (done) {
        waiting.push_back(std::move(done));
    }
    startNext();
}

void AnnotationExporter::queueIfOutdated() {
    Document* doc = control->getDocument();
    auto* undoRedo = control->getUndoRedoHandler();
    if (!doc || !undoRedo || undoRedo->isChanged()) {
        // Unsaved changes: the file on disk is not what would be exported
        return;
    }
    doc->lock_shared();
    const auto xopp = doc->getFilepath();
    const auto target = AnnotationFiles::exportPath(*doc, *control->getSettings());
    doc->unlock_shared();
    if (target.empty()) {
        return;
    }
    std::error_code error;
    if (!AnnotationFiles::hasAnnotations(*doc) && !fs::exists(target, error)) {
        // Nothing to export yet, e.g. right after editing was enabled; an existing export is still updated
        return;
    }
    const auto saved = fs::last_write_time(xopp, error);
    if (error) {
        return;
    }
    if (const auto exported = fs::last_write_time(target, error); !error && exported >= saved) {
        return;
    }
    // This version is already being exported (the PDF is only written once that is done)
    if (running && runningTarget == target && runningSaved == saved) {
        return;
    }
    // An older version waiting for its turn is replaced: only the newest one needs to be exported
    std::erase_if(queued, [&](const auto& s) { return s->target == target; });

    doc->lock_shared();
    auto snapshot = createSnapshot(*doc, target);
    doc->unlock_shared();
    snapshot->saved = saved;
    queued.push_back(std::move(snapshot));
}

void AnnotationExporter::startNext() {
    if (running) {
        return;
    }
    if (queued.empty()) {
        for (auto& done: std::exchange(waiting, {})) {
            done();
        }
        return;
    }
    // Fork: export a copy of the document on its own thread, so the UI is not blocked and the document can be edited
    // or closed meanwhile. Exports run one after another, so two of them never write the same PDF.
    auto snapshot = std::move(queued.front());
    queued.pop_front();
    running = true;
    runningTarget = snapshot->target;
    runningSaved = snapshot->saved;

    struct Finished {
        std::weak_ptr<void> alive;
        AnnotationExporter* self;
        std::unique_ptr<ExportSnapshot> snapshot;
    };
    auto* finished = new Finished{lifetime, this, std::move(snapshot)};
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
                        f->self->exportFinished(f->snapshot->error);
                    }
                    return G_SOURCE_REMOVE;
                },
                finished);
    }).detach();
}

void AnnotationExporter::exportFinished(const std::string& error) {
    running = false;
    runningTarget.clear();
    if (!error.empty()) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), FS(_F("Error exporting PDF: {1}") % error));
    }
    startNext();
}
