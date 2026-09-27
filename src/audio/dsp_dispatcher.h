#pragma once

#include <windows.h>
#include <ole2.h>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace ttplayer::audio::detail {

// One apartment for a chain's legacy callbacks and its modeless dialogs.
// SendMessage services incoming sent messages while the caller waits, unlike a
// mutex/condition-variable wait that deadlocks a DSP querying hwndParent.
class DspDispatcher {
public:
    DspDispatcher() {
        std::promise<HWND> ready;
        auto result = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            thread_id_ = GetCurrentThreadId();
            const HRESULT ole = OleInitialize(nullptr);
            WNDCLASSW wc{};
            wc.lpfnWndProc = Procedure;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"TTPlayerRebuild.DspDispatcher";
            // Multiple independent AudioEngine objects may create apartments.
            RegisterClassW(&wc);
            const HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            ready.set_value(window);
            if (!window) {
                if (SUCCEEDED(ole)) OleUninitialize();
                return;
            }
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                const HWND root = GetAncestor(message.hwnd, GA_ROOT);
                wchar_t name[32]{};
                GetClassNameW(root, name, 32);
                if (root && _wcsicmp(name, L"#32770") == 0 &&
                    IsDialogMessageW(root, &message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            // DialogBox unwinds on WM_QUIT before this point. Quit and DLL
            // unloading therefore cannot invalidate a Config stack frame.
            if (cleanup_) cleanup_();
            while (PeekMessageW(&message, window, kPosted, kPosted, PM_REMOVE))
                delete reinterpret_cast<Task*>(message.lParam);
            DestroyWindow(window);
            if (SUCCEEDED(ole)) OleUninitialize();
        });
        window_ = result.get();
        if (!window_) {
            thread_.join();
            throw std::runtime_error("Unable to create Winamp DSP dispatcher");
        }
    }

    DspDispatcher(const DspDispatcher&) = delete;
    DspDispatcher& operator=(const DspDispatcher&) = delete;
    ~DspDispatcher() { Stop({}); }

    void Invoke(std::function<void()> function) const {
        if (GetCurrentThreadId() == thread_id_) { function(); return; }
        Task task{std::move(function)};
        SendMessageW(window_, kInvoke, 0, reinterpret_cast<LPARAM>(&task));
        if (task.error) std::rethrow_exception(task.error);
        if (!task.executed) throw std::runtime_error("Winamp DSP dispatcher stopped");
    }

    bool Post(std::function<void()> function) const {
        auto task = std::make_unique<Task>();
        task->function = std::move(function);
        if (!PostMessageW(window_, kPosted, 0,
                          reinterpret_cast<LPARAM>(task.get()))) return false;
        task.release();
        return true;
    }

    void InvokeQueued(std::function<void()> function) const {
        if (GetCurrentThreadId() == thread_id_) { function(); return; }
        const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!done) throw std::runtime_error("Unable to create DSP completion event");
        Task task{std::move(function)};
        task.done = done;
        if (!PostMessageW(window_, kQueued, 0, reinterpret_cast<LPARAM>(&task))) {
            CloseHandle(done);
            throw std::runtime_error("Winamp DSP dispatcher stopped");
        }
        while (MsgWaitForMultipleObjects(1, &done, FALSE, INFINITE,
                                         QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(done);
        if (task.error) std::rethrow_exception(task.error);
    }

    void Stop(std::function<void()> cleanup) {
        if (!thread_.joinable()) return;
        Invoke([this, cleanup = std::move(cleanup)]() mutable {
            cleanup_ = std::move(cleanup);
            EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
                PostMessageW(window, WM_CLOSE, 0, 0);
                return TRUE;
            }, 0);
            PostQuitMessage(0);
        });
        const HANDLE handle = thread_.native_handle();
        while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE,
                                         QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
            MSG message{};
            // Deliver incoming SendMessage only; do not re-enter arbitrary UI
            // commands while the chain is being destroyed.
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        thread_.join();
    }

private:
    static constexpr UINT kInvoke = WM_APP + 1;
    static constexpr UINT kPosted = WM_APP + 2;
    static constexpr UINT kQueued = WM_APP + 3;
    struct Task {
        std::function<void()> function;
        std::exception_ptr error;
        bool executed{};
        HANDLE done{};
    };
    static LRESULT CALLBACK Procedure(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
        if (message == kInvoke || message == kPosted || message == kQueued) {
            auto* task = reinterpret_cast<Task*>(lparam);
            try { task->function(); }
            catch (...) { task->error = std::current_exception(); }
            task->executed = true;
            if (task->done) SetEvent(task->done);
            if (message == kPosted) delete task;
            return 1;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    std::thread thread_;
    DWORD thread_id_{};
    HWND window_{};
    std::function<void()> cleanup_;
};

} // namespace ttplayer::audio::detail
