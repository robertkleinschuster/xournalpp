#include "ImageHandler.h"

#include <algorithm>  // for min
#include <fstream>
#include <iterator>  // for istreambuf_iterator
#include <memory>    // for __shared_ptr_access, make...
#include <set>       // for set
#include <sstream>
#include <string>   // for string
#include <utility>  // for operator==, pair

#include <glib-object.h>  // for g_object_unref
#include <glib.h>         // for g_error_free, g_free, GError

#include "control/Control.h"              // for Control
#include "control/tools/EditSelection.h"  // for EditSelection
#include "gui/MainWindow.h"               // for MainWindow
#include "gui/PageView.h"                 // for XojPageView
#include "gui/XournalView.h"              // for XournalView
#include "gui/dialog/ImageCropDialog.h"   // for ImageCropDialog
#include "gui/dialog/XojOpenDlg.h"        // for showOpenImageDialog
#include "model/Image.h"
#include "model/Layer.h"            // for Layer
#include "model/PageRef.h"          // for PageRef
#include "model/XojPage.h"          // for XojPage
#include "undo/InsertUndoAction.h"  // for InsertUndoAction
#include "undo/UndoRedoHandler.h"   // for UndoRedoHandler
#include "util/PathUtil.h"          // for toUtf8
#include "util/StringUtils.h"       // for StringUtils
#include "util/XojMsgBox.h"         // for XojMsgBox
#include "util/i18n.h"              // for _
#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr.h

ImageHandler::ImageHandler(Control* control): control(control) {}

ImageHandler::~ImageHandler() = default;


void ImageHandler::chooseAndCreateImage(std::function<void(std::unique_ptr<Image>)> callback) {
    xoj::OpenDlg::showOpenImageDialog(control->getGtkWindow(), control->getSettings(),
                                      [cb = std::move(callback), ctrl = control](fs::path p, bool) {
                                          auto img = ImageHandler::createImageFromFile(p);

                                          if (!img) {
                                              XojMsgBox::showErrorToUser(ctrl->getGtkWindow(),
                                                                         _("Failed to load image"));
                                              return;
                                          }
                                          cb(std::move(img));
                                      });
}

auto ImageHandler::createImageFromFile(const fs::path& p) -> std::unique_ptr<Image> {

    auto fileToString = [](const fs::path& p) {
        // This is the faster file dump I could come up with. Faster by 20% than g_file_load_contents (with -O3)
        std::ifstream stream(p, std::ios::binary);

        auto pos = stream.tellg();
        stream.seekg(0, std::ios_base::end);  // Go to the end
        auto size = stream.tellg() - pos;     // Get the size
        stream.seekg(pos);                    // Go back

        std::string s(as_unsigned(size), 0);  // Allocate
        stream.read(&s[0], size);             // Dump
        return s;
    };

    auto img = std::make_unique<Image>();
    try {
        img->setImage(fileToString(p));
    } catch (const std::ios_base::failure& e) {
        std::stringstream msg;
        msg << _("Error while opening image file: ") << p.string() << '\n'
            << "Error code: " << e.code() << '\n'
            << "Explanatory string: " << e.what();
        XojMsgBox::showErrorToUser(nullptr, msg.str());
        return nullptr;
    }
    // Render the image.
    if (auto opt = img->renderBuffer(); opt.has_value()) {
        // An error occurred
        XojMsgBox::showErrorToUser(nullptr, opt.value());
        return nullptr;
    }

    return img;
}

bool ImageHandler::addImageToDocument(std::unique_ptr<Image> img, PageRef page, Control* control, bool addUndoAction) {
    Layer* layer = page->getSelectedLayer();

    if (addUndoAction) {
        control->getUndoRedoHandler()->addUndoAction(std::make_unique<InsertUndoAction>(page, layer, img.get()));
    }

    XournalView* xournal = control->getWindow()->getXournal();
    auto pageNr = xournal->getCurrentPage();
    auto* view = xournal->getViewFor(pageNr);

    if (view->getPage() != page) {
        g_warning("Active page changed while you selected the image. Aborting.");
        return false;
    }

    auto sel = SelectionFactory::createFromFloatingElement(control, page, layer, view, std::move(img));
    control->getWindow()->getXournal()->setSelection(sel.release());
    return true;
}

static inline double getScalingToFitInPage(const xoj::util::Rectangle<double>& frame, PageRef page) {
    if (frame.x + frame.width > page->getWidth() || frame.y + frame.height > page->getHeight()) {
        double const maxScalingX = (page->getWidth() - frame.x) / frame.width;
        double const maxScalingY = (page->getHeight() - frame.y) / frame.height;
        return std::min(maxScalingX, maxScalingY);
    }
    return 1.;
}

void ImageHandler::automaticScaling(Image& img, const xoj::util::Point<double>& p, PageRef page) {
    double scaling = getScalingToFitInPage({p, img.getNaturalSize()}, page);
    img.setTransformation({scaling, 0, 0, scaling, p});
}

void ImageHandler::automaticScaling(Image& img, const xoj::util::Rectangle<double>& frame, PageRef page) {
    double scaling = getScalingToFitInPage(frame, page);

    if (auto [width, height] = img.getNaturalSize(); width > 0 && height > 0) [[likely]] {
        img.setTransformation(
                {scaling * frame.width / width, 0, 0, scaling * frame.height / height, frame.getOrigin()});
    } else {
        img.setTransformation({scaling, 0, 0, scaling, frame.getOrigin()});
    }
}

