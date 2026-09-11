#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "modern_file_dialog.h"

#include "ttplayer/core/text.h"
#include "ttplayer/ui/lyric_runtime_policy.h"
#include "ttplayer/ui/window_drag.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <map>
#include <richedit.h>
#include <richole.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tom.h>
#include <windowsx.h>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace ttplayer::ui {
using namespace detail;

namespace {
constexpr int kEditorEncodingUtf8 = 1;
constexpr int kEditorEncodingUtf16Le = 2;
constexpr int kEditorEncodingUtf16Be = 3;
constexpr int kEditorEncodingAnsi = 4;
constexpr UINT kLyricAdjustmentDialog = 0xcf;
constexpr int kLyricAdjustmentEdit = 0x84b;
constexpr int kLyricAdjustmentSpin = 0x84c;
constexpr DWORD kLyricEditorEventMask =
    ENM_CHANGE | ENM_SELCHANGE | ENM_PROTECTED | ENM_LINK;
constexpr DWORD kLyricEditorCharacterMask =
    CFM_SIZE | CFM_COLOR | CFM_FACE | CFM_CHARSET |
    CFM_BOLD | CFM_PROTECTED;
constexpr DWORD kLyricEditorCharacterEffects =
    CFE_AUTOBACKCOLOR | CFE_PROTECTED;

struct LyricEditorStreamIn {
    const unsigned char* current{};
    size_t remaining{};
};

DWORD CALLBACK ReadLyricEditorStream(DWORD_PTR cookie, LPBYTE buffer,
                                     LONG requested, LONG* transferred) {
    if (!transferred) return 1;
    *transferred = 0;
    if (!cookie || !buffer || requested <= 0) return 0;
    auto& stream = *reinterpret_cast<LyricEditorStreamIn*>(cookie);
    const size_t count = std::min<size_t>(
        stream.remaining, static_cast<size_t>(requested) & ~size_t{1});
    if (count != 0) {
        memcpy(buffer, stream.current, count);
        stream.current += count;
        stream.remaining -= count;
        *transferred = static_cast<LONG>(count);
    }
    return 0;
}

struct LyricEditorStreamOut {
    std::wstring text;
};

DWORD CALLBACK WriteLyricEditorStream(DWORD_PTR cookie, LPBYTE buffer,
                                      LONG supplied, LONG* transferred) {
    if (!transferred) return 1;
    *transferred = 0;
    if (!cookie || !buffer || supplied <= 0) return 0;
    if ((supplied & 1) != 0) return 1;
    auto& stream = *reinterpret_cast<LyricEditorStreamOut*>(cookie);
    try {
        const size_t old_size = stream.text.size();
        const size_t characters = static_cast<size_t>(supplied) /
                                  sizeof(wchar_t);
        stream.text.resize(old_size + characters);
        memcpy(stream.text.data() + old_size, buffer,
               static_cast<size_t>(supplied));
        *transferred = supplied;
        return 0;
    } catch (...) {
        return 1;
    }
}

struct LyricAdjustmentDialogState {
    int value{};
};

INT_PTR CALLBACK LyricAdjustmentDialogProc(HWND dialog, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LyricAdjustmentDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<LyricAdjustmentDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        const HWND spin = GetDlgItem(dialog, kLyricAdjustmentSpin);
        if (spin) {
            // FUN_004475DB: signed -10000..10000 ms, with a 500 ms
            // acceleration step after the first second.
            SendMessageW(spin, UDM_SETRANGE, 0,
                         MAKELPARAM(10000, static_cast<short>(-10000)));
            UDACCEL acceleration{1, 500};
            SendMessageW(spin, UDM_SETACCEL, 1,
                         reinterpret_cast<LPARAM>(&acceleration));
            SendMessageW(spin, UDM_SETPOS, 0,
                         MAKELPARAM(static_cast<short>(
                             state ? state->value : 0), 0));
        } else {
            SetDlgItemInt(dialog, kLyricAdjustmentEdit,
                          state ? state->value : 0, TRUE);
        }
        const HWND edit = GetDlgItem(dialog, kLyricAdjustmentEdit);
        if (edit) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            return FALSE;
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            wchar_t text[64]{};
            GetDlgItemTextW(dialog, kLyricAdjustmentEdit, text,
                            static_cast<int>(std::size(text)));
            wchar_t* end{};
            const long value = wcstol(text, &end, 10);
            while (end && iswspace(*end)) ++end;
            if (!end || end == text || *end != L'\0' || value < -10000 ||
                value > 10000) {
                MessageBeep(MB_ICONWARNING);
                const HWND edit = GetDlgItem(dialog, kLyricAdjustmentEdit);
                if (edit) {
                    SetFocus(edit);
                    SendMessageW(edit, EM_SETSEL, 0, -1);
                }
                return TRUE;
            }
            if (state) state->value = static_cast<int>(value);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

std::optional<int> PromptLyricAdjustment(HMODULE resources, HWND owner,
                                         int previous) {
    if (!resources) return std::nullopt;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&common);
    LyricAdjustmentDialogState state{previous};
    const INT_PTR result = DialogBoxParamW(
        resources, MAKEINTRESOURCEW(kLyricAdjustmentDialog), owner,
        LyricAdjustmentDialogProc, reinterpret_cast<LPARAM>(&state));
    return result == IDOK ? std::optional<int>(state.value) : std::nullopt;
}

std::wstring DecodeEditorBytes(const std::string& bytes, int& encoding,
                               bool& bom) {
    bom = false;
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        encoding = kEditorEncodingUtf16Le;
        bom = true;
        std::wstring result;
        result.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            result.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(bytes[i]) |
                (static_cast<unsigned int>(
                    static_cast<unsigned char>(bytes[i + 1])) << 8)));
        }
        return result;
    }
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xfe &&
        static_cast<unsigned char>(bytes[1]) == 0xff) {
        encoding = kEditorEncodingUtf16Be;
        bom = true;
        std::wstring result;
        result.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            result.push_back(static_cast<wchar_t>(
                (static_cast<unsigned int>(
                    static_cast<unsigned char>(bytes[i])) << 8) |
                static_cast<unsigned char>(bytes[i + 1])));
        }
        return result;
    }

    size_t offset = 0;
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xef\xbb\xbf") == 0) {
        offset = 3;
        bom = true;
    }
    const int byte_count = static_cast<int>(bytes.size() - offset);
    int count = byte_count == 0 ? 0 : MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset, byte_count,
        nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    encoding = kEditorEncodingUtf8;
    if (count == 0 && byte_count != 0) {
        // 0043E237 accepts Big5 only when it is the system ACP; every other
        // non-Unicode local lyric falls back to Simplified-Chinese CP936.
        code_page = GetACP() == 950 ? 950U : 936U;
        flags = 0;
        encoding = kEditorEncodingAnsi;
        bom = false;
        count = MultiByteToWideChar(code_page, flags, bytes.data(),
                                    static_cast<int>(bytes.size()), nullptr, 0);
        offset = 0;
    }
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(code_page, flags, bytes.data() + offset,
                        static_cast<int>(bytes.size() - offset),
                        result.data(), count);
    return result;
}

std::wstring ReadEditorFile(const std::filesystem::path& path, int& encoding,
                            bool& bom) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return DecodeEditorBytes(bytes, encoding, bom);
}

