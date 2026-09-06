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
    std::array<unsigned char, 4096> buffer{};

    DirectoryWatch() = default;
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;
    DirectoryWatch(DirectoryWatch&& other) noexcept
        : directory(std::exchange(other.directory, INVALID_HANDLE_VALUE)),
          event(std::exchange(other.event, nullptr)),
          overlapped(other.overlapped), path(std::move(other.path)),
          buffer(other.buffer) {
        overlapped.hEvent = event;
    }
    DirectoryWatch& operator=(DirectoryWatch&&) = delete;
    ~DirectoryWatch() {
        if (directory != INVALID_HANDLE_VALUE) {
            CancelIoEx(directory, &overlapped);
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
    // exactly the mask passed at 0041AD0D.  `TRUE` is the original recursive
    // watch flag stored in OVERLAPPED::InternalHigh by 0041AC7B.
    return ReadDirectoryChangesW(
        watch.directory, watch.buffer.data(),
        static_cast<DWORD>(watch.buffer.size()), TRUE, 0x13, nullptr,
        &watch.overlapped, nullptr) != FALSE;
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
         std::vector<std::filesystem::path> directories) {
    std::vector<DirectoryWatch> watches;
    watches.reserve(directories.size());
    for (const auto& path : directories) {
        if (WaitForSingleObject(control->stop_event, 0) == WAIT_OBJECT_0)
            return;
        watches.emplace_back();
        auto& watch = watches.back();
        watch.path = path;
        // 0041AC7B uses access 1, sharing 7, OPEN_EXISTING and
        // FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED (0x42000000).
        watch.directory = CreateFileW(
            path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (watch.directory == INVALID_HANDLE_VALUE) {
            watches.pop_back();
            continue;
        }
        watch.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!watch.event || !Arm(watch)) {
            watches.pop_back();
            continue;
        }
    }
    if (watches.empty()) return;

    std::vector<HANDLE> waits;
    waits.reserve(watches.size() + 1);
    waits.push_back(control->stop_event);
    for (const auto& watch : watches) waits.push_back(watch.event);

    for (;;) {
        const DWORD result = WaitForMultipleObjects(
            static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) return;
        if (result < WAIT_OBJECT_0 + 1 ||
            result >= WAIT_OBJECT_0 + waits.size()) return;
        const size_t index = static_cast<size_t>(result - WAIT_OBJECT_0 - 1);
        DWORD bytes{};
        if (GetOverlappedResult(watches[index].directory,
                                &watches[index].overlapped, &bytes, FALSE) &&
            bytes >= sizeof(FILE_NOTIFY_INFORMATION)) {
            size_t offset{};
            while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <=
                   bytes) {
                const auto* item = reinterpret_cast<const
                    FILE_NOTIFY_INFORMATION*>(
                        watches[index].buffer.data() + offset);
                const size_t available = bytes - offset -
                    offsetof(FILE_NOTIFY_INFORMATION, FileName);
                if ((item->FileNameLength & 1U) != 0 ||
                    item->FileNameLength > available) break;
                std::wstring relative(item->FileName,
                    item->FileNameLength / sizeof(wchar_t));
                Notify(control, item->Action,
                       watches[index].path / relative);
                if (item->NextEntryOffset == 0) break;
                if (item->NextEntryOffset > bytes - offset) break;
                offset += item->NextEntryOffset;
            }
        }
        if (!Arm(watches[index])) {
            // A removed/unmounted directory makes its event permanently
            // unusable.  Keep the remaining native handles alive by parking
            // this slot on the stop event; the subsequent full index rebuild
            // will reconcile the missing directory.
            Notify(control, FILE_ACTION_MODIFIED, watches[index].path);
            return;
        }
    }
}

} // namespace

DirectoryChangeMonitor::~DirectoryChangeMonitor() { Stop(); }

bool DirectoryChangeMonitor::Start(
    HWND receiver, UINT message,
    std::span<const std::filesystem::path> directories) {
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

    std::vector<std::filesystem::path> paths;
    paths.reserve(std::min<size_t>(directories.size(), 63));
    for (const auto& path : directories) {
        if (path.empty()) continue;
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
