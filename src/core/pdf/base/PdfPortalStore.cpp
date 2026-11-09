#include "PdfPortalStore.h"

#include <fstream>    // for ifstream, ofstream
#include <iterator>   // for istreambuf_iterator
#include <mutex>      // for mutex, lock_guard
#include <optional>   // for optional
#include <set>        // for set
#include <stdexcept>  // for runtime_error
#include <utility>    // for move, pair
#include <vector>     // for vector

#include <glib.h>  // for g_warning

#include "model/PdfPortal.h"  // for PdfPortalPage
#include "util/PathUtil.h"    // for toUtf8, setHiddenFile

#include "config-features.h"  // for ENABLE_QPDF

#ifdef ENABLE_QPDF
#include <qpdf/DLL.h>
#if QPDF_MAJOR_VERSION == 11
#define POINTERHOLDER_TRANSITION 4  // Only used for QPDF 11
#endif
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#endif

namespace {

auto readFile(const fs::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + Util::toUtf8(path));
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Identifies a version of a file, to know whether the keys read from it are still current
struct FileStamp {
    uintmax_t size{};
    fs::file_time_type time{};
    bool operator==(const FileStamp&) const = default;
};

auto stampOf(const fs::path& path) -> std::optional<FileStamp> {
    std::error_code error;
    FileStamp stamp{fs::file_size(path, error), {}};
    if (error) {
        return std::nullopt;
    }
    stamp.time = fs::last_write_time(path, error);
    return error ? std::nullopt : std::optional(stamp);
}

/// The keys of the portal files read or written last, so saving does not read the file again to see it is current
std::mutex cacheMutex;
std::map<fs::path, std::pair<FileStamp, std::set<std::string>>> keyCache;

void remember(const fs::path& file, std::set<std::string> keys) {
    std::lock_guard lock(cacheMutex);
    if (auto stamp = stampOf(file)) {
        keyCache[file] = {*stamp, std::move(keys)};
    } else {
        keyCache.erase(file);
    }
}

auto rememberedKeys(const fs::path& file) -> std::optional<std::set<std::string>> {
    std::lock_guard lock(cacheMutex);
    auto it = keyCache.find(file);
    if (it == keyCache.end() || stampOf(file) != it->second.first) {
        return std::nullopt;
    }
    return it->second.second;
}

}  // namespace

#ifdef ENABLE_QPDF

namespace {
/// Key of a page in its page dictionary
constexpr const char* KEY = "/XournalStudioPortalKey";

auto toString(QPDF& pdf) -> std::string {
    QPDFWriter writer(pdf);
    writer.setDeterministicID(true);  // the same pages give the same data
    writer.setOutputMemory();
    writer.write();
    auto buffer = writer.getBufferSharedPointer();
    return {reinterpret_cast<const char*>(buffer->getBuffer()), buffer->getSize()};
}

/// A PDF with only this page, without what refers to other pages
auto singlePagePdf(const QPDFPageObjectHelper& page) -> std::string {
    QPDF out;
    out.emptyPDF();
    QPDFPageDocumentHelper(out).addPage(page, false);
    // Copying the page copies what it refers to; what is removed here is not written
    auto copied = QPDFPageDocumentHelper(out).getAllPages().front().getObjectHandle();
    copied.removeKey("/B");
    copied.removeKey("/StructParents");
    if (auto annots = copied.getKey("/Annots"); annots.isArray()) {
        for (auto annot: annots.getArrayAsVector()) {
            if (annot.isDictionary()) {
                for (const char* key: {"/A", "/Dest", "/P", "/Parent", "/Popup"}) {
                    annot.removeKey(key);
                }
            }
        }
    }
    return toString(out);
}

auto keyOf(QPDFPageObjectHelper& page) -> std::optional<std::string> {
    auto key = page.getObjectHandle().getKey(KEY);
    return key.isString() ? std::optional(key.getStringValue()) : std::nullopt;
}

template <typename F>
auto convertErrors(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
}
}  // namespace

bool PdfPortalStore::isAvailable() { return true; }

auto PdfPortalStore::extractPage(const fs::path& pdf, size_t pageIndex) -> std::shared_ptr<PdfPortalPage> {
    try {
        return extractPage(readFile(pdf), pageIndex);
    } catch (const std::runtime_error& e) {
        g_warning("Cannot copy page %zu of %s: %s", pageIndex + 1, Util::toUtf8(pdf).c_str(), e.what());
        return nullptr;
    }
}

