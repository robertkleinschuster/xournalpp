/*
 * Xournal++
 *
 * Fork: combines pages of several PDFs into one background PDF ("Insert Pages from PDF")
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>   // for size_t
#include <optional>  // for optional
#include <string>    // for string
#include <vector>    // for vector

#include "filesystem.h"  // for path

namespace PdfCombiner {

/// Whether this build can combine PDFs (needs QPDF)
bool isAvailable();

/**
 * Parses page ranges as typed by the user, e.g. "1-3, 5" or "2-" (to the end), into 0-based page indices in the
 * given order. An empty text means all pages. Returns nullopt if the text is invalid or a page does not exist.
 */
std::optional<std::vector<size_t>> parsePageRanges(const std::string& text, size_t pageCount);

/// The number of pages of a PDF; throws std::runtime_error if it cannot be read
size_t pageCount(const std::filesystem::path& pdf);

/// Whether the PDF was written by append(), i.e. it is a combined background PDF of the fork and not a source file
bool isCombined(const std::filesystem::path& pdf);

/**
 * The pages of base (none if base is empty) followed by the given pages (0-based) of other, as PDF data marked as
 * combined. The pages of base keep their numbers. Throws std::runtime_error if a file cannot be read.
 */
std::string append(const std::filesystem::path& base, const std::filesystem::path& other,
                   const std::vector<size_t>& pages);

}  // namespace PdfCombiner
