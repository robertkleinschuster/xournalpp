#include "MetadataManager.h"

#include <algorithm>  // for sort
#include <cmath>      // for isnan
#include <fstream>    // for operator<<, basic_ostream, basic_stringb...
#include <memory>     // for allocator_traits<>::value_type
#include <sstream>    // for istringstream
#include <string>     // for char_traits, string, getline, operator!=

#include <glib.h>  // for g_get_real_time, g_message, g_warning, gint64

#include "util/PathUtil.h"  // for getConfigSubfolder, getStateSubfolder
#include "util/StringUtils.h"
#include "util/XojMsgBox.h"  // for XojMsgBox
#include "util/serdesstream.h"
#include "util/utf8_view.h"

/**
 * Get directory to store metadata files to
 */
static fs::path getMetadataDirectory() { return Util::getStateSubfolder("metadata"); }

/**
 * Migrate metadata directory from legacy location
 */
static void migrateMetadataDirectory() {
    // do not pass "metadata" to getConfigSubfolder() to avoid creating directory if it doesn't exist
    auto legacyDir = Util::getConfigSubfolder() / "metadata";

    if (!fs::is_directory(legacyDir)) {
        // nothing to do
        return;
    }

    // move all files to the new directory
    auto newDir = getMetadataDirectory();

    if (fs::equivalent(legacyDir, newDir)) {  // Happens on Windows by default
        // nothing to do
        return;
    }

    for (auto const& e: fs::directory_iterator(legacyDir)) {
        auto newPath = newDir / e.path().filename();
        Util::safeRenameFile(e, newPath);
    }

    // remove legacy directory
    try {
        fs::remove(legacyDir);
    } catch (const fs::filesystem_error&) {
        g_warning("Could not delete legacy metadata directory %s", char_cast(legacyDir.u8string().c_str()));
    }

    g_message("Migrated metadata directory from %s to %s", char_cast(legacyDir.u8string().c_str()),
              char_cast(newDir.u8string().c_str()));
}

MetadataEntry::MetadataEntry(): zoom(1), page(0), time(0) {}

MetadataManager::~MetadataManager() { documentChanged(); }

/**
 * Delete an old metadata file
 */
void MetadataManager::deleteMetadataFile(fs::path const& path) {
    // be careful, delete the Metadata file, NOT the Document!
    if (path.extension() != ".metadata") {
        g_warning("Try to delete non-metadata file: %s", char_cast(path.u8string().c_str()));
        return;
    }

    try {
        fs::remove(path);
    } catch (const fs::filesystem_error&) {
        g_warning("Could not delete metadata file %s", char_cast(path.u8string().c_str()));
    }
}

/**
 * Document was closed, a new document was opened etc.
 */
void MetadataManager::documentChanged() {
    // take ownership of metadata
    this->mutex.lock();
    auto m = std::move(this->metadata);
    this->mutex.unlock();

    if (m == nullptr) {
        return;
    }

    storeMetadata(*m);
}

auto sortMetadata(MetadataEntry& a, MetadataEntry& b) -> bool { return a.time > b.time; }

/**
 * Load the metadata list (sorted)
 */
auto MetadataManager::loadList() -> std::vector<MetadataEntry> {
    migrateMetadataDirectory();
    auto folder = getMetadataDirectory();

    std::vector<MetadataEntry> data;
    try {
        for (auto const& f: fs::directory_iterator(folder)) {
            auto path = folder / f;

            auto entry = loadMetadataFile(path);

            if (entry) {
                data.emplace_back(std::move(*entry));
            } else {
                // Invalid metadata file
                deleteMetadataFile(path);
            }
        }
    } catch (const fs::filesystem_error& e) {
        XojMsgBox::showErrorToUser(nullptr, e.what());
        return data;
    }

    std::sort(data.begin(), data.end(), sortMetadata);

    return data;
}

/**
 * Parse a single metadata file
 */
