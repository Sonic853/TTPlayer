#pragma once

#include <string_view>
#include <windows.h>

namespace ttplayer::app {
class SingleInstanceIpc {
public:
    SingleInstanceIpc(std::wstring_view name, DWORD mapping_size);
    ~SingleInstanceIpc();
    SingleInstanceIpc(const SingleInstanceIpc&) = delete;
    SingleInstanceIpc& operator=(const SingleInstanceIpc&) = delete;

    [[nodiscard]] bool IsPrimary() const noexcept { return primary_; }
    [[nodiscard]] bool Valid() const noexcept { return ready_event_ != nullptr; }
    void PublishProcess();
    void PublishWindow(HWND window);
    bool ForwardCommandLine(std::wstring_view path, ULONG_PTR mode = 0) const;
    void Shutdown();

private:
    struct SharedState {
        volatile LONG version;
        volatile LONG process_id;
        volatile LONG thread_id;
        HWND window;
    };
    HANDLE ready_event_{};
    HANDLE mapping_{};
    SharedState* shared_{};
    bool primary_{};
};
} // namespace ttplayer::app
