#include "directory_change_monitor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace ttplayer::ui::detail {

struct DirectoryChangeMonitor::Control {
    HANDLE stop_event{};
    HANDLE worker_thread{};
    std::mutex receiver_mutex;
    HWND receiver{};
    UINT message{};
    std::uintptr_t generation{};

    ~Control() {
        if (worker_thread) CloseHandle(worker_thread);
        if (stop_event) CloseHandle(stop_event);
    }
};

namespace {

struct DirectoryWatch {
    HANDLE directory{INVALID_HANDLE_VALUE};
    HANDLE event{};
    OVERLAPPED overlapped{};
    std::filesystem::path path;
    bool recursive{};
    bool pending{};
    std::array<DWORD, 1024> buffer{};

    DirectoryWatch() = default;
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;
    DirectoryWatch(DirectoryWatch&&) = delete;
    DirectoryWatch& operator=(DirectoryWatch&&) = delete;
    ~DirectoryWatch() {
        if (directory != INVALID_HANDLE_VALUE) {
            if (pending) {
                CancelIoEx(directory, &overlapped);
                DWORD ignored{};
                GetOverlappedResult(directory, &overlapped, &ignored, TRUE);
            }
            CloseHandle(directory);
        }
        if (event) CloseHandle(event);
    }
};

bool Arm(DirectoryWatch& watch) noexcept {
    if (watch.directory == INVALID_HANDLE_VALUE || !watch.event) return false;
    watch.overlapped = {};
    watch.overlapped.hEvent = watch.event;
    // 0x13 == FILE_NOTIFY_CHANGE_FILE_NAME | DIRECTORY_NAME | LAST_WRITE,
    // 0041AC7B stores the per-directory checkbox at object +0x18;
    // 0041AD0D passes it as bWatchSubtree. Unchecked still watches this folder.
    watch.pending = ReadDirectoryChangesW(
        watch.directory, watch.buffer.data(),
        static_cast<DWORD>(sizeof(watch.buffer)), watch.recursive, 0x13, nullptr,
        &watch.overlapped, nullptr) != FALSE;
    return watch.pending;
}

void Notify(const std::shared_ptr<DirectoryChangeMonitor::Control>& control,
            DWORD action, std::filesystem::path path) {
    auto* notification = new (std::nothrow) DirectoryChangeNotification{
        control->generation, action, std::move(path)};
    if (!notification) return;
    std::scoped_lock lock(control->receiver_mutex);
    if (!control->receiver ||
        !PostMessageW(control->receiver, control->message,
                      static_cast<WPARAM>(action),
                      reinterpret_cast<LPARAM>(notification)))
        delete notification;
}

void Run(std::shared_ptr<DirectoryChangeMonitor::Control> control,
         std::vector<DirectoryWatchPath> directories) {
    // Keep OVERLAPPED and its buffer at a stable address while I/O is pending.
    std::vector<std::unique_ptr<DirectoryWatch>> watches(directories.size());
    ULONGLONG next_retry{};
    for (;;) {
        if (WaitForSingleObject(control->stop_event, 0) == WAIT_OBJECT_0) return;
        const auto now = GetTickCount64();
        if (now >= next_retry) {
            next_retry = now + 1000;
            for (size_t i = 0; i < directories.size(); ++i) {
                if (watches[i]) continue;
                if (WaitForSingleObject(control->stop_event, 0) == WAIT_OBJECT_0) return;
                auto watch = std::make_unique<DirectoryWatch>();
                watch->path = directories[i].path;
                watch->recursive = directories[i].recursive;
                watch->directory = CreateFileW(watch->path.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
                if (watch->directory == INVALID_HANDLE_VALUE) continue;
                watch->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!watch->event || !Arm(*watch)) continue;
                watches[i] = std::move(watch);
                // Arm before scanning so changes during reconciliation are retained.
                Notify(control, kDirectoryRescan, directories[i].path);
            }
        }
        std::vector<HANDLE> waits{control->stop_event};
        std::vector<size_t> indices;
        for (size_t i = 0; i < watches.size(); ++i) {
            if (!watches[i]) continue;
            waits.push_back(watches[i]->event);
            indices.push_back(i);
        }
        const DWORD result = WaitForMultipleObjects(
            static_cast<DWORD>(waits.size()), waits.data(), FALSE, 1000);
        if (result == WAIT_TIMEOUT) continue;
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) return;
        if (result < WAIT_OBJECT_0 + 1 || result >= WAIT_OBJECT_0 + waits.size()) return;
        const size_t index = indices[result - WAIT_OBJECT_0 - 1];
        auto& watch = *watches[index];
        DWORD bytes{};
        const bool completed = GetOverlappedResult(watch.directory,
            &watch.overlapped, &bytes, FALSE) != FALSE;
        bool rescan = !completed || bytes < sizeof(FILE_NOTIFY_INFORMATION);
        if (!rescan) {
            size_t offset{};
            while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
                const auto* item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    reinterpret_cast<const unsigned char*>(watch.buffer.data()) + offset);
                const size_t available = bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName);
                if ((item->FileNameLength & 1U) || item->FileNameLength > available) {
                    rescan = true; break;
                }
                Notify(control, item->Action, watch.path / std::wstring(
                    item->FileName, item->FileNameLength / sizeof(wchar_t)));
                if (item->NextEntryOffset == 0) break;
                if (item->NextEntryOffset > bytes - offset ||
                    item->NextEntryOffset < offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
                    rescan = true; break;
                }
                offset += item->NextEntryOffset;
            }
        }
        const bool armed = Arm(watch);
        if (rescan || !armed) Notify(control, kDirectoryRescan, watch.path);
        if (!armed) watches[index].reset(); // Retry only this directory; retain the others.
    }
}

} // namespace

