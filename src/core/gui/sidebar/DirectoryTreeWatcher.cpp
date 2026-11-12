#include "DirectoryTreeWatcher.h"

#include <utility>

#ifdef _WIN32

#include <atomic>
#include <thread>

#include <glib.h>
#include <windows.h>

namespace {

/// Lives as long as the watcher or a change still queued for the main thread; only used on the main thread
struct Shared {
    DirectoryTreeWatcher::Callback callback;
};

struct Posted {
    std::shared_ptr<Shared> shared;
    std::vector<DirectoryTreeWatcher::Change> changes;
    bool overflow;
};

void post(const std::shared_ptr<Shared>& shared, std::vector<DirectoryTreeWatcher::Change> changes, bool overflow) {
    g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer data) -> gboolean {
                auto* posted = static_cast<Posted*>(data);
                if (posted->shared->callback) {  // empty once the watcher is gone
                    posted->shared->callback(posted->changes, posted->overflow);
                }
                return G_SOURCE_REMOVE;
            },
            new Posted{shared, std::move(changes), overflow},
            +[](gpointer data) { delete static_cast<Posted*>(data); });
}

constexpr DWORD FILTER = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
/// The largest buffer that also works on network shares
constexpr size_t BUFFER_BYTES = 64 * 1024;

}  // namespace

struct DirectoryTreeWatcher::Impl {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};  ///< false once the thread gave up (e.g. the folder is gone)
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

    void run(const fs::path& root) {
        std::vector<DWORD> buffer(BUFFER_BYTES / sizeof(DWORD));  // DWORD-aligned, as required
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (true) {
            ResetEvent(overlapped.hEvent);
            if (!ReadDirectoryChangesW(dir, buffer.data(), static_cast<DWORD>(BUFFER_BYTES), TRUE, FILTER, nullptr,
                                       &overlapped, nullptr)) {
                running = false;         // before posting: the caller checks isActive() to watch again
                post(shared, {}, true);  // e.g. the folder is gone: let the caller look
                break;
            }
            const HANDLE handles[] = {stopEvent, overlapped.hEvent};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(dir, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(dir, &overlapped, &ignored, TRUE);
                break;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(dir, &overlapped, &bytes, FALSE)) {
                if (GetLastError() == ERROR_NOTIFY_ENUM_DIR) {
                    post(shared, {}, true);
                    continue;  // too many changes at once: they were dropped, keep watching
                }
                running = false;
                post(shared, {}, true);
                break;
            }
            if (bytes == 0) {
                post(shared, {}, true);  // the buffer overflowed: changes were dropped
                continue;
            }
            std::vector<Change> changes;
            const auto* bytesStart = reinterpret_cast<const char*>(buffer.data());
            for (size_t offset = 0;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(bytesStart + offset);
                const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                changes.push_back({root / name, info->Action != FILE_ACTION_MODIFIED});
                if (info->NextEntryOffset == 0) {
                    break;
                }
                offset += info->NextEntryOffset;
            }
            post(shared, std::move(changes), false);
        }
        CloseHandle(overlapped.hEvent);
        running = false;
    }
};

DirectoryTreeWatcher::DirectoryTreeWatcher(fs::path rootPath, Callback callback):
        root(std::move(rootPath)), impl(std::make_unique<Impl>()) {
    impl->dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (impl->dir == INVALID_HANDLE_VALUE) {
        return;
    }
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->shared->callback = std::move(callback);
    impl->running = true;
    impl->thread = std::thread([impl = impl.get(), root = root] { impl->run(root); });
}

DirectoryTreeWatcher::~DirectoryTreeWatcher() {
    impl->shared->callback = nullptr;  // changes still queued for the main thread are dropped
    if (impl->thread.joinable()) {
        SetEvent(impl->stopEvent);
        impl->thread.join();
    }
    if (impl->stopEvent) {
        CloseHandle(impl->stopEvent);
    }
    if (impl->dir != INVALID_HANDLE_VALUE) {
        CloseHandle(impl->dir);
    }
}

auto DirectoryTreeWatcher::isActive() const -> bool { return impl->running; }

#else

struct DirectoryTreeWatcher::Impl {};

DirectoryTreeWatcher::DirectoryTreeWatcher(fs::path rootPath, Callback):
        root(std::move(rootPath)), impl(std::make_unique<Impl>()) {}

DirectoryTreeWatcher::~DirectoryTreeWatcher() = default;

auto DirectoryTreeWatcher::isActive() const -> bool { return false; }

#endif
