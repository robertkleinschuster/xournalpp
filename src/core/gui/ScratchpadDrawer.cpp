#include "ScratchpadDrawer.h"

#include <algorithm>  // for clamp, max, min
#include <cmath>      // for ceil, floor, hypot
#include <string>     // for string, to_string
#include <utility>    // for move

#include "control/Control.h"
#include "control/Tool.h"
#include "control/ToolHandler.h"
#include "control/actions/ActionDatabase.h"
#include "control/settings/Settings.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "gui/InputModality.h"
#include "gui/PowerEvents.h"
#include "gui/inputdevices/ScrollMomentum.h"
#include "gui/sidebar/Sidebar.h"
#include "model/Document.h"
#include "model/Layer.h"
#include "model/PageType.h"
#include "model/Stroke.h"
#include "model/XojPage.h"
#include "util/PathUtil.h"
#include "util/i18n.h"
#include "view/DocumentView.h"
#include "view/StrokeView.h"

namespace fs = std::filesystem;

namespace {
constexpr int TAB_WIDTH = 120;
constexpr int TAB_HEIGHT = 30;  ///< easy to hit with a finger
/// Lower sheets close when released
constexpr double MIN_SHEET = 120;
/// The document keeps at least this much of the canvas above the sheet
constexpr double DOCUMENT_STRIP = 72;
constexpr gint64 ANIMATION_US = 180'000;
/// A flick of the tab faster than this (pixels per second) opens or closes the sheet
constexpr double FLICK_SPEED = 700;
/// Touches this soon after the pen was seen are a resting palm
constexpr gint64 PALM_US = 700'000;
constexpr guint SAVE_DELAY_MS = 1500;
/// A new sheet: the width of A4 landscape, the height of A4 landscape; it grows while writing
constexpr double SHEET_WIDTH = 842;
constexpr double SHEET_HEIGHT = 595;
/// Pixels per scroll wheel notch
constexpr double WHEEL_STEP = 60;
/// Distance between recorded points, in page units (as the main canvas)
constexpr double MIN_POINT_DISTANCE = 0.3;
constexpr double MIN_ERASER_RADIUS = 5;  ///< page units; whole strokes are removed, so it may be generous

constexpr const char* CSS = R"css(
.scratchpad {
    background-color: @theme_bg_color;
    border-top: 1px solid alpha(@theme_fg_color, 0.25);
}
.scratchpad-tools button {
    min-width: 40px;
    min-height: 40px;
    padding: 2px;
}
)css";

void addCss(GtkWidget* widget) {
    static GtkCssProvider* provider = [] {
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, CSS, -1, nullptr);
        return css;
    }();
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget), GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

auto isEraserDevice(const GdkEvent* event) -> bool {
    GdkDevice* device = gdk_event_get_source_device(event);
    return device && gdk_device_get_source(device) == GDK_SOURCE_ERASER;
}

auto rootY(const GdkEvent* event) -> double {
    double x = 0, y = 0;
    gdk_event_get_root_coords(event, &x, &y);
    return y;
}
}  // namespace

ScratchpadDrawer::ScratchpadDrawer(Control* control, GtkOverlay* overlay): control(control) {
    momentum = std::make_unique<ScrollMomentum>([this](xoj::util::Point<double> move) { scrollBy(-move.y); });
    buildWidgets(overlay);
    registerListener(control);
    powerListener = PowerEvents::addListener([this](PowerEvents::Event event) {
        if (event == PowerEvents::Event::Standby) {
            saveNow();
        }
    });
}

ScratchpadDrawer::~ScratchpadDrawer() {
    PowerEvents::removeListener(powerListener);
    unregisterListener();
    momentum.reset();  // its tick callback belongs to the area
    if (saveTimer) {
        g_source_remove(saveTimer);
        saveTimer = 0;
    }
    saveNow(true);  // finishes a stroke still being written
    if (saveTimer) {
        g_source_remove(saveTimer);
    }
    if (animation && overlay) {
        gtk_widget_remove_tick_callback(overlay, animation);
    }
    if (overlay) {
        g_signal_handlers_disconnect_by_data(overlay, this);
        g_object_remove_weak_pointer(G_OBJECT(overlay), reinterpret_cast<gpointer*>(&overlay));
    }
    for (GtkWidget** widget: {&tab, &panel, &area}) {
        if (*widget) {
            g_signal_handlers_disconnect_by_data(*widget, this);
            g_object_remove_weak_pointer(G_OBJECT(*widget), reinterpret_cast<gpointer*>(widget));
        }
    }
    if (tabDrag) {
        g_object_unref(tabDrag);
    }
    if (cache) {
        cairo_surface_destroy(cache);
    }
}

