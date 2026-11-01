#include "ConvertibleSupport.h"

#ifdef _WIN32

#include <cwchar>
#include <string>

#include <gtk/gtk.h>

#include "control/Control.h"
#include "control/settings/Settings.h"
#include "gui/InputModality.h"  // for modalityOf
#include "gui/MainWindow.h"

#ifdef _WIN32  // separate block, so it stays last: its macros clash with names in the headers above
#include <windows.h>
#endif

namespace {

// --- Touch keyboard ---

/// The touch keyboard's COM interface (not in MinGW's headers)
struct ITipInvocation: public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle(HWND window) = 0;
};
constexpr CLSID CLSID_UIHostNoLaunch = {0x4ce576fa, 0x83dc, 0x4f88, {0x95, 0x1c, 0x9d, 0x07, 0x82, 0xb4, 0xe3, 0x76}};
constexpr IID IID_ITipInvocation = {0x37c994e7, 0x432b, 0x4834, {0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b}};

/// Whether the touch keyboard is shown: its window (of TextInputHost.exe) exists while hidden, but is cloaked then
auto touchKeyboardVisible() -> bool {
    using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static const auto dwmGetWindowAttribute = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmGetWindowAttributeFn>(
                             reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetWindowAttribute"))) :
                     nullptr;
    }();
    bool visible = false;
    EnumWindows(
            +[](HWND hwnd, LPARAM data) -> BOOL {
                wchar_t className[64];
                if (!GetClassNameW(hwnd, className, 64) || std::wcscmp(className, L"Windows.UI.Core.CoreWindow") != 0 ||
                    !IsWindowVisible(hwnd)) {
                    return TRUE;
                }
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!process) {
                    return TRUE;
                }
                wchar_t image[MAX_PATH];
                DWORD size = MAX_PATH;
                const bool inputHost = QueryFullProcessImageNameW(process, 0, image, &size) &&
                                       std::wstring(image, size).ends_with(L"\\TextInputHost.exe");
                CloseHandle(process);
                if (!inputHost) {
                    return TRUE;
                }
                int cloaked = 0;
                const auto get = dwmGetWindowAttribute;
                if (get && SUCCEEDED(get(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked) {
                    return TRUE;
                }
                *reinterpret_cast<bool*>(data) = true;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&visible));
    return visible;
}

void toggleTouchKeyboard() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // GTK's thread is usually initialized already
    ITipInvocation* tip = nullptr;
    HRESULT result = CoCreateInstance(CLSID_UIHostNoLaunch, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER,
                                      IID_ITipInvocation, reinterpret_cast<void**>(&tip));
    if (FAILED(result)) {
        // The keyboard service is started on demand; show the keyboard once it runs
        ShellExecuteW(nullptr, L"open", L"C:\\Program Files\\Common Files\\microsoft shared\\ink\\TabTip.exe", nullptr,
                      nullptr, SW_SHOWNOACTIVATE);
        static int retries = 0;
        if (retries++ < 5) {
            g_timeout_add(
                    300,
                    +[](gpointer) -> gboolean {
                        if (!touchKeyboardVisible()) {
                            toggleTouchKeyboard();
                        }
                        return G_SOURCE_REMOVE;
                    },
                    nullptr);
        }
        return;
    }
    tip->Toggle(GetDesktopWindow());
    tip->Release();
}

/// Shows the touch keyboard if it is not shown
void showTouchKeyboard() {
    // Focusing a field by a tap reports both the tap and the focus; the keyboard takes a moment to appear, so a second
    // request right after the first would toggle it off again
    static gint64 lastRequest = 0;
    const gint64 now = g_get_monotonic_time();
    if (now - lastRequest < G_TIME_SPAN_SECOND) {
        return;
    }
    lastRequest = now;
    if (!touchKeyboardVisible()) {
        toggleTouchKeyboard();
    }
}

// --- State ---

struct State {
    MainWindow* win;
    Control* control;
    bool tablet = false;
    bool portrait = false;
    bool sidebarHidden = false;  ///< hidden by us for portrait orientation
    bool keyboardShown = false;  ///< opened by us
    guint keyboardHideSource = 0;
    gulong focusInHook = 0;
    gulong focusOutHook = 0;
    gulong pressHook = 0;
};

State* state = nullptr;  // one main window

auto isTabletMode() -> bool { return GetSystemMetrics(SM_CONVERTIBLESLATEMODE) == 0; }

