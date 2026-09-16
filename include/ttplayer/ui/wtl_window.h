#pragma once
#include "ttplayer/ui/wtl_runtime.h"
#include <memory>
#include <new>

namespace ttplayer::ui {
// One ATL thunk per HWND. GWLP_USERDATA remains the public owner pointer used
// by the original skin/control protocol; dispatch no longer reads it per message.
template<class Owner>
class WindowBinding final : public ATL::CWindowImpl<WindowBinding<Owner>> {
public:
    using Handler = LRESULT (*)(Owner*, HWND, UINT, WPARAM, LPARAM);
    Owner* owner{};
    Handler handler{};

    BOOL ProcessWindowMessage(HWND window, UINT message, WPARAM wp, LPARAM lp,
                              LRESULT& result, DWORD = 0) override {
        result = handler(owner, window, message, wp, lp);
        if (message == WM_NCDESTROY) {
            // The recovered handler has already called its default procedure.
            // Let ATL defer object release until all nested dispatches unwind.
            this->m_dwState |= this->WINSTATE_DESTROYED;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return TRUE;
    }
    void OnFinalMessage(HWND) override { delete this; }

    static LRESULT Start(HWND window, UINT message, WPARAM wp, LPARAM lp,
                         HWND Owner::*handle, Handler callback) {
        if (message != WM_NCCREATE) return ::DefWindowProcW(window, message, wp, lp);
        auto* value = static_cast<Owner*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        if (!value) return FALSE;
        auto binding = std::unique_ptr<WindowBinding>(new (std::nothrow) WindowBinding);
        if (!binding) return FALSE;
        binding->owner = value;
        binding->handler = callback;
        if (!binding->SubclassWindow(window)) return FALSE;
        // These are private window classes, not native-control subclasses.
        binding->m_pfnSuperWindowProc = ::DefWindowProcW;
        value->*handle = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(value));
        const auto procedure = binding->m_thunk.GetWNDPROC();
        binding.release();
        return ::CallWindowProcW(procedure, window, message, wp, lp);
    }
};
}