void ScratchpadDrawer::buildWidgets(GtkOverlay* gtkOverlay) {
    overlay = GTK_WIDGET(gtkOverlay);
    g_object_add_weak_pointer(G_OBJECT(overlay), reinterpret_cast<gpointer*>(&overlay));

    // The pull tab: a handle centered at the bottom of the canvas, on top of the sheet while it is open
    tab = gtk_drawing_area_new();
    gtk_widget_set_size_request(tab, TAB_WIDTH, TAB_HEIGHT);
    gtk_widget_add_events(tab,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK | GDK_TOUCH_MASK);
    gtk_widget_set_tooltip_text(tab, _("Scratchpad"));
    g_signal_connect(tab, "draw", G_CALLBACK(+[](GtkWidget* w, cairo_t* cr, gpointer) -> gboolean {
                         const double width = gtk_widget_get_allocated_width(w);
                         const double height = gtk_widget_get_allocated_height(w);
                         GtkStyleContext* style = gtk_widget_get_style_context(w);
                         GdkRGBA fg;
                         gtk_style_context_get_color(style, gtk_style_context_get_state(style), &fg);
                         // A tab with rounded upper corners in the window color, with a grip line
                         const double r = 8;
                         cairo_new_path(cr);
                         cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
                         cairo_arc(cr, width - r, r, r, 1.5 * M_PI, 2 * M_PI);
                         cairo_line_to(cr, width, height);
                         cairo_line_to(cr, 0, height);
                         cairo_close_path(cr);
                         cairo_save(cr);
                         cairo_clip_preserve(cr);
                         gtk_render_background(style, cr, 0, 0, width, height);
                         cairo_restore(cr);
                         cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.3);
                         cairo_set_line_width(cr, 1);
                         cairo_stroke(cr);
                         cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.6);
                         cairo_set_line_width(cr, 3);
                         cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                         cairo_move_to(cr, width / 2 - 16, height / 2 + 1);
                         cairo_line_to(cr, width / 2 + 16, height / 2 + 1);
                         cairo_stroke(cr);
                         return TRUE;
                     }),
                     nullptr);
    gtk_style_context_add_class(gtk_widget_get_style_context(tab), "scratchpad");
    addCss(tab);
    tabDrag = gtk_gesture_drag_new(tab);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(tabDrag), false);
    g_signal_connect(tabDrag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer d) {
                         static_cast<ScratchpadDrawer*>(d)->tabDragBegin();
                     }),
                     this);
    g_signal_connect(tabDrag, "drag-update", G_CALLBACK(+[](GtkGestureDrag* g, double, double, gpointer d) {
                         auto* self = static_cast<ScratchpadDrawer*>(d);
                         // In screen coordinates: the tab moves with the finger
                         GdkEventSequence* sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
                         if (const GdkEvent* event = gtk_gesture_get_last_event(GTK_GESTURE(g), sequence)) {
                             self->tabDragUpdate(rootY(event) - self->dragStartRootY);
                         }
                     }),
                     this);
    g_signal_connect(tabDrag, "drag-end", G_CALLBACK(+[](GtkGestureDrag* g, double, double, gpointer d) {
                         auto* self = static_cast<ScratchpadDrawer*>(d);
                         GdkEventSequence* sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
                         const GdkEvent* event = gtk_gesture_get_last_event(GTK_GESTURE(g), sequence);
                         self->tabDragEnd(event ? rootY(event) - self->dragStartRootY : 0);
                     }),
                     this);
    g_signal_connect(tab, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEvent* event, gpointer d) -> gboolean {
                         static_cast<ScratchpadDrawer*>(d)->dragStartRootY = rootY(event);
                         return FALSE;
                     }),
                     this);
    g_signal_connect(tab, "touch-event", G_CALLBACK(+[](GtkWidget*, GdkEvent* event, gpointer d) -> gboolean {
                         if (gdk_event_get_event_type(event) == GDK_TOUCH_BEGIN) {
                             static_cast<ScratchpadDrawer*>(d)->dragStartRootY = rootY(event);
                         }
                         return FALSE;
                     }),
                     this);

    // The sheet: tools on the left (away from the writing hand and the right screen edge), the paper beside them
    panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "scratchpad");
    addCss(panel);
    GtkWidget* tools = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(tools), "scratchpad-tools");
    gtk_widget_set_margin_start(tools, 4);
    gtk_widget_set_margin_end(tools, 4);
    gtk_widget_set_margin_top(tools, 6);
    penButton = addToolButton(tools, "xopp-tool-pencil", _("Pen"), true);
    eraserButton = addToolButton(tools, "xopp-tool-eraser", _("Eraser"), true);
    undoButton = addToolButton(tools, "xopp-edit-undo", _("Undo"), false);
    redoButton = addToolButton(tools, "xopp-edit-redo", _("Redo"), false);
    clearButton = addToolButton(tools, "xopp-page-delete", _("Clear Scratchpad"), false);
    GtkWidget* openButton = addToolButton(tools, "xopp-document-open", _("Open as Document"), false);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(penButton), true);
    g_signal_connect(penButton, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
                         auto* self = static_cast<ScratchpadDrawer*>(d);
                         gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->eraserButton),
                                                      !gtk_toggle_button_get_active(b));
                     }),
                     this);
    g_signal_connect(eraserButton, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
                         auto* self = static_cast<ScratchpadDrawer*>(d);
                         gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->penButton),
                                                      !gtk_toggle_button_get_active(b));
                     }),
                     this);
    g_signal_connect(undoButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<ScratchpadDrawer*>(d)->undo(); }), this);
    g_signal_connect(redoButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<ScratchpadDrawer*>(d)->redo(); }), this);
    g_signal_connect(clearButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<ScratchpadDrawer*>(d)->clear(); }), this);
    g_signal_connect(openButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<ScratchpadDrawer*>(d)->openAsDocument(); }),
                     this);
    gtk_box_pack_start(GTK_BOX(panel), tools, false, false, 0);

    area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area, true);
    gtk_widget_set_vexpand(area, true);
    gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                        GDK_TOUCH_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                                        GDK_PROXIMITY_IN_MASK | GDK_PROXIMITY_OUT_MASK);
    g_signal_connect(area, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer d) -> gboolean {
                         static_cast<ScratchpadDrawer*>(d)->draw(cr);
                         return TRUE;
                     }),
                     this);
    g_signal_connect(area, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer d) {
                         auto* self = static_cast<ScratchpadDrawer*>(d);
                         self->fitHeight();
                         self->scrollBy(0);
                         self->invalidate();
                     }),
                     this);
    g_signal_connect(area, "event", G_CALLBACK(+[](GtkWidget*, GdkEvent* event, gpointer d) -> gboolean {
                         return static_cast<ScratchpadDrawer*>(d)->handleEvent(event);
                     }),
                     this);
    gtk_box_pack_start(GTK_BOX(panel), area, true, true, 0);

    for (GtkWidget** widget: {&tab, &panel, &area}) {
        g_object_add_weak_pointer(G_OBJECT(*widget), reinterpret_cast<gpointer*>(widget));
    }
    gtk_overlay_add_overlay(gtkOverlay, panel);
    gtk_overlay_add_overlay(gtkOverlay, tab);
    g_signal_connect(gtkOverlay, "get-child-position",
                     G_CALLBACK(+[](GtkOverlay*, GtkWidget* child, GdkRectangle* allocation, gpointer d) -> gboolean {
                         return static_cast<ScratchpadDrawer*>(d)->childPosition(child, allocation);
                     }),
                     this);
    gtk_widget_show_all(panel);
    gtk_widget_show(tab);
    gtk_widget_set_child_visible(panel, false);
    updateButtons();
}

