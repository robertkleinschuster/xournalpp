/*
 * Xournal++
 *
 * Saves the open document to its own file automatically, like an IDE
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <gtk/gtk.h>

#include "undo/UndoRedoHandler.h"

class Control;

/**
 * Saves the current document to its file a few seconds after the last change and when the main window loses focus.
 * Only documents that already have a .xopp file are saved; untitled documents keep asking for a location as before.
 *
 * Unlike the built-in autosave, which writes a hidden recovery copy, this updates the document file itself.
 */
class DocumentAutoSaver final: public UndoRedoListener {
public:
    explicit DocumentAutoSaver(Control* control);
    ~DocumentAutoSaver() override;

    DocumentAutoSaver(const DocumentAutoSaver&) = delete;
    DocumentAutoSaver& operator=(const DocumentAutoSaver&) = delete;

    /// Saves pending changes, then calls done. Calls done right away if there is nothing to save.
    void saveNow(std::function<void()> done);

    void undoRedoChanged() override;
    void undoRedoPageChanged(PageRef page) override;

private:
    /// Whether the document has unsaved changes and a file it can be saved to without asking
    bool hasChangesToSave() const;
    /// Saving ends text editing and clears the selection, so wait while the user is working with them
    bool isUserBusy() const;
    void schedule(guint delayMs);
    void trySave();
    void startSave();

    Control* control;
    /// Main window, reset to nullptr by GLib once it is destroyed
    GtkWindow* window = nullptr;
    guint timer = 0;
    guint focusCheck = 0;
    gulong focusHandler = 0;
    bool saving = false;
    /// Callbacks of saveNow() waiting for the running save
    std::vector<std::function<void()>> waiting;
    /// Expires with this object, so a save finishing later does not touch it
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
};