/// In tablet mode, the controls used most get a larger area to hit with a finger (icons and text keep their size):
/// toolbar buttons, sidebar tabs, menu and popover entries (also the explorer's context menu) and tree rows
constexpr const char* TABLET_CSS = R"css(
toolbar button {
    min-width: 32px;
    min-height: 44px;
}
#bxSidebarTopActions button,
#buttonCloseSidebar {
    min-width: 40px;
    min-height: 40px;
}
menu menuitem,
popover modelbutton {
    min-height: 40px;
}
treeview {
    -GtkTreeView-vertical-separator: 12;
}
)css";

/**
 * The window's layout changes (tablet mode, orientation): close open menus and popovers (context menus, menu bar,
 * toolbar dropdowns) instead of leaving them at a position and size that no longer fit, as Windows does on display
 * changes. Dialogs stay open.
 */
void closeTransientPopups() {
    for (int i = 0; i < 8; i++) {  // nested menus each hold a grab, e.g. a submenu and the menu bar
        GtkWidget* grab = gtk_grab_get_current();
        if (GTK_IS_POPOVER(grab)) {
            gtk_popover_popdown(GTK_POPOVER(grab));
        } else if (GTK_IS_MENU_SHELL(grab)) {
            gtk_menu_shell_cancel(GTK_MENU_SHELL(grab));
        } else {
            return;
        }
        if (gtk_grab_get_current() == grab) {
            return;  // e.g. a popover fading out, which keeps its grab until it is hidden
        }
    }
}

/// The keyboard was opened by us for tablet mode: close it when leaving tablet mode
void releaseTouchKeyboard() {
    if (state->keyboardHideSource) {
        g_source_remove(state->keyboardHideSource);
        state->keyboardHideSource = 0;
    }
    if (state->keyboardShown && touchKeyboardVisible()) {
        toggleTouchKeyboard();
    }
    state->keyboardShown = false;
}

void updateTabletMode() {
    const bool tablet = isTabletMode();
    if (tablet != state->tablet) {
        closeTransientPopups();
        if (!tablet) {
            releaseTouchKeyboard();
        }
    }
    static GtkCssProvider* provider = [] {
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, TABLET_CSS, -1, nullptr);
        return css;
    }();
    // Fork: for the whole screen, so that menus and popovers (own windows) are included
    if (tablet && !state->tablet) {
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else if (!tablet && state->tablet) {
        gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider));
    }
    state->tablet = tablet;
}

void updateOrientation(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const bool portrait = height > width;
    if (portrait == state->portrait) {
        return;
    }
    state->portrait = portrait;
    closeTransientPopups();
    if (portrait && state->win->isSidebarVisible()) {
        state->win->setSidebarVisible(false);
        state->sidebarHidden = true;
    } else if (!portrait && state->sidebarHidden) {
        state->win->setSidebarVisible(state->control->getSettings()->isSidebarVisible());
        state->sidebarHidden = false;
    }
}

auto isTextField(GtkWidget* widget) -> bool {
    return (GTK_IS_ENTRY(widget) && gtk_editable_get_editable(GTK_EDITABLE(widget))) ||
           (GTK_IS_TEXT_VIEW(widget) && gtk_text_view_get_editable(GTK_TEXT_VIEW(widget)));
}

/// The field was focused with a finger or the pen (e.g. a tap on it, or on the button opening its dialog)
auto focusedByTouchOrPen() -> bool {
    GdkEvent* event = gtk_get_current_event();
    if (!event) {
        return false;
    }
    const bool touchOrPen = xoj::input::modalityOf(event) != xoj::input::Modality::Mouse;
    gdk_event_free(event);
    return touchOrPen;
}

auto onFocusIn(GSignalInvocationHint*, guint, const GValue* params, gpointer) -> gboolean {
    auto* widget = GTK_WIDGET(g_value_get_object(&params[0]));
    if (!state || !isTextField(widget)) {
        return TRUE;
    }
    if (state->keyboardHideSource) {
        // Focus moved from one field to the next: keep the keyboard
        g_source_remove(state->keyboardHideSource);
        state->keyboardHideSource = 0;
    }
    if (state->tablet && focusedByTouchOrPen()) {
        showTouchKeyboard();
        state->keyboardShown = true;
    }
    return TRUE;
}

/// A tap into a field that has the focus already: the keyboard may have been closed meanwhile, open it again
auto onButtonPress(GSignalInvocationHint*, guint, const GValue* params, gpointer) -> gboolean {
    auto* widget = GTK_WIDGET(g_value_get_object(&params[0]));
    if (!state || !state->tablet || !isTextField(widget) || !gtk_widget_has_focus(widget) || !focusedByTouchOrPen()) {
        return TRUE;
    }
    if (state->keyboardHideSource) {
        g_source_remove(state->keyboardHideSource);
        state->keyboardHideSource = 0;
    }
    showTouchKeyboard();
    state->keyboardShown = true;
    return TRUE;
}

