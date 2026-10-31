/*
 * Xournal++
 *
 * Saves the open document to its own file at moments that do not interrupt the user, like an IDE
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <gtk/gtk.h>

class Control;

/**
 * Saves the current document to its file before another document is opened (saveNow()) and when the user switches to
 * another application. It never saves while the user is working in the window: saving blocks the UI briefly, which
 * would disturb drawing. Only documents that already have a .xopp file are saved; untitled documents keep asking for
 * a location as before. The built-in autosave (a hidden recovery copy) stays responsible for crash protection.
 * On Windows it also saves after a pause without any input (pen, touch, keyboard, mouse), so changes are on disk in
 * tablet mode as well, where there is no keyboard for Ctrl+S.
 */
class DocumentAutoSaver final {
public:
    explicit DocumentAutoSaver(Control* control);
    ~DocumentAutoSaver();

    DocumentAutoSaver(const DocumentAutoSaver&) = delete;
    DocumentAutoSaver& operator=(const DocumentAutoSaver&) = delete;

    /// Saves pending changes, then calls done with whether they were saved (true right away if there is nothing to
    /// save)
    void saveNow(std::function<void(bool saved)> done);

    /// Fork: no saves in the background while paused, e.g. while a git sync may rewrite the document file
    void setPaused(bool paused) { this->paused = paused; }

private:
    /// Whether the document has unsaved changes and a file it can be saved to without asking
    bool hasChangesToSave() const;
    /// Saving ends text editing and clears the selection, so leave the document alone while they are in use
    bool isUserBusy() const;
    void saveInBackground();
    void startSave();

    Control* control;
    /// Main window, reset to nullptr by GLib once it is destroyed
    GtkWindow* window = nullptr;
    guint focusCheck = 0;
    guint idleCheck = 0;
    gulong focusHandler = 0;
    bool saving = false;
    bool paused = false;
    /// Callbacks of saveNow() waiting for the running save
    std::vector<std::function<void(bool)>> waiting;
    /// Expires with this object, so a save finishing later does not touch it
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
};
