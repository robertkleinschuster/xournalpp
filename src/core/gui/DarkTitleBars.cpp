#include "DarkTitleBars.h"

#ifdef _WIN32

#include <gdk/gdkwin32.h>
#include <gtk/gtk.h>

// Windows headers last: their macros clash with names in other headers
#include <windows.h>

namespace {

/// DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1 and later)
constexpr DWORD DARK_MODE_ATTRIBUTE = 20;

using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

auto setAttribute() -> SetAttribute {
    // Loaded on demand, so the build needs no dwmapi import library
    static const auto function = []() -> SetAttribute {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<SetAttribute>(
                             reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) :
                     nullptr;
    }();
    return function;
}

auto prefersDark(GtkWidget* widget) -> bool {
    gboolean dark = false;
    g_object_get(gtk_widget_get_settings(widget), "gtk-application-prefer-dark-theme", &dark, nullptr);
    return dark;
}

void apply(GtkWidget* widget) {
    if (!GTK_IS_WINDOW(widget) || gtk_window_get_window_type(GTK_WINDOW(widget)) != GTK_WINDOW_TOPLEVEL) {
        return;
    }
    GdkWindow* window = gtk_widget_get_window(widget);
    const auto set = setAttribute();
    if (!window || !set) {
        return;
    }
    const BOOL dark = prefersDark(widget);
    set(static_cast<HWND>(gdk_win32_window_get_handle(window)), DARK_MODE_ATTRIBUTE, &dark, sizeof(dark));
}

auto onRealize(GSignalInvocationHint*, guint, const GValue* params, gpointer) -> gboolean {
    apply(GTK_WIDGET(g_value_get_object(params)));
    return true;
}

void applyToAll() {
    GList* windows = gtk_window_list_toplevels();
    for (GList* item = windows; item; item = item->next) {
        if (gtk_widget_get_realized(GTK_WIDGET(item->data))) {
            apply(GTK_WIDGET(item->data));
        }
    }
    g_list_free(windows);
}

}  // namespace

void DarkTitleBars::install() {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, onRealize, nullptr, nullptr);
    g_signal_connect(gtk_settings_get_default(), "notify::gtk-application-prefer-dark-theme",
                     G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) { applyToAll(); }), nullptr);
    applyToAll();
}

#else

void DarkTitleBars::install() {}

#endif
