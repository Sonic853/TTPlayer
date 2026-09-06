#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <windows.h>

namespace ttplayer::ui::detail {

struct DirectoryChangeNotification {
    std::uintptr_t generation{};
    DWORD action{};
    std::filesystem::path path;
};

// CDirectoryWatcher recovered from 0041AC28..0041B0AE.  The original keeps
// one overlapped directory handle per configured path (at most 63 because a
// stop handle occupies the remaining WaitForMultipleObjects slot), watches
// recursively for mask 0x13 and posts a private window message on changes.
//
// This wrapper retains that observable contract while keeping slow network
// directory opens outside the UI thread.  Stop revokes the receiver before
// signalling the worker, so a legacy/network filesystem which never returns
// cannot post through a recycled HWND or make window destruction wait.
class DirectoryChangeMonitor final {
public:
    // Public only so the translation unit's detached worker can retain the
    // opaque control block; callers still receive no definition or handle.
    struct Control;

    DirectoryChangeMonitor() = default;
    ~DirectoryChangeMonitor();

    DirectoryChangeMonitor(const DirectoryChangeMonitor&) = delete;
    DirectoryChangeMonitor& operator=(const DirectoryChangeMonitor&) = delete;

    bool Start(HWND receiver, UINT message,
               std::span<const std::filesystem::path> directories);
    void Stop() noexcept;

    // Returns true only for a notification produced by the current worker.
    [[nodiscard]] bool Accept(std::uintptr_t generation) const noexcept;

private:
    std::shared_ptr<Control> control_;
};

} // namespace ttplayer::ui::detail
