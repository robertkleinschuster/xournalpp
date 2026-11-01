#include "InputModality.h"

namespace xoj::input {

auto modalityOf(const GdkEvent* event) -> Modality {
    GdkDevice* device = event ? gdk_event_get_source_device(event) : nullptr;
    if (!device) {
        return Modality::Mouse;
    }
    switch (gdk_device_get_source(device)) {
        case GDK_SOURCE_TOUCHSCREEN:
            return Modality::Touch;
        case GDK_SOURCE_PEN:
        case GDK_SOURCE_ERASER:
            return Modality::Pen;
        default:
            return Modality::Mouse;
    }
}

auto modalityOf(GtkGesture* gesture) -> Modality {
    GdkEventSequence* sequence = GTK_IS_GESTURE_SINGLE(gesture) ?
                                         gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture)) :
                                         gtk_gesture_get_last_updated_sequence(gesture);
    return modalityOf(gtk_gesture_get_last_event(gesture, sequence));
}

}  // namespace xoj::input
