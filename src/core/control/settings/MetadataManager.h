/*
 * Xournal++
 *
 * Last opened files with all settings
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <mutex>  // for mutex
#include <optional>
#include <vector>  // for vector

#include <glib.h>  // for gint64

#include "filesystem.h"  // for path


class MetadataEntry {
public:
    MetadataEntry();

public:
    fs::path metadataFile;
    fs::path path;
    double zoom;
    int page;
    gint64 time;

    /// Fork: the exact view, stored when the document was closed: the visible area's top left corner relative to
    /// the page (in page coordinates) and whether the zoom fitted the page. Older entries have only page and zoom.
    bool hasViewPosition = false;
    double offsetX = 0;
    double offsetY = 0;
    bool zoomFit = false;
};

class MetadataManager {
public:
    MetadataManager() = default;
    virtual ~MetadataManager();

public:
    /**
     * Get the metadata for a file
     */
    static std::optional<MetadataEntry> getForFile(fs::path const& file);

    /**
     * Store the current data into metadata
     */
    void storeMetadata(fs::path const& file, int page, double zoom);

    /**
     * Fork: store the exact view of the document (see MetadataEntry), e.g. when it is closed
     */
    void storeViewPosition(fs::path const& file, int page, double zoom, double offsetX, double offsetY, bool zoomFit);

    /**
     * Document was closed, a new document was opened etc.
     */
    void documentChanged();

private:
    /**
     * Delete an old metadata file
     */
    static void deleteMetadataFile(fs::path const& path);

    /**
     * Parse a single metadata file
     */
    static std::optional<MetadataEntry> loadMetadataFile(fs::path const& path);

    /**
     * Store metadata to file
     */
    static void storeMetadata(const MetadataEntry& m);

    static void writeMetadataToFile(const MetadataEntry& m, const fs::path& file);

private:
    /**
     * Load the metadata list (sorted)
     */
    static std::vector<MetadataEntry> loadList();

private:
    std::mutex mutex;
    std::unique_ptr<MetadataEntry> metadata;

    friend class Metadata_testRead_Test;
    friend class Metadata_testReadNaNZoom_Test;
    friend class Metadata_testWriteReadCycle_Test;
    friend class Metadata_testViewPositionCycle_Test;
};
