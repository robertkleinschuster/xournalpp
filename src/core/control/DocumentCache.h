/*
 * Xournal++
 *
 * Fork: keeps the documents opened last in memory
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>

#include "control/AnnotationFiles.h"  // for FileStamp
#include "model/DocumentHandler.h"

class Document;
struct KeptRendering;

/**
 * Keeps the documents opened last in memory, together with their rendered PDF pages, so that switching back to one
 * of them (e.g. between an exercise and the lecture notes) needs neither loading nor rendering the PDF again.
 *
 * A document is only reused while its files on disk are exactly as when it was loaded or last saved; a document
 * changed by git, another program or a file operation in the explorer is loaded again.
 */
class DocumentCache final {
public:
    struct Entry {
        std::unique_ptr<Document> doc;
        std::unique_ptr<KeptRendering> rendering;
    };

    DocumentCache();
    ~DocumentCache();

    DocumentCache(const DocumentCache&) = delete;
    DocumentCache& operator=(const DocumentCache&) = delete;

    /// The open document doc (locked by the caller) matches its files on disk: it was just loaded or saved.
    /// documentStamp is the stamp of its document file taken before loading (it may change while loading).
    void openedFromDisk(const Document& doc, std::optional<AnnotationFiles::FileStamp> documentStamp);
    /// Like openedFromDisk(), taking the stamp of the document file now (e.g. right after saving)
    void openedFromDisk(const Document& doc);

    /// Whether the open document doc can be kept: it matches the files it was loaded from or saved to
    bool canKeep(const Document& doc) const;

    /// Keeps a copy of the open document doc (locked by the caller) when another one is opened.
    /// The copy shares pages with doc, which must not be changed afterwards (they are, as doc is closed).
    void keep(const Document& doc, std::unique_ptr<KeptRendering> rendering);

    /// Takes the document kept for path (its document file, or the PDF of a document without one), if its files on
    /// disk are unchanged. The returned document must be destroyed before this cache.
    std::optional<Entry> take(const std::filesystem::path& path);

    /// Forgets all kept documents
    void clear();

    /// The document file of doc, or its PDF if it has none (like Document::getEvMetadataFilename)
    static std::filesystem::path keyOf(const Document& doc);

    /// How many documents are kept besides the open one
    static constexpr std::size_t MAX_ENTRIES = 3;

private:
    struct DiskState {
        std::filesystem::path key;
        std::filesystem::path documentFile;
        std::optional<AnnotationFiles::FileStamp> document;
        std::filesystem::path pdfFile;
        std::optional<AnnotationFiles::FileStamp> pdf;
        bool operator==(const DiskState&) const = default;
    };
    struct Kept {
        DiskState disk;
        std::unique_ptr<Document> doc;
        std::unique_ptr<KeptRendering> rendering;
    };

    static DiskState diskStateOf(const Document& doc, std::optional<AnnotationFiles::FileStamp> documentStamp);
    static bool samePath(const std::filesystem::path& a, const std::filesystem::path& b);

    /// The kept documents refer to it; declared first so that it outlives them
    DocumentHandler handler;
    /// The open document's files as it was loaded or saved (empty key if unknown)
    DiskState open;
    /// Most recently kept first
    std::deque<Kept> kept;
};