auto ScratchpadDrawer::addToolButton(GtkWidget* box, const char* icon, const char* tooltip, bool toggle) -> GtkWidget* {
    GtkWidget* button = toggle ? gtk_toggle_button_new() : gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(button), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_set_can_focus(button, false);
    gtk_box_pack_start(GTK_BOX(box), button, false, false, 0);
    return button;
}

// --- Sliding ---------------------------------------------------------------------------------------------------------

auto ScratchpadDrawer::maxHeight() const -> double {
    return overlay ? std::max(MIN_SHEET, gtk_widget_get_allocated_height(overlay) - DOCUMENT_STRIP) : MIN_SHEET;
}

auto ScratchpadDrawer::openHeight() const -> double {
    const double height = overlay ? gtk_widget_get_allocated_height(overlay) : 0;
    return std::clamp(fraction * height, MIN_SHEET, maxHeight());
}

auto ScratchpadDrawer::childPosition(GtkWidget* child, GdkRectangle* allocation) -> gboolean {
    if (child != panel && child != tab) {
        return FALSE;
    }
    const int width = gtk_widget_get_allocated_width(overlay);
    const int height = gtk_widget_get_allocated_height(overlay);
    if (open && !animation && !dragging) {
        shown = openHeight();  // follows window size changes and rotation
    }
    shown = std::min(shown, maxHeight());
    const int visible = static_cast<int>(std::round(shown));
    if (child == panel) {
        // Allocated at its full height, so the paper does not reflow while sliding; the overlay clips it
        *allocation = {0, height - visible, width, std::max(visible, static_cast<int>(openHeight()))};
    } else {
        int tabWidth = 0, tabHeight = 0;
        gtk_widget_get_preferred_width(tab, nullptr, &tabWidth);
        gtk_widget_get_preferred_height(tab, nullptr, &tabHeight);
        *allocation = {(width - tabWidth) / 2, height - visible - tabHeight, tabWidth, tabHeight};
    }
    return TRUE;
}

