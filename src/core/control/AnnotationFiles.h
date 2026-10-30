/*
 * Xournal++
 *
 * Keeps a source PDF, its annotation document (.xopp) and the exported, annotated PDF together
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

class Control;
class Document;
class Settings;

/**
 * Naming rules shared by the automatic PDF export, opening PDFs and the project explorer.
 * All names follow the default file name settings ("Default name for new documents" and for PDF exports).
 */
namespace AnnotationFiles {

/**
 * Where the annotated PDF of a saved .xopp document is exported to; empty if it has no .xopp file
 * or the export would overwrite the source PDF.
 * The name recorded in the .xopp file takes precedence, so changing the export name setting does not
 * break the association of existing documents; otherwise the setting is used.
 */
std::filesystem::path exportPath(const Document& doc, const Settings& settings);

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

    Control* control;
};
