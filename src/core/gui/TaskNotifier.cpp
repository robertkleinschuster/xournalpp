#include "TaskNotifier.h"

#include <utility>  // for move

#include <glib.h>  // for g_utf8_to_utf16

#ifdef _WIN32
#include <windows.h>
// windows.h first
#include <shellapi.h>
#endif

#ifdef _WIN32

namespace {

constexpr UINT ICON_ID = 1;
constexpr UINT ICON_MESSAGE = WM_APP + 41;
constexpr const wchar_t* WINDOW_CLASS = L"XournalStudioTaskNotifier";

LRESULT CALLBACK notifierProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == ICON_MESSAGE) {
        if (auto* notifier = reinterpret_cast<TaskNotifier*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
            // NOTIFYICON_VERSION_4: the event is in the low word
            notifier->handleIconEvent(LOWORD(lParam));
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/// Copies UTF-8 text into a fixed size UTF-16 field of NOTIFYICONDATAW, shortened if needed
template <size_t N>
void copyText(wchar_t (&target)[N], const std::string& text) {
    gunichar2* wide = g_utf8_to_utf16(text.c_str(), -1, nullptr, nullptr, nullptr);
    if (!wide) {
        target[0] = 0;
        return;
    }
    size_t i = 0;
    for (; i + 1 < N && wide[i]; i++) {
        target[i] = static_cast<wchar_t>(wide[i]);
    }
    target[i] = 0;
    g_free(wide);
}

auto iconData(HWND window) -> NOTIFYICONDATAW {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = ICON_ID;
    return data;
}

}  // namespace

TaskNotifier::TaskNotifier(std::function<void()> onClick): onClick(std::move(onClick)) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = notifierProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&windowClass);  // fails harmlessly if already registered
    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    window = hwnd;
    // The application icon from the executable
    wchar_t exe[MAX_PATH];
    HICON large = nullptr;
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        ExtractIconExW(exe, 0, &large, nullptr, 1);
    }
    icon = large;  // owned, unlike the fallback in show()
}

TaskNotifier::~TaskNotifier() {
    removeIcon();
    if (window) {
        SetWindowLongPtrW(static_cast<HWND>(window), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(window));
    }
    if (icon) {
        DestroyIcon(static_cast<HICON>(icon));
    }
}

auto TaskNotifier::appIcon() const -> void* {
    // IDI_APPLICATION as a wide resource id: the icon of the executable could not be loaded
    return icon ? icon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

void TaskNotifier::show(const std::string& title, const std::string& text) {
    if (!window) {
        return;
    }
    auto data = iconData(static_cast<HWND>(window));
    if (!iconShown) {
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = ICON_MESSAGE;
        data.hIcon = static_cast<HICON>(appIcon());
        copyText(data.szTip, "Xournal Studio");
        if (!Shell_NotifyIconW(NIM_ADD, &data)) {
            return;
        }
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        iconShown = true;
    }
    data = iconData(static_cast<HWND>(window));
    data.uFlags = NIF_INFO;
    copyText(data.szInfoTitle, title);
    copyText(data.szInfo, text);
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    data.hBalloonIcon = static_cast<HICON>(appIcon());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TaskNotifier::handleIconEvent(unsigned event) {
    switch (event) {
        case NIN_BALLOONUSERCLICK:
        case WM_LBUTTONUP:
        case NIN_SELECT:
            removeIcon();
            if (onClick) {
                onClick();
            }
            break;
        case NIN_BALLOONTIMEOUT:
            removeIcon();  // the notification went to the notification center
            break;
        default:
            break;
    }
}

void TaskNotifier::removeIcon() {
    if (!iconShown || !window) {
        return;
    }
    auto data = iconData(static_cast<HWND>(window));
    Shell_NotifyIconW(NIM_DELETE, &data);
    iconShown = false;
}

#else

TaskNotifier::TaskNotifier(std::function<void()> onClick): onClick(std::move(onClick)) {}
TaskNotifier::~TaskNotifier() = default;
void TaskNotifier::show(const std::string&, const std::string&) {}
void TaskNotifier::handleIconEvent(unsigned) {}
void TaskNotifier::removeIcon() {}
auto TaskNotifier::appIcon() const -> void* { return nullptr; }

#endif
