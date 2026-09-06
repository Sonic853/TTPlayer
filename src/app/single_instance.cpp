#include "ttplayer/app/single_instance.h"

#include <string>

namespace ttplayer::app {
SingleInstanceIpc::SingleInstanceIpc(std::wstring_view name, DWORD mapping_size) {
    const std::wstring base(name);
    const std::wstring event_name = base + L"_Event";
    const std::wstring mapping_name = base + L"_Mapping";
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    if (!ready_event_) return;
    primary_ = GetLastError() != ERROR_ALREADY_EXISTS;
    if (mapping_size == 0) return;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, mapping_size, mapping_name.c_str());
    if (!mapping_) return;
    shared_ = static_cast<SharedState*>(MapViewOfFile(
        mapping_, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size));
    if (primary_ && shared_) ZeroMemory(shared_, mapping_size);
}

SingleInstanceIpc::~SingleInstanceIpc() { Shutdown(); }

void SingleInstanceIpc::PublishProcess() {
    if (!shared_) return;
    InterlockedExchange(&shared_->version, 0x00050709);
    InterlockedExchange(&shared_->process_id, static_cast<LONG>(GetCurrentProcessId()));
    InterlockedExchange(&shared_->thread_id, static_cast<LONG>(GetCurrentThreadId()));
}

void SingleInstanceIpc::PublishWindow(HWND window) {
    if (shared_) shared_->window = window;
    if (ready_event_) SetEvent(ready_event_);
}

bool SingleInstanceIpc::ForwardCommandLine(std::wstring_view path, ULONG_PTR mode) const {
    if (!ready_event_ || WaitForSingleObject(ready_event_, 5000) != WAIT_OBJECT_0 ||
        !shared_ || !IsWindow(shared_->window)) return false;
    const HWND target = shared_->window;
    if (!path.empty()) {
        COPYDATASTRUCT data{};
        data.dwData = mode;
        data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
        data.lpData = const_cast<wchar_t*>(path.data());
        SendMessageW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(target),
                     reinterpret_cast<LPARAM>(&data));
    }
    ShowWindow(target, SW_SHOW);
    SendMessageW(target, WM_SYSCOMMAND, SC_RESTORE, 0);
    SetForegroundWindow(target);
    return true;
}

void SingleInstanceIpc::Shutdown() {
    if (ready_event_) { CloseHandle(ready_event_); ready_event_ = nullptr; }
    if (shared_) { UnmapViewOfFile(shared_); shared_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
}
} // namespace ttplayer::app
