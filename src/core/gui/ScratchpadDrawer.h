/*
 * Xournal Studio (fork)
 *
 * The scratchpad: a sheet for quick calculations and notes that slides down over the upper part of the document from a
 * pull tab at the top of the canvas, the same for every document. It is stored as "Schmierzettel.xopp" in the
 * project folder (so the git sync keeps it), else in the config folder. Pen and mouse write with the pen tool's color
 * and width (the pen's eraser end and the eraser button remove whole strokes), a finger scrolls, the sheet grows
 * downwards while writing.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <gtk/gtk.h>

#include "control/AnnotationFiles.h"  // for FileStamp
#include "model/DocumentHandler.h"
#include "model/DocumentListener.h"
#include "model/Element.h"
#include "model/ElementInsertionPosition.h"
#include "util/Point.h"

class Control;
class Document;
class Layer;
class Stroke;
class XojPage;
class ScrollMomentum;

class ScratchpadDrawer final: public DocumentListener {
public:
    /// Adds the pull tab and the sheet to overlay, which holds the canvas
    ScratchpadDrawer(Control* control, GtkOverlay* overlay);
    ~ScratchpadDrawer() override;

    ScratchpadDrawer(const ScratchpadDrawer&) = delete;
    ScratchpadDrawer& operator=(const ScratchpadDrawer&) = delete;

    void setOpen(bool open);
    bool isOpen() const { return open; }

    /// Finishes the stroke being written and writes unsaved strokes now (before a git sync, standby, quitting).
    /// During a git sync nothing is written unless evenWhileSyncing (quitting: better than losing the strokes).
    void saveNow(bool evenWhileSyncing = false);
    /// While a git sync runs nothing is written; afterwards the sheet is reloaded if the sync changed it
    void setSyncing(bool syncing);

    /// The open document changed: the scratchpad is hidden while it is open as the document itself
    void documentChanged(DocumentChangeType type) override;

    static constexpr const char* FILE_NAME = "Schmierzettel.xopp";

private:
    struct UndoStep {
        /// Strokes added (removed again on undo) and removed (inserted again on undo, in reverse order)
        std::vector<Element*> added;
        std::vector<InsertionPosition> removed;
    };

    void buildWidgets(GtkOverlay* overlay);
    GtkWidget* addToolButton(GtkWidget* box, const char* icon, const char* tooltip, bool toggle);

    // Sliding
    gboolean childPosition(GtkWidget* child, GdkRectangle* allocation);
    /// Springs to the height, starting with the velocity (pixels per second, opening is positive)
    void animateTo(double target, double velocity = 0);
    void setOpen(bool open, double velocity);
    /// The heights the sheet snaps to: closed, half, full
    std::vector<double> detents() const;
    /// Beyond the full height, dragging moves the sheet less and less (like iOS)
    double rubberBand(double height) const;
    void setShown(double shown);
    void tabDragBegin();
    void tabDragUpdate(double dy);
    void tabDragEnd(double dy);
    double maxHeight() const;
    double openHeight() const;
    void updateAction();

    // File
    std::filesystem::path currentFile() const;
    void ensureLoaded();
    void load(const std::filesystem::path& file);
    void reloadIfChanged();
    void markChanged();
    bool openedAsDocument() const;
    void openAsDocument();

    // Sheet
    Layer* layer() const;
    XojPage* page() const;
    double zoom() const;
    void fitHeight();
    void scrollBy(double dy);
    void scrollToLatest();
    void invalidate();
    void draw(cairo_t* cr);
    void updateButtons();

    // Input
    gboolean handleEvent(GdkEvent* event);
    void beginStroke(const GdkEvent* event, bool erasing);
    void addPoint(const GdkEvent* event);
    void continueStroke(const GdkEvent* event);
    void endStroke();
    void eraseAt(xoj::util::Point<double> point);
    xoj::util::Point<double> toPage(double x, double y) const;

    void push(UndoStep step);
    /// Undoes step, returns the step that redoes it
    UndoStep revert(UndoStep step);
    void undo();
    void redo();
    void clear();

    Control* control;
    GtkWidget* overlay = nullptr;
    GtkWidget* tab = nullptr;
    GtkWidget* panel = nullptr;
    GtkWidget* area = nullptr;
    GtkWidget* penButton = nullptr;
    GtkWidget* eraserButton = nullptr;
    GtkWidget* undoButton = nullptr;
    GtkWidget* redoButton = nullptr;
    GtkWidget* clearButton = nullptr;
    GtkGesture* tabDrag = nullptr;

    bool open = false;
    bool hidden = false;     ///< the scratchpad is open as the document
    double shown = 0;        ///< visible height of the sheet in pixels
    double fraction = 0.45;  ///< of the canvas height when open
    bool dragging = false;
    double dragStartRootY = 0;
    double dragStartShown = 0;
    std::vector<std::pair<gint64, double>> dragSamples;
    guint animation = 0;
    double springTarget = 0;
    double springVelocity = 0;  ///< pixels per second
    gint64 springTime = 0;      ///< of the last frame

    DocumentHandler handler;  ///< declared before doc: the document refers to it
    std::unique_ptr<Document> doc;
    std::filesystem::path file;
    std::optional<AnnotationFiles::FileStamp> stamp;  ///< of file when loaded or saved
    bool changed = false;
    bool syncing = false;
    guint saveTimer = 0;

    double scrollY = 0;  ///< in pixels
    std::unique_ptr<ScrollMomentum> momentum;
    GdkEventSequence* scrollSequence = nullptr;
    double lastTouchY = 0;
    gint64 lastPenTime = 0;

    std::unique_ptr<Stroke> stroke;  ///< being written
    bool erasing = false;
    bool pressureSensitive = false;
    double lastPressure = 1;
    xoj::util::Point<double> lastErasePoint;
    UndoStep erased;  ///< strokes removed by the running eraser gesture

    std::vector<UndoStep> undoSteps;
    std::vector<UndoStep> redoSteps;

    cairo_surface_t* cache = nullptr;  ///< the sheet without the stroke being written
    guint powerListener = 0;
};
