#include "DocumentAutoSaver.h"

#include <utility>

#include <gtk/gtk.h>

#include "control/Control.h"
#include "gui/MainWindow.h"
#include "gui/XournalView.h"
#include "model/Document.h"
#include "util/PathUtil.h"

namespace {
/// Time without changes after which the document is saved
constexpr guint SAVE_DELAY_MS = 3000;
/// Time to wait before trying again while the user is editing text or a selection
constexpr guint RETRY_DELAY_MS = 2000;
}  // namespace

DocumentAutoSaver::DocumentAutoSaver(Control* control): control(control) {
    control->getUndoRedoHandler()->addUndoRedoListener(this);
    focusHandler = g_signal_connect(control->getGtkWindow(), "notify::is-active",
                                    G_CALLBACK(+[](GObject* window, GParamSpec*, gpointer data) {
                                        if (!gtk_window_is_active(GTK_WINDOW(window))) {
                                            static_cast<DocumentAutoSaver*>(data)->trySave();
                                        }
                                    }),
                                    this);
}

DocumentAutoSaver::~DocumentAutoSaver() {
    if (timer) {
        g_source_remove(timer);
    }
    if (focusHandler) {
        g_signal_handler_disconnect(control->getGtkWindow(), focusHandler);
    }
    control->getUndoRedoHandler()->removeUndoRedoListener(this);
}

void DocumentAutoSaver::undoRedoChanged() {
    if (hasChangesToSave()) {
        schedule(SAVE_DELAY_MS);
    }
}

void DocumentAutoSaver::undoRedoPageChanged(PageRef) {}

auto DocumentAutoSaver::hasChangesToSave() const -> bool {
    if (!control->getUndoRedoHandler()->isChanged()) {
        return false;
    }
    Document* doc = control->getDocument();
    doc->lock_shared();
    const auto path = doc->getFilepath();
    doc->unlock_shared();
    // .xoj files would be converted to a new .xopp file on save, so leave them to the user
    return !path.empty() && path.extension() == ".xopp";
}

auto DocumentAutoSaver::isUserBusy() const -> bool {
    if (control->getTextEditor()) {
        return true;
    }
    auto* win = control->getWindow();
    return win && win->getXournal() && win->getXournal()->getSelection();
}

void DocumentAutoSaver::schedule(guint delayMs) {
    if (timer) {
        g_source_remove(timer);
    }
    timer = g_timeout_add(delayMs, +[](gpointer data) -> gboolean {
        auto* self = static_cast<DocumentAutoSaver*>(data);
        self->timer = 0;
        self->trySave();
        return G_SOURCE_REMOVE;
    }, this);
}

void DocumentAutoSaver::trySave() {
    if (saving || !hasChangesToSave()) {
        return;
    }
    if (isUserBusy()) {
        schedule(RETRY_DELAY_MS);
        return;
    }
    startSave();
}

void DocumentAutoSaver::startSave() {
    saving = true;
    std::weak_ptr<int> alive = lifetime;
    control->save([this, alive](bool) {
        if (alive.expired()) {
            return;
        }
        saving = false;
        for (auto& callback: std::exchange(waiting, {})) {
            callback();
        }
    });
}

void DocumentAutoSaver::saveNow(std::function<void()> done) {
    if (timer) {
        g_source_remove(timer);
        timer = 0;
    }
    if (saving) {
        waiting.push_back(std::move(done));
        return;
    }
    if (!hasChangesToSave()) {
        done();
        return;
    }
    waiting.push_back(std::move(done));
    startSave();
}
