#include "PdfPortal.h"

#include <map>      // for map
#include <utility>  // for move

#include <glib.h>  // for g_warning

#include "util/serializing/InputStreamException.h"  // for InputStreamException
#include "util/serializing/ObjectInputStream.h"     // for ObjectInputStream
#include "util/serializing/ObjectOutputStream.h"    // for ObjectOutputStream

#include "RectangularElement.h"  // for RectangularElement

namespace {
constexpr const char* APPENDIX = "PdfPortals";
}

PdfPortalPage::PdfPortalPage(std::string key, std::string data): key(std::move(key)), data(std::move(data)) {}

auto PdfPortalPage::get(const std::string& key, std::string data) -> std::shared_ptr<PdfPortalPage> {
    static std::mutex registryMutex;
    static std::map<std::string, std::weak_ptr<PdfPortalPage>> registry;

    std::lock_guard lock(registryMutex);
    if (auto it = registry.find(key); it != registry.end()) {
        if (auto page = it->second.lock()) {
            return page;
        }
    }
    // Forget pages no longer in use
    std::erase_if(registry, [](const auto& entry) { return entry.second.expired(); });
    auto page = std::make_shared<PdfPortalPage>(key, std::move(data));
    registry[key] = page;
    return page;
}

auto PdfPortalPage::fromData(std::string data) -> std::shared_ptr<PdfPortalPage> {
    gchar* hash =
            g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(data.data()), data.size());
    std::string key(hash, 32);
    g_free(hash);
    return get(key, std::move(data));
}

auto PdfPortalPage::render(cairo_t* cr, bool forPrinting) const -> bool {
    std::lock_guard lock(this->mutex);
    if (!this->pdf && !this->failed) {
        // The data outlives the document: both belong to this page
        GBytes* bytes = g_bytes_new_static(this->data.data(), this->data.size());
        GError* error = nullptr;
        this->pdf.reset(poppler_document_new_from_bytes(bytes, nullptr, &error), xoj::util::adopt);
        g_bytes_unref(bytes);
        if (!this->pdf || poppler_document_get_n_pages(this->pdf.get()) < 1) {
            g_warning("Cannot read the PDF page of an area: %s", error ? error->message : "no page");
            this->pdf.reset();
            this->failed = true;
        }
        if (error) {
            g_error_free(error);
        }
    }
    if (this->failed) {
        return false;
    }
    PopplerPage* page = poppler_document_get_page(this->pdf.get(), 0);
    if (!page) {
        return false;
    }
    if (forPrinting) {
        poppler_page_render_for_printing(page, cr);
    } else {
        poppler_page_render(page, cr);
    }
    g_object_unref(page);
    return true;
}

auto PdfPortal::draw(cairo_t* cr, double width, double height) const -> bool {
    if (!this->page || this->area.width <= 0 || this->area.height <= 0) {
        return false;
    }
    // Like TexImageView: vector targets get the printing rendering (it leaves out what is not meant to be printed)
    const auto type = cairo_surface_get_type(cairo_get_target(cr));
    const bool forPrinting = type == CAIRO_SURFACE_TYPE_PDF || type == CAIRO_SURFACE_TYPE_PS ||
                             type == CAIRO_SURFACE_TYPE_SVG || type == CAIRO_SURFACE_TYPE_SCRIPT ||
                             type == CAIRO_SURFACE_TYPE_WIN32_PRINTING || type == CAIRO_SURFACE_TYPE_XML ||
                             type == CAIRO_SURFACE_TYPE_RECORDING;

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    // White paper, like the PDF background and the snapshot
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, width / this->area.width, height / this->area.height);
    cairo_translate(cr, -this->area.x, -this->area.y);
    const bool drawn = this->page->render(cr, forPrinting);
    cairo_restore(cr);
    return drawn;
}

void PdfPortal::serializeAppendix(ObjectOutputStream& out, const std::vector<const Element*>& elements) {
    std::vector<std::pair<int, const PdfPortal*>> portals;
    for (size_t i = 0; i < elements.size(); i++) {
        if (const auto* e = dynamic_cast<const RectangularElement*>(elements[i]); e && e->getPdfPortal()) {
            portals.emplace_back(static_cast<int>(i), e->getPdfPortal().get());
        }
    }
    if (portals.empty()) {
        return;
    }
    out.writeObject(APPENDIX);
    out.writeInt(static_cast<int>(portals.size()));
    for (const auto& [index, portal]: portals) {
        out.writeInt(index);
        out.writeString(portal->key);
        out.writeDouble(portal->area.x);
        out.writeDouble(portal->area.y);
        out.writeDouble(portal->area.width);
        out.writeDouble(portal->area.height);
        out.writeImage(portal->page ? std::string_view(portal->page->getData()) : std::string_view());
    }
    out.endObject();
}

void PdfPortal::readAppendix(ObjectInputStream& in, const std::vector<Element*>& elements) {
    try {
        if (in.getNextObjectName() != APPENDIX) {
            return;
        }
    } catch (const InputStreamException&) {
        return;  // the end: copied without portals, e.g. in upstream
    }
    in.readObject(APPENDIX);
    const int count = in.readInt();
    for (int n = 0; n < count; n++) {
        const int index = in.readInt();
        auto portal = std::make_shared<PdfPortal>();
        portal->key = in.readString();
        const double x = in.readDouble();
        const double y = in.readDouble();
        const double width = in.readDouble();
        const double height = in.readDouble();
        portal->area = {x, y, width, height};
        if (std::string data = in.readImage(); !data.empty()) {
            portal->page = PdfPortalPage::get(portal->key, std::move(data));
        }
        if (index >= 0 && static_cast<size_t>(index) < elements.size()) {
            if (auto* e = dynamic_cast<RectangularElement*>(elements[static_cast<size_t>(index)])) {
                e->setPdfPortal(std::move(portal));
            }
        }
    }
    in.endObject();
}