auto onFocusOut(GSignalInvocationHint*, guint, const GValue* params, gpointer) -> gboolean {
    auto* widget = GTK_WIDGET(g_value_get_object(&params[0]));
    if (!state || !state->keyboardShown || !isTextField(widget) || state->keyboardHideSource) {
        return TRUE;
    }
    state->keyboardHideSource = g_timeout_add(
            200,
            +[](gpointer) -> gboolean {
                state->keyboardHideSource = 0;
                state->keyboardShown = false;
                if (touchKeyboardVisible()) {
                    toggleTouchKeyboard();
                }
                return G_SOURCE_REMOVE;
            },
            nullptr);
    return TRUE;
}

auto messageFilter(GdkXEvent* xevent, GdkEvent*, gpointer) -> GdkFilterReturn {
    const auto* msg = static_cast<MSG*>(static_cast<void*>(xevent));
    if (state && msg->message == WM_SETTINGCHANGE && msg->lParam &&
        std::wcscmp(reinterpret_cast<const wchar_t*>(msg->lParam), L"ConvertibleSlateMode") == 0) {
        g_idle_add(
                +[](gpointer) -> gboolean {
                    if (state) {
                        updateTabletMode();
                    }
                    return G_SOURCE_REMOVE;
                },
                nullptr);
    }
    return GDK_FILTER_CONTINUE;
}

}  // namespace

void ConvertibleSupport::textEditingStarted() {
    if (state && state->tablet && focusedByTouchOrPen()) {
        if (state->keyboardHideSource) {
            g_source_remove(state->keyboardHideSource);
            state->keyboardHideSource = 0;
        }
        showTouchKeyboard();
        state->keyboardShown = true;
    }
}

void ConvertibleSupport::textEditingEnded() {
    if (state && state->keyboardShown && touchKeyboardVisible()) {
        toggleTouchKeyboard();
    }
    if (state) {
        state->keyboardShown = false;
    }
}

void ConvertibleSupport::install(MainWindow* win, Control* control) {
    if (state) {
        return;
    }
    state = new State{win, control};
    GtkWidget* window = win->getWindow();

    // Once the window is set up: startup applies the menu bar and sidebar settings itself
    g_idle_add_full(
            G_PRIORITY_LOW,
            +[](gpointer) -> gboolean {
                if (state) {
                    updateTabletMode();
                }
                return G_SOURCE_REMOVE;
            },
            nullptr, nullptr);
    gdk_window_add_filter(nullptr, messageFilter, nullptr);

    g_signal_connect(window, "configure-event", G_CALLBACK(+[](GtkWidget*, GdkEventConfigure* event, gpointer) {
                         if (state) {
                             updateOrientation(event->width, event->height);
                         }
                         return FALSE;
                     }),
                     nullptr);

    state->focusInHook = g_signal_add_emission_hook(g_signal_lookup("focus-in-event", GTK_TYPE_WIDGET), 0, onFocusIn,
                                                    nullptr, nullptr);
    state->focusOutHook = g_signal_add_emission_hook(g_signal_lookup("focus-out-event", GTK_TYPE_WIDGET), 0, onFocusOut,
                                                     nullptr, nullptr);
    state->pressHook = g_signal_add_emission_hook(g_signal_lookup("button-press-event", GTK_TYPE_WIDGET), 0,
                                                  onButtonPress, nullptr, nullptr);

    g_signal_connect(
            window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
                gdk_window_remove_filter(nullptr, messageFilter, nullptr);
                g_signal_remove_emission_hook(g_signal_lookup("focus-in-event", GTK_TYPE_WIDGET), state->focusInHook);
                g_signal_remove_emission_hook(g_signal_lookup("focus-out-event", GTK_TYPE_WIDGET), state->focusOutHook);
                g_signal_remove_emission_hook(g_signal_lookup("button-press-event", GTK_TYPE_WIDGET), state->pressHook);
                if (state->keyboardHideSource) {
                    g_source_remove(state->keyboardHideSource);
                }
                delete state;
                state = nullptr;
            }),
            nullptr);
}

#else

void ConvertibleSupport::install(MainWindow*, Control*) {}
void ConvertibleSupport::textEditingStarted() {}
void ConvertibleSupport::textEditingEnded() {}

#endif