bool WriteEditorFile(const std::filesystem::path& path, std::wstring_view text,
                     int, bool) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    // CLyric::Save at 0043E3CF does not retain the source encoding.  It first
    // asks the active ANSI code page to encode the complete NUL-terminated
    // text and observes lpUsedDefaultChar.  Only when a character would be
    // replaced does it emit an UTF-8 BOM and encode as UTF-8 instead.
    const std::wstring stable(text);
    BOOL used_default = FALSE;
    int count = WideCharToMultiByte(CP_ACP, 0, stable.c_str(), -1, nullptr, 0,
                                    nullptr, &used_default);
    if (count > 0 && !used_default) {
        std::string bytes(static_cast<size_t>(count), '\0');
        used_default = FALSE;
        if (WideCharToMultiByte(CP_ACP, 0, stable.c_str(), -1, bytes.data(),
                                count, nullptr, &used_default) <= 0 ||
            used_default) {
            return false;
        }
        output.write(bytes.data(), static_cast<std::streamsize>(count - 1));
        return output.good();
    }

    output.write("\xef\xbb\xbf", 3);
    if (stable.empty()) return output.good();
    count = WideCharToMultiByte(CP_UTF8, 0, stable.data(),
        static_cast<int>(stable.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    std::string bytes(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, stable.data(),
        static_cast<int>(stable.size()), bytes.data(), count, nullptr, nullptr);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::optional<std::chrono::milliseconds> ParseEditorTimestamp(
    std::wstring_view value) {
    const size_t colon = value.find(L':');
    if (colon == std::wstring_view::npos || colon == 0) return std::nullopt;
    wchar_t* end{};
    const std::wstring minutes_text(value.substr(0, colon));
    const long minutes = wcstol(minutes_text.c_str(), &end, 10);
    if (!end || *end != L'\0' || minutes < 0) return std::nullopt;
    const std::wstring seconds_text(value.substr(colon + 1));
    end = nullptr;
    const double seconds = wcstod(seconds_text.c_str(), &end);
    if (!end || *end != L'\0' || seconds < 0.0 || seconds >= 60.0)
        return std::nullopt;
    return std::chrono::milliseconds(
        minutes * 60000 + static_cast<long long>(seconds * 1000.0 + 0.5));
}

std::optional<std::chrono::milliseconds> ParseEditorTimestampLiteral(
    std::wstring_view value) {
    // 00443460/00443806 pass the entire bracket range to
    // swscanf(L"[%d:%f]").  This deliberately accepts the same legacy loose
    // syntax instead of applying the stricter LRC-model parser above.
    const std::wstring stable(value);
    int minutes{};
    float seconds{};
    if (swscanf_s(stable.c_str(), L"[%d:%f]", &minutes, &seconds) != 2)
        return std::nullopt;
    return std::chrono::milliseconds(
        static_cast<long long>(minutes) * 60000 +
        static_cast<long long>(seconds * 1000.0F));
}

std::wstring FormatEditorTimestamp(std::chrono::milliseconds value) {
    // 00443460/00443806 divide the signed millisecond value first, multiply
    // the signed remainder by 0.001f, then pass it to `%02d:%05.2f`.  Keep
    // that CRT rounding (including its 59.999 -> 60.00 and negative forms)
    // instead of normalizing/clamping the result as an LRC model would.
    const auto total = value.count();
    const int minutes = static_cast<int>(total / 60000);
    const float seconds = static_cast<float>(total % 60000) * 0.001F;
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"[%02d:%05.2f]", minutes,
               static_cast<double>(seconds));
    return buffer;
}

std::wstring NormalizeEditorNewlines(std::wstring text) {
    std::wstring result;
    result.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            result += L"\r\n";
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (text[i] == L'\n') {
            result += L"\r\n";
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

std::wstring CanonicalEditorText(const lyrics::Lyrics& lyrics,
                                 bool apply_offset = false) {
    std::wstring text;
    const auto append_tag = [&text](std::wstring_view tag,
                                    const std::string& value) {
        text += L"[";
        text += tag;
        text += L":";
        try { text += core::Utf8ToWide(value); }
        catch (const std::exception&) {}
        text += L"]\r\n";
    };
    append_tag(L"ti", lyrics.title);
    append_tag(L"ar", lyrics.artist);
    append_tag(L"al", lyrics.album);
    append_tag(L"by", lyrics.author);
    text += L"\r\n";
    for (const auto& line : lyrics.lines) {
        text += FormatEditorTimestamp(line.time - (apply_offset
            ? lyrics.offset : std::chrono::milliseconds::zero()));
        try { text += core::Utf8ToWide(line.text); }
        catch (const std::exception&) {}
        text += L"\r\n";
    }
    return text;
}
} // namespace

LRESULT PlayerWindow::HandleLyricControlMessage(HWND control, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
    const int identifier = GetDlgCtrlID(control);
    const bool text_control = control == lyric_control_ ||
        identifier == kLyricControlId;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(control, &paint);
        PaintLyricControl(control, dc);
        EndPaint(control, &paint);
        return 0;
    }
    case WM_TIMER:
        if (text_control && wparam == kLyricAnimationTimer) {
            // FUN_0044254A leaves the lyric position frozen while its private
            // drag flag (+0xc0) is set.  The decoder clock is sampled directly;
            // it is deliberately not quantized through the 250 ms player timer.
            if (!lyric_line_dragging_ &&
                audio_.State() == audio::PlaybackState::playing) {
                InvalidateRect(control, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return text_control ? DLGC_WANTARROWS | DLGC_WANTCHARS : DLGC_BUTTON;
    case WM_MOUSEMOVE:
        if (text_control) {
            if (lyric_line_dragging_ && GetCapture() == control) {
                lyric_line_drag_offset_ = ActiveLyricScrollMode() != 0
                    ? GET_X_LPARAM(lparam) - lyric_line_drag_origin_.x
                    : GET_Y_LPARAM(lparam) - lyric_line_drag_origin_.y;
                InvalidateRect(control, nullptr, FALSE);
            }
        } else {
            RECT client{};
            GetClientRect(control, &client);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const bool inside = PtInRect(&client, point) != FALSE;
            if (inside) {
                if (GetCapture() != control) SetCapture(control);
                // 0040A5E3 stores the new SkinButton state after SetCapture;
                // the synchronous WM_CAPTURECHANGED from the previous child
                // must not erase the newly entered button.
                if (lyric_hover_command_ != identifier) {
                    lyric_hover_command_ = identifier;
                    InvalidateRect(control, nullptr, FALSE);
                }
            } else {
                if (GetCapture() == control &&
                    (wparam & MK_LBUTTON) == 0) ReleaseCapture();
                if (lyric_hover_command_ == identifier) {
                    lyric_hover_command_ = 0;
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (!text_control && lyric_hover_command_ == identifier) {
            lyric_hover_command_ = 0;
            InvalidateRect(control, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (text_control) {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // FUN_004425F7 enters drag tracking only when FUN_00442360's
            // lyric-text hit test returns -1.  Clicking a rendered line is a
            // lyric/link activation path and must not capture the mouse.
            if (settings_.lyric.drag_lyric && lyrics_.lines.size() > 1 &&
                !LyricTextHitTest(control, point)) {
                lyric_line_dragging_ = true;
                lyric_line_drag_origin_ = point;
                lyric_line_drag_offset_ = 0;
                SetCapture(control);
                SetFocus(control);
            }
        } else {
            if (GetCapture() != control) SetCapture(control);
            lyric_hover_command_ = identifier;
            lyric_pressed_command_ = identifier;
            InvalidateRect(control, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (text_control) {
            if (lyric_line_dragging_) {
                const int drag = ActiveLyricScrollMode() != 0
                    ? GET_X_LPARAM(lparam) - lyric_line_drag_origin_.x
                    : GET_Y_LPARAM(lparam) - lyric_line_drag_origin_.y;
                std::optional<std::chrono::milliseconds> target;
                if (!lyrics_.lines.empty() && drag != 0) {
                    const HDC dc = GetDC(control);
                    if (dc) {
                        const HGDIOBJ old_font = SelectObject(dc,
                            lyric_font_ ? lyric_font_ : GetStockObject(DEFAULT_GUI_FONT));
                        target = LyricDragTime(dc, drag);
                        SelectObject(dc, old_font);
                        ReleaseDC(control, dc);
                    }
                }
                lyric_line_dragging_ = false;
                lyric_line_drag_offset_ = 0;
                // CLyricCtrl::WM_LBUTTONUP at 00442671 invokes generic seek,
                // whose original fade path at 004ABDE4 is additionally gated
                // by a private fade helper, volume and queued-audio amount.
                // The reconstruction lacks that queue object and previously
                // over-applied the fade to every lyric release; preserve the
                // observed original direct-seek behavior here.
                if (target) {
                    audio_.SeekWithoutFade(*target);
                    UpdateDiscordPresence();
                }
                // ReleaseCapture synchronously sends WM_CAPTURECHANGED.
                // Publish the target before that handler invalidates/paints.
                if (GetCapture() == control) ReleaseCapture();
                InvalidateRect(control, nullptr, FALSE);
            }
        } else {
            const int command = std::exchange(lyric_pressed_command_, 0);
            RECT client{};
            GetClientRect(control, &client);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const bool activate = command == identifier &&
                                  PtInRect(&client, point) != FALSE;
            if (GetCapture() == control) ReleaseCapture();
            if (activate)
                HandleLyricCommand(static_cast<UINT>(command));
            InvalidateRect(control, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (text_control) {
            lyric_line_dragging_ = false;
            lyric_line_drag_offset_ = 0;
        } else if (lyric_pressed_command_ == identifier) {
            lyric_pressed_command_ = 0;
        }
        if (!text_control && lyric_hover_command_ == identifier)
            lyric_hover_command_ = 0;
        InvalidateRect(control, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (text_control && settings_.lyric.mouse_wheel_adjust) {
            const int steps = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
            // 00442A97 maps a positive wheel delta to -500 ms and a negative
            // delta to +500 ms.  0043E92C -> 0043D7D0 moves every lyric line;
            // playback itself remains at the same position.
            lyrics_.ShiftLines(-std::chrono::milliseconds(steps * 500));
            InvalidateRect(control, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (text_control) {
            if (wparam == VK_UP || wparam == VK_LEFT) {
                SeekLyricLine(-1);
                return 0;
            }
            if (wparam == VK_DOWN || wparam == VK_RIGHT) {
                SeekLyricLine(1);
                return 0;
            }
            if (wparam == VK_ESCAPE && lyric_line_dragging_) {
                lyric_line_dragging_ = false;
                lyric_line_drag_offset_ = 0;
                if (GetCapture() == control) ReleaseCapture();
                InvalidateRect(control, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && fullscreen_mode_ != 0) {
                SetFullScreenMode(0);
                return 0;
            }
        }
        break;
    case WM_CONTEXTMENU:
        if (fullscreen_lyric_detached_ && text_control) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // WM_CONTEXTMENU already carries screen coordinates. Applying
            // the host offset again moves a secondary-screen popup away
            // from the mouse (the old primary-origin case hid this error).
            if (point.x == -1 && point.y == -1) {
                RECT bounds{};
                GetWindowRect(control, &bounds);
                point = {(bounds.left + bounds.right) / 2,
                         (bounds.top + bounds.bottom) / 2};
            }
            ShowFullScreenLyricContextMenu(point);
            return 0;
        }
        return SendMessageW(lyric_window_, WM_CONTEXTMENU,
            reinterpret_cast<WPARAM>(lyric_window_), lparam);
    case WM_SETCURSOR:
        if (text_control) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(control, &point);
            const LPCWSTR cursor = lyric_line_dragging_
                ? (ActiveLyricScrollMode() != 0 ? IDC_SIZEWE : IDC_SIZENS)
                : LyricTextHitTest(control, point) ? IDC_HAND : IDC_ARROW;
            SetCursor(LoadCursorW(nullptr, cursor));
        } else {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return TRUE;
    }
    return DefWindowProcW(control, message, wparam, lparam);
}

LRESULT PlayerWindow::HandleLyricMessage(UINT message, WPARAM wparam,
                                         LPARAM lparam) {
    static const UINT find_message = RegisterWindowMessageW(FINDMSGSTRINGW);
    if (message == find_message) {
        if (const auto* request =
                reinterpret_cast<const FINDREPLACEW*>(lparam))
            HandleLyricFindRequest(*request);
        return 0;
    }
    switch (message) {
    case WM_ERASEBKGND:
        if (wparam) PaintLyricWindow(reinterpret_cast<HDC>(wparam));
        return 1;
    case WM_PRINTCLIENT:
        if (wparam && (lparam & PRF_CLIENT) != 0)
            PaintLyricWindow(reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        if (!limits || !skin_ || !skin_->Lyric().valid) return 0;
        if (mini_mode_) {
            RECT player{};
            GetWindowRect(window_, &player);
            const LONG fixed_height = std::max<LONG>(1, player.bottom - player.top);
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            limits->ptMinTrackSize = {10, fixed_height};
            limits->ptMaxTrackSize = {
                std::max<LONG>(10, work.right - work.left), fixed_height};
            return 0;
        }
        const auto& layout = skin_->Lyric();
        limits->ptMinTrackSize = {layout.background.size.cx,
                                  layout.background.size.cy};
        if (layout.resize_rect.right <= layout.resize_rect.left ||
            layout.resize_rect.bottom <= layout.resize_rect.top) {
            limits->ptMaxTrackSize = limits->ptMinTrackSize;
        } else {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            limits->ptMaxTrackSize = {work.right - work.left, work.bottom - work.top};
        }
        return 0;
    }
    case WM_SIZE:
        LayoutLyricControls();
        LayoutLyricEditor();
        UpdateLyricWindowRegion();
        InvalidateRect(lyric_window_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(lyric_window_, &paint);
        PaintLyricWindow(dc);
        EndPaint(lyric_window_, &paint);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header && header->hwndFrom == lyric_editor_) {
            if (header->code == EN_PROTECTED) {
                const auto& notification =
                    *reinterpret_cast<const ENPROTECTED*>(lparam);
                if (!lyric_editor_internal_change_) {
                    RecordLyricEditorEdit(notification.msg, notification.wParam,
                        notification.chrg.cpMin, notification.chrg.cpMax);
                }
                // RichEdit treats zero as permission to edit a protected
                // range.  The protection bit exists solely so the original
                // editor can observe the pre-edit range.
                return 0;
            }
            if (header->code == EN_LINK) {
                const auto& notification =
                    *reinterpret_cast<const ENLINK*>(lparam);
                OpenLyricEditorLink(notification.msg,
                    notification.chrg.cpMin, notification.chrg.cpMax);
                return 0;
            }
        }
        if (HandleToolTipNotification(lyric_window_, lparam)) return 0;
        break;
    }
    case WM_CLOSE:
        SetSkinWindowVisible(lyric_window_, false);
        ActiveLyricVisible() = false;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(lyric_window_, &point);
        const unsigned int hit = LyricDragHitTest(point);
        if (hit != kDragMove) {
            LPCWSTR cursor = IDC_ARROW;
            if (hit == (kDragRight | kDragBottom) ||
                hit == (kDragLeft | kDragTop)) cursor = IDC_SIZENWSE;
            else if (hit == (kDragLeft | kDragBottom) ||
                     hit == (kDragRight | kDragTop)) cursor = IDC_SIZENESW;
            else if ((hit & (kDragLeft | kDragRight)) != 0) cursor = IDC_SIZEWE;
            else cursor = IDC_SIZENS;
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (dragging_skin_background_ && skin_drag_window_ == lyric_window_ &&
            GetCapture() == lyric_window_) {
            ContinueSkinBackgroundDrag(lyric_window_, point);
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        BeginSkinBackgroundDrag(lyric_window_, point, LyricDragHitTest(point));
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging_skin_background_ && skin_drag_window_ == lyric_window_)
            EndSkinMouseCapture();
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (skin_drag_window_ == lyric_window_) EndSkinMouseCapture();
        return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(lyric_window_, &bounds);
            point = {(bounds.left + bounds.right) / 2,
                     (bounds.top + bounds.bottom) / 2};
        }
        // FUN_0044B066 only owns a lyric/editor popup while the click is
        // inside the active text child. Right-clicking the surrounding skin
        // chrome is relayed to CPlayerWnd's ordinary 0x8A popup.
        const HWND active_text = lyric_editor_ ? lyric_editor_ : lyric_control_;
        RECT text_bounds{};
        if (active_text && GetWindowRect(active_text, &text_bounds) &&
            !PtInRect(&text_bounds, point)) {
            // Keep the main-menu contents/command owner, but fullscreen must
            // still start on the lyric host's monitor when its chrome is hit.
            ShowContextMenu(point, lyric_window_);
            return 0;
        }
        ShowLyricContextMenu(point);
        return 0;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lparam) == lyric_editor_ &&
            HIWORD(wparam) == EN_CHANGE) {
            HandleLyricEditorChange();
            return 0;
        }
        if (HandleLyricCommand(LOWORD(wparam))) return 0;
        if (HandleContextCommand(LOWORD(wparam))) return 0;
        break;
    case WM_DRAWITEM:
        if (lparam && DrawPopupMenuItem(
                *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_MEASUREITEM:
        if (lparam && MeasurePopupMenuItem(
                *reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_DESTROY:
        desktop_lyrics_.Destroy();
        desktop_lyric_mode_ = false;
        RevokeFileDropTarget(lyric_window_);
        if (skin_drag_window_ == lyric_window_) EndSkinMouseCapture();
        DestroyLyricControls();
        RemoveToolTipTools(lyric_window_);
        lyric_window_ = nullptr;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(lyric_window_, message, wparam, lparam);
}

RECT& PlayerWindow::ActiveLyricWindowBounds() noexcept {
    return mini_mode_ ? settings_.player.mini_lyric_window
                      : settings_.player.lyric_window;
}

const RECT& PlayerWindow::ActiveLyricWindowBounds() const noexcept {
    return mini_mode_ ? settings_.player.mini_lyric_window
                      : settings_.player.lyric_window;
}

bool& PlayerWindow::ActiveLyricVisible() noexcept {
    return mini_mode_ ? settings_.player.mini_lyric_visible
                      : settings_.player.lyric_visible;
}

bool PlayerWindow::ActiveLyricVisible() const noexcept {
    return mini_mode_ ? settings_.player.mini_lyric_visible
                      : settings_.player.lyric_visible;
}

bool& PlayerWindow::ActiveLyricTopMost() noexcept {
    return mini_mode_ ? settings_.player.mini_lyric_top_most
                      : settings_.player.lyric_top_most;
}

bool PlayerWindow::ActiveLyricTopMost() const noexcept {
    return mini_mode_ ? settings_.player.mini_lyric_top_most
                      : settings_.player.lyric_top_most;
}

int& PlayerWindow::ActiveLyricScrollMode() noexcept {
    // FUN_00401CA7 selects settings+0x344 for the detached full-screen
    // control, +0x2B8 in mini mode and +0x2B4 in normal mode.
    if (fullscreen_lyric_detached_)
        return settings_.lyric.fullscreen_scroll_mode;
    return mini_mode_ ? settings_.lyric.mini_scroll_mode
                      : settings_.lyric.scroll_mode;
}

int PlayerWindow::ActiveLyricScrollMode() const noexcept {
    if (fullscreen_lyric_detached_)
        return settings_.lyric.fullscreen_scroll_mode;
    return mini_mode_ ? settings_.lyric.mini_scroll_mode
                      : settings_.lyric.scroll_mode;
}

int PlayerWindow::ActiveLyricTextAlign() const noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_text_align
        : settings_.lyric.text_align;
}

int PlayerWindow::ActiveLyricRowInterval() const noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_row_interval
        : settings_.lyric.row_interval;
}

int PlayerWindow::ActiveLyricFadeIndex() const noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_fade_index
        : settings_.lyric.fade_index;
}

bool& PlayerWindow::ActiveLyricFadeHighlight() noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_fade_highlight
        : settings_.lyric.fade_highlight;
}

bool PlayerWindow::ActiveLyricFadeHighlight() const noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_fade_highlight
        : settings_.lyric.fade_highlight;
}

bool& PlayerWindow::ActiveLyricKaraokeMode() noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_karaoke_mode
        : settings_.lyric.karaoke_mode;
}

bool PlayerWindow::ActiveLyricKaraokeMode() const noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_karaoke_mode
        : settings_.lyric.karaoke_mode;
}

bool PlayerWindow::ActiveLyricTransparent() const noexcept {
    return fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_transparent
        : settings_.lyric.transparent;
}

COLORREF PlayerWindow::ActiveLyricTextColor() const noexcept {
    if (fullscreen_lyric_detached_)
        return settings_.lyric.fullscreen_text_color;
    if (settings_.lyric.text_color != CLR_INVALID)
        return settings_.lyric.text_color;
    return skin_ && skin_->Lyric().valid
        ? skin_->Lyric().text_color : RGB(255, 255, 255);
}

COLORREF PlayerWindow::ActiveLyricHighlightColor() const noexcept {
    if (fullscreen_lyric_detached_)
        return settings_.lyric.fullscreen_highlight_color;
    if (settings_.lyric.highlight_color != CLR_INVALID)
        return settings_.lyric.highlight_color;
    return skin_ && skin_->Lyric().valid
        ? skin_->Lyric().highlight_color : RGB(255, 255, 255);
}

COLORREF PlayerWindow::ActiveLyricBackgroundColor() const noexcept {
    if (fullscreen_lyric_detached_)
        return settings_.lyric.fullscreen_background_color;
    if (settings_.lyric.background_color != CLR_INVALID)
        return settings_.lyric.background_color;
    return skin_ && skin_->Lyric().valid
        ? skin_->Lyric().background_color : RGB(0, 0, 0);
}

void PlayerWindow::UpdateLyricScrollTimer() {
    if (!lyric_control_) return;
    KillTimer(lyric_control_, kLyricAnimationTimer);
    SetTimer(lyric_control_, kLyricAnimationTimer,
        ActiveLyricScrollMode() != 0
            ? kLyricHorizontalIntervalMs : kLyricVerticalIntervalMs,
        nullptr);
}

void PlayerWindow::CaptureActiveLyricWindowState() {
    if (!lyric_window_ || !IsWindow(lyric_window_)) return;
    RECT bounds{};
    if (GetWindowRect(lyric_window_, &bounds)) ActiveLyricWindowBounds() = bounds;
    ActiveLyricVisible() = IsWindowVisible(lyric_window_) != FALSE;
}

void PlayerWindow::ApplyActiveLyricWindowState() {
    if (!lyric_window_ || !skin_ || !skin_->Lyric().valid) return;
    // 00464B6C swaps the active lyric object before installing its settings.
    // A drag cannot cross that swap, and the new ScrollMode/ScrollMode2 must
    // also select its own animation cadence immediately.
    lyric_line_dragging_ = false;
    lyric_line_drag_offset_ = 0;
    if (lyric_control_ && GetCapture() == lyric_control_) ReleaseCapture();
    UpdateLyricScrollTimer();
    const auto& layout = skin_->Lyric();
    RECT target = ActiveLyricWindowBounds();
    const bool have_target = target.right > target.left &&
                             target.bottom > target.top;
    if (mini_mode_) {
        // FUN_00464B6C keeps a separate LyricWnd2.  An empty rectangle is
        // materialized immediately to the right of the mini player with the
        // original 200-pixel width; its height always follows mini_window.
        RECT player{};
        GetWindowRect(window_, &player);
        const int height = std::max<LONG>(1, player.bottom - player.top);
        const int width = have_target
            ? std::max<LONG>(10, target.right - target.left) : 200;
        const int left = have_target ? target.left : player.right;
        const int top = have_target ? target.top : player.top;
        target = {left, top, left + width, top + height};
    } else {
        RECT player{};
        GetWindowRect(window_, &player);
        const int left = have_target ? target.left
                                     : player.left + layout.position.left;
        const int top = have_target ? target.top
                                    : player.top + layout.position.top;
        int width = layout.background.size.cx;
        int height = layout.background.size.cy;
        const bool resizable = layout.resize_rect.right > layout.resize_rect.left &&
                               layout.resize_rect.bottom > layout.resize_rect.top;
        if (resizable && have_target) {
            width = std::max<LONG>(width, target.right - target.left);
            height = std::max<LONG>(height, target.bottom - target.top);
        }
        target = {left, top, left + width, top + height};
    }
    ActiveLyricWindowBounds() = target;
    SetWindowPos(lyric_window_, ActiveLyricTopMost()
            ? HWND_TOPMOST : HWND_NOTOPMOST,
        target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    LayoutLyricControls();
    UpdateLyricWindowRegion();
    UpdateLyricToolRects();
    ShowWindow(lyric_window_, ActiveLyricVisible()
        ? SW_SHOWNOACTIVATE : SW_HIDE);
    RedrawWindow(lyric_window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN |
        RDW_UPDATENOW);
}

bool PlayerWindow::CreateLyricWindow() {
    if (lyric_window_) return true;
    if (!skin_ || !skin_->Lyric().valid) return false;
    const auto& layout = skin_->Lyric();
    RECT player{};
    GetWindowRect(window_, &player);
    int x = player.left + layout.position.left;
    int y = player.top + layout.position.top;
    int width = layout.background.size.cx;
    int height = layout.background.size.cy;
    const RECT saved = ActiveLyricWindowBounds();
    if (saved.right > saved.left && saved.bottom > saved.top) {
        x = saved.left;
        y = saved.top;
        if (mini_mode_) {
            width = std::max<LONG>(10, saved.right - saved.left);
            height = skin_->MiniWindowSize().cy;
        } else if (layout.resize_rect.right > layout.resize_rect.left &&
                   layout.resize_rect.bottom > layout.resize_rect.top) {
            width = std::max<int>(width, saved.right - saved.left);
            height = std::max<int>(height, saved.bottom - saved.top);
        }
    } else if (mini_mode_) {
        width = 200;
        height = skin_->MiniWindowSize().cy;
        x = player.right;
        y = player.top;
    }
    if (width <= 0 || height <= 0) return false;

    const DWORD extended = WS_EX_TOOLWINDOW |
        (ActiveLyricTopMost() ? WS_EX_TOPMOST : 0);
    // 0046AACA creates the normal lyric view as an owned, captionless popup.
    // The title is observable through window enumeration and is kept exactly.
    lyric_window_ = CreateWindowExW(extended, kLyricWindowClass, L"Lyric",
        WS_POPUP | WS_CLIPSIBLINGS, x, y, width, height,
        window_, nullptr, instance_, this);
    if (!lyric_window_) return false;
    RegisterFileDropTarget(lyric_window_, FileDropSurface::lyric);
    CreateLyricControls();
    CreateToolTipWindow();
    UpdateLyricWindowRegion();
    UpdateLyricToolRects();
    CreateDesktopLyrics();
    return true;
}

bool PlayerWindow::CreateDesktopLyrics() {
    if (desktop_lyrics_.ControlHandle()) return true;
    if (!instance_ || !lyric_window_) return false;

    const auto track_popup = [this](HMENU menu, POINT point, HWND owner) {
        if (!menu || context_menu_open_ || !window_ ||
            !IsWindowEnabled(window_)) return UINT{};
        context_menu_open_ = true;
        SetForegroundWindow(owner);
        BeginPopupMenuStyle(menu, true);
        const UINT command = TrackPopupMenuEx(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            point.x, point.y, owner, nullptr);
        EndPopupMenuStyle();
        context_menu_open_ = false;
        PostMessageW(owner, WM_NULL, 0, 0);
        return command;
    };

    if (!desktop_lyrics_.Create(instance_, lyric_window_, window_,
            ResourceModule(), &settings_.desktop_lyric,
            &settings_.player.desktop_lyric_window,
            std::move(track_popup))) {
        return false;
    }
    desktop_lyrics_.SetSkin(skin_ ? &*skin_ : nullptr);
    desktop_lyrics_.SetLyrics(&lyrics_);
    desktop_lyrics_.UpdatePlayback(
        audio_.Position(), audio_.State() == audio::PlaybackState::playing);
    return true;
}

void PlayerWindow::EnterDesktopLyricMode() {
    if (!lyric_window_ && !CreateLyricWindow()) return;
    if (!CreateDesktopLyrics()) return;
    if (lyric_editor_) LeaveLyricEditor(true);

    // CLyricWnd::FUN_0044E2FB is dispatched before CDeskLrcCtrl is shown.
    // Showing in this order is significant: DeskLrcCtrl is owned by the now
    // hidden LyricWnd, and a later owner hide would hide the desktop popup.
    desktop_lyric_mode_ = true;
    ActiveLyricVisible() = true;
    ShowWindow(lyric_window_, SW_HIDE);
    desktop_lyrics_.ApplySettings();
    desktop_lyrics_.Show(true);
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::LeaveDesktopLyricMode() {
    if (!desktop_lyric_mode_) return;
    desktop_lyrics_.CaptureBounds();
    desktop_lyrics_.Show(false);
    // FUN_0044D48D releases a locked desktop strip before returning to the
    // normal LyricCtrl so it cannot remain an invisible click-through state.
    if (settings_.desktop_lyric.lock) {
        settings_.desktop_lyric.lock = false;
        desktop_lyrics_.ApplySettings();
    }
    desktop_lyric_mode_ = false;
    if (lyric_window_ && ActiveLyricVisible()) {
        ShowWindow(lyric_window_, SW_SHOWNOACTIVATE);
        BringWindowToTop(lyric_window_);
    }
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::CreateLyricControls() {
    if (!lyric_window_ || !skin_ || !skin_->Lyric().valid) return;

    const auto create_button = [this](HWND& destination, UINT command,
                                      bool tab_stop = true) {
        if (destination && IsWindow(destination)) return;
        destination = CreateWindowExW(0, kEqualizerButtonClass, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                (tab_stop ? WS_TABSTOP : 0),
            0, 0, 0, 0, lyric_window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(command)), instance_, this);
    };
    // FUN_0044AD55 creates all three commands even when a particular skin
    // supplies an empty rectangle/image; LayoutLyricControls then leaves that
    // control at zero extent. This is visible in LX-iPlay's child enumeration.
    create_button(lyric_desklrc_, kCmdDesktopLyrics, false);
    create_button(lyric_ontop_, kCmdLyricTopMost);
    // 00449451 -> 0044E5CE -> 0044919C rebinds existing controls. In
    // particular, destroying LyricCtrl also destroyed the unsaved RichEdit
    // document, its selection/undo history and the full-screen HWND.
    if (!lyric_control_ || !IsWindow(lyric_control_))
        lyric_control_ = CreateWindowExW(0, kLyricControlClass, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, lyric_window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLyricControlId)),
            instance_, this);
    UpdateLyricScrollTimer();
    create_button(lyric_close_, kCmdShowLyrics);
    if (!lyric_hidden_button_ || !IsWindow(lyric_hidden_button_))
        lyric_hidden_button_ = CreateWindowExW(0, kEqualizerButtonClass, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
            -16, -16, 16, 16, lyric_window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(0x82dc)),
            instance_, this);

    RebuildLyricFont(false);
    if (lyric_editor_) {
        const bool modified = SendMessageW(lyric_editor_, EM_GETMODIFY, 0, 0) != 0;
        SetLyricEditorFont(settings_.lyric.font_valid
            ? settings_.lyric.font : skin_->Lyric().font);
        SendMessageW(lyric_editor_, EM_SETBKGNDCOLOR, 0,
            settings_.lyric.background_color != CLR_INVALID
                ? settings_.lyric.background_color : skin_->Lyric().background_color);
        FormatLyricEditorAll();
        SendMessageW(lyric_editor_, EM_SETMODIFY, modified, 0);
    }
    LayoutLyricControls();
    UpdateLyricToolRects();
}

void PlayerWindow::RebuildLyricFont(bool repaint) {
    if (!skin_ || !skin_->Lyric().valid) return;

    LOGFONTW font = fullscreen_lyric_detached_
        ? settings_.lyric.fullscreen_font
        : (settings_.lyric.font_valid
            ? settings_.lyric.font : skin_->Lyric().font);
    // FUN_004499AD/FUN_0043F197 apply AutoFontFS only to the detached
    // vertical layout.  The first pass deliberately uses the control's
    // current font to choose one widest row.  Only when that raw extent does
    // not fit does the second pass try the configured full-screen font in
    // four-pixel steps; that pass adds the current absolute font height to
    // the row width, just like CLyricCtrl::FUN_0043F092.
    if (fullscreen_lyric_detached_ &&
        settings_.lyric.fullscreen_auto_font &&
        ActiveLyricScrollMode() == 0 && lyric_control_ &&
        !lyrics_.lines.empty()) {
        font.lfHeight = std::labs(font.lfHeight);
        RECT client{};
        GetClientRect(lyric_control_, &client);
        const int available = client.right - client.left;
        const HDC dc = GetDC(lyric_control_);
        if (dc && available > 0) {
            const HFONT current = reinterpret_cast<HFONT>(
                SendMessageW(lyric_control_, WM_GETFONT, 0, 0));
            const HGDIOBJ previous = current
                ? SelectObject(dc, current) : nullptr;
            const auto measure_row = [dc](const std::wstring& text) {
                RECT extent{};
                DrawTextW(dc, text.c_str(), static_cast<int>(text.size()),
                          &extent, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
                return extent.right - extent.left;
            };
            size_t widest{};
            LONG widest_width{};
            for (size_t index = 0; index < lyrics_.lines.size(); ++index) {
                const auto text = LyricLineText(index);
                const LONG extent = measure_row(text);
                if (extent > widest_width) {
                    widest = index;
                    widest_width = extent;
                }
            }
            if (widest_width >= available && widest_width > 0 && current) {
                const auto text = LyricLineText(widest);
                while (font.lfHeight > 15) {
                    const HFONT candidate = CreateFontIndirectW(&font);
                    bool fits = false;
                    if (candidate) {
                        SelectObject(dc, candidate);
                        const LONG compensated = measure_row(text) +
                            std::labs(font.lfHeight);
                        fits = compensated < available;
                        SelectObject(dc, current);
                        DeleteObject(candidate);
                    }
                    if (fits) break;
                    // The original tests 16 first and then leaves 12 as the
                    // final untested fallback because the loop condition is
                    // checked before font creation.
                    font.lfHeight -= 4;
                }
            }
            if (previous) SelectObject(dc, previous);
            ReleaseDC(lyric_control_, dc);
        } else if (dc) {
            ReleaseDC(lyric_control_, dc);
        }
    }

    // FUN_004499AD mutates only its stack copy after fitting.  CharSet and
    // color-key-safe glyph quality therefore affect the installed font, not
    // candidate measurement or the persisted FontFS value.
    if (settings_.lyric.charset != 0)
        font.lfCharSet = static_cast<BYTE>(settings_.lyric.charset);
    if (ActiveLyricTransparent()) font.lfQuality = NONANTIALIASED_QUALITY;

    HFONT replacement = CreateFontIndirectW(&font);

    if (!replacement) return;
    const HFONT previous = lyric_font_;
    lyric_font_ = replacement;
    if (lyric_control_)
        SendMessageW(lyric_control_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(lyric_font_), FALSE);
    if (previous) DeleteObject(previous);
    if (repaint && lyric_control_)
        InvalidateRect(lyric_control_, nullptr, FALSE);
}

void PlayerWindow::ApplyFullScreenLyricTransparency() {
    if (!fullscreen_lyric_detached_ || !lyric_control_) return;
    LONG_PTR extended = GetWindowLongPtrW(lyric_control_, GWL_EXSTYLE);
    if (settings_.lyric.fullscreen_transparent) {
        extended |= WS_EX_LAYERED;
        SetWindowLongPtrW(lyric_control_, GWL_EXSTYLE, extended);
        SetLayeredWindowAttributes(lyric_control_,
            settings_.lyric.fullscreen_background_color, 255,
            LWA_COLORKEY);
    } else {
        extended &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
        SetWindowLongPtrW(lyric_control_, GWL_EXSTYLE, extended);
    }
    SetWindowPos(lyric_control_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
        SWP_FRAMECHANGED);
    RedrawWindow(lyric_control_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

void PlayerWindow::DestroyLyricControls() {
    if (lyric_window_) RemoveToolTipTools(lyric_window_);
    DestroyLyricEditor();
    if (lyric_control_)
        KillTimer(lyric_control_, kLyricAnimationTimer);
    for (HWND* control : {&lyric_close_, &lyric_ontop_,
                          &lyric_desklrc_, &lyric_hidden_button_,
                          &lyric_control_}) {
        if (*control && IsWindow(*control)) DestroyWindow(*control);
        *control = nullptr;
    }
    if (lyric_font_) {
        DeleteObject(lyric_font_);
        lyric_font_ = nullptr;
    }
    lyric_hover_command_ = 0;
    lyric_pressed_command_ = 0;
    lyric_line_dragging_ = false;
    lyric_line_drag_offset_ = 0;
}

void PlayerWindow::ToggleLyricWindow() {
    if (!lyric_window_ && !CreateLyricWindow()) return;
    if (desktop_lyric_mode_) {
        const bool visible = desktop_lyrics_.Visible();
        desktop_lyrics_.Show(!visible);
        ActiveLyricVisible() = !visible;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    CompleteSkinWindowFadeForReplacement();
    if (!window_ || !IsWindow(window_) || close_after_skin_window_fade_) return;
    const bool visible = IsWindowVisible(lyric_window_) != FALSE;
    SetSkinWindowVisible(lyric_window_, !visible);
    ActiveLyricVisible() = !visible;
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::UpdateLyricWindowSkin(bool saved_bounds) {
    const bool desktop_was_visible =
        desktop_lyric_mode_ && desktop_lyrics_.Visible();
    if (!skin_ || !skin_->Lyric().valid) {
        if (lyric_window_) {
            ShowWindow(lyric_window_, SW_HIDE);
        }
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        desktop_lyrics_.SetSkin(skin_ ? &*skin_ : nullptr);
        return;
    }
    if (!lyric_window_ && !CreateLyricWindow()) return;
    ScopedSkinRedraw redraw(lyric_window_);
    if (mini_mode_ || saved_bounds) {
        CreateLyricControls();
        redraw.Resume();
        ApplyActiveLyricWindowState();
        if (desktop_lyric_mode_) {
            ShowWindow(lyric_window_, SW_HIDE);
            desktop_lyrics_.SetSkin(&*skin_);
            desktop_lyrics_.ApplySettings();
            desktop_lyrics_.Show(desktop_was_visible);
        }
        return;
    }
    const auto& layout = skin_->Lyric();
    RECT player{};
    GetWindowRect(window_, &player);
    int width = layout.background.size.cx;
    int height = layout.background.size.cy;
    const bool resizable = layout.resize_rect.right > layout.resize_rect.left &&
                           layout.resize_rect.bottom > layout.resize_rect.top;
    if (resizable) {
        width = std::max<int>(width, layout.position.right - layout.position.left);
        height = std::max<int>(height, layout.position.bottom - layout.position.top);
    }
    // 00468363 -> 00449451 rebinds the existing lyric HWND to the package's
    // position. A fixed-size skin such as LX-iPlay must not inherit the prior
    // package's larger dimensions.
    SetWindowPos(lyric_window_, ActiveLyricTopMost()
            ? HWND_TOPMOST : HWND_NOTOPMOST,
        player.left + layout.position.left, player.top + layout.position.top,
        width, height, SWP_NOACTIVATE);
    CreateLyricControls();
    UpdateLyricWindowRegion();
    redraw.Resume();
    RedrawWindow(lyric_window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN |
        RDW_UPDATENOW);
    if (ActiveLyricVisible())
        ShowWindow(lyric_window_, SW_SHOWNOACTIVATE);
    desktop_lyrics_.SetSkin(&*skin_);
    desktop_lyrics_.ApplySettings();
    if (desktop_lyric_mode_) {
        ShowWindow(lyric_window_, SW_HIDE);
        ActiveLyricVisible() = desktop_was_visible;
        desktop_lyrics_.Show(desktop_was_visible);
    }
}

RECT PlayerWindow::LyricElementBounds(const skin::SkinElement& element) const {
    RECT client{};
    if (lyric_window_) GetClientRect(lyric_window_, &client);
    const SIZE native = skin_ ? skin_->Lyric().background.size : SIZE{};
    return ResolveAlignedRect(element.bounds, element.alignment, native,
                              client.right, client.bottom);
}

RECT PlayerWindow::LyricTextBounds() const {
    if (!skin_ || !skin_->Lyric().valid) return {};
    if (mini_mode_) {
        RECT client{};
        if (lyric_window_) GetClientRect(lyric_window_, &client);
        // FUN_004495C8: InflateRect(-2,-2), followed by right -= 2.
        return {2, 2, std::max<LONG>(2, client.right - 4),
                std::max<LONG>(2, client.bottom - 2)};
    }
    RECT result = skin_->Lyric().lyric_bounds;
    RECT client{};
    if (lyric_window_) GetClientRect(lyric_window_, &client);
    result.right += std::max<LONG>(0, client.right - skin_->Lyric().background.size.cx);
    result.bottom += std::max<LONG>(0, client.bottom - skin_->Lyric().background.size.cy);
    return result;
}

void PlayerWindow::LayoutLyricControls() {
    if (!lyric_window_ || !skin_ || !skin_->Lyric().valid) return;
    const auto move = [this](HWND control, const skin::SkinElement& element) {
        if (!control) return;
        const RECT bounds = LyricElementBounds(element);
        MoveWindow(control, bounds.left, bounds.top,
                   std::max<LONG>(0, bounds.right - bounds.left),
                   std::max<LONG>(0, bounds.bottom - bounds.top), TRUE);
    };
    // FUN_004495C8 has a third layout in addition to normal and mini mode:
    // Transparent+TransSkin parks the chrome and expands LyricCtrl over the
    // complete client. The colour-keyed parent then leaves only glyphs.
    const bool text_only = settings_.lyric.transparent &&
                           settings_.lyric.transparent_skin;
    if (mini_mode_ || text_only) {
        // The three normal chrome buttons remain real/visible child HWNDs in
        // the original, but FUN_004495C8 parks them at -1000,-1000 while the
        // same LyricCtrl is rebound to the mini rectangle.
        const auto park = [](HWND control, const skin::SkinElement& element) {
            if (!control) return;
            MoveWindow(control, -1000, -1000,
                std::max<LONG>(0, element.bounds.right - element.bounds.left),
                std::max<LONG>(0, element.bounds.bottom - element.bounds.top), TRUE);
        };
        park(lyric_close_, skin_->Lyric().close);
        park(lyric_ontop_, skin_->Lyric().ontop);
        park(lyric_desklrc_, skin_->Lyric().desklrc);
    } else {
        move(lyric_close_, skin_->Lyric().close);
        move(lyric_ontop_, skin_->Lyric().ontop);
        move(lyric_desklrc_, skin_->Lyric().desklrc);
    }
    if (lyric_control_) {
        RECT bounds = LyricTextBounds();
        if (text_only) {
            RECT client{};
            GetClientRect(lyric_window_, &client);
            bounds = client;
        }
        MoveWindow(lyric_control_, bounds.left, bounds.top,
                   std::max<LONG>(0, bounds.right - bounds.left),
                   std::max<LONG>(0, bounds.bottom - bounds.top), TRUE);
    }
    LayoutLyricEditor();
}

void PlayerWindow::UpdateLyricToolRects() {
    if (!lyric_window_ || !CreateToolTipWindow()) return;
    RemoveToolTipTools(lyric_window_);
    if (!skin_ || !skin_->Lyric().valid) return;
    if (skin_->Lyric().close.image) AddToolTipControl(lyric_close_, kCmdShowLyrics);
    if (skin_->Lyric().ontop.image) AddToolTipControl(lyric_ontop_, kCmdLyricTopMost);
    if (skin_->Lyric().desklrc.image) AddToolTipControl(lyric_desklrc_, kCmdDesktopLyrics);
}

void PlayerWindow::UpdateLyricWindowRegion() {
    if (!lyric_window_ || !skin_ || !skin_->Lyric().valid) return;
    if (mini_mode_) {
        // FUN_00449577 removes the shaped normal-skin region in mini mode.
        SetWindowRgn(lyric_window_, nullptr, TRUE);
        return;
    }
    RECT client{};
    GetClientRect(lyric_window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) return;
    const auto& layout = skin_->Lyric();
    HRGN region{};
    if (width == layout.background.size.cx && height == layout.background.size.cy) {
        region = CreateColorKeyRegion(layout.background.image, width, height,
                                      skin_->TransparentColor());
    } else {
        const HDC screen = GetDC(nullptr);
        const HDC canvas = CreateCompatibleDC(screen);
        const HBITMAP rendered = CreateCompatibleBitmap(screen, width, height);
        ReleaseDC(nullptr, screen);
        if (canvas && rendered) {
            const HGDIOBJ old = SelectObject(canvas, rendered);
            DrawResizableSkinBitmap(canvas, layout.background, layout.resize_rect,
                                    width, height, layout.resize_tile);
            SelectObject(canvas, old);
            region = CreateColorKeyRegion(rendered, width, height,
                                          skin_->TransparentColor());
        }
        if (rendered) DeleteObject(rendered);
        if (canvas) DeleteDC(canvas);
    }
    RECT bounds{};
    if (!region || GetRgnBox(region, &bounds) == NULLREGION ||
        !SetWindowRgn(lyric_window_, region, TRUE)) {
        if (region) DeleteObject(region);
    }
}

void PlayerWindow::PaintLyricWindow(HDC dc) const {
    RECT client{};
    GetClientRect(lyric_window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) return;
    const HDC canvas = CreateCompatibleDC(dc);
    const HBITMAP buffer = CreateCompatibleBitmap(dc, width, height);
    const HGDIOBJ old_buffer = SelectObject(canvas, buffer);
    FillRect(canvas, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (skin_ && skin_->Lyric().valid) {
        const auto& layout = skin_->Lyric();
        const COLORREF background_color =
            settings_.lyric.background_color != CLR_INVALID
                ? settings_.lyric.background_color : layout.background_color;
        const bool text_only = settings_.lyric.transparent &&
                               settings_.lyric.transparent_skin;
        if (text_only) {
            // FUN_00449313 omits both normal chrome and mini borders when
            // background colour-keying and TransSkin are enabled together.
            const HBRUSH background = CreateSolidBrush(background_color);
            FillRect(canvas, &client, background);
            DeleteObject(background);
        } else if (!mini_mode_) {
            // FUN_00449313 takes the normal-mode branch through FUN_0044E8CC:
            // paint the lyric-window skin without consulting mini_border.
            DrawResizableSkinBitmap(canvas, layout.background, layout.resize_rect,
                                    width, height, layout.resize_tile);
            DrawElementFrame(canvas, layout.title, LyricElementBounds(layout.title),
                             0, skin_->TransparentColor());
        } else {
            // mini_border belongs to FUN_00449313's DAT_0054775C (mini-mode)
            // branch and surrounds the lyric popup client, not LyricCtrl.
            // The native fallback uses the active lyric text color for both
            // edges when the skin leaves either colour unspecified.
            const HBRUSH background = CreateSolidBrush(background_color);
            FillRect(canvas, &client, background);
            DeleteObject(background);
            COLORREF left_top = settings_.lyric.text_color;
            COLORREF right_bottom = settings_.lyric.text_color;
            if (layout.mini_border_left_top != 0xff000000 &&
                layout.mini_border_right_bottom != 0xff000000) {
                left_top = layout.mini_border_left_top;
                right_bottom = layout.mini_border_right_bottom;
            }
            const HPEN light = CreatePen(PS_SOLID, 1, left_top);
            const HPEN dark = CreatePen(PS_SOLID, 1, right_bottom);
            const HGDIOBJ old_pen = SelectObject(canvas, light);
            MoveToEx(canvas, 0, height - 1, nullptr);
            LineTo(canvas, 0, 0);
            LineTo(canvas, width - 1, 0);
            SelectObject(canvas, dark);
            LineTo(canvas, width - 1, height - 1);
            LineTo(canvas, 0, height - 1);
            SelectObject(canvas, old_pen);
            DeleteObject(light);
            DeleteObject(dark);
        }
    }
    BitBlt(dc, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
    SelectObject(canvas, old_buffer);
    DeleteObject(buffer);
    DeleteDC(canvas);
}

void PlayerWindow::PaintLyricControl(HWND control, HDC dc) const {
    RECT client{};
    GetClientRect(control, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0 || !skin_ || !skin_->Lyric().valid) return;
    const auto& layout = skin_->Lyric();
    const bool text_control = control == lyric_control_ ||
        GetDlgCtrlID(control) == kLyricControlId;

    const HDC canvas = CreateCompatibleDC(dc);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* lyric_pixels{};
    HBITMAP buffer = CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS,
                                      &lyric_pixels, nullptr, 0);
    if (!buffer) buffer = CreateCompatibleBitmap(dc, width, height);
    if (!canvas || !buffer) {
        if (buffer) DeleteObject(buffer);
        if (canvas) DeleteDC(canvas);
        return;
    }
    const HGDIOBJ old_buffer = SelectObject(canvas, buffer);
    if (!text_control) {
        RECT parent_client{};
        GetClientRect(lyric_window_, &parent_client);
        const HDC parent_dc = CreateCompatibleDC(dc);
        const HBITMAP parent_bitmap = CreateCompatibleBitmap(
            dc, std::max<LONG>(1, parent_client.right),
            std::max<LONG>(1, parent_client.bottom));
        const HGDIOBJ old_parent = SelectObject(parent_dc, parent_bitmap);
        DrawResizableSkinBitmap(parent_dc, layout.background, layout.resize_rect,
            parent_client.right, parent_client.bottom, layout.resize_tile);
        DrawElementFrame(parent_dc, layout.title, LyricElementBounds(layout.title),
                         0, skin_->TransparentColor());
        POINT origin{};
        MapWindowPoints(control, lyric_window_, &origin, 1);
        BitBlt(canvas, 0, 0, width, height, parent_dc,
               origin.x, origin.y, SRCCOPY);
        SelectObject(parent_dc, old_parent);
        DeleteObject(parent_bitmap);
        DeleteDC(parent_dc);

        const int command = GetDlgCtrlID(control);
        const skin::SkinElement* element{};
        if (command == static_cast<int>(kCmdShowLyrics)) element = &layout.close;
        else if (command == static_cast<int>(kCmdLyricTopMost)) element = &layout.ontop;
        else if (command == static_cast<int>(kCmdDesktopLyrics)) element = &layout.desklrc;
        if (element) {
            int state = lyric_pressed_command_ == command &&
                        lyric_hover_command_ == command &&
                        GetCapture() == control
                ? 2 : lyric_hover_command_ == command ? 1 : 0;
            if (command == static_cast<int>(kCmdLyricTopMost) &&
                ActiveLyricTopMost()) state = 2;
            const RECT local{0, 0, width, height};
            DrawElementFrame(canvas, *element, local, state,
                             skin_->TransparentColor());
        }
    } else {
        const COLORREF text_color = ActiveLyricTextColor();
        const COLORREF highlight_color = ActiveLyricHighlightColor();
        const COLORREF background_color = ActiveLyricBackgroundColor();
        const HBRUSH background = CreateSolidBrush(background_color);
        FillRect(canvas, &client, background);
        DeleteObject(background);
        SetBkMode(canvas, TRANSPARENT);
        const HGDIOBJ old_font = SelectObject(canvas,
            lyric_font_ ? lyric_font_ : GetStockObject(DEFAULT_GUI_FONT));
        const int drag = lyric_line_dragging_ ? lyric_line_drag_offset_ : 0;
        // FUN_0043F766 samples the decoder callback once and derives the line,
        // start/end interval and pixel phase from that single value.  Sampling
        // Position() twice can cross a timestamp between the two reads.
        const auto playback_position = audio_.Position();
        const auto playback_line = lyrics_.LineAt(playback_position);
        const auto position = LyricDragPosition(canvas, drag,
                                                playback_position);
        const size_t current = position.first;
        const int phase = position.second;

        const auto animated_line_color = [&](size_t index) {
            COLORREF color = text_color;
            if (!ActiveLyricFadeHighlight() || lyric_line_dragging_ ||
                !playback_line || *playback_line >= lyrics_.lines.size())
                return color;
            constexpr auto transition = std::chrono::milliseconds(500);
            const size_t active_line = *playback_line;
            const auto clock = playback_position - lyrics_.offset;
            const auto start = lyrics_.lines[active_line].time;
            const auto end = active_line + 1 < lyrics_.lines.size()
                ? lyrics_.lines[active_line + 1].time
                : start + std::chrono::seconds(60);
            if (index + 1 == active_line) {
                const int elapsed = static_cast<int>(std::clamp<long long>(
                    (clock - start).count(), 0, transition.count()));
                if (elapsed < transition.count())
                    color = BlendColor(highlight_color, text_color, elapsed,
                                       static_cast<int>(transition.count()));
            } else if (!ActiveLyricKaraokeMode() &&
                       index == active_line + 1) {
                const int remaining = static_cast<int>(std::clamp<long long>(
                    (end - clock).count(), 0, transition.count()));
                if (remaining < transition.count())
                    color = BlendColor(highlight_color, text_color, remaining,
                                       static_cast<int>(transition.count()));
            }
            return color;
        };

        const auto draw_lyric_line = [&](size_t index, RECT line, UINT format,
                                         bool horizontal_stream) {
            const auto text = LyricLineText(index);
            if (index != current) {
                SetTextColor(canvas, animated_line_color(index));
                DrawTextW(canvas, text.c_str(), -1, &line, format);
                return;
            }
            if (!ActiveLyricKaraokeMode() || lyric_line_dragging_ ||
                !playback_line || *playback_line != current) {
                SetTextColor(canvas, highlight_color);
                DrawTextW(canvas, text.c_str(), -1, &line, format);
                return;
            }

            // With KaraokeMode=1 the original draws the same current line in
            // two clipped passes: highlighted before the playback boundary,
            // normal after it (0043FC10 at 00440426/0044080E).
            SetTextColor(canvas, text_color);
            DrawTextW(canvas, text.c_str(), -1, &line, format);
            RECT highlight_clip = line;
            if (horizontal_stream) {
                highlight_clip.right = width / 2;
            } else {
                SIZE measured{};
                GetTextExtentPoint32W(canvas, text.c_str(),
                                      static_cast<int>(text.size()), &measured);
                int text_left = line.left;
                if (ActiveLyricTextAlign() == 1)
                    text_left += ((line.right - line.left) - measured.cx) / 2;
                else if (ActiveLyricTextAlign() >= 2)
                    text_left = line.right - measured.cx;
                const auto start = lyrics_.lines[current].time;
                const auto end = current + 1 < lyrics_.lines.size()
                    ? lyrics_.lines[current + 1].time
                    : start + std::chrono::seconds(60);
                const auto clock = playback_position - lyrics_.offset;
                const auto span = std::max<long long>(1, (end - start).count());
                const int highlighted = static_cast<int>(
                    std::clamp<long long>((clock - start).count(), 0, span) *
                    measured.cx / span);
                highlight_clip.left = text_left;
                highlight_clip.right = text_left + highlighted;
            }
            if (highlight_clip.right > highlight_clip.left) {
                const int saved = SaveDC(canvas);
                IntersectClipRect(canvas, highlight_clip.left,
                                  highlight_clip.top, highlight_clip.right,
                                  highlight_clip.bottom);
                SetTextColor(canvas, highlight_color);
                DrawTextW(canvas, text.c_str(), -1, &line, format);
                if (saved != 0) RestoreDC(canvas, saved);
            }
        };

        if (const auto* fallback = PlaybackTrackForUi();
            lyrics_.lines.empty() && fallback) {
            LOGFONTW active_font{};
            const LONG configured_height = lyric_font_ &&
                    GetObjectW(lyric_font_, sizeof(active_font), &active_font)
                ? active_font.lfHeight
                : (fullscreen_lyric_detached_
                    ? settings_.lyric.fullscreen_font.lfHeight
                    : (settings_.lyric.font_valid
                        ? settings_.lyric.font.lfHeight : layout.font.lfHeight));
            const int line_height = std::max(1L, std::labs(configured_height));
            RECT line{3, height / 2 - line_height / 2,
                      std::max(3, width - 3), height / 2 + line_height / 2};
            UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER;
            if (ActiveLyricScrollMode() != 0) format |= DT_CENTER;
            else if (ActiveLyricTextAlign() == 1) format |= DT_CENTER;
            else if (ActiveLyricTextAlign() >= 2) format |= DT_RIGHT;
            SetTextColor(canvas, highlight_color);
            const auto text = DisplayName(*fallback);
            DrawTextW(canvas, text.c_str(), -1, &line, format);
        } else if (!lyrics_.lines.empty()) {
            if (ActiveLyricScrollMode() != 0) {
                // XML ScrollMode=1 maps to CLyricCtrl's internal +0x6c == 0;
                // FUN_00441FEF then lays it out as one horizontal stream.
                // Every line owns its measured text width plus the recovered
                // 20-pixel inter-line gap; +0xac is the pixel phase at centre.
                int left = width / 2 - phase;
                for (size_t index = current; index > 0; --index)
                    left -= LyricLineExtent(canvas, index - 1);
                UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_CENTER;
                if (ActiveLyricTextAlign() == 1) format |= DT_VCENTER;
                else if (ActiveLyricTextAlign() >= 2) format |= DT_BOTTOM;
                for (size_t index = 0; index < lyrics_.lines.size(); ++index) {
                    const int extent = LyricLineExtent(canvas, index);
                    const RECT line{left, 0, left + extent, height};
                    if (line.right > 0 && line.left < width) {
                        draw_lyric_line(index, line, format, true);
                    }
                    left += extent;
                }
            } else {
                const int line_height = LyricLineExtent(canvas, current);
                int top = height / 2 - phase;
                for (size_t index = current; index > 0; --index)
                    top -= LyricLineExtent(canvas, index - 1);
                UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER;
                if (ActiveLyricTextAlign() == 1) format |= DT_CENTER;
                else if (ActiveLyricTextAlign() >= 2) format |= DT_RIGHT;
                for (size_t index = 0; index < lyrics_.lines.size(); ++index) {
                    const int extent = index == current ? line_height
                        : LyricLineExtent(canvas, index);
                    RECT line{0, top, width, top + extent};
                    if (line.bottom > 0 && line.top < height) {
                        draw_lyric_line(index, line, format, false);
                    }
                    top += extent;
                }
            }
        }

        if (lyric_line_dragging_) {
            // FUN_00441330 paints a centre seek guide while capture is active.
            const COLORREF guide_color = BlendColor(
                highlight_color, background_color, 1, 2);
            const HPEN pen = CreatePen(PS_SOLID, 1, guide_color);
            const HGDIOBJ old_pen = SelectObject(canvas, pen);
            std::optional<std::chrono::milliseconds> target =
                LyricDragTime(canvas, drag);
            const HGDIOBJ guide_font = SelectObject(
                canvas, GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(canvas, guide_color);
            SetBkMode(canvas, TRANSPARENT);
            if (ActiveLyricScrollMode() != 0) {
                const int centre = width / 2;
                MoveToEx(canvas, centre - 4, 1, nullptr); LineTo(canvas, centre + 4, 1);
                MoveToEx(canvas, centre, 1, nullptr); LineTo(canvas, centre, height - 1);
                MoveToEx(canvas, centre - 4, height - 1, nullptr);
                LineTo(canvas, centre + 4, height - 1);
                if (target) {
                    RECT label{centre + 1, 1, width, height - 1};
                    const auto text = FormatInfoDuration(*target);
                    DrawTextW(canvas, text.c_str(), -1, &label,
                              DT_SINGLELINE | DT_BOTTOM);
                }
            } else {
                const int centre = height / 2;
                MoveToEx(canvas, 1, centre - 4, nullptr); LineTo(canvas, 1, centre + 4);
                MoveToEx(canvas, 1, centre, nullptr); LineTo(canvas, width - 1, centre);
                MoveToEx(canvas, width - 1, centre - 4, nullptr);
                LineTo(canvas, width - 1, centre + 4);
                if (target) {
                    RECT label{1, centre + 1, width - 1, height};
                    const auto text = FormatInfoDuration(*target);
                    DrawTextW(canvas, text.c_str(), -1, &label,
                              DT_SINGLELINE | DT_RIGHT);
                }
            }
            SelectObject(canvas, guide_font);
            SelectObject(canvas, old_pen);
            DeleteObject(pen);
        }

        if (lyric_pixels && !ActiveLyricTransparent() &&
            ActiveLyricFadeIndex() > 0) {
            // FUN_004416E0 divides the active axis by FadeIndex, then blends
            // every edge pixel toward BkgndColor.  It is a bitmap gradient,
            // not a per-line color shortcut, so antialiased glyph pixels must
            // take part in the same operation.
            const bool horizontal_stream = ActiveLyricScrollMode() != 0;
            const int axis_length = horizontal_stream ? width : height;
            const int fade_extent = std::max(
                1, axis_length / ActiveLyricFadeIndex());
            auto* pixels = static_cast<BYTE*>(lyric_pixels);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int coordinate = horizontal_stream ? x : y;
                    const int edge_distance = std::min(
                        coordinate, axis_length - 1 - coordinate);
                    if (edge_distance >= fade_extent) continue;
                    const int foreground = std::clamp(
                        MulDiv(edge_distance, 256, fade_extent), 0, 256);
                    BYTE* pixel = pixels +
                        (static_cast<size_t>(y) * width + x) * 4;
                    pixel[0] = static_cast<BYTE>((pixel[0] * foreground +
                        GetBValue(background_color) * (256 - foreground)) >> 8);
                    pixel[1] = static_cast<BYTE>((pixel[1] * foreground +
                        GetGValue(background_color) * (256 - foreground)) >> 8);
                    pixel[2] = static_cast<BYTE>((pixel[2] * foreground +
                        GetRValue(background_color) * (256 - foreground)) >> 8);
                }
            }
        }
        SelectObject(canvas, old_font);
    }
    BitBlt(dc, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
    SelectObject(canvas, old_buffer);
    DeleteObject(buffer);
    DeleteDC(canvas);
}

std::wstring PlayerWindow::LyricLineText(size_t index) const {
    if (index >= lyrics_.lines.size()) return {};
    try { return core::Utf8ToWide(lyrics_.lines[index].text); }
    catch (const std::exception&) { return {}; }
}

int PlayerWindow::LyricLineExtent(HDC dc, size_t index) const {
    if (!skin_ || !skin_->Lyric().valid) return 1;
    if (ActiveLyricScrollMode() == 0) {
        LOGFONTW active_font{};
        const LONG font_height = lyric_font_ &&
                GetObjectW(lyric_font_, sizeof(active_font), &active_font)
            ? active_font.lfHeight
            : (fullscreen_lyric_detached_
                ? settings_.lyric.fullscreen_font.lfHeight
                : (settings_.lyric.font_valid
                    ? settings_.lyric.font.lfHeight
                    : skin_->Lyric().font.lfHeight));
        return std::max(1L, std::labs(font_height)) +
               std::max(0, ActiveLyricRowInterval());
    }
    const auto text = LyricLineText(index);
    RECT measured{};
    DrawTextW(dc, text.c_str(), -1, &measured,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    // FUN_0043F092 returns the rendered width plus object offset +0x94,
    // initialized to 20 by CLyricCtrl's constructor.
    return std::max(1L, measured.right - measured.left + 20);
}

std::pair<size_t, int> PlayerWindow::LyricDragPosition(HDC dc, int delta) const {
    return LyricDragPosition(dc, delta, audio_.Position());
}

std::pair<size_t, int> PlayerWindow::LyricDragPosition(
    HDC dc, int delta, std::chrono::milliseconds playback_position) const {
    if (lyrics_.lines.empty()) return {0, 0};
    const auto selected = lyrics_.LineAt(playback_position);
    size_t line = selected.value_or(0);
    line = std::min(line, lyrics_.lines.size() - 1);
    int extent = LyricLineExtent(dc, line);
    int phase{};
    if (selected) {
        const auto start = lyrics_.lines[line].time;
        const auto end = line + 1 < lyrics_.lines.size()
            ? lyrics_.lines[line + 1].time : start + std::chrono::seconds(60);
        const auto span = end - start;
        const auto elapsed = playback_position - lyrics_.offset - start;
        if (span.count() > 0) {
            phase = std::clamp<int>(MulDiv(
                static_cast<int>(std::clamp<long long>(elapsed.count(), 0,
                    std::min<long long>(span.count(), INT_MAX))),
                extent, static_cast<int>(std::min<long long>(span.count(), INT_MAX))),
                0, extent);
        }
    }

    // This is FUN_0043FA1D: subtract the captured pixel delta, walking over
    // variable horizontal widths or the fixed vertical row interval.
    phase -= delta;
    while (phase < 0 && line > 0) {
        --line;
        phase += LyricLineExtent(dc, line);
    }
    extent = LyricLineExtent(dc, line);
    while (phase > extent && line + 1 < lyrics_.lines.size()) {
        phase -= extent;
        ++line;
        extent = LyricLineExtent(dc, line);
    }
    if (line == 0 && phase < 0) phase = 0;
    if (line + 1 == lyrics_.lines.size() && phase > extent) phase = extent;
    return {line, std::clamp(phase, 0, extent)};
}

std::optional<std::chrono::milliseconds> PlayerWindow::LyricDragTime(
    HDC dc, int delta) const {
    if (lyrics_.lines.empty()) return std::nullopt;
    const auto [line, phase] = LyricDragPosition(dc, delta);
    const int extent = std::max(1, LyricLineExtent(dc, line));
    const auto start = lyrics_.lines[line].time;
    const auto end = line + 1 < lyrics_.lines.size()
        ? lyrics_.lines[line + 1].time : start + std::chrono::seconds(60);
    auto target = start;
    if (end > start) {
        target += std::chrono::milliseconds(
            static_cast<long long>(phase) * (end - start).count() / extent);
    }
    return std::max(std::chrono::milliseconds::zero(), target + lyrics_.offset);
}

bool PlayerWindow::LyricTextHitTest(HWND control, POINT point) const {
    if (!control || lyrics_.lines.empty() || !skin_ || !skin_->Lyric().valid)
        return false;
    RECT client{};
    GetClientRect(control, &client);
    const HDC dc = GetDC(control);
    if (!dc) return false;
    const HGDIOBJ old_font = SelectObject(dc,
        lyric_font_ ? lyric_font_ : GetStockObject(DEFAULT_GUI_FONT));
    const auto [current, phase] = LyricDragPosition(dc, 0);
    bool hit{};
    if (ActiveLyricScrollMode() != 0) {
        int left = (client.right - client.left) / 2 - phase;
        for (size_t index = current; index > 0; --index)
            left -= LyricLineExtent(dc, index - 1);
        for (size_t index = 0; index < lyrics_.lines.size() && !hit; ++index) {
            const auto text = LyricLineText(index);
            SIZE measured{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &measured);
            const int extent = LyricLineExtent(dc, index);
            int top{};
            if (ActiveLyricTextAlign() == 1)
                top = ((client.bottom - client.top) - measured.cy) / 2;
            else if (ActiveLyricTextAlign() >= 2)
                top = (client.bottom - client.top) - measured.cy;
            const RECT text_bounds{left + (extent - measured.cx) / 2, top,
                left + (extent + measured.cx) / 2, top + measured.cy};
            hit = !text.empty() && PtInRect(&text_bounds, point) != FALSE;
            left += extent;
        }
    } else {
        int top = (client.bottom - client.top) / 2 - phase;
        for (size_t index = current; index > 0; --index)
            top -= LyricLineExtent(dc, index - 1);
        for (size_t index = 0; index < lyrics_.lines.size() && !hit; ++index) {
            const auto text = LyricLineText(index);
            SIZE measured{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &measured);
            const int extent = LyricLineExtent(dc, index);
            int left{};
            if (ActiveLyricTextAlign() == 1)
                left = ((client.right - client.left) - measured.cx) / 2;
            else if (ActiveLyricTextAlign() >= 2)
                left = (client.right - client.left) - measured.cx;
            const RECT text_bounds{left, top + (extent - measured.cy) / 2,
                left + measured.cx, top + (extent + measured.cy) / 2};
            hit = !text.empty() && PtInRect(&text_bounds, point) != FALSE;
            top += extent;
        }
    }
    SelectObject(dc, old_font);
    ReleaseDC(control, dc);
    return hit;
}

unsigned int PlayerWindow::LyricDragHitTest(POINT point) const {
    if (!lyric_window_ || !skin_ || !skin_->Lyric().valid) return kDragMove;
    if (mini_mode_) {
        // FUN_004495A0 collapses every generic resize hit except 0x10
        // (right edge) to the move code 1. Mini lyrics therefore resize only
        // horizontally from the right and keep mini_window's fixed height.
        RECT client{};
        GetClientRect(lyric_window_, &client);
        const int margin = std::max(1, std::min(4,
            GetSystemMetrics(SM_CXFRAME)));
        return point.x >= client.right - margin && point.x <= client.right
            ? kDragRight : kDragMove;
    }
    const RECT resize = skin_->Lyric().resize_rect;
    if (resize.right <= resize.left || resize.bottom <= resize.top) return kDragMove;
    RECT client{};
    GetClientRect(lyric_window_, &client);
    const int horizontal = std::max(1, std::min(4, GetSystemMetrics(SM_CXFRAME)));
    const int vertical = std::max(1, std::min(4, GetSystemMetrics(SM_CYFRAME)));
    unsigned int hit{};
    if (point.x >= client.right - horizontal) hit |= kDragRight;
    else if (point.x <= client.left + horizontal) hit |= kDragLeft;
    if (point.y >= client.bottom - vertical) hit |= kDragBottom;
    else if (point.y <= client.top + vertical) hit |= kDragTop;
    if ((hit & (kDragLeft | kDragRight)) == 0 &&
        (hit & (kDragTop | kDragBottom)) != 0) {
        if (point.x >= client.right - horizontal * 2) hit |= kDragRight;
        else if (point.x <= client.left + horizontal * 2) hit |= kDragLeft;
    } else if ((hit & (kDragLeft | kDragRight)) != 0 &&
               (hit & (kDragTop | kDragBottom)) == 0) {
        if (point.y >= client.bottom - vertical * 2) hit |= kDragBottom;
        else if (point.y <= client.top + vertical * 2) hit |= kDragTop;
    }
    return hit == 0 ? kDragMove : hit;
}

std::filesystem::path PlayerWindow::DefaultLyricEditorPath() const {
    const auto* track = PlaybackTrackForUi();
    if (!track)
        return std::filesystem::path(L"New Lyrics.lrc");
    auto path = track->path;
    path.replace_extension(L".lrc");
    return path;
}

std::wstring PlayerWindow::LyricEditorText() const {
    if (!lyric_editor_ || !IsWindow(lyric_editor_)) return {};
    LyricEditorStreamOut output;
    EDITSTREAM stream{};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(&output);
    stream.pfnCallback = WriteLyricEditorStream;
    SendMessageW(lyric_editor_, EM_STREAMOUT, SF_TEXT | SF_UNICODE,
                 reinterpret_cast<LPARAM>(&stream));
    return stream.dwError == 0 ? std::move(output.text) : std::wstring{};
}

void PlayerWindow::SetLyricEditorText(std::wstring_view text, bool modified) {
    if (!lyric_editor_) return;
    const std::wstring stable(text);
    LyricEditorStreamIn input{
        reinterpret_cast<const unsigned char*>(stable.data()),
        stable.size() * sizeof(wchar_t)};
    EDITSTREAM stream{};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(&input);
    stream.pfnCallback = ReadLyricEditorStream;
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomSuspend, nullptr);
    SendMessageW(lyric_editor_, EM_STREAMIN, SF_TEXT | SF_UNICODE,
                 reinterpret_cast<LPARAM>(&stream));
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomResume, nullptr);
    lyric_editor_edit_kind_ = 0;
    SendMessageW(lyric_editor_, EM_SETMODIFY, modified, 0);
}

void PlayerWindow::InitializeLyricEditorRichText() {
    if (!lyric_editor_) return;
    if (lyric_editor_document_) {
        lyric_editor_document_->Release();
        lyric_editor_document_ = nullptr;
    }
    IRichEditOle* rich_ole{};
    SendMessageW(lyric_editor_, EM_GETOLEINTERFACE, 0,
                 reinterpret_cast<LPARAM>(&rich_ole));
    if (rich_ole) {
        static_cast<void>(rich_ole->QueryInterface(
            __uuidof(ITextDocument),
            reinterpret_cast<void**>(&lyric_editor_document_)));
        rich_ole->Release();
    }

    // CRichEditCtrlEx::Init (00443B37) suspends the TOM undo engine while it
    // installs the control contract.  Formatting must never become a user
    // undo record.
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomSuspend, nullptr);
    SendMessageW(lyric_editor_, EM_AUTOURLDETECT, TRUE, 0);
    // EM_SETTEXTMODE is legal only while the document is empty, which is why
    // this initialization intentionally precedes EM_STREAMIN.
    SendMessageW(lyric_editor_, EM_SETTEXTMODE, TM_MULTILEVELUNDO, 0);
    SendMessageW(lyric_editor_, EM_SETEVENTMASK, 0,
                 kLyricEditorEventMask);
    SendMessageW(lyric_editor_, EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    PARAFORMAT paragraph{};
    paragraph.cbSize = sizeof(paragraph); // 0x9c, as in 00443FCE.
    paragraph.dwMask = PFM_TABSTOPS;
    paragraph.cTabCount = MAX_TAB_STOPS;
    std::fill(std::begin(paragraph.rgxTabs), std::end(paragraph.rgxTabs),
              0x168);
    SendMessageW(lyric_editor_, EM_SETPARAFORMAT, 0,
                 reinterpret_cast<LPARAM>(&paragraph));

    // The private wrapper is constructed with SimSun/DEFAULT_CHARSET and a
    // nine-point (180 twip) default before application lyric settings replace
    // it in 0044CB58.
    CHARFORMAT2W format{sizeof(format)};
    format.dwMask = kLyricEditorCharacterMask;
    format.dwEffects = kLyricEditorCharacterEffects;
    format.yHeight = 180;
    format.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
    format.bCharSet = DEFAULT_CHARSET;
    format.bPitchAndFamily = 9;
    wcsncpy_s(format.szFaceName, L"SimSun", _TRUNCATE);
    // 00443B37 calls 004439E6 while its own TOM suspension is still active;
    // 004439E6 adds this second suspend/resume pair around SCF_DEFAULT.
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomSuspend, nullptr);
    SendMessageW(lyric_editor_, EM_SETCHARFORMAT, SCF_DEFAULT,
                 reinterpret_cast<LPARAM>(&format));
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomResume, nullptr);
    // The refresh flag passed by 00443B37 makes 004439E6 execute its normal
    // text-length/full-format branch here.  The newly created document is
    // empty, but retaining the query preserves the original call sequence.
    FormatLyricEditorAll();
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomResume, nullptr);
}

void PlayerWindow::SetLyricEditorFont(const LOGFONTW& font) {
    if (!lyric_editor_) return;
    LONG height = 180;
    if (font.lfHeight != 0) {
        const HDC dc = GetDC(lyric_editor_);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(lyric_editor_, dc);
        if (dpi > 0) {
            const LONG scaled = static_cast<LONG>(
                (static_cast<long long>(std::abs(font.lfHeight)) * 1440) /
                dpi);
            height = std::max<LONG>(180, scaled);
        }
    }
    const COLORREF text_color = settings_.lyric.text_color != CLR_INVALID
        ? settings_.lyric.text_color
        : (skin_ ? skin_->Lyric().text_color : GetSysColor(COLOR_WINDOWTEXT));
    CHARFORMAT2W format{sizeof(format)};
    format.dwMask = kLyricEditorCharacterMask;
    format.dwEffects = kLyricEditorCharacterEffects;
    format.yHeight = height;
    format.crTextColor = text_color;
    format.bCharSet = font.lfCharSet;
    // The legacy stack byte is not assigned explicitly by 004439E6, but the
    // original's stable realized SimSun/GB2312 format reports 9 here.  Set the
    // recovered runtime value deterministically instead of inheriting stack
    // contents.
    format.bPitchAndFamily = 9;
    wcsncpy_s(format.szFaceName, font.lfFaceName, _TRUNCATE);
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomSuspend, nullptr);
    SendMessageW(lyric_editor_, EM_SETCHARFORMAT, SCF_DEFAULT,
                 reinterpret_cast<LPARAM>(&format));
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomResume, nullptr);
}

LONG PlayerWindow::FindLyricEditorCharacter(LONG begin, LONG end,
                                             wchar_t character) const {
    if (!lyric_editor_ || begin < 0 || end < 0 || begin == end) return -1;
    const wchar_t needle[]{character, L'\0'};
    FINDTEXTW find{{begin, end}, needle};
    return static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_FINDTEXTW, begin < end ? FR_DOWN : 0,
        reinterpret_cast<LPARAM>(&find)));
}

std::wstring PlayerWindow::LyricEditorRangeText(LONG begin, LONG end) const {
    if (!lyric_editor_ || begin < 0 || begin >= end) return {};
    std::wstring result(static_cast<size_t>(end - begin) + 1, L'\0');
    TEXTRANGEW range{{begin, end}, result.data()};
    const LRESULT copied = SendMessageW(lyric_editor_, EM_GETTEXTRANGE, 0,
        reinterpret_cast<LPARAM>(&range));
    result.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return result;
}

void PlayerWindow::FormatLyricEditorRange(LONG begin, LONG end) {
    if (!lyric_editor_ || begin < 0 || begin >= end) return;
    CHARRANGE saved{};
    SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&saved));
    const LOGFONTW font = settings_.lyric.font_valid
        ? settings_.lyric.font : (skin_ ? skin_->Lyric().font : LOGFONTW{});
    LONG height = 180;
    if (font.lfHeight != 0) {
        const HDC dc = GetDC(lyric_editor_);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(lyric_editor_, dc);
        if (dpi > 0) {
            height = std::max<LONG>(180, static_cast<LONG>(
                (static_cast<long long>(std::abs(font.lfHeight)) * 1440) /
                dpi));
        }
    }
    const COLORREF text_color = settings_.lyric.text_color != CLR_INVALID
        ? settings_.lyric.text_color
        : (skin_ ? skin_->Lyric().text_color : GetSysColor(COLOR_WINDOWTEXT));
    const COLORREF tag_color = settings_.lyric.highlight_color != CLR_INVALID
        ? settings_.lyric.highlight_color
        : (skin_ ? skin_->Lyric().highlight_color : RGB(0, 0, 255));

    const auto apply = [this, &font, height](LONG first, LONG last,
                                             COLORREF color) {
        if (first >= last) return;
        CHARRANGE target{first, last};
        SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&target));
        CHARFORMAT2W format{sizeof(format)};
        format.dwMask = kLyricEditorCharacterMask;
        format.dwEffects = kLyricEditorCharacterEffects;
        format.yHeight = height;
        format.crTextColor = color;
        format.bCharSet = font.lfCharSet;
        format.bPitchAndFamily = 9;
        wcsncpy_s(format.szFaceName, font.lfFaceName, _TRUNCATE);
        if (lyric_editor_document_)
            lyric_editor_document_->Undo(tomSuspend, nullptr);
        SendMessageW(lyric_editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&format));
        if (lyric_editor_document_)
            lyric_editor_document_->Undo(tomResume, nullptr);
    };

    SendMessageW(lyric_editor_, WM_SETREDRAW, FALSE, 0);
    lyric_editor_internal_change_ = true;
    LONG cursor = begin;
    while (cursor < end) {
        const LONG open = FindLyricEditorCharacter(cursor, end, L'[');
        if (open < 0) {
            apply(cursor, end, text_color);
            break;
        }
        apply(cursor, open, text_color);
        const LONG close = FindLyricEditorCharacter(open + 1, end, L']');
        if (close < 0) {
            apply(open, end, text_color);
            break;
        }
        apply(open, close + 1, tag_color);
        cursor = close + 1;
    }
    SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&saved));
    lyric_editor_internal_change_ = false;
    SendMessageW(lyric_editor_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lyric_editor_, nullptr, TRUE);
}