void ScratchpadDrawer::setShown(double value) {
    shown = std::max(0.0, value);
    if (shown <= 0.5 && scrollSequence) {
        scrollSequence = nullptr;  // its end is not delivered to a hidden sheet
        momentum->forgetMovement();
    }
    if (panel) {
        gtk_widget_set_child_visible(panel, shown > 0.5);
    }
    if (overlay) {
        gtk_widget_queue_allocate(overlay);
    }
}

void ScratchpadDrawer::animateTo(double target) {
    if (!overlay) {
        return;
    }
    if (animation) {
        gtk_widget_remove_tick_callback(overlay, animation);
        animation = 0;
    }
    animationFrom = shown;
    animationTo = target;
    animationStart = 0;
    if (!gtk_widget_get_mapped(overlay)) {
        setShown(target);
        return;
    }
    animation = gtk_widget_add_tick_callback(
            overlay,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer d) -> gboolean {
                auto* self = static_cast<ScratchpadDrawer*>(d);
                const gint64 now = gdk_frame_clock_get_frame_time(clock);
                if (!self->animationStart) {
                    self->animationStart = now;
                }
                const double t = std::min(1.0, static_cast<double>(now - self->animationStart) / ANIMATION_US);
                const double eased = 1 - (1 - t) * (1 - t) * (1 - t);
                self->setShown(self->animationFrom + (self->animationTo - self->animationFrom) * eased);
                if (t >= 1) {
                    self->animation = 0;
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
}

void ScratchpadDrawer::setOpen(bool value) {
    if (value && hidden) {
        value = false;
    }
    if (value) {
        ensureLoaded();
    } else {
        endStroke();
        saveNow();
    }
    // Also when the state stays: a drag released below the minimum height slides back
    if (const double target = value ? openHeight() : 0; open != value || shown != target) {
        open = value;
        animateTo(target);
    }
    updateAction();
}

void ScratchpadDrawer::tabDragBegin() {
    if (animation) {
        gtk_widget_remove_tick_callback(overlay, animation);
        animation = 0;
    }
    dragging = true;
    dragStartShown = shown;
    dragSamples.clear();
    if (!doc) {
        ensureLoaded();
    }
}

void ScratchpadDrawer::tabDragUpdate(double dy) {
    if (!dragging || hidden) {
        return;
    }
    if (shown == 0 && dragStartShown == 0 && std::abs(dy) >= xoj::input::DRAG_THRESHOLD) {
        ensureLoaded();  // reloads the sheet if git or the document view changed it
    }
    setShown(std::clamp(dragStartShown - dy, 0.0, maxHeight()));
    const gint64 now = g_get_monotonic_time();
    dragSamples.emplace_back(now, shown);
    while (dragSamples.size() > 2 && now - dragSamples.front().first > 100'000) {
        dragSamples.erase(dragSamples.begin());
    }
}

void ScratchpadDrawer::tabDragEnd(double dy) {
    if (!dragging) {
        return;
    }
    dragging = false;
    if (hidden) {
        return;
    }
    if (std::abs(dy) < xoj::input::TOUCH_SLOP) {
        setOpen(!open);  // a tap
        return;
    }
    double speed = 0;  // pixels per second, opening is positive
    if (dragSamples.size() >= 2) {
        const auto& [t0, s0] = dragSamples.front();
        const auto& [t1, s1] = dragSamples.back();
        if (t1 > t0) {
            speed = (s1 - s0) / (static_cast<double>(t1 - t0) / 1e6);
        }
    }
    const double height = gtk_widget_get_allocated_height(overlay);
    if (speed < -FLICK_SPEED || (speed <= FLICK_SPEED && shown < MIN_SHEET)) {
        setOpen(false);
        return;
    }
    if (shown >= MIN_SHEET && height > 0) {
        fraction = shown / height;  // the sheet keeps the height it was dragged to
    }
    setOpen(true);
}

void ScratchpadDrawer::updateAction() {
    if (auto* actions = control->getActionDatabase()) {
        actions->setActionState(Action::SHOW_SCRATCHPAD, open);
    }
}

// --- File ------------------------------------------------------------------------------------------------------------

auto ScratchpadDrawer::currentFile() const -> fs::path {
    fs::path root = control->getSidebar() ? control->getSidebar()->currentProject() : fs::path();
    return root.empty() ? Util::getConfigFile(FILE_NAME) : root / FILE_NAME;
}

void ScratchpadDrawer::ensureLoaded() {
    const auto wanted = currentFile();
    if (!doc || wanted != file) {
        saveNow();
        load(wanted);
    } else {
        reloadIfChanged();
    }
}

void ScratchpadDrawer::load(const fs::path& path) {
    stroke.reset();
    erased = {};
    undoSteps.clear();
    redoSteps.clear();
    changed = false;
    file = path;
    stamp = AnnotationFiles::fileStamp(path);
    doc = std::make_unique<Document>(&handler);
    std::error_code error;
    if (fs::exists(path, error)) {
        bool loadedOk = false;
        try {
            LoadHandler loader;
            if (auto loaded = loader.loadDocument(path); loaded && loaded->getPageCount() > 0) {
                doc->lock();  // the assignment expects it
                *doc = *loaded;
                doc->unlock();
                loadedOk = true;
            }
        } catch (const std::exception& e) {
            g_warning("Scratchpad: could not load %s: %s", Util::toUtf8(path).c_str(), e.what());
        }
        if (!loadedOk) {
            // Kept next to it instead of being overwritten by the next stroke, e.g. after an interrupted sync
            fs::path aside;
            for (int i = 1; aside.empty() || fs::exists(aside, error); i++) {
                aside = path.parent_path() / (Util::toUtf8(path.stem()) + " (unreadable" +
                                              (i > 1 ? " " + std::to_string(i) : std::string()) + ").xopp");
            }
            fs::rename(path, aside, error);
            g_warning("Scratchpad: moved the unreadable sheet to %s%s", Util::toUtf8(aside).c_str(),
                      error ? " (failed, it will not be saved over)" : "");
            if (error) {
                file.clear();  // saveNow() skips an empty path
            }
            stamp.reset();
        }
    }
    if (doc->getPageCount() == 0 || doc->getPage(0)->getLayers().empty()) {
        doc = std::make_unique<Document>(&handler);
        auto newPage = std::make_shared<XojPage>(SHEET_WIDTH, SHEET_HEIGHT);  // with one layer
        newPage->setBackgroundType(PageType(PageTypeFormat::Graph));
        doc->addPage(newPage);
    }
    doc->setFilepath(path);
    fitHeight();
    scrollToLatest();
    invalidate();
    updateButtons();
}

void ScratchpadDrawer::reloadIfChanged() {
    if (!doc || syncing) {
        return;
    }
    const auto now = AnnotationFiles::fileStamp(file);
    if (now == stamp) {
        return;
    }
    if (changed) {
        // Written here and changed elsewhere since: keep what is on screen, it is saved over the other version
        g_warning("Scratchpad: %s changed on disk while it had unsaved strokes", Util::toUtf8(file).c_str());
        stamp = now;
        return;
    }
    load(file);
}

void ScratchpadDrawer::markChanged() {
    changed = true;
    if (saveTimer) {
        g_source_remove(saveTimer);
    }
    saveTimer = g_timeout_add(
            SAVE_DELAY_MS,
            +[](gpointer d) -> gboolean {
                auto* self = static_cast<ScratchpadDrawer*>(d);
                self->saveTimer = 0;
                if (!self->stroke && !self->erasing) {
                    self->saveNow();
                } else {
                    self->markChanged();  // again after the stroke
                }
                return G_SOURCE_REMOVE;
            },
            this);
}

void ScratchpadDrawer::saveNow(bool evenWhileSyncing) {
    endStroke();
    if (!changed || !doc || (syncing && !evenWhileSyncing) || file.empty()) {
        return;  // during a sync the strokes are written when it ends
    }
    if (saveTimer) {
        g_source_remove(saveTimer);
        saveTimer = 0;
    }
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    SaveHandler saver;
    saver.prepareSave(doc.get(), file);
    saver.saveTo(file);
    if (!saver.getErrorMessage().empty()) {
        g_warning("Scratchpad: could not save %s: %s", Util::toUtf8(file).c_str(), saver.getErrorMessage().c_str());
        return;
    }
    changed = false;
    stamp = AnnotationFiles::fileStamp(file);
}

void ScratchpadDrawer::setSyncing(bool value) {
    if (value) {
        saveNow();  // so the sync commits the latest strokes
    }
    syncing = value;
    if (!value && doc) {
        reloadIfChanged();
        if (changed) {
            markChanged();  // written during the sync
        }
    }
}

auto ScratchpadDrawer::openedAsDocument() const -> bool {
    Document* main = control->getDocument();
    if (!main) {
        return false;
    }
    main->lock_shared();
    const auto path = main->getFilepath();
    main->unlock_shared();
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    const auto scratchpad = currentFile();
    return fs::equivalent(path, scratchpad, error) || path.lexically_normal() == scratchpad.lexically_normal();
}

void ScratchpadDrawer::documentChanged(DocumentChangeType type) {
    if (type != DOCUMENT_CHANGE_COMPLETE && type != DOCUMENT_CHANGE_CLEARED) {
        return;
    }
    const bool nowHidden = openedAsDocument();
    if (nowHidden == hidden) {
        return;
    }
    if (nowHidden) {
        setOpen(false);  // saves it before the document view uses the file
    }
    hidden = nowHidden;
    if (tab) {
        gtk_widget_set_child_visible(tab, !hidden);
    }
}

void ScratchpadDrawer::openAsDocument() {
    ensureLoaded();
    endStroke();
    if (changed || !fs::exists(file)) {
        changed = true;  // an untouched new sheet is written first, so the document view finds a file
        saveNow();
    }
    setOpen(false);
    if (auto* sidebar = control->getSidebar()) {
        sidebar->openDocumentAt(file, 0);
    }
}

// --- Sheet -----------------------------------------------------------------------------------------------------------

auto ScratchpadDrawer::page() const -> XojPage* { return doc && doc->getPageCount() ? doc->getPage(0).get() : nullptr; }

auto ScratchpadDrawer::layer() const -> Layer* {
    XojPage* p = page();
    return p && !p->getLayers().empty() ? p->getLayers().front() : nullptr;
}

auto ScratchpadDrawer::zoom() const -> double {
    const int width = area ? gtk_widget_get_allocated_width(area) : 0;
    return page() && width > 1 ? width / page()->getWidth() : 1.0;
}

void ScratchpadDrawer::fitHeight() {
    XojPage* p = page();
    Layer* l = layer();
    if (!p || !l) {
        return;
    }
    double bottom = 0;
    for (const auto& element: l->getElements()) {
        const auto& box = element->getBoundingBox();
        bottom = std::max(bottom, box.y + box.height);
    }
    // Always a screen of empty paper below the last stroke to write on
    const int areaHeight = area ? gtk_widget_get_allocated_height(area) : 0;
    const double room = std::max(areaHeight / zoom(), SHEET_HEIGHT / 2);
    const double height = std::max(SHEET_HEIGHT, std::ceil((bottom + room) / 50) * 50);
    if (height != p->getHeight()) {
        p->setSize(p->getWidth(), height);
    }
}

void ScratchpadDrawer::scrollBy(double dy) {
    const double contentHeight = page() ? page()->getHeight() * zoom() : 0;
    const double viewHeight = area ? gtk_widget_get_allocated_height(area) : 0;
    const double value = std::clamp(scrollY + dy, 0.0, std::max(0.0, contentHeight - viewHeight));
    if (value != scrollY) {
        scrollY = value;
        invalidate();
    }
}

void ScratchpadDrawer::scrollToLatest() {
    // The newest stroke is where the user stopped writing
    Layer* l = layer();
    scrollY = 0;
    if (!l || l->getElements().empty()) {
        return;
    }
    const auto& box = l->getElements().back()->getBoundingBox();
    const double viewHeight = area ? gtk_widget_get_allocated_height(area) : 0;
    scrollBy((box.y + box.height) * zoom() - viewHeight * 0.6);
}

void ScratchpadDrawer::invalidate() {
    if (cache) {
        cairo_surface_destroy(cache);
        cache = nullptr;
    }
    if (area) {
        gtk_widget_queue_draw(area);
    }
}

void ScratchpadDrawer::draw(cairo_t* cr) {
    const int width = gtk_widget_get_allocated_width(area);
    const int height = gtk_widget_get_allocated_height(area);
    if (!page() || width <= 0 || height <= 0) {
        return;
    }
    const double z = zoom();
    if (!cache) {
        cache = gdk_window_create_similar_surface(gtk_widget_get_window(area), CAIRO_CONTENT_COLOR, width, height);
        cairo_t* c = cairo_create(cache);
        cairo_set_source_rgb(c, 0.5, 0.5, 0.5);
        cairo_paint(c);
        cairo_translate(c, 0, -scrollY);
        cairo_scale(c, z, z);
        DocumentView view;
        view.drawPage(doc->getPage(0), c, true);
        cairo_destroy(c);
    }
    cairo_set_source_surface(cr, cache, 0, 0);
    cairo_paint(cr);
    if (stroke && stroke->getPointCount() >= 2) {
        cairo_save(cr);
        cairo_translate(cr, 0, -scrollY);
        cairo_scale(cr, z, z);
        xoj::view::StrokeView(stroke.get()).draw(xoj::view::Context::createDefault(cr));
        cairo_restore(cr);
    }
}

void ScratchpadDrawer::updateButtons() {
    if (panel && undoButton) {  // the buttons are gone with the panel
        gtk_widget_set_sensitive(undoButton, !undoSteps.empty());
        gtk_widget_set_sensitive(redoButton, !redoSteps.empty());
        gtk_widget_set_sensitive(clearButton, layer() && !layer()->getElements().empty());
    }
}

// --- Input -----------------------------------------------------------------------------------------------------------

auto ScratchpadDrawer::toPage(double x, double y) const -> xoj::util::Point<double> {
    const double z = zoom();
    return {x / z, (y + scrollY) / z};
}

auto ScratchpadDrawer::handleEvent(GdkEvent* event) -> gboolean {
    const auto type = gdk_event_get_event_type(event);
    const auto modality = xoj::input::modalityOf(event);
    const gint64 now = g_get_monotonic_time();
    if (modality == xoj::input::Modality::Pen) {
        lastPenTime = now;
    }
    switch (type) {
        case GDK_TOUCH_BEGIN: {
            if (stroke || erasing || scrollSequence || now - lastPenTime < PALM_US) {
                return TRUE;  // a palm while writing, or a second finger
            }
            scrollSequence = gdk_event_get_event_sequence(event);
            double x = 0;
            gdk_event_get_coords(event, &x, &lastTouchY);
            momentum->begin();
            return TRUE;
        }
        case GDK_TOUCH_UPDATE: {
            if (gdk_event_get_event_sequence(event) != scrollSequence) {
                return TRUE;
            }
            double x = 0, y = 0;
            gdk_event_get_coords(event, &x, &y);
            const double dy = y - lastTouchY;
            lastTouchY = y;
            scrollBy(-dy);
            momentum->addMovement({0, dy});
            return TRUE;
        }
        case GDK_TOUCH_END:
        case GDK_TOUCH_CANCEL:
            if (gdk_event_get_event_sequence(event) == scrollSequence) {
                scrollSequence = nullptr;
                if (type == GDK_TOUCH_END) {
                    momentum->release(area);
                } else {
                    momentum->forgetMovement();
                }
            }
            return TRUE;
        case GDK_BUTTON_PRESS: {
            if (modality == xoj::input::Modality::Touch || stroke || erasing) {
                return TRUE;  // touches are handled as touch events
            }
            guint button = 0;
            gdk_event_get_button(event, &button);
            if (button != 1 && button != 2 && button != 3) {
                return FALSE;
            }
            scrollSequence = nullptr;
            momentum->stop();
            const bool erase = button != 1 || isEraserDevice(event) ||
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(eraserButton));
            beginStroke(event, erase);
            return TRUE;
        }
        case GDK_MOTION_NOTIFY:
            if (modality != xoj::input::Modality::Touch && (stroke || erasing)) {
                continueStroke(event);
            }
            return TRUE;
        case GDK_BUTTON_RELEASE:
            if (modality != xoj::input::Modality::Touch && (stroke || erasing)) {
                continueStroke(event);
                endStroke();
            }
            return TRUE;
        case GDK_GRAB_BROKEN:
            endStroke();
            scrollSequence = nullptr;
            return FALSE;
        case GDK_SCROLL: {
            double dx = 0, dy = 0;
            GdkScrollDirection direction{};
            if (gdk_event_get_scroll_deltas(event, &dx, &dy)) {
                scrollBy(dy * WHEEL_STEP);
            } else if (gdk_event_get_scroll_direction(event, &direction)) {
                scrollBy(direction == GDK_SCROLL_UP ? -WHEEL_STEP : direction == GDK_SCROLL_DOWN ? WHEEL_STEP : 0);
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

void ScratchpadDrawer::beginStroke(const GdkEvent* event, bool erase) {
    if (!layer()) {
        return;
    }
    double x = 0, y = 0;
    gdk_event_get_coords(event, &x, &y);
    const auto point = toPage(x, y);
    if (erase) {
        erasing = true;
        erased = {};
        lastErasePoint = point;
        eraseAt(point);
        return;
    }
    Tool& pen = control->getToolHandler()->getTool(TOOL_PEN);
    stroke = std::make_unique<Stroke>();
    stroke->setWidth(pen.getThickness(pen.getSize()));
    stroke->setColor(pen.getColor());
    stroke->setToolType(StrokeTool::PEN);
    double pressure = 0;
    pressureSensitive =
            control->getSettings()->isPressureSensitivity() && gdk_event_get_axis(event, GDK_AXIS_PRESSURE, &pressure);
    addPoint(event);
}

void ScratchpadDrawer::addPoint(const GdkEvent* event) {
    double x = 0, y = 0;
    gdk_event_get_coords(event, &x, &y);
    const auto point = toPage(x, y);
    double z = Point::NO_PRESSURE;
    if (pressureSensitive) {
        double pressure = 0;
        if (!gdk_event_get_axis(event, GDK_AXIS_PRESSURE, &pressure)) {
            pressure = lastPressure;
        }
        if (pressure <= 0 && stroke->getPointCount() > 0) {
            return;  // some pens report a last point without pressure when lifting
        }
        lastPressure = pressure;
        Settings* settings = control->getSettings();
        z = std::max(settings->getMinimumPressure(), pressure * settings->getPressureMultiplier()) * stroke->getWidth();
    }
    xoj::util::Point<double> previous = point;
    if (const auto count = stroke->getPointCount(); count > 0) {
        const Point& last = stroke->getPoint(count - 1);
        if (std::hypot(point.x - last.x, point.y - last.y) < MIN_POINT_DISTANCE) {
            return;
        }
        previous = {last.x, last.y};
    }
    stroke->addPoint(pressureSensitive ? Point(point.x, point.y, z) : Point(point.x, point.y));
    // Only around the new segment: the rest of the stroke is unchanged on screen
    const double scale = zoom();
    const double margin = stroke->getWidth() * scale + 4;
    const double left = std::min(previous.x, point.x) * scale - margin;
    const double top = std::min(previous.y, point.y) * scale - scrollY - margin;
    const double right = std::max(previous.x, point.x) * scale + margin;
    const double bottom = std::max(previous.y, point.y) * scale - scrollY + margin;
    gtk_widget_queue_draw_area(area, static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top)),
                               static_cast<int>(std::ceil(right - left)) + 1,
                               static_cast<int>(std::ceil(bottom - top)) + 1);
}

void ScratchpadDrawer::continueStroke(const GdkEvent* event) {
    if (stroke) {
        addPoint(event);
        return;
    }
    if (!erasing) {
        return;
    }
    double x = 0, y = 0;
    gdk_event_get_coords(event, &x, &y);
    const auto point = toPage(x, y);
    // Also between events, so a fast eraser does not jump over thin strokes
    const double step = MIN_ERASER_RADIUS;
    const double distance = std::hypot(point.x - lastErasePoint.x, point.y - lastErasePoint.y);
    const int steps = std::max(1, static_cast<int>(std::ceil(distance / step)));
    for (int i = 1; i <= steps; i++) {
        const double t = static_cast<double>(i) / steps;
        eraseAt({lastErasePoint.x + (point.x - lastErasePoint.x) * t,
                 lastErasePoint.y + (point.y - lastErasePoint.y) * t});
    }
    lastErasePoint = point;
}

void ScratchpadDrawer::endStroke() {
    if (erasing) {
        erasing = false;
        if (!erased.removed.empty()) {
            push(std::move(erased));
            fitHeight();
            markChanged();
        }
        erased = {};
        return;
    }
    if (!stroke) {
        return;
    }
    auto finished = std::move(stroke);
    if (finished->getPointCount() == 0) {
        return;
    }
    if (finished->getPointCount() == 1) {
        finished->addPoint(finished->getPoint(0));  // a dot
    }
    finished->freeUnusedPointItems();
    Element* added = finished.get();
    layer()->addElement(std::move(finished));
    UndoStep step;
    step.added.push_back(added);
    push(std::move(step));
    fitHeight();
    markChanged();
    invalidate();
}

void ScratchpadDrawer::eraseAt(xoj::util::Point<double> point) {
    Layer* l = layer();
    if (!l) {
        return;
    }
    Tool& eraser = control->getToolHandler()->getTool(TOOL_ERASER);
    const double radius = std::max(MIN_ERASER_RADIUS, eraser.getThickness(eraser.getSize()) / 2);
    std::vector<const Element*> hits;
    for (const auto& element: l->getElements()) {
        if (element->getType() == ELEMENT_STROKE &&
            element->intersectsArea(point.x - radius, point.y - radius, 2 * radius, 2 * radius) &&
            element->distanceTo(point.x, point.y) <= radius) {
            hits.push_back(element.get());
        }
    }
    for (const Element* hit: hits) {
        erased.removed.push_back(l->removeElement(hit));
    }
    if (!hits.empty()) {
        invalidate();
    }
}

void ScratchpadDrawer::push(UndoStep step) {
    undoSteps.push_back(std::move(step));
    redoSteps.clear();
    updateButtons();
}

auto ScratchpadDrawer::revert(UndoStep step) -> UndoStep {
    Layer* l = layer();
    UndoStep inverse;
    for (auto it = step.added.rbegin(); it != step.added.rend(); ++it) {
        inverse.removed.push_back(l->removeElement(*it));
    }
    // In reverse order of removal, so every element returns to its position
    for (auto it = step.removed.rbegin(); it != step.removed.rend(); ++it) {
        Element* element = it->e.get();
        l->insertElement(std::move(it->e), it->pos);
        inverse.added.push_back(element);
    }
    return inverse;
}

void ScratchpadDrawer::undo() {
    if (undoSteps.empty() || !layer() || stroke || erasing) {
        return;
    }
    auto step = std::move(undoSteps.back());
    undoSteps.pop_back();
    redoSteps.push_back(revert(std::move(step)));
    fitHeight();
    scrollBy(0);
    markChanged();
    invalidate();
    updateButtons();
}

void ScratchpadDrawer::redo() {
    if (redoSteps.empty() || !layer() || stroke || erasing) {
        return;
    }
    auto step = std::move(redoSteps.back());
    redoSteps.pop_back();
    undoSteps.push_back(revert(std::move(step)));
    fitHeight();
    scrollBy(0);
    markChanged();
    invalidate();
    updateButtons();
}

void ScratchpadDrawer::clear() {
    Layer* l = layer();
    if (!l || l->getElements().empty() || stroke || erasing) {
        return;
    }
    UndoStep step;
    while (!l->getElements().empty()) {
        step.removed.push_back(l->removeElement(l->getElements().back().get()));
    }
    push(std::move(step));
    scrollY = 0;
    fitHeight();
    markChanged();
    invalidate();
    updateButtons();
}