void ImageHandler::insertImageWithSize(PageRef page, const xoj::util::Rectangle<double>& space) {
    chooseAndCreateImage([space, page, ctrl = control](std::unique_ptr<Image> img) {
        xoj_assert(img);

        if (static_cast<int>(space.width) != 0 && static_cast<int>(space.height) != 0) {
            // scale down
            auto [width, height] = img->getNaturalSize();
            const double scaling = std::min(space.height / height, space.width / width);

            // center
            xoj::util::Point<double> move = space.getOrigin();
            if (scaling * height < space.height) {
                move.y += (space.height - scaling * height) * 0.5;
            }
            if (scaling * width < space.width) {
                move.x += (space.width - scaling * width) * 0.5;
            }

            img->setTransformation({scaling, 0, 0, scaling, move});
        } else {
            // zero space is selected, scale original image size down to fit on the page
            automaticScaling(*img, space.getOrigin(), page);
        }
        addImageToDocument(std::move(img), page, ctrl, true);
    });
}

auto ImageHandler::isImageFile(const fs::path& path) -> bool {
    // The extensions of the formats GdkPixbuf reads, collected once: the explorer asks for every file it shows
    static const std::set<std::string> extensions = [] {
        std::set<std::string> result;
        GSList* formats = gdk_pixbuf_get_formats();
        for (GSList* f = formats; f; f = f->next) {
            auto* format = static_cast<GdkPixbufFormat*>(f->data);
            if (gdk_pixbuf_format_is_disabled(format)) {
                continue;
            }
            gchar** names = gdk_pixbuf_format_get_extensions(format);
            for (gchar** e = names; e && *e; e++) {
                result.insert(StringUtils::toLowerCase(*e));
            }
            g_strfreev(names);
        }
        g_slist_free(formats);
        return result;
    }();
    const auto ext = Util::toUtf8(path.extension());
    return ext.size() > 1 && extensions.count(StringUtils::toLowerCase(ext.substr(1)));
}

void ImageHandler::insertCroppedFromFile(Control* control, const fs::path& path) {
    if (control->isReadOnly()) {
        control->notifyReadOnly();
        return;
    }
    if (control->getCurrentPageNo() == npos) {
        return;
    }
    GtkWindow* parent = control->getGtkWindow();
    std::string data;
    {
        std::ifstream stream(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (!stream.good() && !stream.eof()) {
            data.clear();
        }
    }
    xoj::util::GObjectSPtr<GdkPixbuf> pixbuf;
    if (!data.empty()) {
        xoj::util::GObjectSPtr<GdkPixbufLoader> loader(gdk_pixbuf_loader_new(), xoj::util::adopt);
        if (gdk_pixbuf_loader_write(loader.get(), reinterpret_cast<const guchar*>(data.data()), data.size(), nullptr) &&
            gdk_pixbuf_loader_close(loader.get(), nullptr) && gdk_pixbuf_loader_get_pixbuf(loader.get())) {
            // As shown by Image::renderBuffer, which applies the orientation too
            pixbuf.reset(gdk_pixbuf_apply_embedded_orientation(gdk_pixbuf_loader_get_pixbuf(loader.get())),
                         xoj::util::adopt);
        }
    }
    if (!pixbuf) {
        XojMsgBox::showErrorToUser(parent,
                                   FS(_F("“{1}” could not be read as an image.") % Util::toUtf8(path.filename())));
        return;
    }

    auto area = ImageCropDialog::run(parent, pixbuf.get(), Util::toUtf8(path.filename()));
    if (!area) {
        return;
    }
    const int width = gdk_pixbuf_get_width(pixbuf.get());
    const int height = gdk_pixbuf_get_height(pixbuf.get());
    if (area->x != 0 || area->y != 0 || area->width != width || area->height != height) {
        // Store only the chosen part: as JPEG for photos (PNG would be many times larger), else as PNG
        xoj::util::GObjectSPtr<GdkPixbuf> cropped(
                gdk_pixbuf_new_subpixbuf(pixbuf.get(), area->x, area->y, area->width, area->height), xoj::util::adopt);
        GdkPixbufFormat* format = gdk_pixbuf_get_file_info(Util::toUtf8(path).c_str(), nullptr, nullptr);
        gchar* formatName = format ? gdk_pixbuf_format_get_name(format) : nullptr;
        const bool jpeg =
                formatName && std::string_view(formatName) == "jpeg" && !gdk_pixbuf_get_has_alpha(cropped.get());
        g_free(formatName);
        gchar* buffer = nullptr;
        gsize size = 0;
        const bool saved = jpeg ? gdk_pixbuf_save_to_buffer(cropped.get(), &buffer, &size, "jpeg", nullptr, "quality",
                                                            "92", nullptr) :
                                  gdk_pixbuf_save_to_buffer(cropped.get(), &buffer, &size, "png", nullptr, nullptr);
        if (!saved) {
            XojMsgBox::showErrorToUser(parent, _("Failed to load image"));
            return;
        }
        data.assign(buffer, size);
        g_free(buffer);
    }
    // Else the file itself, unchanged

    auto img = std::make_unique<Image>();
    img->setImage(std::move(data));
    if (auto error = img->renderBuffer(); error.has_value()) {
        XojMsgBox::showErrorToUser(parent, error.value());
        return;
    }
    control->pasteImage(std::move(img));
}