void PlayerWindow::FormatLyricEditorAll() {
    if (!lyric_editor_) return;
    const LONG length = static_cast<LONG>(
        SendMessageW(lyric_editor_, WM_GETTEXTLENGTH, 0, 0));
    if (length > 0) FormatLyricEditorRange(0, length);
}

void PlayerWindow::FormatLyricEditorLines(LONG begin, LONG end) {
    if (!lyric_editor_) return;
    begin = std::max<LONG>(0, begin);
    end = std::max<LONG>(0, end);
    const LONG first_line = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_EXLINEFROMCHAR, 0, begin));
    const LONG last_line = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_EXLINEFROMCHAR, 0, end));
    if (first_line < 0 || last_line < 0) return;
    const LONG first = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_LINEINDEX, first_line, 0));
    const LONG last = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_LINEINDEX, last_line, 0));
    const LONG last_length = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_LINELENGTH, end, 0));
    if (first >= 0 && last >= 0 && last_length >= 0 &&
        first < last + last_length) {
        FormatLyricEditorRange(first, last + last_length);
    }
}

void PlayerWindow::RecordLyricEditorEdit(UINT message, WPARAM wparam,
                                          LONG begin, LONG end) {
    switch (message) {
    case EM_REPLACESEL:
    case WM_CHAR:
        lyric_editor_edit_kind_ = 2;
        break;
    case WM_KEYDOWN:
        if (wparam == VK_BACK)
            lyric_editor_edit_kind_ = 4;
        else if (wparam == VK_DELETE)
            lyric_editor_edit_kind_ = 3;
        else
            lyric_editor_edit_kind_ = 1;
        break;
    case WM_CUT:
        lyric_editor_edit_kind_ = 5;
        break;
    case WM_PASTE:
        lyric_editor_edit_kind_ = lyric_editor_edit_kind_ == 5 ? 7 : 6;
        break;
    case EM_SETCHARFORMAT:
        break;
    default:
        lyric_editor_edit_kind_ = 1;
        break;
    }
    if (message != EM_SETCHARFORMAT && lyric_editor_edit_kind_ != 7) {
        lyric_editor_edit_begin_ = begin;
        lyric_editor_edit_end_ = end;
    }
}