DirectoryChangeMonitor::~DirectoryChangeMonitor() { Stop(); }

bool DirectoryChangeMonitor::Start(
    HWND receiver, UINT message,
    std::span<const DirectoryWatchPath> directories) {
    Stop();
    if (!receiver || message == 0 || directories.empty()) return false;
    auto control = std::make_shared<Control>();
    control->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!control->stop_event) return false;
    control->receiver = receiver;
    control->message = message;
    static std::atomic_uintptr_t next_generation{1};
    control->generation = next_generation.fetch_add(
        1, std::memory_order_relaxed);

    std::vector<DirectoryWatchPath> paths;
    paths.reserve(std::min<size_t>(directories.size(), 63));
    for (const auto& path : directories) {
        if (path.path.empty()) continue;
        paths.push_back(path);
        if (paths.size() == 63) break;
    }
    if (paths.empty()) return false;
    control_ = control;
    try {
        std::thread worker([control,
                            paths = std::move(paths)]() mutable {
            Run(std::move(control), std::move(paths));
        });
        // CreateFileW against an unavailable UNC root can itself block before
        // the worker owns a directory handle which CancelIoEx could reach.
        // Retain a duplicate of the native thread handle so Stop can apply
        // CancelSynchronousIo to that in-flight open without ever waiting on
        // the UI thread.  The original watcher also performs these opens off
        // the window thread; this closes the reconstruction's repeated
        // Stop/Start resource leak while preserving that non-blocking model.
        HANDLE thread_handle{};
        if (DuplicateHandle(GetCurrentProcess(), worker.native_handle(),
                GetCurrentProcess(), &thread_handle, 0, FALSE,
                DUPLICATE_SAME_ACCESS)) {
            control->worker_thread = thread_handle;
        }
        worker.detach();
    } catch (const std::system_error&) {
        control_.reset();
        return false;
    }
    return true;
}

void DirectoryChangeMonitor::Stop() noexcept {
    auto control = std::exchange(control_, {});
    if (!control) return;
    {
        std::scoped_lock lock(control->receiver_mutex);
        control->receiver = nullptr;
    }
    SetEvent(control->stop_event);
    if (control->worker_thread)
        static_cast<void>(CancelSynchronousIo(control->worker_thread));
}

bool DirectoryChangeMonitor::Accept(
    std::uintptr_t generation) const noexcept {
    return control_ && control_->generation == generation;
}

} // namespace ttplayer::ui::detail
