#include "ImageCropDialog.h"

#include <algorithm>  // for clamp, min, max
#include <cmath>      // for abs, lround

#include "gui/InputModality.h"      // for modalityOf, Modality
#include "util/i18n.h"              // for _, FS, _F
#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr

namespace {

/// Space around the image, so that the handles on its border can be grabbed
constexpr double MARGIN = 20;
/// How far from a frame edge a press grabs it (logical pixels)
constexpr double GRAB_DISTANCE = 14;
constexpr double TOUCH_GRAB_DISTANCE = 28;
/// The smallest frame, in logical pixels on screen
constexpr double MIN_FRAME = 16;

struct Frame {
    double left, top, right, bottom;  ///< image pixels
};

class Dialog {
public:
    Dialog(GtkWindow* parent, GdkPixbuf* image, const std::string& name): image(image) {
        width = gdk_pixbuf_get_width(image);
        height = gdk_pixbuf_get_height(image);
        frame = whole();

        dialog = gtk_dialog_new_with_buttons(_("Insert Image"), parent, GTK_DIALOG_MODAL, _("Cancel"),
                                             GTK_RESPONSE_CANCEL, nullptr);
        auto* insertButton = gtk_dialog_add_button(GTK_DIALOG(dialog), _("Insert"), GTK_RESPONSE_ACCEPT);
        gtk_style_context_add_class(gtk_widget_get_style_context(insertButton), "suggested-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 900, 700);

        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 12);

        auto* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto* whole = gtk_button_new_with_label(_("Whole Image"));
        auto* hint = gtk_label_new(_("Drag the corners or edges to crop, or draw a new frame."));
        gtk_label_set_line_wrap(GTK_LABEL(hint), true);
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
        gtk_box_pack_start(GTK_BOX(top), whole, false, false, 0);
        gtk_box_pack_start(GTK_BOX(top), hint, true, true, 0);
        gtk_box_pack_start(GTK_BOX(box), top, false, false, 0);

        area = gtk_drawing_area_new();
        gtk_widget_set_hexpand(area, true);
        gtk_widget_set_vexpand(area, true);
        gtk_widget_add_events(
                area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK | GDK_TOUCH_MASK);
        gtk_box_pack_start(GTK_BOX(box), area, true, true, 0);

        sizeLabel = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(sizeLabel), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(sizeLabel), "dim-label");
        gtk_box_pack_start(GTK_BOX(box), sizeLabel, false, false, 0);

        gtk_window_set_title(GTK_WINDOW(dialog), (std::string(_("Insert Image")) + " – " + name).c_str());

        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, true, true, 0);

        g_signal_connect(area, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer self) -> gboolean {
                             static_cast<Dialog*>(self)->draw(cr);
                             return true;
                         }),
                         this);
        g_signal_connect(whole, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer self) { static_cast<Dialog*>(self)->selectWhole(); }),
                         this);

        drag = gtk_gesture_drag_new(area);
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
        g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* g, double x, double y, gpointer self) {
                             static_cast<Dialog*>(self)->begin(GTK_GESTURE(g), x, y);
                         }),
                         this);
        g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag*, double dx, double dy, gpointer self) {
                             static_cast<Dialog*>(self)->update(dx, dy);
                         }),
                         this);

        gtk_widget_show_all(dialog);
        changed();
    }

    ~Dialog() {
        g_object_unref(drag);
        gtk_widget_destroy(dialog);
    }

    auto run() -> std::optional<GdkRectangle> {
        if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
            return std::nullopt;
        }
        return pixelFrame();
    }

