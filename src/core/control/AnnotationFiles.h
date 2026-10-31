/*
 * Xournal++
 *
 * Keeps a source PDF, its annotation document (.xopp) and the exported, annotated PDF together
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Control;
class Document;
class Settings;

/**
 * Naming rules shared by the automatic PDF export, opening PDFs and the project explorer.
 * All names follow the default file name settings ("Default name for new documents" and for PDF exports).
 */
namespace AnnotationFiles {

/// Recovery copies written by Xournal++ itself: ".<name>.autosave.xopp" and "<name>.xopp~" (written while saving).
/// They are never the document of a PDF.
bool isRecoveryCopy(const std::filesystem::path& path);

/**
 * Where the annotated PDF of a saved .xopp document is exported to; empty if it has no .xopp file
 * or the export would overwrite the source PDF.
 * The name recorded in the .xopp file takes precedence, so changing the export name setting does not
 * break the association of existing documents; otherwise the setting is used.
 */
std::filesystem::path exportPath(const Document& doc, const Settings& settings);

/// The exact modification time (in platform units) and size of a file; std::filesystem has only seconds with MinGW
struct FileStamp {
    uint64_t modified;
    uint64_t size;
    bool operator==(const FileStamp&) const = default;
};
std::optional<FileStamp> fileStamp(const std::filesystem::path& path);

/// Whether pdf exists and is not older than the document file xopp
bool isExportUpToDate(const std::filesystem::path& xopp, const std::filesystem::path& pdf);

/// exportPath() for a .xopp file on disk, without loading it
std::filesystem::path exportPathForFile(const std::filesystem::path& xopp, const Settings& settings);

/// The export file name recorded in a .xopp file (the "fork-export" attribute), or empty
std::string recordedExportFilename(const std::filesystem::path& xopp);

/// The PDF a .xopp file annotates (its first PDF background), or empty
std::filesystem::path recordedSourcePdf(const std::filesystem::path& xopp);

/// The .xopp document in the same folder that annotates pdf, if any, found by its recorded background
/// (so it is found even after the document was renamed)
std::optional<std::filesystem::path> documentForSource(const std::filesystem::path& pdf);

/// The export file name to record when saving doc to target: the one already recorded there, or a new one
std::string exportFilenameForSave(const Document& doc, const std::filesystem::path& target, const Settings& settings);

/**
 * Makes the closed document xopp use pdf as its background PDF, e.g. after the PDF was renamed; keeps the recorded
 * export name, and an up-to-date export stays up to date. Returns an error message, empty on success.
 */
std::string relinkSourcePdf(const std::filesystem::path& xopp, const std::filesystem::path& pdf,
                            const Settings& settings);

/**
 * Like relinkSourcePdf(), for each given part: the background PDF and/or the recorded export file name (a plain file
 * name next to the document), e.g. after renaming them. Returns an error message, empty on success.
 */
std::string relinkDocument(const std::filesystem::path& xopp, const std::optional<std::filesystem::path>& pdf,
                           const std::optional<std::string>& exportFilename, const Settings& settings);

/// A document with the files that are renamed or moved along with it, and what it has to record afterwards
struct DocumentGroup {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moves;  ///< from, to; the document first
    std::filesystem::path newXopp;
    std::optional<std::filesystem::path> newSource;  ///< the background PDF to record, if it changes
    std::optional<std::string> newExportName;        ///< the export file name to record, if it changes
};

/**
 * Renaming a document: its source PDF moves along if it is named like the document (a PDF with another name keeps
 * it), its exported PDF if its name starts with the document's name. The recorded export name follows the new name
 * also if the exported PDF does not exist (yet).
 */
DocumentGroup groupForRename(const std::filesystem::path& oldXopp, const std::filesystem::path& newXopp,
                             const Settings& settings);

/**
 * Moving a document to another folder: its source PDF moves along if it is named like the document (otherwise it
 * stays, and the document records its path from the new place), its exported PDF always.
 */
DocumentGroup groupForMove(const std::filesystem::path& xopp, const std::filesystem::path& dir,
                           const Settings& settings);

/// The name of a target of the group that exists already or is the target of two moves, if any
std::optional<std::string> groupConflict(const DocumentGroup& group);

/// The .xopp document in the same folder whose exported PDF is pdf, if any
std::optional<std::filesystem::path> documentForExport(const std::filesystem::path& pdf, const Settings& settings);

/// After a PDF was opened for annotation: gives the document its .xopp file next to the PDF and saves it,
/// unless that file already exists.
void createDocumentFileForPdf(Control* control);

/// Whether the document has anything beyond the plain PDF pages: strokes, text, images or added/removed pages
auto hasAnnotations(Document& doc) -> bool;

}  // namespace AnnotationFiles

/**
 * Exports the annotated PDF of the current document before it is closed (when switching documents or quitting)
 * and on request, using the regular PDF export. Only exports if the PDF is older than the saved .xopp file.
 */
class AnnotationExporter final {
public:
    explicit AnnotationExporter(Control* control);
    ~AnnotationExporter();

    AnnotationExporter(const AnnotationExporter&) = delete;
    AnnotationExporter& operator=(const AnnotationExporter&) = delete;

    /// Exports the current document if its PDF is missing or outdated, then calls done
    void exportIfOutdated(std::function<void()> done);

private:
    /// Whether the current document's exported PDF exists
    bool hasExport() const;

    /// Continues on the UI thread once the export thread is done
    void exportFinished(const std::string& error, std::function<void()> done);

    Control* control;
    bool running = false;
    /// Requests made while an export was running
    std::vector<std::function<void()>> waiting;
    /// Expires with this object, so that a finishing export thread does not call into it afterwards
    std::shared_ptr<void> lifetime = std::make_shared<int>(0);
};
