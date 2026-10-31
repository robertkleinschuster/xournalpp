#include "PenFeedbackFix.h"

#ifdef _WIN32

#include <initializer_list>

#include <gdk/gdkwin32.h>
#include <windows.h>

namespace {

// From tpcshrd.h (Tablet PC platform), read by the pen service from the window property below
constexpr DWORD TABLET_DISABLE_PRESSANDHOLD = 0x00000001;
constexpr DWORD TABLET_DISABLE_PENTAPFEEDBACK = 0x00000008;
constexpr DWORD TABLET_DISABLE_PENBARRELFEEDBACK = 0x00000010;
constexpr DWORD TABLET_DISABLE_FLICKS = 0x00010000;
constexpr DWORD TABLET_DISABLE_FLICKFALLBACKKEYS = 0x00100000;
constexpr const wchar_t* TABLET_PROPERTY = L"MicrosoftTabletPenServiceProperty";

// SetWindowFeedbackSetting (Windows 8+), loaded at runtime; the FEEDBACK_TYPE values of winuser.h
using SetWindowFeedbackSettingFn = BOOL(WINAPI*)(HWND, int, DWORD, UINT32, const void*);
constexpr int FEEDBACK_PEN_BARRELVISUALIZATION = 2;
constexpr int FEEDBACK_PEN_TAP = 3;
constexpr int FEEDBACK_PEN_DOUBLETAP = 4;
constexpr int FEEDBACK_PEN_PRESSANDHOLD = 5;
constexpr int FEEDBACK_PEN_RIGHTTAP = 6;

void applyToWindow(GtkWidget* toplevel) {
    GdkWindow* window = gtk_widget_get_window(toplevel);
    if (!window || !GDK_IS_WIN32_WINDOW(window)) {
        return;
    }
    auto hwnd = static_cast<HWND>(gdk_win32_window_get_handle(window));

    const DWORD flags = TABLET_DISABLE_PRESSANDHOLD | TABLET_DISABLE_PENTAPFEEDBACK | TABLET_DISABLE_PENBARRELFEEDBACK |
                        TABLET_DISABLE_FLICKS | TABLET_DISABLE_FLICKFALLBACKKEYS;
    // The property name must be an atom for the pen service to find it
    GlobalAddAtomW(TABLET_PROPERTY);
    SetPropW(hwnd, TABLET_PROPERTY, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(flags)));

    if (auto setFeedback = reinterpret_cast<SetWindowFeedbackSettingFn>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowFeedbackSetting")))) {
        const BOOL off = FALSE;
        for (int feedback: {FEEDBACK_PEN_BARRELVISUALIZATION, FEEDBACK_PEN_TAP, FEEDBACK_PEN_DOUBLETAP,
                            FEEDBACK_PEN_PRESSANDHOLD, FEEDBACK_PEN_RIGHTTAP}) {
            setFeedback(hwnd, feedback, 0, sizeof(off), &off);
        }
    }
}

}  // namespace

void PenFeedbackFix::apply(GtkWidget* toplevel) {
    if (gtk_widget_get_realized(toplevel)) {
        applyToWindow(toplevel);
    }
    // A window can get a new native window, e.g. when it is unrealized and shown again
    g_signal_connect(toplevel, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer) { applyToWindow(widget); }),
                     nullptr);
}

#else

void PenFeedbackFix::apply(GtkWidget*) {}

#endif