void PlayerWindow::HandleLyricEditorChange() {
    if (!lyric_editor_ || lyric_editor_internal_change_) return;
    CHARRANGE current{};
    SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&current));
    if (lyric_editor_edit_kind_ == 7 && current.cpMin == current.cpMax)
        lyric_editor_edit_kind_ = 6;
    LONG begin = current.cpMin;
    LONG end = current.cpMax;
    switch (lyric_editor_edit_kind_) {
    case 1:
        FormatLyricEditorAll();
        lyric_editor_edit_kind_ = 0;
        return;
    case 2:
    case 6:
        begin = lyric_editor_edit_begin_;
        break;
    case 7:
        FormatLyricEditorLines(current.cpMin, current.cpMax);
        begin = current.cpMin > lyric_editor_edit_begin_
            ? lyric_editor_edit_begin_ : lyric_editor_edit_end_;
        end = begin;
        break;
    default:
        break;
    }
    FormatLyricEditorLines(begin, end);
    lyric_editor_edit_kind_ = 0;
}

void PlayerWindow::OpenLyricEditorLink(UINT message, LONG begin, LONG end) {
    if (message != WM_LBUTTONUP ||
        (GetKeyState(VK_CONTROL) & 0x8000) == 0) return;
    const std::wstring target = LyricEditorRangeText(begin, end);
    if (!target.empty()) {
        ShellExecuteW(lyric_editor_, L"open", target.c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
}

void PlayerWindow::ReplaceLyricEditorRange(LONG begin, LONG end,
                                            std::wstring_view replacement,
                                            bool can_undo, bool reformat) {
    if (!lyric_editor_ || begin < 0 || (end != -1 && begin > end)) return;
    const std::wstring stable(replacement);
    CHARRANGE range{begin, end};
    SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&range));
    SendMessageW(lyric_editor_, WM_SETREDRAW, FALSE, 0);
    lyric_editor_internal_change_ = true;
    SendMessageW(lyric_editor_, EM_REPLACESEL, can_undo,
                 reinterpret_cast<LPARAM>(stable.c_str()));
    lyric_editor_internal_change_ = false;
    SendMessageW(lyric_editor_, WM_SETREDRAW, TRUE, 0);
    if (reformat) {
        FormatLyricEditorLines(begin,
            begin + static_cast<LONG>(stable.size()));
    }
    InvalidateRect(lyric_editor_, nullptr, TRUE);
}