private:
    enum Edge { LEFT = 1, TOP = 2, RIGHT = 4, BOTTOM = 8 };
    enum class Mode { None, Resize, Move, New };

    auto whole() const -> Frame { return {0, 0, static_cast<double>(width), static_cast<double>(height)}; }

    void selectWhole() {
        frame = whole();
        changed();
    }

    auto pixelFrame() const -> GdkRectangle {
        const int x = std::clamp(static_cast<int>(std::lround(frame.left)), 0, width - 1);
        const int y = std::clamp(static_cast<int>(std::lround(frame.top)), 0, height - 1);
        const int right = std::clamp(static_cast<int>(std::lround(frame.right)), x + 1, width);
        const int bottom = std::clamp(static_cast<int>(std::lround(frame.bottom)), y + 1, height);
        return {x, y, right - x, bottom - y};
    }

    /// Fits the image into the area: its scale (screen pixels per image pixel) and origin
    void layout() {
        const double w = gtk_widget_get_allocated_width(area) - 2 * MARGIN;
        const double h = gtk_widget_get_allocated_height(area) - 2 * MARGIN;
        scale = std::max(0.01, std::min({w / width, h / height, 4.0}));
        originX = (gtk_widget_get_allocated_width(area) - width * scale) / 2;
        originY = (gtk_widget_get_allocated_height(area) - height * scale) / 2;
    }

    void changed() {
        const auto r = pixelFrame();
        auto text = FS(_F("{1} × {2} pixels") % r.width % r.height);
        if (r.width < width || r.height < height) {
            text += "  ·  " + FS(_F("of {1} × {2}") % width % height);
        }
        gtk_label_set_text(GTK_LABEL(sizeLabel), text.c_str());
        gtk_widget_queue_draw(area);
    }

    void draw(cairo_t* cr) {
        layout();
        // The image, scaled once per size (a photo has many megapixels; scaling it every frame would lag)
        const int factor = gtk_widget_get_scale_factor(area);
        const int previewWidth = std::max(1, static_cast<int>(std::lround(width * scale * factor)));
        const int previewHeight = std::max(1, static_cast<int>(std::lround(height * scale * factor)));
        if (!preview || gdk_pixbuf_get_width(preview.get()) != previewWidth ||
            gdk_pixbuf_get_height(preview.get()) != previewHeight) {
            preview.reset(gdk_pixbuf_scale_simple(image, previewWidth, previewHeight, GDK_INTERP_BILINEAR),
                          xoj::util::adopt);
        }
        cairo_save(cr);
        cairo_translate(cr, originX, originY);
        cairo_scale(cr, 1.0 / factor, 1.0 / factor);
        gdk_cairo_set_source_pixbuf(cr, preview.get(), 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);

        // Darken what is cut off
        const double l = originX + frame.left * scale;
        const double t = originY + frame.top * scale;
        const double r = originX + frame.right * scale;
        const double b = originY + frame.bottom * scale;
        cairo_rectangle(cr, originX, originY, width * scale, height * scale);
        cairo_rectangle(cr, l, t, r - l, b - t);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_fill(cr);

        // The frame, visible on light and dark images
        cairo_rectangle(cr, l, t, r - l, b - t);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_set_line_width(cr, 4);
        cairo_stroke_preserve(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);

        const double mx = (l + r) / 2;
        const double my = (t + b) / 2;
        for (auto [x, y]: {std::pair{l, t}, {mx, t}, {r, t}, {r, my}, {r, b}, {mx, b}, {l, b}, {l, my}}) {
            cairo_arc(cr, x, y, 7, 0, 2 * G_PI);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
    }

    void begin(GtkGesture* gesture, double x, double y) {
        layout();
        startFrame = frame;
        startX = x;
        startY = y;
        const double grab =
                xoj::input::modalityOf(gesture) == xoj::input::Modality::Touch ? TOUCH_GRAB_DISTANCE : GRAB_DISTANCE;
        const double l = originX + frame.left * scale;
        const double t = originY + frame.top * scale;
        const double r = originX + frame.right * scale;
        const double b = originY + frame.bottom * scale;
        edges = 0;
        if (y >= t - grab && y <= b + grab) {
            // The nearer edge, if the frame is so small that both are in reach
            if (std::abs(x - l) <= grab && std::abs(x - l) <= std::abs(x - r)) {
                edges |= LEFT;
            } else if (std::abs(x - r) <= grab) {
                edges |= RIGHT;
            }
        }
        if (x >= l - grab && x <= r + grab) {
            if (std::abs(y - t) <= grab && std::abs(y - t) <= std::abs(y - b)) {
                edges |= TOP;
            } else if (std::abs(y - b) <= grab) {
                edges |= BOTTOM;
            }
        }
        if (edges) {
            mode = Mode::Resize;
        } else if (x > l && x < r && y > t && y < b) {
            mode = Mode::Move;
        } else {
            mode = Mode::New;
        }
    }

    void update(double dx, double dy) {
        const double minSize = MIN_FRAME / scale;
        const double w = width;
        const double h = height;
        switch (mode) {
            case Mode::Move: {
                const double fw = startFrame.right - startFrame.left;
                const double fh = startFrame.bottom - startFrame.top;
                const double left = std::clamp(startFrame.left + dx / scale, 0.0, w - fw);
                const double top = std::clamp(startFrame.top + dy / scale, 0.0, h - fh);
                frame = {left, top, left + fw, top + fh};
                break;
            }
            case Mode::Resize:
                frame = startFrame;
                if (edges & LEFT) {
                    frame.left = std::clamp(startFrame.left + dx / scale, 0.0, std::max(0.0, frame.right - minSize));
                }
                if (edges & RIGHT) {
                    frame.right = std::clamp(startFrame.right + dx / scale, std::min(w, frame.left + minSize), w);
                }
                if (edges & TOP) {
                    frame.top = std::clamp(startFrame.top + dy / scale, 0.0, std::max(0.0, frame.bottom - minSize));
                }
                if (edges & BOTTOM) {
                    frame.bottom = std::clamp(startFrame.bottom + dy / scale, std::min(h, frame.top + minSize), h);
                }
                break;
            case Mode::New: {
                // Ignore the jitter of a tap outside the frame
                if (std::abs(dx) < xoj::input::DRAG_THRESHOLD && std::abs(dy) < xoj::input::DRAG_THRESHOLD) {
                    return;
                }
                const double ax = std::clamp((startX - originX) / scale, 0.0, w);
                const double ay = std::clamp((startY - originY) / scale, 0.0, h);
                const double bx = std::clamp((startX + dx - originX) / scale, 0.0, w);
                const double by = std::clamp((startY + dy - originY) / scale, 0.0, h);
                if (std::abs(bx - ax) < minSize || std::abs(by - ay) < minSize) {
                    return;
                }
                frame = {std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
                break;
            }
            case Mode::None:
                return;
        }
        changed();
    }

    GdkPixbuf* image;
    int width{};
    int height{};
    Frame frame{};
    Frame startFrame{};
    double startX{};
    double startY{};
    Mode mode = Mode::None;
    int edges = 0;

    double scale = 1;
    double originX = 0;
    double originY = 0;
    xoj::util::GObjectSPtr<GdkPixbuf> preview;

    GtkWidget* dialog{};
    GtkWidget* area{};
    GtkWidget* sizeLabel{};
    GtkGesture* drag{};
};

}  // namespace

auto ImageCropDialog::run(GtkWindow* parent, GdkPixbuf* image, const std::string& name) -> std::optional<GdkRectangle> {
    Dialog dialog(parent, image, name);
    return dialog.run();
}