auto MetadataManager::loadMetadataFile(fs::path const& path) -> std::optional<MetadataEntry> {
    MetadataEntry entry;
    entry.metadataFile = path;

    std::string line;
    std::ifstream infile(path);

    auto time = path.stem().string();
    entry.time = g_ascii_strtoll(time.c_str(), nullptr, 10);

    if (!getline(infile, line) || line != "XOJ-METADATA/1.0") {
        // Not valid
        return std::nullopt;
    }

    if (!getline(infile, line)) {
        // Not valid
        return std::nullopt;
    }
    std::istringstream iss(line);
    std::string p;
    iss >> std::quoted(p);
    entry.path = fs::path(xoj::util::utf8(p));

    if (!getline(infile, line) || line.length() < 6 || line.substr(0, 5) != "page=") {
        // Not valid
        return std::nullopt;
    }
    entry.page = static_cast<int>(g_ascii_strtoll(line.substr(5).c_str(), nullptr, 10));

    if (!getline(infile, line) || line.length() < 6 || line.substr(0, 5) != "zoom=") {
        // Not valid
        return std::nullopt;
    }
    const auto zoom = g_ascii_strtod(line.substr(5).c_str(), nullptr);
    if (!std::isnan(zoom)) {
        entry.zoom = zoom;
    }

    // Fork: optional exact view position (older files end here)
    if (getline(infile, line) && line.rfind("offset=", 0) == 0) {
        char* end = nullptr;
        const auto x = g_ascii_strtod(line.c_str() + 7, &end);
        const auto y = g_ascii_strtod(end, nullptr);
        if (std::isfinite(x) && std::isfinite(y) && getline(infile, line) && line.rfind("fit=", 0) == 0) {
            entry.hasViewPosition = true;
            entry.offsetX = x;
            entry.offsetY = y;
            entry.zoomFit = line.substr(4) == "1";
        }
    }

    return entry;
}

/**
 * Get the metadata for a file
 */
auto MetadataManager::getForFile(fs::path const& file) -> std::optional<MetadataEntry> {
    std::vector<MetadataEntry> files = loadList();

    std::optional<MetadataEntry> entry = std::nullopt;
    for (const MetadataEntry& e: files) {
        if (e.path == file) {
            entry = e;
            break;
        }
    }

    for (size_t i = 20; i < files.size(); i++) {
        auto path = files[i].metadataFile;
        deleteMetadataFile(path);
    }

    return entry;
}

/**
 * Store metadata to file
 */
void MetadataManager::storeMetadata(const MetadataEntry& m) {
    std::vector<MetadataEntry> files = loadList();
    for (const MetadataEntry& e: files) {
        if (e.path == m.path) {
            // This is an old entry with the same path
            deleteMetadataFile(e.metadataFile);
        }
    }

    auto path = getMetadataDirectory();
    gint64 time = g_get_real_time();
    path /= std::to_string(time);
    path += ".metadata";
    writeMetadataToFile(m, path);
}

void MetadataManager::writeMetadataToFile(const MetadataEntry& m, const fs::path& path) {
    auto out = serdes_stream<std::ofstream>(path);
    out << "XOJ-METADATA/1.0\n";
    out << std::quoted(char_cast(m.path.u8string())) << "\n";
    out << "page=" << m.page << "\n";
    out << "zoom=" << m.zoom << "\n";
    if (m.hasViewPosition) {
        out << "offset=" << m.offsetX << " " << m.offsetY << "\n";
        out << "fit=" << (m.zoomFit ? 1 : 0) << "\n";
    }
    out.close();
}

/**
 * Store the current data into metadata
 */
void MetadataManager::storeMetadata(fs::path const& file, int page, double zoom) {
    if (file.empty()) {
        return;
    }

    this->mutex.lock();
    if (metadata == nullptr) {
        metadata = std::make_unique<MetadataEntry>();
    }

    metadata->path = file;
    metadata->zoom = zoom;
    metadata->page = page;
    metadata->time = g_get_real_time();
    metadata->hasViewPosition = false;  // e.g. the page changed: a stored position within the page no longer applies
    this->mutex.unlock();
}

void MetadataManager::storeViewPosition(fs::path const& file, int page, double zoom, double offsetX, double offsetY,
                                        bool zoomFit) {
    storeMetadata(file, page, zoom);
    if (file.empty()) {
        return;
    }
    std::lock_guard lock(this->mutex);
    metadata->hasViewPosition = true;
    metadata->offsetX = offsetX;
    metadata->offsetY = offsetY;
    metadata->zoomFit = zoomFit;
}