bool PlayerWindow::EnterLyricEditor() {
    if (!lyric_window_ || !lyric_control_) return false;

    // FUN_0044CB58 creates the editor only after RichEdit20W is available.
    // Keep the module loaded for exactly the lifetime of the editor controls.
    if (!lyric_editor_module_)
        lyric_editor_module_ = LoadLibraryW(L"Riched20.dll");
    if (!lyric_editor_module_) return false;

    if (!lyric_editor_toolbar_) {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        WNDCLASSEXW toolbar_class{sizeof(toolbar_class)};
        if (!GetClassInfoExW(nullptr, TOOLBARCLASSNAMEW, &toolbar_class))
            GetClassInfoExW(GetModuleHandleW(L"comctl32.dll"),
                            TOOLBARCLASSNAMEW, &toolbar_class);
        if (toolbar_class.lpfnWndProc) {
            toolbar_class.hInstance = instance_;
            toolbar_class.style &= ~CS_GLOBALCLASS;
            toolbar_class.lpszClassName = kLyricEditorToolbarClass;
            if (!RegisterClassExW(&toolbar_class) &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                toolbar_class.lpfnWndProc = nullptr;
            }
        }
        const wchar_t* toolbar_name = toolbar_class.lpfnWndProc
            ? kLyricEditorToolbarClass : TOOLBARCLASSNAMEW;
        lyric_editor_toolbar_ = CreateWindowExW(0, toolbar_name, nullptr,
            static_cast<DWORD>(0x46018944UL), 0, 0, 0, 0, lyric_window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLyricEditorToolbarId)),
            instance_, nullptr);
        if (lyric_editor_toolbar_) {
            // The WTL wrapper adds TBSTYLE_TOOLTIPS after receiving the exact
            // 0x46018944 creation style.  Keep the toolbar's cached style
            // hidden until the later ShowWindow, as observed through
            // TB_GETSTYLE in the original process.
            SendMessageW(lyric_editor_toolbar_, TB_SETSTYLE, 0,
                static_cast<LPARAM>(0x46018945UL));
            SendMessageW(lyric_editor_toolbar_, TB_BUTTONSTRUCTSIZE,
                         sizeof(TBBUTTON), 0);
            const HMODULE resources = ResourceModule();
            const HRSRC toolbar_resource = resources ? FindResourceW(
                resources, MAKEINTRESOURCEW(kMenuLyricEditor),
                MAKEINTRESOURCEW(241)) : nullptr;
            const HGLOBAL loaded = toolbar_resource
                ? LoadResource(resources, toolbar_resource) : nullptr;
            const auto* words = loaded
                ? static_cast<const WORD*>(LockResource(loaded)) : nullptr;
            const DWORD bytes = toolbar_resource
                ? SizeofResource(resources, toolbar_resource) : 0;
            if (words && bytes >= 8 && words[0] == 1 && words[1] == 16 &&
                words[2] == 16 &&
                bytes >= static_cast<DWORD>((4 + words[3]) * sizeof(WORD))) {
                const HBITMAP bitmap = LoadBitmapW(
                    resources, MAKEINTRESOURCEW(kMenuLyricEditor));
                if (bitmap) {
                    lyric_editor_images_ = ImageList_Create(
                        16, 16, ILC_COLOR32 | ILC_MASK,
                        std::max<int>(1, words[3]), 1);
                    if (lyric_editor_images_)
                        ImageList_AddMasked(lyric_editor_images_, bitmap,
                                            RGB(192, 192, 192));
                    DeleteObject(bitmap);
                }
                if (lyric_editor_images_)
                    SendMessageW(lyric_editor_toolbar_, TB_SETIMAGELIST, 0,
                        reinterpret_cast<LPARAM>(lyric_editor_images_));
                std::vector<TBBUTTON> buttons;
                buttons.reserve(words[3]);
                int image = 0;
                for (size_t index = 0; index < words[3]; ++index) {
                    TBBUTTON button{};
                    const UINT command = words[4 + index];
                    if (command == 0) {
                        button.iBitmap = 8;
                        button.fsStyle = BTNS_SEP;
                    } else {
                        button.iBitmap = image++;
                        button.idCommand = static_cast<int>(command);
                        button.fsState = TBSTATE_ENABLED;
                        button.fsStyle = BTNS_BUTTON;
                    }
                    button.iString = -1;
                    buttons.push_back(button);
                }
                SendMessageW(lyric_editor_toolbar_, TB_SETBITMAPSIZE, 0,
                             MAKELPARAM(16, 16));
                SendMessageW(lyric_editor_toolbar_, TB_SETBUTTONSIZE, 0,
                             MAKELPARAM(23, 22));
                SendMessageW(lyric_editor_toolbar_, TB_ADDBUTTONS,
                    static_cast<WPARAM>(buttons.size()),
                    reinterpret_cast<LPARAM>(buttons.data()));
            }
        }
    }

    if (!lyric_editor_) {
        lyric_editor_ = CreateWindowExW(0, L"RichEdit20W", nullptr,
            static_cast<DWORD>(0x563081C4UL), 0, 0, 0, 0, lyric_window_,
            nullptr, instance_, nullptr);
        if (!lyric_editor_) return false;
        SetWindowSubclass(lyric_editor_, LyricEditorProc, 0x4c594544,
                          reinterpret_cast<DWORD_PTR>(this));
        InitializeLyricEditorRichText();
    }

    // +0x32C remains empty for a new document.  The adjacent sound-file name
    // is only a Save-dialog suggestion; treating it as an existing target
    // would make toolbar Save overwrite/create it without the original dialog.
    lyric_editor_path_ = lyrics_embedded_ ? std::filesystem::path{} : lyric_path_;
    lyric_editor_encoding_ = kEditorEncodingUtf8;
    lyric_editor_bom_ = true;
    std::error_code error;
    const bool have_source = !lyric_editor_path_.empty() &&
        std::filesystem::exists(lyric_editor_path_, error) && !error;
    const bool have_pending_offset = lyrics_.offset !=
        std::chrono::milliseconds::zero();
    if (have_source && !have_pending_offset) {
        // 0044CB58 -> 004431A0 -> 0043E237 streams an ordinary local lyric
        // file into RichEdit verbatim.  This is why the reference 千千阙歌
        // editor begins at [00:00.00], without synthesized ti/ar/al/by rows,
        // and retains its three-digit timestamps until the user edits them.
        SetLyricEditorText(ReadEditorFile(lyric_editor_path_,
            lyric_editor_encoding_, lyric_editor_bom_), false);
    } else {
        // A pending [offset:] cannot remain as a separate tag in edit mode:
        // CLyric serializes adjusted timestamps, removes the offset tag and
        // marks the control modified so LeaveLyricEditor offers to save it.
        SetLyricEditorText(CanonicalEditorText(lyrics_, have_pending_offset),
                           have_pending_offset);
    }

    const LOGFONTW font = settings_.lyric.font_valid
        ? settings_.lyric.font : skin_->Lyric().font;
    SetLyricEditorFont(font);
    const COLORREF background = settings_.lyric.background_color != CLR_INVALID
        ? settings_.lyric.background_color : skin_->Lyric().background_color;
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomSuspend, nullptr);
    SendMessageW(lyric_editor_, EM_SETBKGNDCOLOR, 0,
                 background);
    if (lyric_editor_document_)
        lyric_editor_document_->Undo(tomResume, nullptr);
    FormatLyricEditorAll();
    SendMessageW(lyric_editor_, EM_SETMODIFY, have_pending_offset, 0);

    ShowWindow(lyric_control_, SW_HIDE);
    EnableWindow(lyric_desklrc_, FALSE);
    if (lyric_editor_toolbar_) ShowWindow(lyric_editor_toolbar_, SW_SHOW);
    LayoutLyricEditor();
    SetActiveWindow(window_);
    SetFocus(lyric_editor_);
    if (!lyric_editor_accelerators_) {
        constexpr BYTE control_key = FVIRTKEY | FCONTROL;
        ACCEL accelerators[]{
            {control_key, L'X', static_cast<WORD>(kCmdEditorCut)},
            {control_key, L'C', static_cast<WORD>(kCmdEditorCopy)},
            {control_key, L'V', static_cast<WORD>(kCmdEditorPaste)},
            {control_key, L'Z', static_cast<WORD>(kCmdEditorUndo)},
            {control_key, L'Y', static_cast<WORD>(kCmdEditorRedo)},
        };
        lyric_editor_accelerators_ = CreateAcceleratorTableW(
            accelerators, static_cast<int>(std::size(accelerators)));
    }
    return true;
}