auto PdfPortalStore::extractPage(const std::string& data, size_t pageIndex) -> std::shared_ptr<PdfPortalPage> {
    try {
        auto page = convertErrors([&] {
            QPDF source;
            source.processMemoryFile("PDF", data.data(), data.size());
            auto pages = QPDFPageDocumentHelper(source).getAllPages();
            if (pageIndex >= pages.size()) {
                throw std::runtime_error("no such page");
            }
            return singlePagePdf(pages[pageIndex]);
        });
        return PdfPortalPage::fromData(std::move(page));
    } catch (const std::runtime_error& e) {
        g_warning("Cannot copy page %zu of a PDF: %s", pageIndex + 1, e.what());
        return nullptr;
    }
}

auto PdfPortalStore::readPages(const fs::path& file) -> std::map<std::string, std::string> {
    const std::string data = readFile(file);
    return convertErrors([&] {
        QPDF store;
        store.processMemoryFile(Util::toUtf8(file).c_str(), data.data(), data.size());
        std::map<std::string, std::string> pages;
        std::set<std::string> keys;
        for (auto& page: QPDFPageDocumentHelper(store).getAllPages()) {
            if (auto key = keyOf(page); key && keys.insert(*key).second) {
                pages.emplace(*key, singlePagePdf(page));
            }
        }
        remember(file, std::move(keys));
        return pages;
    });
}

void PdfPortalStore::write(const fs::path& file,
                           const std::map<std::string, std::shared_ptr<const PdfPortalPage>>& pages, bool keepIfEmpty) {
    std::error_code error;
    if (pages.empty()) {
        if (!keepIfEmpty && fs::exists(file, error)) {
            fs::remove(file, error);
            if (error) {
                throw std::runtime_error("cannot remove " + Util::toUtf8(file) + ": " + error.message());
            }
            remember(file, {});
        }
        return;
    }

    std::set<std::string> keys;
    for (const auto& entry: pages) {
        keys.insert(entry.first);
    }
    auto current = rememberedKeys(file);
    if (!current && fs::exists(file, error)) {
        try {
            readPages(file);  // remembers its keys
            current = rememberedKeys(file);
        } catch (const std::runtime_error&) {
            // Unreadable: it is replaced
        }
    }
    if (current == keys) {
        return;  // unchanged: the file stays as it is, also for git
    }

    const std::string data = convertErrors([&] {
        QPDF out;
        out.emptyPDF();
        std::vector<std::unique_ptr<QPDF>> sources;  // must live until the output is written
        for (const auto& [key, page]: pages) {
            auto& source = sources.emplace_back(std::make_unique<QPDF>());
            source->processMemoryFile(key.c_str(), page->getData().data(), page->getData().size());
            auto sourcePage = QPDFPageDocumentHelper(*source).getAllPages().front();
            sourcePage.getObjectHandle().replaceKey(KEY, QPDFObjectHandle::newString(key));
            QPDFPageDocumentHelper(out).addPage(sourcePage, false);
        }
        return toString(out);
    });

    // Like the document: replace the file only once the new one is complete
    auto part = fs::path(file) += "~";
    {
        std::ofstream stream(part, std::ios::binary | std::ios::trunc);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        stream.close();
        if (stream.fail()) {
            fs::remove(part, error);
            throw std::runtime_error("cannot write " + Util::toUtf8(part));
        }
    }
    Util::setHiddenFile(file, false);  // Windows does not replace hidden files (e.g. hidden by the user)
    fs::rename(part, file, error);
    if (error) {
        const std::string message = error.message();
        fs::remove(part, error);
        throw std::runtime_error("cannot replace " + Util::toUtf8(file) + ": " + message);
    }
    remember(file, std::move(keys));
}

#else

bool PdfPortalStore::isAvailable() { return false; }
auto PdfPortalStore::extractPage(const fs::path&, size_t) -> std::shared_ptr<PdfPortalPage> { return nullptr; }
auto PdfPortalStore::extractPage(const std::string&, size_t) -> std::shared_ptr<PdfPortalPage> { return nullptr; }
auto PdfPortalStore::readPages(const fs::path&) -> std::map<std::string, std::string> {
    throw std::runtime_error("built without QPDF");
}
void PdfPortalStore::write(const fs::path& file,
                           const std::map<std::string, std::shared_ptr<const PdfPortalPage>>& pages, bool keepIfEmpty) {
    std::error_code error;
    if (pages.empty() && !keepIfEmpty) {
        fs::remove(file, error);
    } else if (!pages.empty()) {
        throw std::runtime_error("built without QPDF");
    }
}

#endif
