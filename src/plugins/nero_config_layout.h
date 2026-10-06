#pragma once
#include <windows.h>

namespace ttplayer::plugins::detail {
// Aac.dll's configuration UI clamps screen-coordinate right edges to zero.
// Keep its text-driven sizing, including on monitors to the left of primary.
class NeroConfigLayoutScope {
public:
    NeroConfigLayoutScope() noexcept;
    ~NeroConfigLayoutScope();
    NeroConfigLayoutScope(const NeroConfigLayoutScope&) = delete;
    NeroConfigLayoutScope& operator=(const NeroConfigLayoutScope&) = delete;
private:
    static LRESULT CALLBACK Hook(int, WPARAM, LPARAM);
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static thread_local NeroConfigLayoutScope* current_;
    NeroConfigLayoutScope* previous_{};
    HHOOK hook_{};
    HWND windows_[2]{};
};
}
