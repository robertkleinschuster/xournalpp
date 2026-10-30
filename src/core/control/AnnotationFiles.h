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

class Control;
class Document;
class Settings;

/**
 * Naming rules shared by the automatic PDF export, opening PDFs and the project explorer.
 * All names follow the default file name settings ("Default name for new documents" and for PDF exports).
 */
namespace AnnotationFiles {

/// Where the annotated PDF of a saved .xopp document is exported to; empty if it has no .xopp file
/// or the export would overwrite the source PDF.
std::filesystem::path exportPath(const Document& doc, const Settings& settings);

/// exportPath() for a .xopp file on disk, without loading it
std::filesystem::path exportPathForFile(const std::filesystem::path& xopp, const Settings& settings);

/// The .xopp document in the same folder whose exported PDF is pdf, if any
std::optional<std::filesystem::path> documentForExport(const std::filesystem::path& pdf, const Settings& settings);

/// After a PDF was opened for annotation: gives the document its .xopp file next to the PDF and saves it,
/// unless that file already exists.
void createDocumentFileForPdf(Control* control);

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
    Control* control;
};