void PlayerWindow::LayoutLyricEditor() {
    if (!lyric_editor_ && !lyric_editor_toolbar_) return;
    RECT bounds = LyricTextBounds();
    if (settings_.lyric.transparent && settings_.lyric.transparent_skin) {
        GetClientRect(lyric_window_, &bounds);
    }
    const int width = std::max<LONG>(0, bounds.right - bounds.left);
    const int height = std::max<LONG>(0, bounds.bottom - bounds.top);
    const int toolbar_height = std::min(24, height);
    if (lyric_editor_toolbar_)
        MoveWindow(lyric_editor_toolbar_, bounds.left, bounds.top,
                   width, toolbar_height, TRUE);
    if (lyric_editor_)
        MoveWindow(lyric_editor_, bounds.left, bounds.top + toolbar_height,
                   width, std::max(0, height - toolbar_height), TRUE);
}

void PlayerWindow::DestroyLyricEditor() {
    if (lyric_editor_accelerators_) {
        DestroyAcceleratorTable(lyric_editor_accelerators_);
        lyric_editor_accelerators_ = nullptr;
    }
    if (lyric_find_dialog_ && IsWindow(lyric_find_dialog_))
        SendMessageW(lyric_find_dialog_, WM_CLOSE, 0, 0);
    lyric_find_dialog_ = nullptr;
    if (lyric_editor_document_) {
        lyric_editor_document_->Release();
        lyric_editor_document_ = nullptr;
    }
    if (lyric_editor_) {
        RemoveWindowSubclass(lyric_editor_, LyricEditorProc, 0x4c594544);
        if (IsWindow(lyric_editor_)) DestroyWindow(lyric_editor_);
        lyric_editor_ = nullptr;
    }
    if (lyric_editor_toolbar_) {
        if (IsWindow(lyric_editor_toolbar_)) DestroyWindow(lyric_editor_toolbar_);
        lyric_editor_toolbar_ = nullptr;
    }
    if (lyric_editor_images_) {
        ImageList_Destroy(lyric_editor_images_);
        lyric_editor_images_ = nullptr;
    }
    if (lyric_editor_module_) {
        FreeLibrary(lyric_editor_module_);
        lyric_editor_module_ = nullptr;
    }
    lyric_editor_path_.clear();
    lyric_editor_internal_change_ = false;
    lyric_editor_edit_kind_ = 0;
}

bool PlayerWindow::SaveLyricEditor(bool save_as) {
    if (!lyric_editor_) return false;
    if (!save_as && lyrics_embedded_)
        return WriteEmbeddedLyrics(LyricEditorText(), false);
    auto target = lyric_editor_path_;
    if (save_as || target.empty()) {
        const auto suggested = target.empty() ? DefaultLyricEditorPath() : target;
        auto filter = BuildDialogFilter(ResourceModule(), {
            {0x8128U, L"*.lrc;*.txt"}, {0x8124U, L"*.*"}});
        ModernSaveFileOptions dialog;
        dialog.owner = lyric_window_;
        dialog.filters = ParseLegacyDialogFilter(
            std::span<const wchar_t>(filter.data(), filter.size()));
        dialog.initial_path = suggested;
        dialog.default_extension = L"lrc";
        const auto selected = ModernSaveFile(dialog);
        if (!selected) return false;
        target = *selected;
    }
    const std::wstring text = NormalizeEditorNewlines(LyricEditorText());
    if (!WriteEditorFile(target, text, lyric_editor_encoding_,
                         lyric_editor_bom_)) {
        // FUN_0044D6F1 uses 0x8181 for Save As; the ordinary 0x8022 path in
        // FUN_0044D824 uses the adjacent 0x8182 failure text.
        std::wstring message = ResourceText(save_as ? 0x8181 : 0x8182);
        if (message.empty()) message = L"Failed to save lyric file %s.";
        if (const size_t marker = message.find(L"%s");
            marker != std::wstring::npos)
            message.replace(marker, 2, target.wstring());
        wchar_t caption[128]{};
        GetWindowTextW(window_, caption, static_cast<int>(std::size(caption)));
        MessageBoxW(lyric_window_, message.c_str(), caption, MB_ICONERROR);
        return false;
    }
    lyric_editor_path_ = target;
    lyric_path_ = target;
    if (save_as) {
        associated_lyric_path_ = target;
        lyrics_embedded_ = false;
    }
    SendMessageW(lyric_editor_, EM_SETMODIFY, FALSE, 0);
    return true;
}

void PlayerWindow::LeaveLyricEditor(bool prompt_to_save) {
    if (!lyric_editor_) return;
    const bool modified = SendMessageW(
        lyric_editor_, EM_GETMODIFY, 0, 0) != FALSE;
    bool saved = false;
    if (modified && prompt_to_save) {
        std::wstring question = ResourceText(
            lyrics_embedded_ ? 0x817c : 0x817d);
        if (question.empty())
            question = L"The lyric file was modified. Save it?";
        wchar_t caption[128]{};
        GetWindowTextW(window_, caption, static_cast<int>(std::size(caption)));
        if (MessageBoxW(lyric_window_, question.c_str(), caption,
                        MB_ICONQUESTION | MB_YESNO) == IDYES)
            saved = SaveLyricEditor(false);
    }
    const auto edited_path = lyric_editor_path_;
    const bool associated = !associated_lyric_path_.empty();
    const bool embedded = lyrics_embedded_;
    const std::wstring editor_text = LyricEditorText();
    if (lyric_find_dialog_ && IsWindow(lyric_find_dialog_))
        SendMessageW(lyric_find_dialog_, WM_CLOSE, 0, 0);
    lyric_find_dialog_ = nullptr;
    // 0044D646 destroys +0x47C before destroying the RichEdit child.
    if (lyric_editor_accelerators_) {
        DestroyAcceleratorTable(lyric_editor_accelerators_);
        lyric_editor_accelerators_ = nullptr;
    }
    if (lyric_editor_document_) {
        lyric_editor_document_->Release();
        lyric_editor_document_ = nullptr;
    }
    RemoveWindowSubclass(lyric_editor_, LyricEditorProc, 0x4c594544);
    DestroyWindow(lyric_editor_);
    lyric_editor_ = nullptr;
    lyric_editor_internal_change_ = false;
    lyric_editor_edit_kind_ = 0;
    if (lyric_editor_toolbar_) ShowWindow(lyric_editor_toolbar_, SW_HIDE);
    if (lyric_control_) ShowWindow(lyric_control_, SW_SHOW);
    EnableWindow(lyric_desklrc_, TRUE);
    if (embedded) {
        if (!modified || saved) {
            try {
                lyrics_ = lyrics::ParseLrc(core::WideToUtf8(editor_text));
                ApplyLyricTrimSpaces(lyrics_, settings_.lyric.trim_spaces);
                desktop_lyrics_.SetLyrics(&lyrics_);
            }
            catch (const std::exception&) {}
        }
        lyric_path_.clear();
        associated_lyric_path_.clear();
        if (lyric_control_) InvalidateRect(lyric_control_, nullptr, FALSE);
    } else {
        std::error_code error;
        if (!edited_path.empty() &&
            std::filesystem::exists(edited_path, error) && !error)
            LoadLyricsFrom(edited_path, associated);
        else
            LoadCurrentLyrics(true);
    }
    if (lyric_control_) SetFocus(lyric_control_);
}

void PlayerWindow::EditLyricTimestamp(UINT command) {
    if (!lyric_editor_) return;
    std::optional<std::chrono::milliseconds> playback_position;
    if (command == kCmdLyricEditorInsertTag ||
        command == kCmdLyricEditorReplaceTag) {
        // 0044DAE2/0044DB11 sample 0045F0AD before the first RichEdit
        // message; even the centisecond shown in the tag uses this one value.
        playback_position = audio_.Position();
    } else if (command != kCmdLyricEditorDeleteTag) {
        return;
    }
    CHARRANGE selection{};
    SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    // 00443460/004436B8 collapse the visible selection to cpMin before doing
    // any line arithmetic. RichEdit's cp values, not WM_GETTEXT offsets, own
    // all subsequent positioning.
    CHARRANGE caret{selection.cpMin, selection.cpMin};
    SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&caret));
    const LONG line = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_EXLINEFROMCHAR, 0, selection.cpMin));
    const LONG line_begin = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_LINEINDEX, line, 0));
    const LONG line_length = static_cast<LONG>(SendMessageW(
        lyric_editor_, EM_LINELENGTH, static_cast<WPARAM>(-1), 0));
    if (line < 0 || line_begin < 0 || line_length < 0) return;
    const LONG line_end = line_begin + line_length;

    if (command == kCmdLyricEditorDeleteTag) {
        // 004436B8 searches after the original selection first, then retries
        // at its start. It removes the next complete bracket pair regardless
        // of whether that pair parses as a timestamp.
        LONG open = FindLyricEditorCharacter(selection.cpMax, line_end, L'[');
        if (open < 0)
            open = FindLyricEditorCharacter(selection.cpMin, line_end, L'[');
        if (open < 0) return;
        const LONG close = FindLyricEditorCharacter(open + 1, line_end, L']');
        if (close < 0) return;
        ReplaceLyricEditorRange(open, close + 1, {}, true, false);
        SetFocus(lyric_editor_);
        return;
    }
    LONG replace_begin = line_begin;
    LONG replace_end = line_begin;
    if (command == kCmdLyricEditorReplaceTag) {
        // Replacement is based on time, not textual order: of every valid
        // tag on the caret line, 00443460 selects the one nearest playback.
        long long best_distance = LLONG_MAX;
        LONG cursor = line_begin;
        while (cursor < line_end) {
            const LONG open = FindLyricEditorCharacter(cursor, line_end, L'[');
            if (open < 0) break;
            const LONG close = FindLyricEditorCharacter(
                open + 1, line_end, L']');
            if (close < 0) break;
            if (const auto timestamp = ParseEditorTimestampLiteral(
                    LyricEditorRangeText(open, close + 1))) {
                const long long distance = std::llabs(
                    timestamp->count() - playback_position->count());
                if (distance < best_distance) {
                    best_distance = distance;
                    replace_begin = open;
                    replace_end = close + 1;
                }
            }
            cursor = close + 1;
        }
    }
    ReplaceLyricEditorRange(replace_begin, replace_end,
                            FormatEditorTimestamp(*playback_position), true);
    // Despite menu 0x804F's “修改标签后换行” check, the 5.7.9 binary never
    // reads DAT_00547B48 here: both 0044DAE2 and 0044DB11 call 00443789
    // unconditionally after a successful insert/replace.
    {
        CHARRANGE current{};
        SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                     reinterpret_cast<LPARAM>(&current));
        const LRESULT current_line = SendMessageW(
            lyric_editor_, EM_EXLINEFROMCHAR, 0, current.cpMin);
        const LRESULT current_line_begin = SendMessageW(
            lyric_editor_, EM_LINEINDEX, current_line, 0);
        const LRESULT current_line_length = SendMessageW(
            lyric_editor_, EM_LINELENGTH, static_cast<WPARAM>(-1), 0);
        const LRESULT next_begin = SendMessageW(
            lyric_editor_, EM_LINEINDEX, current_line + 1, 0);
        if (current_line_begin >= 0 && current_line_length >= 0) {
            // 00443789 selects the existing line delimiter and replaces it
            // with CRLF. A missing next line deliberately leaves cpMax=-1,
            // which selects through the end and appends the delimiter.
            CHARRANGE delimiter{
                static_cast<LONG>(current_line_begin + current_line_length),
                static_cast<LONG>(next_begin)};
            ReplaceLyricEditorRange(delimiter.cpMin, delimiter.cpMax,
                                    L"\r\n", true, false);
        }
    }
    SetFocus(lyric_editor_);
}

