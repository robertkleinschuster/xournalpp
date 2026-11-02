/*
 * Xournal++
 *
 * Fork: choosing the part of an image to insert (cropping), by mouse, pen and touch
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <optional>  // for optional
#include <string>    // for string

#include <gtk/gtk.h>  // for GtkWindow, GdkPixbuf, GdkRectangle

namespace ImageCropDialog {

/// Shows the image (modal) with a crop frame covering all of it; the chosen area in image pixels, nullopt if cancelled
std::optional<GdkRectangle> run(GtkWindow* parent, GdkPixbuf* image, const std::string& name);

}  // namespace ImageCropDialog
