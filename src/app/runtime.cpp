#include "ttplayer/app/runtime.h"

#include <commctrl.h>
#include <iterator>
#include <string_view>

namespace ttplayer::app {
namespace {
constexpr UINT_PTR kTooltipSubclassId = 0x004B56E7;
constexpr UINT_PTR kTooltipTopmostTimer = 0x7B;

bool WindowClassEquals(HWND window, std::wstring_view expected) noexcept {
    if (!window || expected.empty()) return false;
    wchar_t name[1024]{};
    const int length = GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return length > 0 && expected.size() == static_cast<size_t>(length) &&
           CompareStringOrdinal(name, length, expected.data(),
                                static_cast<int>(expected.size()), TRUE) ==
               CSTR_EQUAL;
}

void SetTooltipTopmost(HWND window, bool topmost) noexcept {
    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool current = (extended & WS_EX_TOPMOST) != 0;
    if (current == topmost) return;
    if (topmost) {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        // FUN_004B5713 -> FUN_00405417 removes the extended-style bit
        // directly when the tooltip is hidden; it does not reorder the
        // window until the next paint makes it topmost again.
        SetWindowLongPtrW(window, GWL_EXSTYLE,
                          extended & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
    }
}

LRESULT CALLBACK TooltipSubclassProc(HWND window, UINT message,
                                     WPARAM wparam, LPARAM lparam,
                                     UINT_PTR subclass_id,
                                     DWORD_PTR reference_data) {
    static_cast<void>(reference_data);
    switch (message) {
    case WM_TIMER:
        if (wparam == kTooltipTopmostTimer) {
            if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0)
                SetTooltipTopmost(window, false);
            return 0;
        }
        break;
    case WM_PAINT:
        SetTooltipTopmost(window, true);
        SetTimer(window, kTooltipTopmostTimer, 50, nullptr);
        break;
    case WM_DESTROY:
        KillTimer(window, kTooltipTopmostTimer);
        break;
    case WM_NCDESTROY: {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        RemoveWindowSubclass(window, TooltipSubclassProc, subclass_id);
        return result;
    }
    default:
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void AttachTooltipBehavior(HWND window) noexcept {
    DWORD_PTR existing{};
    if (GetWindowSubclass(window, TooltipSubclassProc, kTooltipSubclassId,
                          &existing))
        return;
    SetWindowSubclass(window, TooltipSubclassProc, kTooltipSubclassId, 0);
}
} // namespace

std::filesystem::path RuntimePath(const std::filesystem::path& relative) {
    // TTPlayer_wWinMain (004C0E8F), CPlayerApp_InitializeResources
    // (004C077E) and the skin enumerator (0045E518) all build paths from the
    // executable module directory.  In particular, they never search the
    // process working directory or repository parents.
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / relative;
}

std::filesystem::path FindRuntimePath(const std::filesystem::path& relative) {
    const auto candidate = RuntimePath(relative);
    if (candidate.empty()) return {};
    std::error_code error;
    if (std::filesystem::exists(candidate, error) && !error) return candidate;
    return {};
}

TtpCommRuntime::~TtpCommRuntime() { Shutdown(); }
bool TtpCommRuntime::Initialize() {
    if (api_.module) return true;
    const auto path = FindRuntimePath(L"ttpcomm.dll");
    return !path.empty() && TtpComm_LoadApi(&api_, path.c_str());
}
void TtpCommRuntime::Shutdown() { if (api_.module) TtpComm_UnloadApi(&api_); }

ResourceRuntime::~ResourceRuntime() { Shutdown(); }
bool ResourceRuntime::Initialize() {
    const auto path = FindRuntimePath(L"ttpres.dll");
    if (path.empty()) return false;
    // CPlayerApp_InitializeResources (004C077E) uses LoadLibraryW and keeps
    // this module live through the whole player session.  Loading the DLL is
    // not sufficient: the original also reads string 0x80 and accepts only
    // its simplified/traditional product names or the neutral TTPlayer name.
    // This prevents an unrelated DLL with the same filename from becoming the
    // process resource module.
    module_ = LoadLibraryW(path.c_str());
    if (!module_) return false;

    const wchar_t* title{};
    const int title_length = LoadStringW(module_, 0x80,
        reinterpret_cast<LPWSTR>(&title), 0);
    const std::wstring_view value = title_length > 0 && title
        ? std::wstring_view(title, static_cast<size_t>(title_length))
        : std::wstring_view{};
    if (value != L"千千静听" && value != L"千千靜聽" && value != L"TTPlayer") {
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
    }
    return true;
}
void ResourceRuntime::Shutdown() { if (module_) { FreeLibrary(module_); module_ = nullptr; } }

HRESULT SoundLibraryRuntime::Initialize(const std::filesystem::path& addin_directory) {
    try {
        return manager_.Load(addin_directory);
    } catch (const std::exception&) {
        manager_.Shutdown();
        return E_FAIL;
    }
}
SoundLibraryRuntime::~SoundLibraryRuntime() { Shutdown(); }
void SoundLibraryRuntime::Shutdown() { manager_.Shutdown(); }

ThreadMessageHooksRuntime::~ThreadMessageHooksRuntime() { Shutdown(); }

bool ThreadMessageHooksRuntime::Install() {
    const DWORD thread = GetCurrentThreadId();
    if (!call_window_hook_)
        call_window_hook_ = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, nullptr, thread);
    if (!cbt_hook_)
        cbt_hook_ = SetWindowsHookExW(WH_CBT, CbtHook, nullptr, thread);
    return call_window_hook_ && cbt_hook_;
}

void ThreadMessageHooksRuntime::Shutdown() {
    if (cbt_hook_) { UnhookWindowsHookEx(cbt_hook_); cbt_hook_ = nullptr; }
    if (call_window_hook_) { UnhookWindowsHookEx(call_window_hook_); call_window_hook_ = nullptr; }
}

LRESULT CALLBACK ThreadMessageHooksRuntime::CallWndProcHook(
    int code, WPARAM wparam, LPARAM lparam) {
    // The final WM_CREATE branch of 004B591D compares against the literal
    // "tooltips_class32" (0x0051A750), subclasses the tooltip and keeps it
    // topmost while painted.  Reproduce the complete observable timer/style
    // behavior without fabricating the private WTL object's binary layout.
    if (code == HC_ACTION && lparam != 0) {
        const auto* event = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (event->message == WM_CREATE &&
            WindowClassEquals(event->hwnd, TOOLTIPS_CLASSW))
            AttachTooltipBehavior(event->hwnd);
    }

    // The other two branches install the old WTL command-bar wrapper for
    // WM_INITMENU/WM_INITMENUPOPUP and theme eligible BUTTON controls.  The
    // rebuild's command-bar owner drawing and ComCtl32-v6 buttons already own
    // those HWNDs; double-subclassing them here would corrupt ownership.
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK ThreadMessageHooksRuntime::CbtHook(int code, WPARAM wparam, LPARAM lparam) {
    // 004B5B13 handles HCBT_CREATEWND by constructing the corresponding
    // private shadow/theme wrapper and registering it in the same object map.
    // CS_DROPSHADOW itself is reproduced by PlayerWindow::ApplyWindowShadow;
    // fabricating the missing wrapper here is not ABI-safe.
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
} // namespace ttplayer::app
