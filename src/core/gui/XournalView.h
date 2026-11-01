/*
 * Xournal++
 *
 * The widget which displays the PDF and the drawings
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <limits>   // for numeric_limits
#include <memory>   // for unique_ptr
#include <string>   // for string
#include <utility>  // for pair
#include <vector>   // for vector

#include <gdk/gdk.h>  // for GdkEventKey, GdkEventExpose
#include <glib.h>     // for gboolean
#include <gtk/gtk.h>  // for GtkWidget, GtkAllocation

#include "control/zoom/ZoomListener.h"     // for ZoomListener
#include "gui/inputdevices/InputEvents.h"  // for KeyEvent
#include "model/DocumentChangeType.h"      // for DocumentChangeType
#include "model/DocumentListener.h"        // for DocumentListener
#include "model/PageRef.h"                 // for PageRef
#include "pdf/base/XojPdfPage.h"           // for XojPdfRectangle
#include "util/Util.h"                     // for npos
#include "view/Mask.h"                     // for Mask

class Control;
class XournalppCursor;
class Document;
class EditSelection;
class XojPageView;
class XojPdfRectangle;
class PdfCache;

/// Fork: what was rendered for a document, kept with it in the document cache to show it again without rendering
struct KeptRendering {
    std::unique_ptr<PdfCache> pdfCache;
    /// The visible pages as they were last drawn
    std::vector<std::pair<PageRef, xoj::view::Mask>> pages;
    ~KeptRendering();
};
class RepaintHandler;
class ScrollHandling;
class TextEditor;
class HandRecognition;
class Layout;
namespace xoj::util {
template <class T>
class Rectangle;
}  // namespace xoj::util

class XournalView: public DocumentListener, public ZoomListener {
public:
    XournalView(GtkWidget* parent, Control* control, ScrollHandling* scrollHandling);
    ~XournalView() override;

public:
    // Recalculate the layout width and height amd layout the pages with the updated layout size
    void layoutPages();

    void scrollTo(size_t pageNo, XojPdfRectangle rect = {0, 0, -1, -1});

    /// Fork: the top left corner of the visible area relative to the page, in page coordinates (negative if the page
    /// starts inside the visible area)
    std::pair<double, double> getViewOffsetInPage(size_t pageNo) const;
    /// Fork: scrolls so that the top left corner of the visible area is at (x, y) relative to the page
    void scrollToPageOffset(size_t pageNo, double x, double y);
    /// Fork: queues the pages still to be rendered so that the current page comes first, then the other visible
    /// ones, then those around it (after jumping to a position, the order would follow the pages passed on the way)
    void renderCurrentPagesFirst();

    // Relative navigation in current layout:
    void pageRelativeXY(int offCol, int offRow);

    size_t getCurrentPage() const;

    void clearSelection();

    void layerChanged(size_t page);

    void requestFocus();
    void resetTouchInputSequence();

    void forceUpdatePagenumbers();

    XojPageView* getViewFor(size_t pageNr) const;

    bool searchTextOnPage(const std::string& text, size_t pageNumber, size_t index, size_t* occurrences,
                          XojPdfRectangle* matchRect);

    bool cut();
    bool copy();
    bool paste();

    void getPasteTarget(double& x, double& y) const;

    bool actionDelete();

    void endTextAllPages(XojPageView* except = nullptr) const;

    void endLinkAllPages(XojPageView* except = nullptr) const;

    void endSplineAllPages() const;

    int getDisplayWidth() const;
    int getDisplayHeight() const;

    bool isPageVisible(size_t page, int* visibleHeight) const;

    void ensureRectIsVisible(int x, int y, int width, int height);

    void setSelection(EditSelection* selection);
    EditSelection* getSelection() const;
    void deleteSelection(EditSelection* sel = nullptr);
    void repaintSelection(bool evenWithoutSelection = false);

    TextEditor* getTextEditor() const;
    std::vector<std::unique_ptr<XojPageView>> const& getViewPages() const;

    Control* getControl() const;
    double getZoom() const;
    int getDpiScaleFactor() const;
    Document* getDocument() const;
    PdfCache* getCache() const;
    RepaintHandler* getRepaintHandler() const;
    GtkWidget* getWidget() const;
    XournalppCursor* getCursor() const;
    Layout* getLayout() const;


    /// Return the rectangle (if any) which is visible on screen in page cooordinates
    xoj::util::Rectangle<double>* getVisibleRect(size_t page) const;
    /// Return the rectangle (if any) which is visible on screen in page cooordinates
    xoj::util::Rectangle<double>* getVisibleRect(const XojPageView* redrawable) const;

    /**
     * Recreate the PDF cache, for example after the underlying PDF file has changed
     */
    void recreatePdfCache();

    /// Fork: hands out what was rendered for the current document (to keep it with the document when another one is
    /// opened), or adopts what was kept for the document that was just opened again
    std::unique_ptr<KeptRendering> takeRendering();
    void adoptRendering(std::unique_ptr<KeptRendering> rendering);

    /**
     * A pen action was detected now, therefore ignore touch events
     * for a short time
     */
    void penActionDetected();

    /**
     * @return Helper class for Touch specific fixes
     */
    HandRecognition* getHandRecognition() const;

    /**
     * @return Scrollbars
     */
    ScrollHandling* getScrollHandling() const;

public:
    // ZoomListener interface
    void zoomChanged() override;

public:
    // DocumentListener interface
    void pageSelected(size_t page) override;
    void pageSizeChanged(size_t page) override;
    void pageChanged(size_t page) override;
    void pageInserted(size_t page) override;
    void pageDeleted(size_t page) override;
    void documentChanged(DocumentChangeType type) override;

public:
    bool onKeyPressEvent(const KeyEvent& event);
    bool onKeyReleaseEvent(const KeyEvent& event);

    void onSettingsChanged();

private:
    void fireZoomChanged();

    std::pair<size_t, size_t> preloadPageBounds(size_t page, size_t maxPage);

    static auto clearMemoryTimer(XournalView* widget) -> gboolean;

    void cleanupBufferCache();

private:
    /**
     * Scrollbars
     */
    ScrollHandling* scrollHandling = nullptr;

    GtkWidget* widget = nullptr;

    std::vector<std::unique_ptr<XojPageView>> viewPages;

    Control* control = nullptr;

    size_t currentPage = 0;
    size_t lastSelectedPage = npos;

    std::unique_ptr<PdfCache> cache;

    /**
     * Handler for rerendering pages / repainting pages
     */
    std::unique_ptr<RepaintHandler> repaintHandler;

    /**
     * Memory cleanup timeout
     */
    guint cleanupTimeout = std::numeric_limits<guint>::max();

    friend class Layout;
};
