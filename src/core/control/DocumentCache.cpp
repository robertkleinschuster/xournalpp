#include "DocumentCache.h"

#include <algorithm>
#include <utility>

#include "control/PdfCache.h"
#include "gui/XournalView.h"
#include "model/Document.h"

namespace fs = std::filesystem;

DocumentCache::DocumentCache() = default;

DocumentCache::~DocumentCache() { clear(); }

auto DocumentCache::keyOf(const Document& doc) -> fs::path { return doc.getEvMetadataFilename(); }

auto DocumentCache::samePath(const fs::path& a, const fs::path& b) -> bool {
    return a.lexically_normal() == b.lexically_normal();
}

auto DocumentCache::diskStateOf(const Document& doc, std::optional<AnnotationFiles::FileStamp> documentStamp)
        -> DiskState {
    DiskState state;
    state.key = keyOf(doc);
    state.documentFile = doc.getFilepath();
    state.document = std::move(documentStamp);
    state.pdfFile = doc.getPdfFilepath();
    if (!state.pdfFile.empty()) {
        state.pdf = AnnotationFiles::fileStamp(state.pdfFile);
    }
    return state;
}

void DocumentCache::openedFromDisk(const Document& doc, std::optional<AnnotationFiles::FileStamp> documentStamp) {
    open = diskStateOf(doc, std::move(documentStamp));
}

void DocumentCache::openedFromDisk(const Document& doc) {
    const auto file = doc.getFilepath();
    openedFromDisk(doc, file.empty() ? std::nullopt : AnnotationFiles::fileStamp(file));
}

auto DocumentCache::canKeep(const Document& doc) const -> bool {
    if (open.key.empty() || !samePath(open.key, keyOf(doc)) || doc.getPageCount() == 0) {
        return false;
    }
    // A file that could not be stamped (e.g. a document without a file) cannot be checked later
    return (open.documentFile.empty() || open.document) && (open.pdfFile.empty() || open.pdf);
}

void DocumentCache::keep(const Document& doc, std::unique_ptr<KeptRendering> rendering) {
    if (!canKeep(doc)) {
        return;
    }
    // Copying shares the pages and the loaded PDF; it does not render or load anything
    auto copy = std::make_unique<Document>(&handler);
    copy->lock();
    *copy = doc;
    copy->unlock();

    std::erase_if(kept, [&](const Kept& k) { return samePath(k.disk.key, open.key); });
    kept.push_front(Kept{open, std::move(copy), std::move(rendering)});
    while (kept.size() > MAX_ENTRIES) {
        kept.pop_back();
    }
    open = {};
}

auto DocumentCache::take(const fs::path& path) -> std::optional<Entry> {
    auto it = std::find_if(kept.begin(), kept.end(), [&](const Kept& k) { return samePath(k.disk.key, path); });
    if (it == kept.end()) {
        return std::nullopt;
    }
    Kept entry = std::move(*it);
    kept.erase(it);

    // Changed on disk since it was kept (git, another program, the explorer): load it again
    entry.doc->lock_shared();
    const auto now = diskStateOf(*entry.doc, entry.disk.documentFile.empty() ?
                                                     std::nullopt :
                                                     AnnotationFiles::fileStamp(entry.disk.documentFile));
    entry.doc->unlock_shared();
    if (!(now == entry.disk)) {
        return std::nullopt;
    }
    return Entry{std::move(entry.doc), std::move(entry.rendering)};
}

void DocumentCache::clear() {
    kept.clear();
    open = {};
}