void PlayerWindow::ShiftLyricEditorTimestamps(
    std::chrono::milliseconds delta) {
    if (!lyric_editor_) return;
    const LONG length = static_cast<LONG>(
        SendMessageW(lyric_editor_, WM_GETTEXTLENGTH, 0, 0));
    LONG cursor{};
    while (cursor < length) {
        const LONG open = FindLyricEditorCharacter(cursor, length, L'[');
        if (open < 0) break;
        const LONG close = FindLyricEditorCharacter(open + 1, length, L']');
        if (close < 0) break;
        const auto time = ParseEditorTimestampLiteral(
            LyricEditorRangeText(open, close + 1));
        if (!time) {
            cursor = close + 1;
            continue;
        }
        const std::wstring replacement = FormatEditorTimestamp(*time + delta);
        // 00443806 advances using the pre-replacement close position and
        // keeps its initial WM_GETTEXTLENGTH bound, even if formatting changes
        // the textual width of an unusual legacy timestamp.
        cursor = close + 1;
        ReplaceLyricEditorRange(open, close + 1, replacement, true, false);
    }
    SetFocus(lyric_editor_);
}

void PlayerWindow::ReflowLyricEditor(bool expand) {
    if (!lyric_editor_) return;
    std::wstring source = NormalizeEditorNewlines(LyricEditorText());
    if (source.empty()) return;
    std::vector<std::wstring> source_lines;
    size_t begin = 0;
    while (begin <= source.size()) {
        const size_t end = source.find(L"\r\n", begin);
        source_lines.push_back(source.substr(begin,
            end == std::wstring::npos ? end : end - begin));
        if (end == std::wstring::npos) break;
        begin = end + 2;
    }

    lyrics::Lyrics parsed;
    try { parsed = lyrics::ParseLrc(core::WideToUtf8(source)); }
    catch (const std::exception&) { return; }

    struct TimedText {
        std::chrono::milliseconds time;
        std::wstring text;
    };
    std::vector<TimedText> timed;
    std::vector<std::wstring> plain;
    for (const auto& line : source_lines) {
        size_t cursor{};
        std::vector<std::chrono::milliseconds> times;
        bool metadata{};
        while (cursor < line.size() && line[cursor] == L'[') {
            const size_t close = line.find(L']', cursor + 1);
            if (close == std::wstring::npos) break;
            const std::wstring_view tag(line.data() + cursor + 1,
                                        close - cursor - 1);
            if (const auto time = ParseEditorTimestamp(tag)) {
                times.push_back(*time - parsed.offset);
            } else if (tag.starts_with(L"ti:") || tag.starts_with(L"ar:") ||
                       tag.starts_with(L"al:") || tag.starts_with(L"by:") ||
                       tag.starts_with(L"offset:")) {
                metadata = true;
            } else {
                break;
            }
            cursor = close + 1;
        }
        if (!times.empty()) {
            const std::wstring body = line.substr(cursor);
            for (const auto time : times)
                timed.push_back({time, body});
        } else if (!metadata && !line.empty()) {
            plain.push_back(line);
        }
    }
    std::stable_sort(timed.begin(), timed.end(),
        [](const TimedText& left, const TimedText& right) {
            return left.time < right.time;
        });

    std::wstring result;
    const auto append_metadata = [&result](std::wstring_view name,
                                           const std::string& value) {
        result += L"[";
        result += name;
        result += L":";
        try { result += core::Utf8ToWide(value); }
        catch (const std::exception&) {}
        result += L"]\r\n";
    };
    append_metadata(L"ti", parsed.title);
    append_metadata(L"ar", parsed.artist);
    append_metadata(L"al", parsed.album);
    append_metadata(L"by", parsed.author);
    result += L"\r\n";

    if (expand) {
        for (const auto& item : timed)
            result += FormatEditorTimestamp(item.time) + item.text + L"\r\n";
    } else {
        std::vector<std::pair<std::wstring, std::wstring>> grouped;
        std::map<std::wstring, size_t> positions;
        for (const auto& item : timed) {
            const std::wstring tag = FormatEditorTimestamp(item.time);
            const auto found = positions.find(item.text);
            if (found == positions.end()) {
                positions.emplace(item.text, grouped.size());
                grouped.emplace_back(item.text, tag);
            } else {
                // CLyric's compact serializer prepends each later timestamp,
                // producing [latest]...[earliest] before the shared text.
                grouped[found->second].second.insert(0, tag);
            }
        }
        for (const auto& [body, tags] : grouped)
            result += tags + body + L"\r\n";
        // The compact serializer terminates the timed group with an empty
        // row even when there is no following un-timed text.
        if (!timed.empty()) result += L"\r\n";
    }
    for (const auto& line : plain) result += line + L"\r\n";
    // 0044DB40/0044DC07 always mark a successful non-empty serialization
    // dirty after 0044311E has streamed it in and cleared the undo history,
    // even when its visible result happens to equal the source.
    SetLyricEditorText(result, true);
    SetFocus(lyric_editor_);
}

void PlayerWindow::ConvertLyricEditorText(DWORD mapping) {
    if (!lyric_editor_) return;
    CHARRANGE selection{};
    SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    const std::wstring all = LyricEditorText();
    if (selection.cpMin == selection.cpMax) return;
    const size_t begin = static_cast<size_t>(selection.cpMin);
    const size_t end = static_cast<size_t>(selection.cpMax);
    if (begin >= end || end > all.size()) return;
    const std::wstring_view source(all.data() + begin, end - begin);
    const int count = LCMapStringW(LOCALE_SYSTEM_DEFAULT, mapping,
        source.data(), static_cast<int>(source.size()), nullptr, 0);
    if (count <= 0) return;
    std::wstring converted(static_cast<size_t>(count), L'\0');
    LCMapStringW(LOCALE_SYSTEM_DEFAULT, mapping, source.data(),
                 static_cast<int>(source.size()), converted.data(), count);
    CHARRANGE replace{static_cast<LONG>(begin), static_cast<LONG>(end)};
    SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&replace));
    SendMessageW(lyric_editor_, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(converted.c_str()));
    SetFocus(lyric_editor_);
}

void PlayerWindow::ShowLyricFindDialog(bool replace) {
    if (!lyric_editor_ || !IsWindow(lyric_editor_)) return;
    if (lyric_find_dialog_ && IsWindow(lyric_find_dialog_)) {
        if (lyric_find_replace_ == replace) {
            SetForegroundWindow(lyric_find_dialog_);
            return;
        }
        SendMessageW(lyric_find_dialog_, WM_CLOSE, 0, 0);
        lyric_find_dialog_ = nullptr;
    }

    CHARRANGE selection{};
    SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMax > selection.cpMin) {
        const LONG length = std::min<LONG>(
            selection.cpMax - selection.cpMin,
            static_cast<LONG>(std::size(lyric_find_text_) - 1));
        TEXTRANGEW range{{selection.cpMin, selection.cpMin + length},
                         lyric_find_text_};
        SendMessageW(lyric_editor_, EM_GETTEXTRANGE, 0,
                     reinterpret_cast<LPARAM>(&range));
        lyric_find_text_[length] = L'\0';
    }

    lyric_find_ = {};
    lyric_find_.lStructSize = sizeof(lyric_find_);
    lyric_find_.hwndOwner = lyric_window_;
    lyric_find_.lpstrFindWhat = lyric_find_text_;
    lyric_find_.wFindWhatLen =
        static_cast<WORD>(std::size(lyric_find_text_));
    lyric_find_.Flags = FR_DOWN;
    lyric_find_replace_ = replace;
    if (replace) {
        lyric_find_.lpstrReplaceWith = lyric_replace_text_;
        lyric_find_.wReplaceWithLen =
            static_cast<WORD>(std::size(lyric_replace_text_));
        lyric_find_dialog_ = ReplaceTextW(&lyric_find_);
    } else {
        lyric_find_dialog_ = FindTextW(&lyric_find_);
    }
}

void PlayerWindow::HandleLyricFindRequest(const FINDREPLACEW& request) {
    if ((request.Flags & FR_DIALOGTERM) != 0) {
        lyric_find_dialog_ = nullptr;
        return;
    }
    if (!lyric_editor_ || lyric_find_text_[0] == L'\0') return;

    const auto find_next = [this, &request]() {
        CHARRANGE selection{};
        SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                     reinterpret_cast<LPARAM>(&selection));
        const LONG text_length = static_cast<LONG>(SendMessageW(
            lyric_editor_, WM_GETTEXTLENGTH, 0, 0));
        FINDTEXTEXW find{};
        find.chrg.cpMin = (request.Flags & FR_DOWN) != 0
            ? selection.cpMax : selection.cpMin;
        find.chrg.cpMax = (request.Flags & FR_DOWN) != 0 ? -1 : 0;
        find.lpstrText = lyric_find_text_;
        DWORD flags = (request.Flags & FR_DOWN) != 0 ? FR_DOWN : 0;
        if ((request.Flags & FR_MATCHCASE) != 0) flags |= FR_MATCHCASE;
        if ((request.Flags & FR_WHOLEWORD) != 0) flags |= FR_WHOLEWORD;
        if (text_length == 0 || SendMessageW(lyric_editor_, EM_FINDTEXTEXW,
                flags, reinterpret_cast<LPARAM>(&find)) == -1) {
            MessageBeep(MB_ICONASTERISK);
            return false;
        }
        SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&find.chrgText));
        SendMessageW(lyric_editor_, EM_SCROLLCARET, 0, 0);
        return true;
    };

    const auto selection_matches = [this, &request]() {
        CHARRANGE selection{};
        SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                     reinterpret_cast<LPARAM>(&selection));
        const LONG wanted = static_cast<LONG>(wcslen(lyric_find_text_));
        if (selection.cpMax - selection.cpMin != wanted) return false;
        std::vector<wchar_t> selected(static_cast<size_t>(wanted) + 1);
        TEXTRANGEW range{{selection.cpMin, selection.cpMax}, selected.data()};
        SendMessageW(lyric_editor_, EM_GETTEXTRANGE, 0,
                     reinterpret_cast<LPARAM>(&range));
        return (request.Flags & FR_MATCHCASE) != 0
            ? wcscmp(selected.data(), lyric_find_text_) == 0
            : _wcsicmp(selected.data(), lyric_find_text_) == 0;
    };

    if ((request.Flags & FR_REPLACEALL) != 0) {
        CHARRANGE start{0, 0};
        SendMessageW(lyric_editor_, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&start));
        while (find_next()) {
            SendMessageW(lyric_editor_, EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(lyric_replace_text_));
        }
        return;
    }
    if ((request.Flags & FR_REPLACE) != 0) {
        if (selection_matches())
            SendMessageW(lyric_editor_, EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(lyric_replace_text_));
        find_next();
        return;
    }
    if ((request.Flags & FR_FINDNEXT) != 0) find_next();
}

std::optional<std::wstring> PlayerWindow::ReadEmbeddedLyrics() const {
    const auto* track = PlaybackTrackForUi();
    if (!sound_library_ || !track)
        return std::nullopt;
    HRESULT result{};
    const auto reader = sound_library_->OpenReader(track->path, &result);
    if (!reader) return std::nullopt;
    if (const auto value = reader->MetadataValue("Lyrics");
        value && !value->empty())
        return value;
    for (const auto& entry : reader->Metadata()) {
        if ((_wcsicmp(entry.name.c_str(), L"Lyrics") == 0 ||
             _wcsicmp(entry.name.c_str(), L"WM/Lyrics") == 0) &&
            !entry.value.empty())
            return entry.value;
    }
    return std::nullopt;
}

bool PlayerWindow::WriteEmbeddedLyrics(std::wstring_view text,
                                       bool deleting) {
    const auto* track = PlaybackTrackForUi();
    if (!sound_library_ || !track)
        return false;
    const auto path = track->path;
    const bool was_active = audio_.State() != audio::PlaybackState::stopped;
    if (was_active) {
        audio_.Stop();
        playback_was_active_ = false;
        playback_source_open_ = false;
    }
    HRESULT result{};
    const auto reader = sound_library_->OpenReader(path, &result);
    if (reader)
        result = reader->SetMetadataValue("Lyrics", deleting
            ? std::wstring_view{} : text);
    if (FAILED(result)) {
        if (!deleting) {
            const UINT resource = !reader || result == E_NOINTERFACE ||
                    result == E_ACCESSDENIED ? 0x817f : 0x8180;
            const auto message = ResourceText(resource);
            wchar_t caption[128]{};
            GetWindowTextW(window_, caption,
                           static_cast<int>(std::size(caption)));
            MessageBoxW(lyric_window_, message.c_str(), caption,
                        MB_ICONERROR);
        }
        RefreshPlaybackUi();
        return false;
    }
    lyrics_embedded_ = !deleting;
    if (lyric_editor_)
        SendMessageW(lyric_editor_, EM_SETMODIFY, FALSE, 0);
    RefreshPlaybackUi();
    return true;
}

void PlayerWindow::ShowLyricContextMenu(POINT screen_point) {
    if (!lyric_window_ || context_menu_open_) return;
    if (fullscreen_lyric_detached_ && !lyric_editor_) {
        ShowFullScreenLyricContextMenu(screen_point);
        return;
    }
    HMENU menu = DetachFirstPopup(
        LoadMenuW(ResourceModule(), MAKEINTRESOURCEW(
            lyric_editor_ ? kMenuLyricEditor : kMenuLyricDisplay)));
    if (!menu) return;
    if (lyric_editor_) PrepareLyricEditorMenu(menu);
    else PrepareLyricMenu(menu);
    context_menu_open_ = true;
    const HWND owner = lyric_window_;
    SetForegroundWindow(owner);
    BeginPopupMenuStyle(menu, true);
    const UINT command = TrackPopupMenuEx(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screen_point.x, screen_point.y, owner, nullptr);
    EndPopupMenuStyle();
    DestroyMenu(menu);
    context_menu_open_ = false;
    if (command && !HandleLyricCommand(command))
        HandleContextCommand(command, owner);
    PostMessageW(owner, WM_NULL, 0, 0);
}

void PlayerWindow::ShowFullScreenLyricContextMenu(POINT screen_point) {
    if (!lyric_control_ || context_menu_open_ || !IsWindowEnabled(window_))
        return;
    HMENU menu = DetachFirstPopup(LoadMenuW(
        ResourceModule(), MAKEINTRESOURCEW(kMenuLyricDisplay)));
    if (!menu) return;
    PrepareFullScreenLyricMenu(menu);
    PopulateFullScreenMonitorMenu(menu);

    // FUN_004427B1 deliberately bypasses TTPlayer's owner-drawn popup skin.
    // It activates the detached lyric surface, but sends the selected command
    // to the (hidden) CPlayerWnd owner through ordinary WM_COMMAND routing.
    context_menu_open_ = true;
    SetForegroundWindow(lyric_control_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen_point.x, screen_point.y,
                   0, window_, nullptr);
    DestroyMenu(menu);
    context_menu_open_ = false;
    PostMessageW(window_, WM_NULL, 0, 0);
}

void PlayerWindow::PrepareLyricMenu(HMENU menu) const {
    if (!menu) return;
    const bool have_lyrics = !lyrics_.lines.empty();
    CheckCommand(menu, kCmdLyricTopMost, ActiveLyricTopMost());
    // CPlayerWnd's menu refresh at 0045Axxx changes command 0x409 into the
    // action that will be performed, rather than placing a check beside the
    // current mode. Resource 0x409 is "vertical|horizontal".
    if (const HMENU display = FindCommandMenu(menu, kCmdLyricScrollMode)) {
        const auto action = ResourceListItem(ResourceModule(),
            kCmdLyricScrollMode, ActiveLyricScrollMode() != 0 ? 0 : 1);
        if (!action.empty()) {
            ModifyMenuW(display, kCmdLyricScrollMode,
                        MF_BYCOMMAND | MF_STRING, kCmdLyricScrollMode,
                        action.c_str());
        }
    }
    CheckCommand(menu, kCmdLyricFadeHighlight,
                 ActiveLyricFadeHighlight());
    CheckCommand(menu, kCmdLyricKaraoke, ActiveLyricKaraokeMode());
    CheckCommand(menu, kCmdLyricTransparent, ActiveLyricTransparent());
    CheckCommand(menu, kCmdLyricMouseWheel,
                 settings_.lyric.mouse_wheel_adjust);
    EnableCommand(menu, kCmdLyricCopy, have_lyrics);
    EnableCommand(menu, kCmdLyricEdit,
                  PlaybackTrackForUi() || !lyric_path_.empty());
    EnableCommand(menu, kCmdLyricReload, !lyric_path_.empty());
    // FUN_00449E98 gates 0x802E on CLyricWnd+0x2C4 (the active lyric
    // document), not on the separately remembered association string.
    EnableCommand(menu, kCmdLyricUnassociate, have_lyrics);
    for (UINT command = kCmdLyricAdjustCurrentEarlier;
         command <= kCmdLyricAdjustAllDialog; ++command)
        EnableCommand(menu, command, have_lyrics);

    if (const HMENU parent = FindCommandMenu(menu, kMenuFullscreen)) {
        MENUITEMINFOW existing{sizeof(existing)};
        existing.fMask = MIIM_SUBMENU;
        GetMenuItemInfoW(parent, kMenuFullscreen, FALSE, &existing);
        if (!existing.hSubMenu) {
            HMENU fullscreen = DetachFirstPopup(LoadMenuW(
                ResourceModule(), MAKEINTRESOURCEW(kMenuFullscreen)));
            if (fullscreen) {
                MENUITEMINFOW information{sizeof(information)};
                information.fMask = MIIM_SUBMENU;
                information.hSubMenu = fullscreen;
                if (!SetMenuItemInfoW(parent, kMenuFullscreen, FALSE,
                                      &information))
                    DestroyMenu(fullscreen);
            }
        }
    }
    const bool may_enter =
        audio_.State() == audio::PlaybackState::playing;
    EnableCommand(menu, kCmdFullscreenLyrics, may_enter);
    EnableCommand(menu, kCmdFullscreenVisual, may_enter);
    EnableCommand(menu, kCmdFullscreenAll, may_enter);
    CheckCommand(menu, kCmdFullscreenLyrics, fullscreen_mode_ == 1);
    CheckCommand(menu, kCmdFullscreenVisual, fullscreen_mode_ == 2);
    CheckCommand(menu, kCmdFullscreenAll, fullscreen_mode_ == 3);
}

