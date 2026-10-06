#include "nero_config_layout.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <commctrl.h>

namespace ttplayer::plugins::detail {
namespace {
bool IsClass(HWND window, const wchar_t* expected) noexcept {
    wchar_t name[32]{};
    return GetClassNameW(window, name, 32) && _wcsicmp(name, expected) == 0;
}

void CorrectLayout(HMODULE module, HWND window, WINDOWPOS& position) noexcept {
    if (!module || (position.flags & SWP_NOSIZE) || !(position.flags & SWP_NOMOVE)) return;
    const bool group = GetDlgCtrlID(window) == 0x13B2;
    const HWND dialog = group ? GetParent(window) : window;
    if (!IsClass(dialog, L"#32770") ||
        reinterpret_cast<HMODULE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)) != module ||
        !IsClass(GetDlgItem(dialog, 0x13B2), L"Button")) return;
    if (group && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) != BS_GROUPBOX) return;

    // Aac.dll!10011F54 returns GetWindowRect(check).right, not a width.
    // 100130C4 starts its maximum at zero and then expands
    // group 0x13B2 by 8 pixels and the dialog by 24 pixels. All five checks
    // are measured, including hidden/disabled ones. Do not clip negatives.
    LONG right = (std::numeric_limits<LONG>::min)();
    for (int id : {0x139B, 0x1398, 0x139C, 0x13A8, 0x139D}) {
        const HWND check = GetDlgItem(dialog, id);
        RECT bounds{};
        if (!IsClass(check, L"Button") || !GetWindowRect(check, &bounds)) return;
        right = (std::max)(right, bounds.right);
    }
    if (right >= 0) return;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return;
    const int margin = group ? 8 : 24;
    const int64_t faulty_width = int64_t((std::max)(bounds.right, LONG(margin))) - bounds.left;
    const int64_t corrected_width =
        (std::max)(int64_t(bounds.right), int64_t(right) + margin) - bounds.left;
    // Only correct the exact legacy expansion. Other positioning/sizing
    // requests, UI languages, fonts and user actions retain their behavior.
    if (position.cx == faulty_width && position.cy == bounds.bottom - bounds.top &&
        corrected_width > 0 && corrected_width <= (std::numeric_limits<int>::max)())
        position.cx = static_cast<int>(corrected_width);
}
}

thread_local NeroConfigLayoutScope* NeroConfigLayoutScope::current_{};

NeroConfigLayoutScope::NeroConfigLayoutScope() noexcept : previous_(current_) {
    hook_ = SetWindowsHookExW(WH_CALLWNDPROC, Hook, nullptr, GetCurrentThreadId());
    if (hook_) current_ = this;
}

NeroConfigLayoutScope::~NeroConfigLayoutScope() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        for (HWND window : windows_)
            if (window) RemoveWindowSubclass(window, WindowProc, reinterpret_cast<UINT_PTR>(this));
        current_ = previous_;
    }
}

LRESULT CALLBACK NeroConfigLayoutScope::Hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp && current_) {
        const auto& event = *reinterpret_cast<const CWPSTRUCT*>(lp);
        if (event.message == WM_INITDIALOG && IsClass(event.hwnd, L"#32770")) {
            // The encoder can load Aac.dll lazily inside Configure. Observe the
            // dialog after creation, before Nero performs its text measurement.
            const HMODULE module = GetModuleHandleW(L"Aac.dll");
            const HWND group = GetDlgItem(event.hwnd, 0x13B2);
            if (module && reinterpret_cast<HMODULE>(GetWindowLongPtrW(event.hwnd, GWLP_HINSTANCE)) == module &&
                IsClass(group, L"Button") && !current_->windows_[0] && !current_->windows_[1]) {
                const HWND targets[]{event.hwnd, group};
                for (int i = 0; i < 2; ++i)
                    if (SetWindowSubclass(targets[i], WindowProc, reinterpret_cast<UINT_PTR>(current_),
                                          reinterpret_cast<DWORD_PTR>(current_)))
                        current_->windows_[i] = targets[i];
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK NeroConfigLayoutScope::WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                                  UINT_PTR id, DWORD_PTR data) {
    if (message == WM_WINDOWPOSCHANGING && lp)
        CorrectLayout(GetModuleHandleW(L"Aac.dll"), window, *reinterpret_cast<WINDOWPOS*>(lp));
    if (message == WM_NCDESTROY) {
        auto* scope = reinterpret_cast<NeroConfigLayoutScope*>(data);
        for (HWND& target : scope->windows_)
            if (target == window) target = nullptr;
        RemoveWindowSubclass(window, WindowProc, id);
    }
    return DefSubclassProc(window, message, wp, lp);
}
}
