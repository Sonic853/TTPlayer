#pragma once

#include <windows.h>
#include <commctrl.h>
#include <new>

namespace ttplayer::ui::detail {

// The original msctls_hotkey32 capture path clears Backspace even with a
// modifier. Keep the native value/painting and extend only that input path.
struct BackspaceHotKeyState {
    WORD chord{};
    bool down{};
    bool setting{};
};

inline WORD HotKeyModifiers() {
    WORD value{};
    if (GetKeyState(VK_CONTROL) < 0) value |= HOTKEYF_CONTROL;
    if (GetKeyState(VK_SHIFT) < 0) value |= HOTKEYF_SHIFT;
    if (GetKeyState(VK_MENU) < 0) value |= HOTKEYF_ALT;
    return value;
}

inline bool IsHotKeyModifier(WPARAM key) {
    return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
        key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
        key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
}

inline LRESULT CALLBACK BackspaceHotKeyProc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam, UINT_PTR identifier, DWORD_PTR data) {
    auto& state = *reinterpret_cast<BackspaceHotKeyState*>(data);
    const auto set_chord = [&](WORD chord) {
        state.chord = chord;
        const bool changed = SendMessageW(window, HKM_GETHOTKEY, 0, 0) != chord;
        state.setting = true;
        SendMessageW(window, HKM_SETHOTKEY, chord, 0);
        state.setting = false;
        // HKM_SETHOTKEY does not send the user-input notification itself.
        if (changed) SendMessageW(GetParent(window), WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(window), EN_CHANGE), reinterpret_cast<LPARAM>(window));
    };
    switch (message) {
    case HKM_SETHOTKEY:
        if (!state.setting) state = {};
        break;
    case WM_GETDLGCODE:
        if (lparam) {
            const auto& key = *reinterpret_cast<const MSG*>(lparam);
            if ((key.message == WM_KEYDOWN || key.message == WM_SYSKEYDOWN) &&
                key.wParam == VK_BACK)
                return DefSubclassProc(window, message, wparam, lparam) | DLGC_WANTMESSAGE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam == VK_BACK) {
            const bool repeat = state.down && (lparam & (1L << 30)) != 0;
            state.down = true;
            const WORD modifiers = HotKeyModifiers();
            if (modifiers) {
                set_chord(MAKEWORD(VK_BACK, modifiers));
                return 0;
            }
            // Releasing the modifier first must not turn the remaining
            // Backspace auto-repeat into a request to clear this chord.
            if (repeat && state.chord) return 0;
            state.chord = 0;
        } else if (IsHotKeyModifier(wparam)) {
            if (state.down && HotKeyModifiers()) {
                set_chord(MAKEWORD(VK_BACK, HotKeyModifiers()));
                return 0;
            }
            state.chord = 0;
        } else {
            state.chord = 0;
            state.down = false;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam == VK_BACK) {
            state.down = false;
            if (state.chord) return 0;
        } else if (IsHotKeyModifier(wparam) && state.chord) {
            // Let the native control update its modifier state, then keep
            // the captured value through either key-release order.
            const WORD chord = state.chord;
            DefSubclassProc(window, message, wparam, lparam);
            set_chord(chord);
            return 0;
        }
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        // Ctrl+Backspace translates to DEL (0x7f), other variants to BS.
        // The keydown path already handled capture or bare-key clearing.
        if (wparam == L'\b' || wparam == 0x7f) return 0;
        break;
    case WM_KILLFOCUS:
        state = {};
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, BackspaceHotKeyProc, identifier);
        delete &state;
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

inline bool ExtendBackspaceHotKeyControl(HWND window) {
    constexpr UINT_PTR identifier = 0x54544253;
    DWORD_PTR existing{};
    if (GetWindowSubclass(window, BackspaceHotKeyProc, identifier, &existing)) return true;
    auto* state = new (std::nothrow) BackspaceHotKeyState;
    if (!state) return false;
    if (SetWindowSubclass(window, BackspaceHotKeyProc, identifier,
                          reinterpret_cast<DWORD_PTR>(state))) return true;
    delete state;
    return false;
}

} // namespace ttplayer::ui::detail
