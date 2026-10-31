/*
 * Xournal++
 *
 * Fork: desktop notifications for task reminders
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>  // for function
#include <string>      // for string

/**
 * Shows notifications on Windows as the balloon of a notification area icon, which Windows 10/11 shows as a toast
 * (respecting Focus Assist). The icon is only there while a notification is shown. Does nothing on other systems.
 */
class TaskNotifier {
public:
    /// onClick is called when the user clicks the notification
    explicit TaskNotifier(std::function<void()> onClick);
    ~TaskNotifier();

    TaskNotifier(const TaskNotifier&) = delete;
    TaskNotifier& operator=(const TaskNotifier&) = delete;

    void show(const std::string& title, const std::string& text);

    /// Internal: an event of the notification area icon (NIN_* or a mouse message)
    void handleIconEvent(unsigned event);

private:
    void removeIcon();
    /// The icon to show: the application's, or the generic one if it could not be loaded (HICON)
    auto appIcon() const -> void*;

    std::function<void()> onClick;
    void* window = nullptr;  ///< HWND of a hidden message window receiving the icon's events
    void* icon = nullptr;    ///< HICON
    bool iconShown = false;
};
