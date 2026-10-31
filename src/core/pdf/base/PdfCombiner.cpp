#include "PdfCombiner.h"

#include <cctype>     // for isdigit, isspace
#include <fstream>    // for ifstream
#include <iterator>   // for istreambuf_iterator
#include <set>        // for set
#include <stdexcept>  // for runtime_error

#include "util/PathUtil.h"  // for toUtf8

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

namespace fs = std::filesystem;

namespace {
/// Key in the document information dictionary of combined PDFs
constexpr const char* COMBINED_KEY = "/XournalStudioCombined";

auto readFile(const fs::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + Util::toUtf8(path));
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Parses a page number (1-based) at text[pos], advancing pos
auto parseNumber(const std::string& text, size_t& pos) -> std::optional<size_t> {
    size_t value = 0;
    const size_t start = pos;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        value = value * 10 + static_cast<size_t>(text[pos] - '0');
        if (value > 1'000'000) {
            return std::nullopt;
        }
        pos++;
    }
    return pos == start ? std::nullopt : std::optional(value);
}

void skipSpaces(const std::string& text, size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        pos++;
    }
}
}  // namespace

auto PdfCombiner::parsePageRanges(const std::string& text, size_t pageCount) -> std::optional<std::vector<size_t>> {
    std::vector<size_t> pages;
    size_t pos = 0;
    skipSpaces(text, pos);
    if (pos == text.size()) {
        for (size_t i = 0; i < pageCount; i++) {
            pages.push_back(i);
        }
        return pageCount ? std::optional(pages) : std::nullopt;
    }
    while (pos < text.size()) {
        skipSpaces(text, pos);
        const auto first = parseNumber(text, pos);
        if (!first || *first < 1 || *first > pageCount) {
            return std::nullopt;
        }
        size_t last = *first;
        skipSpaces(text, pos);
        if (pos < text.size() && (text[pos] == '-' || text.compare(pos, 3, "\xE2\x80\x93") == 0)) {  // - or en dash
            pos += text[pos] == '-' ? 1 : 3;
            skipSpaces(text, pos);
            if (pos == text.size() || text[pos] == ',' || text[pos] == ';') {
                last = pageCount;  // "2-": to the end
            } else {
                const auto to = parseNumber(text, pos);
                if (!to || *to < *first || *to > pageCount) {
                    return std::nullopt;
                }
                last = *to;
            }
        }
        for (size_t page = *first; page <= last; page++) {
            pages.push_back(page - 1);
        }
        skipSpaces(text, pos);
        if (pos < text.size()) {
            if (text[pos] != ',' && text[pos] != ';') {
                return std::nullopt;
            }
            pos++;
            skipSpaces(text, pos);
            if (pos == text.size()) {
                return std::nullopt;  // trailing separator
            }
        }
    }
    return pages;
}

#ifdef ENABLE_QPDF

bool PdfCombiner::isAvailable() { return true; }

auto PdfCombiner::pageCount(const fs::path& pdf) -> size_t {
    const auto data = readFile(pdf);
    try {
        QPDF qpdf;
        qpdf.processMemoryFile(Util::toUtf8(pdf).c_str(), data.data(), data.size());
        return QPDFPageDocumentHelper(qpdf).getAllPages().size();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
}

auto PdfCombiner::isCombined(const fs::path& pdf) -> bool {
    try {
        const auto data = readFile(pdf);
        QPDF qpdf;
        qpdf.processMemoryFile(Util::toUtf8(pdf).c_str(), data.data(), data.size());
        auto info = qpdf.getTrailer().getKey("/Info");
        return info.isDictionary() && info.hasKey(COMBINED_KEY);
    } catch (const std::exception&) {
        return false;
    }
}

auto PdfCombiner::append(const fs::path& base, const fs::path& other, const std::vector<size_t>& pages) -> std::string {
    // QPDF reads from memory: no questions about file name encodings, and the buffers outlive the QPDF objects
    const std::string baseData = base.empty() ? std::string() : readFile(base);
    const std::string otherData = readFile(other);
    try {
        QPDF combined;
        if (base.empty()) {
            combined.emptyPDF();
        } else {
            combined.processMemoryFile(Util::toUtf8(base).c_str(), baseData.data(), baseData.size());
        }
        QPDF source;
        source.processMemoryFile(Util::toUtf8(other).c_str(), otherData.data(), otherData.size());

        QPDFPageDocumentHelper target(combined);
        auto sourcePages = QPDFPageDocumentHelper(source).getAllPages();
        std::set<size_t> added;
        for (size_t index: pages) {
            if (index >= sourcePages.size()) {
                throw std::runtime_error("page " + std::to_string(index + 1) + " does not exist");
            }
            // The same page twice needs its own page object
            auto page = added.insert(index).second ? sourcePages[index] : sourcePages[index].shallowCopyPage();
            target.addPage(page, false);
        }

        auto trailer = combined.getTrailer();
        auto info = trailer.getKey("/Info");
        if (!info.isDictionary()) {
            info = combined.makeIndirectObject(QPDFObjectHandle::newDictionary());
            trailer.replaceKey("/Info", info);
        }
        info.replaceKey(COMBINED_KEY, QPDFObjectHandle::newBool(true));

        QPDFWriter writer(combined);
        writer.setOutputMemory();
        writer.write();
        auto buffer = writer.getBufferSharedPointer();
        return {reinterpret_cast<const char*>(buffer->getBuffer()), buffer->getSize()};
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
}

#else

bool PdfCombiner::isAvailable() { return false; }
auto PdfCombiner::pageCount(const fs::path&) -> size_t { throw std::runtime_error("built without QPDF"); }
auto PdfCombiner::isCombined(const fs::path&) -> bool { return false; }
auto PdfCombiner::append(const fs::path&, const fs::path&, const std::vector<size_t>&) -> std::string {
    throw std::runtime_error("built without QPDF");
}

#endif