void PlayerWindow::PrepareFullScreenLyricMenu(HMENU menu) const {
    if (!menu) return;
    PrepareLyricMenu(menu);

    // CLyricCtrl::OnContextMenu (FUN_004427B1) starts with menu 143 and
    // removes precisely the commands which belong to the normal lyric host.
    // Keep this positional order: later deletions rely on the shifted indices.
    if (HMENU adjust = GetSubMenu(menu, 0)) {
        DeleteMenu(adjust, 6, MF_BYPOSITION);
        DeleteMenu(adjust, 6, MF_BYPOSITION);
    }
    if (HMENU display = FindCommandMenu(menu, kCmdLyricScrollMode))
        DeleteMenu(display, 3, MF_BYPOSITION);

    DeleteMenu(menu, 1, MF_BYPOSITION);
    DeleteMenu(menu, 10, MF_BYPOSITION);
    DeleteMenu(menu, 10, MF_BYPOSITION);
    const int penultimate = GetMenuItemCount(menu) - 2;
    if (penultimate >= 0) DeleteMenu(menu, penultimate, MF_BYPOSITION);
    for (const UINT command : {
            kCmdLyricDownload, kCmdLyricAssociate, kCmdLyricEdit,
            kCmdLyricUpload, kCmdLyricTopMost, kCmdLyricOptions,
            kCmdDesktopLyrics}) {
        DeleteMenu(menu, command, MF_BYCOMMAND);
    }
}

void PlayerWindow::PrepareLyricEditorMenu(HMENU menu) const {
    if (!menu || !lyric_editor_) return;
    CHARRANGE selection{};
    SendMessageW(lyric_editor_, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    const bool selected = selection.cpMin != selection.cpMax;
    CheckCommand(menu, kCmdLyricEditorNewLine, lyric_editor_new_line_);
    // FUN_0044B066 changes these two states in the editor popup;
    // FUN_00449E98 performs the ordinary RichEdit update immediately before
    // the popup is displayed.
    EnableCommand(menu, kCmdChineseTraditional, selected);
    EnableCommand(menu, kCmdChineseSimplified, selected);
    EnableCommand(menu, kCmdEditorUndo,
                  SendMessageW(lyric_editor_, EM_CANUNDO, 0, 0) != FALSE);
    EnableCommand(menu, kCmdEditorRedo,
                  SendMessageW(lyric_editor_, EM_CANREDO, 0, 0) != FALSE);
    EnableCommand(menu, kCmdEditorCut, selected);
    EnableCommand(menu, kCmdEditorCopy, selected);
    EnableCommand(menu, kCmdEditorPaste,
                  SendMessageW(lyric_editor_, EM_CANPASTE, 0, 0) != FALSE);
    const bool have_track = PlaybackTrackForUi() != nullptr;
    const bool have_lyrics = !lyrics_.lines.empty();
    EnableCommand(menu, kCmdLyricEmbeddedRead, have_track);
    EnableCommand(menu, kCmdLyricEmbeddedDelete, have_track);
    EnableCommand(menu, kCmdLyricEmbeddedWrite,
                  have_track && have_lyrics);
}

bool PlayerWindow::HandleLyricCommand(UINT command) {
    switch (command) {
    case kCmdLyricEdit:
        EnterLyricEditor();
        return true;
    case kCmdLyricReturn:
        LeaveLyricEditor(true);
        return true;
    case kCmdLyricSave:
        SaveLyricEditor(false);
        return true;
    case kCmdLyricSaveAs:
        SaveLyricEditor(true);
        return true;
    case kCmdLyricEditorInsertTag:
    case kCmdLyricEditorReplaceTag:
    case kCmdLyricEditorDeleteTag:
        EditLyricTimestamp(command);
        return true;
    case kCmdLyricEditorEarlier:
        ShiftLyricEditorTimestamps(std::chrono::milliseconds(-500));
        return true;
    case kCmdLyricEditorLater:
        ShiftLyricEditorTimestamps(std::chrono::milliseconds(500));
        return true;
    case kCmdLyricEditorExpand:
        ReflowLyricEditor(true);
        return true;
    case kCmdLyricEditorCompress:
        ReflowLyricEditor(false);
        return true;
    case kCmdLyricEditorNewLine:
        lyric_editor_new_line_ = !lyric_editor_new_line_;
        settings_.lyric.new_line_after_tag = lyric_editor_new_line_;
        return true;
    case kCmdChineseTraditional:
        ConvertLyricEditorText(LCMAP_TRADITIONAL_CHINESE);
        return true;
    case kCmdChineseSimplified:
        ConvertLyricEditorText(LCMAP_SIMPLIFIED_CHINESE);
        return true;
    case kCmdEditorFind:
        ShowLyricFindDialog(false);
        return true;
    case kCmdEditorReplace:
        ShowLyricFindDialog(true);
        return true;
    case kCmdEditorCut:
        if (lyric_editor_) SendMessageW(lyric_editor_, WM_CUT, 0, 0);
        return true;
    case kCmdEditorCopy:
        if (lyric_editor_) SendMessageW(lyric_editor_, WM_COPY, 0, 0);
        return true;
    case kCmdEditorPaste:
        if (lyric_editor_) SendMessageW(lyric_editor_, WM_PASTE, 0, 0);
        return true;
    case kCmdEditorSelectAll:
        if (lyric_editor_) SendMessageW(lyric_editor_, EM_SETSEL, 0, -1);
        return true;
    case kCmdEditorUndo:
        if (lyric_editor_) SendMessageW(lyric_editor_, WM_UNDO, 0, 0);
        return true;
    case kCmdEditorRedo:
        if (lyric_editor_) SendMessageW(lyric_editor_, EM_REDO, 0, 0);
        return true;
    case kCmdLyricEmbeddedRead: {
        const auto embedded = ReadEmbeddedLyrics();
        if (!embedded) {
            const auto message = ResourceText(0x817e);
            wchar_t caption[128]{};
            GetWindowTextW(window_, caption,
                           static_cast<int>(std::size(caption)));
            MessageBoxW(lyric_window_, message.c_str(), caption,
                        MB_ICONERROR);
            return true;
        }
        try {
            lyrics_ = lyrics::ParseLrc(core::WideToUtf8(*embedded));
            ApplyLyricTrimSpaces(lyrics_, settings_.lyric.trim_spaces);
            desktop_lyrics_.SetLyrics(&lyrics_);
            lyric_path_.clear();
            associated_lyric_path_.clear();
            lyrics_embedded_ = true;
            if (lyric_editor_) {
                lyric_editor_path_.clear();
                // 0044C648 calls 0044311E directly when RichEdit already
                // exists: embedded text is shown exactly as returned by the
                // reader rather than being normalized through CLyric.
                SetLyricEditorText(*embedded, false);
                SetFocus(lyric_editor_);
            } else if (lyric_control_) {
                InvalidateRect(lyric_control_, nullptr, FALSE);
            }
        } catch (const std::exception&) {
        }
        return true;
    }
    case kCmdLyricEmbeddedWrite:
        WriteEmbeddedLyrics(lyric_editor_
            ? LyricEditorText() : CanonicalEditorText(lyrics_), false);
        return true;
    case kCmdLyricEmbeddedDelete:
        WriteEmbeddedLyrics({}, true);
        return true;
    case 0x7d00:
        if (audio_.State() == audio::PlaybackState::playing) audio_.Pause();
        else if (audio_.State() == audio::PlaybackState::paused) audio_.Resume();
        else PlayCurrent();
        RefreshPlaybackUi();
        return true;
    case 0x7d01:
        audio_.Pause();
        RefreshPlaybackUi();
        return true;
    case 0x7d03:
        audio_.Seek(std::max(std::chrono::milliseconds(0),
            audio_.Position() - std::chrono::seconds(5)));
        return true;
    case 0x7d04:
        audio_.Seek(audio_.Position() + std::chrono::seconds(5));
        return true;
    case kCmdShowLyrics:
        ToggleLyricWindow();
        return true;
    case kCmdLyricTopMost:
        ActiveLyricTopMost() = !ActiveLyricTopMost();
        if (lyric_window_) SetWindowPos(lyric_window_,
            ActiveLyricTopMost() ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (lyric_ontop_) InvalidateRect(lyric_ontop_, nullptr, FALSE);
        return true;
    case kCmdLyricScrollMode:
        ActiveLyricScrollMode() = ActiveLyricScrollMode() == 0 ? 1 : 0;
        lyric_line_dragging_ = false;
        lyric_line_drag_offset_ = 0;
        if (lyric_control_) {
            if (GetCapture() == lyric_control_) ReleaseCapture();
            RebuildLyricFont(false);
            UpdateLyricScrollTimer();
            InvalidateRect(lyric_control_, nullptr, FALSE);
        }
        return true;
    case kCmdLyricFadeHighlight:
        ActiveLyricFadeHighlight() = !ActiveLyricFadeHighlight();
        if (lyric_control_) InvalidateRect(lyric_control_, nullptr, FALSE);
        return true;
    case kCmdLyricKaraoke:
        ActiveLyricKaraokeMode() = !ActiveLyricKaraokeMode();
        if (lyric_control_) InvalidateRect(lyric_control_, nullptr, FALSE);
        return true;
    case kCmdLyricTransparent:
        if (fullscreen_lyric_detached_) return true;
        settings_.lyric.transparent = !settings_.lyric.transparent;
        RebuildLyricFont(false);
        LayoutLyricControls();
        ApplySkinWindowAlpha(EffectiveSkinWindowAlpha(window_));
        if (lyric_window_) {
            RedrawWindow(lyric_window_, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN |
                RDW_UPDATENOW);
        }
        return true;
    case kCmdLyricMouseWheel:
        settings_.lyric.mouse_wheel_adjust =
            !settings_.lyric.mouse_wheel_adjust;
        return true;
    case kCmdLyricAdjustCurrentEarlier:
    case kCmdLyricAdjustCurrentLater:
    case kCmdLyricAdjustFollowingEarlier:
    case kCmdLyricAdjustFollowingLater:
    case kCmdLyricAdjustAllEarlier:
    case kCmdLyricAdjustAllLater: {
        if (lyrics_.lines.empty()) return true;
        const auto current = lyrics_.LineAt(audio_.Position()).value_or(0);
        const auto delta = (command == kCmdLyricAdjustCurrentEarlier ||
                            command == kCmdLyricAdjustFollowingEarlier ||
                            command == kCmdLyricAdjustAllEarlier)
            ? std::chrono::milliseconds(-500)
            : std::chrono::milliseconds(500);
        size_t begin = 0;
        size_t end = lyrics_.lines.size();
        if (command == kCmdLyricAdjustCurrentEarlier ||
            command == kCmdLyricAdjustCurrentLater) {
            begin = current;
            end = std::min(lyrics_.lines.size(), current + 1);
        } else if (command == kCmdLyricAdjustFollowingEarlier ||
                   command == kCmdLyricAdjustFollowingLater) {
            begin = current;
        }
        for (size_t index = begin; index < end; ++index)
            lyrics_.lines[index].time += delta;
        if (lyric_control_) InvalidateRect(lyric_control_, nullptr, FALSE);
        return true;
    }
    case kCmdLyricAdjustAllDialog: {
        const auto value = PromptLyricAdjustment(
            ResourceModule(), lyric_window_, lyric_adjustment_ms_);
        if (!value) return true;
        lyric_adjustment_ms_ = *value;
        if (lyric_editor_) {
            ShiftLyricEditorTimestamps(
                std::chrono::milliseconds(*value));
        } else if (!lyrics_.lines.empty()) {
            lyrics_.ShiftLines(std::chrono::milliseconds(*value));
            if (lyric_control_)
                InvalidateRect(lyric_control_, nullptr, FALSE);
        }
        return true;
    }
    case kCmdLyricReload:
        if (!lyric_path_.empty()) LoadLyricsFrom(lyric_path_,
                                                  !associated_lyric_path_.empty());
        else LoadCurrentLyrics(true);
        return true;
    case kCmdLyricAssociate: {
        auto filter = BuildDialogFilter(ResourceModule(), {
            {0x8128U, L"*.lrc;*.txt"}, {0x8124U, L"*.*"}});
        ModernOpenFileOptions dialog;
        dialog.owner = lyric_window_;
        dialog.filters = ParseLegacyDialogFilter(
            std::span<const wchar_t>(filter.data(), filter.size()));
        if (!lyric_path_.empty())
            dialog.initial_path = lyric_path_.parent_path();
        if (const auto selected = ModernOpenFile(dialog))
            LoadLyricsFrom(*selected, true);
        return true;
    }
    case kCmdLyricUnassociate:
        associated_lyric_path_.clear();
        LoadCurrentLyrics(true);
        return true;
    case kCmdLyricCopy:
        CopyLyricsToClipboard();
        return true;
    case kCmdLyricDownload:
        // The command remains present because it belongs to the recovered
        // ttpres.dll menu. The legacy network service is not contacted by the
        // offline rebuild; local association/reload continue to work.
        return true;
    case kCmdDesktopLyrics:
        EnterDesktopLyricMode();
        return true;
    case kCmdDesktopLyricReturn:
        LeaveDesktopLyricMode();
        return true;
    case kCmdDesktopLyricLock:
    case kCmdDesktopLyricUnlock:
        // FUN_0044D568 is the central 0x8040/0x8041 route.  Do not leave
        // this mutation inside DeskLrcBar: once locked, both the paint and
        // control surfaces are mouse-transparent and only CPlayerWnd's menu
        // remains available to reverse the state.
        settings_.desktop_lyric.lock =
            command == kCmdDesktopLyricLock;
        desktop_lyrics_.ApplySettings();
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return true;
    default:
        return false;
    }
}

void PlayerWindow::ClearLyrics() {
    lyrics_ = {};
    desktop_lyrics_.SetLyrics(&lyrics_);
    lyric_path_.clear();
    lyrics_embedded_ = false;
    if (lyric_control_) {
        SetWindowTextW(lyric_control_, L"");
        if (fullscreen_lyric_detached_)
            RebuildLyricFont(false);
        InvalidateRect(lyric_control_, nullptr, FALSE);
    }
}

void PlayerWindow::ApplyAutoLyricVisibility() {
    if (!settings_.lyric.auto_visible) return;
    const bool have_lyrics = !lyrics_.lines.empty();
    ActiveLyricVisible() = have_lyrics;
    if (desktop_lyric_mode_) {
        desktop_lyrics_.Show(have_lyrics);
    } else if (have_lyrics) {
        if (!lyric_window_ && !CreateLyricWindow()) return;
        ShowWindow(lyric_window_, SW_SHOWNOACTIVATE);
        BringWindowToTop(lyric_window_);
    } else if (lyric_window_) {
        ShowWindow(lyric_window_, SW_HIDE);
    }
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::LoadLyricsFrom(const std::filesystem::path& path,
                                  bool associated) {
    lyrics_embedded_ = false;
    try {
        auto loaded = lyrics::LoadLrc(path);
        ApplyLyricTrimSpaces(loaded, settings_.lyric.trim_spaces);
        lyrics_ = std::move(loaded);
        desktop_lyrics_.SetLyrics(&lyrics_);
        lyric_path_ = path;
        if (associated) associated_lyric_path_ = path;
    } catch (const std::exception&) {
        lyrics_ = {};
        desktop_lyrics_.SetLyrics(&lyrics_);
        lyric_path_.clear();
        if (associated) associated_lyric_path_.clear();
    }
    if (lyric_control_) {
        if (fullscreen_lyric_detached_)
            RebuildLyricFont(false);
        InvalidateRect(lyric_control_, nullptr, FALSE);
    }
    ApplyAutoLyricVisibility();
}

void PlayerWindow::LoadDroppedLyrics(const std::filesystem::path& path) {
    // CLyricWnd::Drop (0044AC70) consumes only HDROP item zero.  In ordinary
    // display mode it takes the same decoder/parser route as an associated
    // lyric; while the RichEdit editor is active it streams the source bytes
    // into that document instead of adding the file to the music playlist.
    if (!lyric_editor_) {
        LoadLyricsFrom(path, true);
        return;
    }
    int encoding = kEditorEncodingUtf8;
    bool bom = true;
    const auto text = ReadEditorFile(path, encoding, bom);
    std::error_code error;
    if (text.empty() && std::filesystem::file_size(path, error) != 0) return;
    lyric_editor_path_ = path;
    lyric_editor_encoding_ = encoding;
    lyric_editor_bom_ = bom;
    SetLyricEditorText(text, false);
    FormatLyricEditorAll();
}

void PlayerWindow::LoadCurrentLyrics(bool force) {
    ClearLyrics();
    const auto* playback_track = PlaybackTrackForUi();
    if (!playback_track || (!force && !settings_.lyric.auto_load_lyric)) {
        ApplyAutoLyricVisibility();
        return;
    }
    if (!associated_lyric_path_.empty()) {
        std::error_code error;
        if (std::filesystem::exists(associated_lyric_path_, error) && !error) {
            LoadLyricsFrom(associated_lyric_path_, true);
            return;
        }
        associated_lyric_path_.clear();
    }

    const auto& track = *playback_track;
    std::wstring artist;
    std::wstring title;
    try { artist = core::Utf8ToWide(track.artist); }
    catch (const std::exception&) {}
    try { title = core::Utf8ToWide(track.title); }
    catch (const std::exception&) {}
    const auto candidates = BuildLocalLyricCandidates(
        track.path, artist, title, PlayerRuntimeDirectory(),
        settings_.lyric.download_folder, settings_.lyric.folders);
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::exists(candidate, error) || error) continue;
        LoadLyricsFrom(candidate, false);
        return;
    }
    ApplyAutoLyricVisibility();
}

void PlayerWindow::CopyLyricsToClipboard() const {
    if (lyrics_.lines.empty() || !lyric_window_) return;
    std::wstring text;
    for (const auto& line : lyrics_.lines) {
        try { text += core::Utf8ToWide(line.text); }
        catch (const std::exception&) {}
        text += L"\r\n";
    }
    if (!OpenClipboard(lyric_window_)) return;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void PlayerWindow::SeekLyricLine(std::ptrdiff_t delta) {
    if (lyrics_.lines.empty()) return;
    const auto current = lyrics_.LineAt(audio_.Position()).value_or(0);
    const auto next = std::clamp<std::ptrdiff_t>(
        static_cast<std::ptrdiff_t>(current) + delta, 0,
        static_cast<std::ptrdiff_t>(lyrics_.lines.size() - 1));
    // WM_KEYDOWN at 00442AC1 uses the same lyric position callback and hence
    // the same observed no-transition result as drag release.
    audio_.SeekWithoutFade(
        lyrics_.lines[static_cast<size_t>(next)].time + lyrics_.offset);
    UpdateDiscordPresence();
    if (lyric_control_) InvalidateRect(lyric_control_, nullptr, FALSE);
}

LRESULT CALLBACK PlayerWindow::LyricEditorProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR reference) {
    auto* self = reinterpret_cast<PlayerWindow*>(reference);
    if (!self) return DefSubclassProc(window, message, wparam, lparam);
    switch (message) {
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            point = {bounds.left + 8, bounds.top + 8};
        }
        self->ShowLyricContextMenu(point);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            self->LeaveLyricEditor(true);
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        RemoveWindowSubclass(window, LyricEditorProc, subclass);
        if (self->lyric_editor_ == window) self->lyric_editor_ = nullptr;
        return result;
    }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK PlayerWindow::LyricWindowProc(HWND window, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        self->lyric_window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleLyricMessage(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

} // namespace ttplayer::ui
