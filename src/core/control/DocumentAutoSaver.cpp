#include "DocumentAutoSaver.h"

#include <utility>

#include "control/Control.h"
#include "gui/MainWindow.h"
#include "gui/XournalView.h"
#include "model/Document.h"
#include "undo/UndoRedoHandler.h"

#ifdef _WIN32
#include <windows.h>  // last: its macros clash with names in the headers above
#endif

namespace {
constexpr guint IDLE_CHECK_INTERVAL_S = 5;
constexpr unsigned long IDLE_SAVE_AFTER_MS = 30'000;

/// Whether any window of this application has the focus, e.g. a dialog opened from the main window
auto applicationHasFocus() -> bool {
    GList* windows = gtk_window_list_toplevels();
    bool active = false;
    for (GList* item = windows; item && !active; item = item->next) {
        active = gtk_window_is_active(GTK_WINDOW(item->data));
    }
    g_list_free(windows);
    return active;
}
}  // namespace

DocumentAutoSaver::DocumentAutoSaver(Control* control): control(control) {
    window = control->getGtkWindow();
    // The window is destroyed before the Control (and this object) on shutdown
    g_object_add_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer*>(&window));
    focusHandler = g_signal_connect(window, "notify::is-active", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                                        auto* self = static_cast<DocumentAutoSaver*>(data);
                                        // Decide once focus has settled: switching to one of our own dialogs (like
                                        // "Save changes?" when quitting) must not trigger a save
                                        if (!self->focusCheck) {
                                            self->focusCheck = g_idle_add(
                                                    +[](gpointer data) -> gboolean {
                                                        auto* self = static_cast<DocumentAutoSaver*>(data);
                                                        self->focusCheck = 0;
                                                        if (!applicationHasFocus()) {
                                                            self->saveInBackground();
                                                        }
                                                        return G_SOURCE_REMOVE;
                                                    },
                                                    self);
                                        }
                                    }),
                                    this);
#ifdef _WIN32
    // Save after a pause without input. The time of the last input covers the whole system, so the check is cheap and
    // also waits while the user writes in another application.
    idleCheck = g_timeout_add_seconds(
            IDLE_CHECK_INTERVAL_S,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<DocumentAutoSaver*>(data);
                LASTINPUTINFO info{sizeof(LASTINPUTINFO), 0};
                if (GetLastInputInfo(&info) && GetTickCount() - info.dwTime >= IDLE_SAVE_AFTER_MS) {
                    self->saveInBackground();
                }
                return G_SOURCE_CONTINUE;
            },
            this);
#endif
}

DocumentAutoSaver::~DocumentAutoSaver() {
    if (focusCheck) {
        g_source_remove(focusCheck);
    }
    if (idleCheck) {
        g_source_remove(idleCheck);
    }
    if (window) {
        g_signal_handler_disconnect(window, focusHandler);
        g_object_remove_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer*>(&window));
    }
}

auto DocumentAutoSaver::hasChangesToSave() const -> bool {
    auto* undoRedo = control->getUndoRedoHandler();
    if (!undoRedo || !undoRedo->isChanged() || !control->isAutoSaveAllowed()) {
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

void DocumentAutoSaver::saveInBackground() {
    if (!saving && hasChangesToSave() && !isUserBusy()) {
        startSave();
    }
}

void DocumentAutoSaver::startSave() {
    saving = true;
    std::weak_ptr<int> alive = lifetime;
    control->save([this, alive](bool saved) {
        if (alive.expired()) {
            return;
        }
        saving = false;
        for (auto& callback: std::exchange(waiting, {})) {
            callback(saved);
        }
    });
}

void DocumentAutoSaver::saveNow(std::function<void(bool saved)> done) {
    if (saving) {
        waiting.push_back(std::move(done));
        return;
    }
    if (!hasChangesToSave()) {
        done(true);
        return;
    }
    waiting.push_back(std::move(done));
    startSave();
}
