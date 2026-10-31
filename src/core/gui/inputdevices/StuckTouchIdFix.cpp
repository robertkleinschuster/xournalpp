#include "StuckTouchIdFix.h"

#ifdef _WIN32
#include <map>

#include <gdk/gdk.h>

// Windows headers last: their macros clash with names in Xournal++ headers
#include <windows.h>

namespace {

/// Touch pointer id -> how often GDK has put it on its list of ignored interactions (minus the times it removed it)
std::map<UINT32, UINT32> ignoredIds;
/// Set while GDK handles one of our WM_POINTERLEAVE messages
bool sendingLeave = false;

auto isTouch(UINT32 pointerId) -> bool {
    POINTER_INPUT_TYPE type = PT_POINTER;
    return GetPointerType(pointerId, &type) && type == PT_TOUCH;
}

/// The number of entries GDK adds for a cancelled message: one per pointer frame in the message's history
auto historyCount(UINT32 pointerId) -> UINT32 {
    UINT32 count = 0;
    if (!GetPointerInfoHistory(pointerId, &count, nullptr) || count == 0) {
        return 1;
    }
    return count;
}

auto filter(GdkXEvent* xevent, GdkEvent*, gpointer) -> GdkFilterReturn {
    const MSG* msg = static_cast<MSG*>(xevent);
    switch (msg->message) {
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        case WM_POINTERUPDATE:
        case WM_POINTERENTER:
        case WM_POINTERLEAVE:
            break;
        default:
            return GDK_FILTER_CONTINUE;
    }
    if (sendingLeave) {
        return GDK_FILTER_CONTINUE;
    }
    const UINT32 id = GET_POINTERID_WPARAM(msg->wParam);
    if (!isTouch(id)) {
        return GDK_FILTER_CONTINUE;
    }

    const bool rejected = IS_POINTER_CANCELED_WPARAM(msg->wParam) || !HAS_POINTER_CONFIDENCE_WPARAM(msg->wParam);
    if (rejected) {
        // GDK is about to ignore this touch (again), see gdk_winpointer_input_events()
        ignoredIds[id] += historyCount(id);
        return GDK_FILTER_CONTINUE;
    }

    auto it = ignoredIds.find(id);
    if (it == ignoredIds.end()) {
        return GDK_FILTER_CONTINUE;
    }
    if (msg->message == WM_POINTERLEAVE) {
        // GDK removes one entry itself
        if (--it->second == 0) {
            ignoredIds.erase(it);
        }
        return GDK_FILTER_CONTINUE;
    }
    if (msg->message != WM_POINTERDOWN) {
        return GDK_FILTER_CONTINUE;
    }

    // A new finger with an id GDK still ignores: GDK forgets one entry per WM_POINTERLEAVE. "In range" without the
    // primary flag makes GDK skip everything else for the message (no events, no crossing).
    g_message("Touch input: touch id %u was still ignored by GDK, re-enabling it (%u entries)", id, it->second);
    const WPARAM leave = MAKEWPARAM(id, POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_CONFIDENCE);
    sendingLeave = true;
    for (UINT32 i = 0; i < it->second; i++) {
        SendMessageW(msg->hwnd, WM_POINTERLEAVE, leave, msg->lParam);
    }
    sendingLeave = false;
    ignoredIds.erase(it);
    return GDK_FILTER_CONTINUE;
}

}  // namespace

void StuckTouchIdFix::install() { gdk_window_add_filter(nullptr, filter, nullptr); }

#else

void StuckTouchIdFix::install() {}

#endif
