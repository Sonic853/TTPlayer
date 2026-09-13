#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "project_links.h"

#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/core/text.h"
#include "ttplayer/skin/skin_package.h"
#include "ttplayer/ui/playback_track_state.h"
#include "ttplayer/ui/playlist_transforms.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "ttplayer/ui/tooltip_policy.h"
#include "ttplayer/ui/window_fade_policy.h"
#include "ttplayer/ui/window_drag.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <limits>
#include <map>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <string>
#include <sstream>
#include <system_error>
#include <vector>
#include <utility>

namespace ttplayer::ui {
namespace detail {
constexpr int kFadeCompleteNone = 0;
constexpr int kFadeCompleteHideTarget = 1;
constexpr int kFadeCompleteDestroyMain = 2;
constexpr int kFadeCompleteSwitchMini = 3;
constexpr int kFadeCompleteFinishMini = 4;
constexpr int kFadeCompleteStartupMinimize = 5;
constexpr int kFadeCompleteFinishStartup = 6;

int CompareSkinNames(std::wstring_view left, std::wstring_view right) {
    // FUN_004C54D1 resolves this function dynamically and falls back to
    // lstrcmpiW on older shells.  Do the same instead of sorting package
    // paths: numeric prefixes are deliberately only installation names.
    using StrCmpLogicalWFn = int (WINAPI*)(PCWSTR, PCWSTR);
    static const StrCmpLogicalWFn logical = [] {
        const HMODULE shlwapi = GetModuleHandleW(L"shlwapi.dll");
        return shlwapi ? reinterpret_cast<StrCmpLogicalWFn>(
            GetProcAddress(shlwapi, "StrCmpLogicalW")) : nullptr;
    }();
    const std::wstring left_text(left);
    const std::wstring right_text(right);
    return logical ? logical(left_text.c_str(), right_text.c_str())
                   : lstrcmpiW(left_text.c_str(), right_text.c_str());
}

std::filesystem::path PlayerRuntimeDirectory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::filesystem::path FindRuntimePath(const std::filesystem::path& relative) {
    const auto directory = PlayerRuntimeDirectory();
    if (directory.empty()) return {};
    const auto candidate = directory / relative;
    std::error_code error;
    if (std::filesystem::exists(candidate, error) && !error) return candidate;
    return {};
}

struct IconLocation {
    std::filesystem::path path;
    int index{};
};

IconLocation ParseIconLocation(std::wstring_view value) {
    IconLocation result;
    if (value.empty()) return result;
    std::wstring path;
    std::wstring_view suffix;
    if (value.front() == L'"') {
        const size_t close = value.find(L'"', 1);
        if (close == std::wstring_view::npos) return result;
        path.assign(value.substr(1, close - 1));
        suffix = value.substr(close + 1);
    } else {
        const size_t comma = value.rfind(L',');
        if (comma == std::wstring_view::npos) {
            path.assign(value);
        } else {
            path.assign(value.substr(0, comma));
            suffix = value.substr(comma);
        }
    }
    const size_t comma = suffix.find(L',');
    if (comma != std::wstring_view::npos) {
        const std::wstring index_text(suffix.substr(comma + 1));
        wchar_t* end{};
        const long parsed = std::wcstol(index_text.c_str(), &end, 10);
        if (end != index_text.c_str()) result.index = static_cast<int>(parsed);
    }
    result.path = std::move(path);
    return result;
}

HMENU DetachFirstPopup(HMENU menu) {
    if (!menu) return nullptr;
    const HMENU popup = GetSubMenu(menu, 0);
    if (popup) RemoveMenu(menu, 0, MF_BYPOSITION);
    DestroyMenu(menu);
    return popup;
}

HMENU DetachPopup(HMENU menu, int position) {
    if (!menu) return nullptr;
    const HMENU popup = GetSubMenu(menu, position);
    if (popup) RemoveMenu(menu, position, MF_BYPOSITION);
    DestroyMenu(menu);
    return popup;
}

HMENU ConvertMenuBarToPopup(HMENU menu) {
    if (!menu) return nullptr;
    const HMENU popup = CreatePopupMenu();
    if (!popup) {
        DestroyMenu(menu);
        return nullptr;
    }
    UINT position = 0;
    while (GetMenuItemCount(menu) > 0) {
        wchar_t text[256]{};
        GetMenuStringW(menu, 0, text, static_cast<int>(std::size(text)), MF_BYPOSITION);
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU |
            MIIM_BITMAP | MIIM_CHECKMARKS | MIIM_DATA;
        GetMenuItemInfoW(menu, 0, TRUE, &item);
        RemoveMenu(menu, 0, MF_BYPOSITION);

        // FUN_00488FEF does not merely detach the seven menu-bar popups from
        // resource 0x8B.  It inserts each one into the context popup with the
        // synthetic command id 10000 + its original position.  Those ids are
        // subsequently visible to the legacy menu owner-draw/state path even
        // though selecting a popup header never dispatches a WM_COMMAND.
        item.fMask |= MIIM_ID | MIIM_STRING;
        item.wID = 10000 + position++;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(wcslen(text));
        InsertMenuItemW(popup, GetMenuItemCount(popup), TRUE, &item);
    }
    DestroyMenu(menu);
    return popup;
}

std::wstring LoadResourceText(HMODULE module, UINT identifier) {
    if (!module) return {};
    const wchar_t* value = nullptr;
    const int length = LoadStringW(module, identifier,
        reinterpret_cast<LPWSTR>(&value), 0);
    return length > 0 && value ? std::wstring(value, static_cast<size_t>(length))
                               : std::wstring{};
}

std::wstring ResourceCommandLabel(HMODULE module, UINT identifier) {
    auto value = LoadResourceText(module, identifier);
    const auto separator = value.find_last_of(L"\r\n");
    if (separator != std::wstring::npos) {
        value.erase(0, separator + 1);
        while (!value.empty() && (value.front() == L'\r' || value.front() == L'\n'))
            value.erase(value.begin());
    }
    return value;
}

std::wstring ResourceListItem(HMODULE module, UINT identifier, size_t index) {
    auto value = LoadResourceText(module, identifier);
    size_t begin = 0;
    for (size_t current = 0; current < index; ++current) {
        begin = value.find(L'|', begin);
        if (begin == std::wstring::npos) return {};
        ++begin;
    }
    const size_t end = value.find(L'|', begin);
    return value.substr(begin, end == std::wstring::npos ? end : end - begin);
}

std::wstring MenuPositionText(HMODULE module, UINT identifier, UINT position) {
    if (!module) return {};
    const HMENU menu = LoadMenuW(module, MAKEINTRESOURCEW(identifier));
    if (!menu) return {};
    wchar_t text[256]{};
    const int length = GetMenuStringW(menu, position, text,
        static_cast<int>(std::size(text)), MF_BYPOSITION);
    DestroyMenu(menu);
    return length > 0 ? std::wstring(text, static_cast<size_t>(length))
                      : std::wstring{};
}

std::wstring RuntimeHotKeyText(
    const settings::HotKeyAccelerator& shortcut) {
    if (shortcut.virtual_key == 0) return {};
    const auto key_name = [](int virtual_key, bool extended = false) {
        LONG key_data = static_cast<LONG>(
            MapVirtualKeyW(static_cast<UINT>(virtual_key),
                           MAPVK_VK_TO_VSC) << 16);
        if (extended) key_data |= 1 << 24;
        std::array<wchar_t, 128> name{};
        if (GetKeyNameTextW(key_data, name.data(),
                            static_cast<int>(name.size())) > 0) {
            return std::wstring(name.data());
        }
        return std::to_wstring(virtual_key);
    };
    std::wstring text;
    const auto append_modifier = [&text, &key_name](int virtual_key) {
        text += key_name(virtual_key);
        text += L" + ";
    };
    if ((shortcut.modifiers & HOTKEYF_CONTROL) != 0)
        append_modifier(VK_CONTROL);
    if ((shortcut.modifiers & HOTKEYF_SHIFT) != 0)
        append_modifier(VK_SHIFT);
    if ((shortcut.modifiers & HOTKEYF_ALT) != 0)
        append_modifier(VK_MENU);
    text += key_name(shortcut.virtual_key,
        (shortcut.modifiers & HOTKEYF_EXT) != 0);
    return text;
}

std::vector<wchar_t> BuildDialogFilter(
    HMODULE module,
    std::initializer_list<std::pair<UINT, std::wstring_view>> entries) {
    std::vector<wchar_t> result;
    for (const auto& [identifier, pattern] : entries) {
        auto label = LoadResourceText(module, identifier);
        if (label.empty()) continue;
        result.insert(result.end(), label.begin(), label.end());
        result.push_back(L'\0');
        result.insert(result.end(), pattern.begin(), pattern.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

void AppendDialogFilter(std::vector<wchar_t>& result, std::wstring_view label,
                        std::wstring_view pattern) {
    if (label.empty() || pattern.empty()) return;
    result.insert(result.end(), label.begin(), label.end());
    result.push_back(L'\0');
    result.insert(result.end(), pattern.begin(), pattern.end());
    result.push_back(L'\0');
}

std::wstring PatternFromDescription(std::wstring_view description) {
    // FUN_004CB18D extracts the text between the first '(' and ')' from each
    // registered reader description before FUN_004CB1DB builds 0x8123.
    const size_t begin = description.find(L'(');
    if (begin == std::wstring_view::npos) return {};
    const size_t end = description.find(L')', begin + 1);
    if (end == std::wstring_view::npos || end == begin + 1) return {};
    return std::wstring(description.substr(begin + 1, end - begin - 1));
}

std::wstring DialogDescription(std::wstring description) {
    // FUN_004CB1DB calls FUN_00415951(description, L"*.", L"") after
    // extracting the actual pattern. The original combo therefore displays
    // "MP3 音频文件(mp3;...)" while OPENFILENAME receives "*.mp3;...".
    size_t offset{};
    while ((offset = description.find(L"*.", offset)) != std::wstring::npos)
        description.erase(offset, 2);
    return description;
}

std::vector<wchar_t> BuildAudioDialogFilter(
    HMODULE resources, const std::vector<plugins::ReaderFormat>& registered_readers) {
    struct FormatEntry {
        std::wstring description;
        std::wstring pattern;
    };

    // These seven creators are installed directly by
    // CSoundLibrary_Initialize (004CA48E). Window Media is deliberately not
    // in this list: in the supplied build it comes from ttp_asf.dll and must
    // disappear when that add-in cannot be instantiated.
    constexpr std::array built_in{
        0x811cU, // CD
        0x811dU, // CUE
        0x811eU, // MP3
        0x811fU, // MIDI
        0x8120U, // Wave
        0x8121U, // AIFF
        0x8122U, // AU
    };

    std::vector<FormatEntry> formats;
    formats.reserve(built_in.size() + registered_readers.size());
    for (const UINT identifier : built_in) {
        auto description = LoadResourceText(resources, identifier);
        auto pattern = PatternFromDescription(description);
        if (!description.empty() && !pattern.empty())
            formats.push_back(FormatEntry{std::move(description), std::move(pattern)});
    }
    for (const auto& reader : registered_readers) {
        if (!reader.description.empty() && !reader.pattern.empty())
            formats.push_back(FormatEntry{reader.description, reader.pattern});
    }

    std::wstring all_patterns;
    const auto append_all_pattern = [&](std::wstring_view pattern) {
        if (pattern.empty()) return;
        if (!all_patterns.empty() && all_patterns.back() != L';')
            all_patterns.push_back(L';');
        all_patterns.append(pattern);
        if (all_patterns.back() != L';') all_patterns.push_back(L';');
    };
    // 004CB1DB builds the aggregate extension string while walking creators
    // in registration order.  It sorts only the subsequently displayed
    // per-reader entries; the aggregate therefore must be captured first.
    for (const auto& format : formats) append_all_pattern(format.pattern);
    // FUN_004CB1DB explicitly adds archive and playlist extensions to the
    // combined filter after walking the registered readers.
    append_all_pattern(L"*.zip;*.rar");
    const auto playlist_description = LoadResourceText(resources, 0x8125);
    const auto playlist_pattern = PatternFromDescription(playlist_description);
    append_all_pattern(playlist_pattern);

    // FUN_004CDD0E sorts the temporary reader vector before serializing its
    // visible entries. FUN_004CE697 uses wcscmp, not a locale/case-folded
    // comparison. The native vector is not deduplicated.
    std::ranges::sort(formats, [](const FormatEntry& left, const FormatEntry& right) {
        return std::wcscmp(left.description.c_str(), right.description.c_str()) < 0;
    });

    std::vector<wchar_t> result;
    AppendDialogFilter(result, LoadResourceText(resources, 0x8123), all_patterns);
    for (const auto& format : formats)
        AppendDialogFilter(result, DialogDescription(format.description), format.pattern);

    AppendDialogFilter(result, DialogDescription(playlist_description),
                       playlist_pattern);
    // 0x8126/0x8127 are appended only to CSoundLibrary's playlist-specific
    // DAT_00547FD0 filter.  0048059D consumes DAT_00547FC8, whose sole
    // playlist entry is the combined 0x8125 item.
    AppendDialogFilter(result, LoadResourceText(resources, 0x8118), L"*.zip;*.rar");
    const auto all_files_description = LoadResourceText(resources, 0x8124);
    AppendDialogFilter(result, all_files_description,
                       PatternFromDescription(all_files_description));
    result.push_back(L'\0');
    return result;
}

void ReplaceAll(std::wstring& value, std::wstring_view needle,
                std::wstring_view replacement) {
    if (needle.empty()) return;
    size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::wstring::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

HMENU FindCommandMenu(HMENU menu, UINT command) {
    if (!menu) return nullptr;
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        if (GetMenuItemID(menu, index) == command) return menu;
        if (const HMENU child = GetSubMenu(menu, index)) {
            if (const HMENU found = FindCommandMenu(child, command)) return found;
        }
    }
    return nullptr;
}

void EnableCommand(HMENU root, UINT command, bool enabled) {
    if (const HMENU menu = FindCommandMenu(root, command))
        EnableMenuItem(menu, command, MF_BYCOMMAND |
            (enabled ? MF_ENABLED : MF_DISABLED | MF_GRAYED));
}

void CheckCommand(HMENU root, UINT command, bool checked) {
    if (const HMENU menu = FindCommandMenu(root, command))
        CheckMenuItem(menu, command, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
}

COLORREF InterpolateMenuColor(COLORREF first, COLORREF second, int numerator,
                              int denominator) {
    const auto channel = [=](int shift) {
        const int left = (first >> shift) & 0xff;
        const int right = (second >> shift) & 0xff;
        return (left * (denominator - numerator) + right * numerator) /
               denominator;
    };
    return RGB(channel(0), channel(8), channel(16));
}

void DrawVerticalGradient(HDC target, const RECT& bounds,
                          COLORREF first, COLORREF second);

void FillPopupMenuBackground(HDC dc, const RECT& bounds) {
    constexpr int gutter_width = 20;
    RECT text = bounds;
    text.left = std::min<LONG>(text.right, text.left + gutter_width);
    SetDCBrushColor(dc, RGB(252, 252, 249));
    FillRect(dc, &text, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    const int width = std::min<int>(gutter_width, bounds.right - bounds.left);
    const COLORREF start = RGB(254, 254, 251);
    const COLORREF middle = RGB(236, 231, 224);
    const COLORREF finish = RGB(196, 196, 173);
    for (int offset = 0; offset < width; ++offset) {
        const COLORREF color = offset < gutter_width / 2
            ? InterpolateMenuColor(start, middle, offset, gutter_width / 2)
            : InterpolateMenuColor(middle, finish, offset - gutter_width / 2,
                                   gutter_width / 2);
        SetDCBrushColor(dc, color);
        RECT stripe{bounds.left + offset, bounds.top,
                    bounds.left + offset + 1, bounds.bottom};
        FillRect(dc, &stripe, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
}

using PopupMenuImageMask = std::array<unsigned char, 16 * 16>;

PopupMenuImageMask ReadPopupMenuBitmapMask(HBITMAP bitmap, int frame) {
    PopupMenuImageMask mask{};
    HDC source = CreateCompatibleDC(nullptr);
    if (!source) return mask;
    const HGDIOBJ previous = SelectObject(source, bitmap);
    for (int row = 0; row < 16; ++row) {
        for (int column = 0; column < 16; ++column) {
            mask[static_cast<size_t>(row) * 16 + column] =
                GetPixel(source, frame * 16 + column, row) != RGB(192, 192, 192);
        }
    }
    SelectObject(source, previous);
    DeleteDC(source);
    return mask;
}

void DrawPopupMenuBitmapMask(HDC target, const PopupMenuImageMask& mask,
                             int x, int y, COLORREF color) {
    for (int row = 0; row < 16; ++row) {
        for (int column = 0; column < 16; ++column) {
            if (mask[static_cast<size_t>(row) * 16 + column])
                SetPixelV(target, x + column, y + row, color);
        }
    }
}

void DrawPopupMenuSideTitle(HDC dc, std::wstring_view title) {
    HWND window = WindowFromDC(dc);
    RECT client{};
    if (!window || !GetClientRect(window, &client)) GetClipBox(dc, &client);
    client.right = std::min<LONG>(client.right, client.left + 22);
    DrawVerticalGradient(dc, client, RGB(152, 194, 200), RGB(102, 144, 150));

    // FUN_004700D9 starts with the system menu LOGFONT stored at offset 224
    // in the pre-padded-border NONCLIENTMETRICS layout.  Its charset, pitch,
    // precision, and orientation are therefore deliberately retained.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics) - sizeof(metrics.iPaddedBorderWidth);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                               &metrics, 0)) {
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                              &metrics, 0);
    }
    LOGFONTW description = metrics.lfMenuFont;
    // The value compared with U+5343 U+5343 before FUN_00465CB8 is the
    // branding helper's short-name result, while param_2 is the already
    // assembled long caption.  The localized caption therefore takes the
    // upright word-wrap branch even though it also contains the slogan.
    const bool word_wrap = title.starts_with(L"\u5343\u5343");
    description.lfHeight = 14;
    description.lfEscapement = word_wrap ? 0 : 900;
    description.lfWeight = FW_NORMAL;
    description.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(description.lfFaceName, L"\u5b8b\u4f53");
    const HFONT font = CreateFontIndirectW(&description);
    const HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    RECT text = client;
    const UINT measure_format = (word_wrap ? DT_WORDBREAK : DT_SINGLELINE) |
                                DT_CALCRECT | DT_NOPREFIX;
    const int measured_height = DrawTextW(dc, title.data(),
        static_cast<int>(title.size()), &text, measure_format);
    int height{};
    if (!word_wrap) {
        text.left += ((text.top - text.bottom) - client.left + client.right) / 2;
        height = description.lfHeight * 2 - text.left + text.right;
    } else {
        OffsetRect(&text,
            ((text.left - text.right) - client.left + client.right) / 2, 0);
        height = measured_height;
    }
    text.bottom = client.bottom - GetSystemMetrics(SM_CYEDGE);
    text.top = text.bottom - height;

    const UINT draw_format = word_wrap ? DT_WORDBREAK
                                       : DT_SINGLELINE | DT_BOTTOM;
    OffsetRect(&text, 1, word_wrap ? 1 : -1);
    SetTextColor(dc, GetSysColor(COLOR_3DDKSHADOW));
    DrawTextW(dc, title.data(), static_cast<int>(title.size()), &text,
              draw_format);
    OffsetRect(&text, -1, word_wrap ? -1 : 1);
    SetTextColor(dc, GetSysColor(COLOR_3DHIGHLIGHT));
    DrawTextW(dc, title.data(), static_cast<int>(title.size()), &text,
              draw_format);

    if (font) {
        SelectObject(dc, previous);
        DeleteObject(font);
    }
}

void TrimSingleTrackMenu(HMENU menu, const std::filesystem::path& path) {
    if (!menu) return;
    const auto value = path.wstring();
    const bool network = value.find(L"://") != std::wstring::npos;
    if (network) {
        // FUN_00488FEF removes the local-file rename/send-to entries by
        // position, then removes Browse File for URL-backed tracks.
        DeleteMenu(menu, 14, MF_BYPOSITION);
        DeleteMenu(menu, 14, MF_BYPOSITION);
        DeleteMenu(menu, 0x7efc, MF_BYCOMMAND);
    } else {
        // The corresponding local-file branch removes URL reporting and
        // downloading, including the separator left after the report item.
        DeleteMenu(menu, kPlaylistReportOnline, MF_BYCOMMAND);
        DeleteMenu(menu, 20, MF_BYPOSITION);
        DeleteMenu(menu, kPlaylistDownload, MF_BYCOMMAND);
    }
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    if (extension != L".cda" && extension != L".cue")
        DeleteMenu(menu, kPlaylistFreeDb, MF_BYCOMMAND);
}

std::wstring FromUtf8OrFallback(const std::string& value, const std::filesystem::path& fallback) {
    if (!value.empty()) {
        try { return core::Utf8ToWide(value); }
        catch (const std::exception&) {}
    }
    return fallback.stem().wstring();
}

std::wstring DisplayName(const playlist::Track& track) {
    return FromUtf8OrFallback(track.title, track.path);
}

std::wstring ArtistName(const playlist::Track& track, std::wstring fallback) {
    if (!track.artist.empty()) {
        try { return core::Utf8ToWide(track.artist); }
        catch (const std::exception&) {}
    }
    return fallback;
}

bool IsPlaylistFile(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return extension == L".m3u" || extension == L".m3u8" ||
           extension == L".ttbl" || extension == L".ttpl";
}

RECT PlaylistToolbarItemBounds(const skin::PlaylistSkin& layout, RECT toolbar,
                               size_t index) {
    if (index >= 7 || IsRectEmpty(&toolbar)) return {};
    if (layout.toolbar_items) {
        RECT item = (*layout.toolbar_items)[index];
        if (IsRectEmpty(&item)) return {};
        OffsetRect(&item, toolbar.left, toolbar.top);
        RECT clipped{};
        IntersectRect(&clipped, &item, &toolbar);
        return clipped;
    }
    const int width = toolbar.right - toolbar.left;
    RECT item{toolbar.left + static_cast<int>(index) * width / 7, toolbar.top,
        toolbar.left + static_cast<int>(index + 1) * width / 7, toolbar.bottom};
    if (toolbar.bottom - toolbar.top > 0x1e) {
        if (index == 4) item.top += 0x1e;
        else item.bottom = item.top + 0x1e;
    }
    return item;
}

void DrawPlaylistToolbarBitmap(HDC target, const skin::SkinBitmap& bitmap,
                               RECT bounds, COLORREF transparent,
                               std::optional<size_t> only_button, BYTE opacity,
                               const skin::PlaylistSkin* layout) {
    if (!bitmap.image || bitmap.size.cx <= 0 || bitmap.size.cy <= 0) return;
    constexpr int button_count = 7;
    const int control_width = bounds.right - bounds.left;
    const int control_height = bounds.bottom - bounds.top;
    if (control_width <= 0 || control_height <= 0) return;
    if (layout && layout->toolbar_items) {
        const RECT clip = only_button ? PlaylistToolbarItemBounds(*layout, bounds, *only_button) : bounds;
        if (IsRectEmpty(&clip)) return;
        const int saved = SaveDC(target);
        if (!saved) return;
        IntersectClipRect(target, clip.left, clip.top, clip.right, clip.bottom);
        bitmap.image.Draw(target, bounds.left, bounds.top, bitmap.size.cx, bitmap.size.cy,
                          0, 0, bitmap.size.cx, bitmap.size.cy, transparent, opacity);
        RestoreDC(target, saved);
        return;
    }
    // Several legacy packages provide one already-composed toolbar strip
    // whose width differs from the XML control by only the native toolbar's
    // one/two-pixel padding.  The original keeps that strip intact and clips
    // it at the control edge; splitting it would insert seams between labels.
    if (control_width == bitmap.size.cx ||
        (control_width > bitmap.size.cx &&
         std::abs(control_width - bitmap.size.cx) <= 2 &&
         bitmap.size.cx % button_count != 5)) {
        const int draw_width = std::min<int>(control_width, bitmap.size.cx);
        const int draw_height = std::min<int>(control_height, bitmap.size.cy);
        const int source_y = std::max<int>(0, (bitmap.size.cy - control_height) / 2);
        const int destination_y = bounds.top +
            std::max<int>(0, (control_height - bitmap.size.cy) / 2);
        if (!only_button) {
            bitmap.image.Draw(target, bounds.left, destination_y, draw_width, draw_height,
                              0, source_y, draw_width, draw_height, transparent, opacity);
            return;
        }
    }
    const size_t first = only_button.value_or(0);
    const size_t last = only_button ? first + 1 : button_count;
    for (size_t button = first; button < last && button < button_count; ++button) {
        // The common-control image list uses integer frame widths; remainder
        // pixels at the end of a strip are not redistributed across frames.
        // The toolbar applies the same integer truncation to button cells.
        // This is observable in the 236-pixel TT-07/PurpleMyth/Qingping
        // strips: proportional slicing shifts every later glyph by a pixel.
        int source_width = bitmap.size.cx / button_count;
        int slot_width = control_width / button_count;
        int source_left = static_cast<int>(button) * source_width;
        int slot_left = bounds.left + static_cast<int>(button) * slot_width;
        if (control_width < bitmap.size.cx && bitmap.size.cx % button_count != 0) {
            // Narrow toolbars (173 Keenwood) crop proportionally from each
            // image-list item instead of discarding all remainder pixels at
            // the right edge.
            const int source_right = static_cast<int>(button + 1) *
                bitmap.size.cx / button_count;
            const int slot_right = bounds.left + static_cast<int>(button + 1) *
                control_width / button_count;
            source_left = static_cast<int>(button) * bitmap.size.cx / button_count;
            slot_left = bounds.left + static_cast<int>(button) *
                control_width / button_count;
            source_width = source_right - source_left;
            slot_width = slot_right - slot_left;
        }
        const bool fixed_narrow = control_width < bitmap.size.cx &&
                                  bitmap.size.cx % button_count == 0;
        const int draw_width = fixed_narrow ? source_width
                                            : std::min(source_width, slot_width);
        const int draw_height = std::min<int>(bitmap.size.cy, control_height);
        const int source_x = source_left + (source_width - draw_width) / 2;
        const int source_y = (bitmap.size.cy - draw_height) / 2;
        const int destination_x = slot_left + (slot_width - draw_width) / 2;
        const int destination_y = bounds.top + (control_height - draw_height) / 2;
        bitmap.image.Draw(target, destination_x, destination_y, draw_width, draw_height,
                          source_x, source_y, draw_width, draw_height, transparent, opacity);
    }
}

void TileBitmap(HDC target, const skin::SkinBitmap& bitmap, const RECT& bounds) {
    if (!bitmap.image || bitmap.size.cx <= 0 || bitmap.size.cy <= 0 ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    const int saved = SaveDC(target);
    IntersectClipRect(target, bounds.left, bounds.top, bounds.right, bounds.bottom);
    for (int y = bounds.top; y < bounds.bottom; y += bitmap.size.cy) {
        for (int x = bounds.left; x < bounds.right; x += bitmap.size.cx) {
            bitmap.image.Draw(target, x, y, bitmap.size.cx, bitmap.size.cy,
                              0, 0, bitmap.size.cx, bitmap.size.cy);
        }
    }
    RestoreDC(target, saved);
}

COLORREF InterpolateColor(COLORREF first, COLORREF second,
                          int position, int count) {
    if (count <= 0) return first;
    const int remaining = count - position;
    return RGB((GetRValue(first) * remaining + GetRValue(second) * position) / count,
               (GetGValue(first) * remaining + GetGValue(second) * position) / count,
               (GetBValue(first) * remaining + GetBValue(second) * position) / count);
}

void DrawVerticalGradient(HDC target, const RECT& bounds,
                          COLORREF first, COLORREF second) {
    const int height = bounds.bottom - bounds.top;
    if (bounds.right <= bounds.left || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        const RECT line{bounds.left, bounds.top + row,
                        bounds.right, bounds.top + row + 1};
        const HBRUSH brush = CreateSolidBrush(
            InterpolateColor(first, second, row, height));
        FillRect(target, &line, brush);
        DeleteObject(brush);
    }
}

void DrawHorizontalGradient(HDC target, const RECT& bounds,
                            COLORREF first, COLORREF second) {
    const int width = bounds.right - bounds.left;
    if (width <= 0 || bounds.bottom <= bounds.top) return;
    for (int column = 0; column < width; ++column) {
        const COLORREF color = column == width - 1 ? first :
            InterpolateColor(first, second, column, width);
        const RECT line{bounds.left + column, bounds.top,
                        bounds.left + column + 1, bounds.bottom};
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(target, &line, brush);
        DeleteObject(brush);
    }
}

void DrawSolidFrame(HDC target, const RECT& bounds, COLORREF color) {
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    const HBRUSH brush = CreateSolidBrush(color);
    RECT edge{bounds.left, bounds.top, bounds.right, bounds.top + 1};
    FillRect(target, &edge, brush);
    edge = {bounds.left, bounds.top, bounds.left + 1, bounds.bottom};
    FillRect(target, &edge, brush);
    edge = {bounds.right - 1, bounds.top, bounds.right, bounds.bottom};
    FillRect(target, &edge, brush);
    edge = {bounds.left, bounds.bottom - 1, bounds.right, bounds.bottom};
    FillRect(target, &edge, brush);
    DeleteObject(brush);
}

void DrawBitmapPatch(HDC target, const skin::SkinImage& source, const RECT& destination,
                     const RECT& source_rect, bool tile) {
    const int destination_width = destination.right - destination.left;
    const int destination_height = destination.bottom - destination.top;
    const int source_width = source_rect.right - source_rect.left;
    const int source_height = source_rect.bottom - source_rect.top;
    if (destination_width <= 0 || destination_height <= 0 ||
        source_width <= 0 || source_height <= 0) return;
    if (!tile) {
        source.Draw(target, destination.left, destination.top,
                    destination_width, destination_height,
                    source_rect.left, source_rect.top, source_width, source_height);
        return;
    }
    const int saved = SaveDC(target);
    IntersectClipRect(target, destination.left, destination.top,
                      destination.right, destination.bottom);
    for (int y = destination.top; y < destination.bottom; y += source_height) {
        for (int x = destination.left; x < destination.right; x += source_width) {
            source.Draw(target, x, y, source_width, source_height,
                        source_rect.left, source_rect.top, source_width, source_height);
        }
    }
    RestoreDC(target, saved);
}

void DrawResizableSkinBitmap(HDC target, const skin::SkinBitmap& bitmap,
                             RECT resize_rect, int width, int height, bool tile) {
    if (!bitmap.image || bitmap.size.cx <= 0 || bitmap.size.cy <= 0 ||
        width <= 0 || height <= 0) return;
    if (resize_rect.right <= resize_rect.left ||
        resize_rect.bottom <= resize_rect.top ||
        width < bitmap.size.cx || height < bitmap.size.cy) {
        bitmap.image.Draw(target, 0, 0, width, height, 0, 0,
                          bitmap.size.cx, bitmap.size.cy);
        return;
    }

    resize_rect.left = std::clamp<LONG>(resize_rect.left, 0, bitmap.size.cx);
    resize_rect.right = std::clamp<LONG>(resize_rect.right,
                                         resize_rect.left, bitmap.size.cx);
    resize_rect.top = std::clamp<LONG>(resize_rect.top, 0, bitmap.size.cy);
    resize_rect.bottom = std::clamp<LONG>(resize_rect.bottom,
                                          resize_rect.top, bitmap.size.cy);
    const int destination_right = width - (bitmap.size.cx - resize_rect.right);
    const int destination_bottom = height - (bitmap.size.cy - resize_rect.bottom);
    const int source_x[] = {0, resize_rect.left, resize_rect.right, bitmap.size.cx};
    const int source_y[] = {0, resize_rect.top, resize_rect.bottom, bitmap.size.cy};
    const int destination_x[] = {0, resize_rect.left, destination_right, width};
    const int destination_y[] = {0, resize_rect.top, destination_bottom, height};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const RECT source_patch{source_x[column], source_y[row],
                                    source_x[column + 1], source_y[row + 1]};
            const RECT destination_patch{destination_x[column], destination_y[row],
                                         destination_x[column + 1], destination_y[row + 1]};
            DrawBitmapPatch(target, bitmap.image, destination_patch, source_patch,
                            tile && (row == 1 || column == 1));
        }
    }
}

HRGN CreateColorKeyRegion(HBITMAP bitmap, int width, int height,
                          COLORREF transparent) {
    if (!bitmap || width <= 0 || height <= 0) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const HDC screen = GetDC(nullptr);
    const int rows = GetDIBits(screen, bitmap, 0, static_cast<UINT>(height),
                               pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (rows != height) return nullptr;

    HRGN result = CreateRectRgn(0, 0, 0, 0);
    if (!result) return nullptr;
    for (int y = 0; y < height; ++y) {
        int run = -1;
        for (int x = 0; x <= width; ++x) {
            bool opaque = false;
            if (x < width) {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                const COLORREF color = RGB(pixels[offset + 2], pixels[offset + 1],
                                           pixels[offset]);
                opaque = color != transparent;
            }
            if (opaque && run < 0) run = x;
            if (!opaque && run >= 0) {
                const HRGN row = CreateRectRgn(run, y, x, y + 1);
                if (row) {
                    CombineRgn(result, result, row, RGN_OR);
                    DeleteObject(row);
                }
                run = -1;
            }
        }
    }
    return result;
}

HRGN CreateSkinWindowRegion(const skin::SkinBitmap& bitmap, RECT resize_rect,
                           int width, int height, bool tile, COLORREF transparent) {
    // Resize the coverage mask with exactly the same nine-patch/tile mapping
    // as the background. Opaque magenta in PNG is not a BMP colour key.
    auto source = bitmap;
    if (bitmap.image.IsGdiPlus()) {
        source.image = bitmap.image.CoverageMask();
        transparent = RGB(0, 0, 0);
    }
    if (!source.image || width <= 0 || height <= 0) return nullptr;
    if (width == source.size.cx && height == source.size.cy)
        return CreateColorKeyRegion(source.image, width, height, transparent);
    const HDC screen = GetDC(nullptr);
    const HDC canvas = CreateCompatibleDC(screen);
    const HBITMAP rendered = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(nullptr, screen);
    HRGN region{};
    if (canvas && rendered) {
        const HGDIOBJ old = SelectObject(canvas, rendered);
        const RECT bounds{0, 0, width, height};
        const HBRUSH brush = CreateSolidBrush(transparent);
        FillRect(canvas, &bounds, brush);
        DeleteObject(brush);
        DrawResizableSkinBitmap(canvas, source, resize_rect, width, height, tile);
        SelectObject(canvas, old);
        region = CreateColorKeyRegion(rendered, width, height, transparent);
    }
    if (rendered) DeleteObject(rendered);
    if (canvas) DeleteDC(canvas);
    return region;
}

RECT ResolveAlignedRect(RECT bounds, unsigned int alignment, SIZE native,
                        int width, int height, SIZE image_size) {
    const int item_width = image_size.cx > 0 ? image_size.cx : bounds.right - bounds.left;
    const int item_height = image_size.cy > 0 ? image_size.cy : bounds.bottom - bounds.top;
    // FUN_0047A9C5 centers the current item, not its XML origin plus half
    // the window's resize delta. It suppresses alignment separately on each
    // axis when the client is smaller than the native skin background.
    // FUN_0042912E supplies bitmap/frame dimensions for image-backed titles.
    if (width < native.cx) alignment &= 0xf0U;
    if (height < native.cy) alignment &= 0x0fU;
    if ((alignment & 0x0fU) == 2U) {
        bounds.left = (width - item_width) / 2;
    } else if ((alignment & 0x0fU) == 3U) {
        bounds.left = bounds.right - item_width + width - native.cx;
    }
    if ((alignment & 0xf0U) == 0x20U) {
        bounds.top = (height - item_height) / 2;
    } else if ((alignment & 0xf0U) == 0x30U) {
        bounds.top = bounds.bottom - item_height + height - native.cy;
    }
    bounds.right = bounds.left + item_width;
    bounds.bottom = bounds.top + item_height;
    return bounds;
}

void DrawElementFrame(HDC target, const skin::SkinElement& element, RECT bounds,
                      int state, COLORREF transparent) {
    if (!element.image || element.image_size.cx <= 0 || element.image_size.cy <= 0) return;
    const int frames = std::max(1, element.frames);
    const int frame_width = element.image_size.cx / frames;
    state = std::clamp(state, 0, frames - 1);
    // FUN_0040A35B copies a SkinButton frame at its native dimensions from
    // client origin (0,0); it does not stretch the frame to an XML rectangle
    // that contains one or two pixels of padding. Title bounds are resolved
    // at their native image dimensions by ResolveAlignedRect.
    const int destination_width = frames > 1 ? frame_width
                                              : bounds.right - bounds.left;
    const int destination_height = frames > 1 ? element.image_size.cy
                                               : bounds.bottom - bounds.top;
    element.image.Draw(target, bounds.left, bounds.top, destination_width,
                       destination_height, state * frame_width, 0,
                       frame_width, element.image_size.cy, transparent);
}

PlaylistGeometry MakePlaylistGeometry(const skin::PlaylistSkin& layout,
                                      int split_on_lists, int width, int height,
                                      size_t track_count) {
    PlaylistGeometry result;
    result.list = layout.list_bounds;
    const int extra_width = std::max<int>(0, width - layout.background.size.cx);
    const int extra_height = std::max<int>(0, height - layout.background.size.cy);
    result.list.right += extra_width;
    result.list.bottom += extra_height;
    // The selected bitmap is a tiled highlight, not the ListCtrl item height.
    // Runtime pixel probes show 16-pixel rows with LX-iPlay's 19-pixel
    // selected.bmp clipped and restarted for every item.
    result.row_height = 16;
    const int available_height = std::max<LONG>(0, result.list.bottom - result.list.top);
    result.visible_rows = std::max(1, (available_height + result.row_height - 1) /
                                      result.row_height);
    // The original track ListCtrl creates its vertical scrollbar only when
    // its item count exceeds the page.  Reserving this strip unconditionally
    // shortens a one-row selection by 15 px in LX-iPlay (9 px in TT2012).
    result.scrollbar_width = track_count > static_cast<size_t>(result.visible_rows) &&
                             layout.scrollbar_buttons.size.cx >= 3
        ? layout.scrollbar_buttons.size.cx / 3 : 0;
    // SplitterCtrl retains its five-pixel default renderer when a skin omits
    // splitter_bar_image (TT2012); image-backed skins use the native width.
    const int splitter_width = layout.splitter_bar.image
        ? std::max<int>(0, layout.splitter_bar.size.cx) : 5;
    const int maximum_split = std::max<int>(0, result.list.right - result.list.left -
        splitter_width - result.scrollbar_width - 24);
    const int split = std::clamp(split_on_lists, 0, maximum_split);
    result.list_titles = {result.list.left, result.list.top,
                          result.list.left + split, result.list.bottom};
    result.splitter = {result.list_titles.right, result.list.top,
                       result.list_titles.right + splitter_width, result.list.bottom};
    result.scrollbar = {result.list.right - result.scrollbar_width, result.list.top,
                        result.list.right, result.list.bottom};
    result.tracks = {result.splitter.right, result.list.top,
                     result.scrollbar_width ? result.scrollbar.left : result.list.right,
                     result.list.bottom};
    result.toolbar = ResolveAlignedRect(layout.toolbar_bounds, layout.toolbar_alignment,
                                        layout.background.size, width, height);
    result.close = ResolveAlignedRect(layout.close.bounds, layout.close.alignment,
                                      layout.background.size, width, height);
    result.title = ResolveAlignedRect(layout.title.bounds, layout.title.alignment,
                                      layout.background.size, width, height,
                                      layout.title.image_size);
    return result;
}

void ApplyPlaylistSkinDefaults(const skin::PlaylistSkin& source,
                               settings::PlaylistSettings& target) {
    target.font = source.font;
    target.font_height = source.font_height;
    // LegacySkin currently exposes only these two font fields.  Do not carry
    // a complete LOGFONT imported for the preceding skin into this package.
    target.font_descriptor = {};
    target.font_descriptor_valid = false;
    target.text_color = source.text_color;
    target.highlight_color = source.highlight_color;
    target.background_color = source.background_color;
    target.number_color = source.number_color;
    target.duration_color = source.duration_color;
    target.selected_color = source.selected_color;
    target.alternate_background_color = source.alternate_background_color;
}

void ApplyLyricSkinDefaults(const skin::LyricSkin& source,
                            settings::LyricSettings& target) {
    target.font = source.font;
    target.font_valid = true;
    target.text_color = source.text_color;
    target.highlight_color = source.highlight_color;
    target.background_color = source.background_color;
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

bool IsSuppressedSkinControl(std::wstring_view name) {
    return name == L"login" || name == L"login_name" || name == L"browser";
}

bool IsSkinButton(std::wstring_view name) {
    if (IsSuppressedSkinControl(name)) return false;
    constexpr std::wstring_view names[] = {
        L"icon",
        L"play", L"pause", L"prev", L"next", L"mute",
        L"stop", L"open", L"lyric", L"equalizer", L"playlist",
        L"browser", L"minimize", L"minimode", L"exit",
        L"progress", L"volume", L"led", L"set", L"mode_single", L"mode_loop",
        L"mode_slider", L"mode_circle", L"mode_random"
    };
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

bool IsPlayModeSkinVisible(std::wstring_view name, int mode) {
    constexpr std::wstring_view modes[] = {L"mode_single", L"mode_loop",
        L"mode_slider", L"mode_circle", L"mode_random"};
    return !name.starts_with(L"mode_") || modes[std::clamp(mode, 0, 4)] == name;
}

HFONT CreateSkinFont(const skin::SkinElement& element) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    LOGFONTW descriptor{};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0)) {
        // FUN_004A954D copies lfCaptionFont before CSkinParser_CreateFont
        // replaces the face and height supplied by the skin.
        descriptor = metrics.lfCaptionFont;
    }
    descriptor.lfHeight = element.font_size;
    descriptor.lfWeight = FW_NORMAL;
    descriptor.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy_s(descriptor.lfFaceName, element.font.c_str(), _TRUNCATE);
    HFONT font = CreateFontIndirectW(&descriptor);
    if (!font) {
        wcscpy_s(descriptor.lfFaceName, L"SimSun");
        font = CreateFontIndirectW(&descriptor);
    }
    return font;
}

void DrawSkinText(HDC dc, const skin::SkinElement& element, const wchar_t* text) {
    if (element.background != 0xff000000) {
        const HBRUSH brush = CreateSolidBrush(element.background);
        FillRect(dc, &element.bounds, brush);
        DeleteObject(brush);
    }
    const HFONT font = CreateSkinFont(element);
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, element.color);
    UINT format = DT_SINGLELINE | DT_NOPREFIX;
    switch (element.alignment & 0x0fU) {
    case 2: format |= DT_CENTER; break;
    case 3: format |= DT_RIGHT; break;
    default: format |= DT_LEFT; break;
    }
    // FUN_00409023 obtains vertical centering from SS_CENTERIMAGE. The
    // original dialog gives that style to the info static only; the
    // stereo/status statics use DrawText's top-aligned default. FUN_004710FD
    // applies only the horizontal part of the parsed XML alignment.
    format |= _wcsicmp(element.name.c_str(), L"info") == 0 ? DT_VCENTER : DT_TOP;
    RECT bounds = element.bounds;
    DrawTextW(dc, text, -1, &bounds, format);
    if (font) {
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
}

COLORREF BlendColor(COLORREF from, COLORREF to, int amount, int total) {
    if (total <= 0) return to;
    amount = std::clamp(amount, 0, total);
    const auto channel = [amount, total](BYTE first, BYTE second) {
        return static_cast<BYTE>((static_cast<int>(first) * (total - amount) +
                                  static_cast<int>(second) * amount) / total);
    };
    return RGB(channel(GetRValue(from), GetRValue(to)),
               channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

void DrawScrollingSkinInfo(HDC dc, const skin::SkinElement& element,
                           const wchar_t* current, const wchar_t* next,
                           int horizontal_offset, int vertical_offset) {
    const int height = element.bounds.bottom - element.bounds.top;
    if (element.background != 0xff000000) {
        const HBRUSH brush = CreateSolidBrush(element.background);
        FillRect(dc, &element.bounds, brush);
        DeleteObject(brush);
    }

    const HFONT font = CreateSkinFont(element);
    const int saved = SaveDC(dc);
    if (saved != 0) {
        IntersectClipRect(dc, element.bounds.left, element.bounds.top,
                          element.bounds.right, element.bounds.bottom);
    }
    const HGDIOBJ selected = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    const COLORREF color = element.background == 0xff000000
        ? element.color
        : BlendColor(element.color, element.background, vertical_offset, height);
    SetTextColor(dc, color);
    UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER;
    const bool right_aligned = (element.alignment & 0x0fU) == 3;
    if (right_aligned) format |= DT_RIGHT;
    RECT bounds = element.bounds;
    if (right_aligned) bounds.right -= horizontal_offset;
    else bounds.left += horizontal_offset;
    if (element.background == 0xff000000) {
        bounds.top -= vertical_offset;
        bounds.bottom -= vertical_offset;
    }
    DrawTextW(dc, current, -1, &bounds, format);
    if (next && element.background == 0xff000000) {
        OffsetRect(&bounds, 0, height);
        DrawTextW(dc, next, -1, &bounds, format);
    }
    if (font) SelectObject(dc, selected);
    if (saved != 0) RestoreDC(dc, saved);
    if (font) DeleteObject(font);
}

std::wstring FormatInfoDuration(std::chrono::milliseconds duration) {
    const auto total = std::max<int64_t>(0, duration.count() / 1000);
    const auto hours = total / 3600;
    const auto minutes = total / 60 % 60;
    const auto seconds = total % 60;
    wchar_t value[32]{};
    if (hours > 0) swprintf_s(value, L"%lld:%02lld:%02lld", hours, minutes, seconds);
    else swprintf_s(value, L"%lld:%02lld", total / 60, seconds);
    return value;
}

std::wstring FormatAudioDescription(const audio::AudioFormat& format) {
    if (format.sample_rate == 0 || format.bytes_per_second == 0)
        return format.codec_name;
    const std::wstring codec = !format.codec_name.empty() ? format.codec_name :
        format.format_tag == WAVE_FORMAT_PCM ? L"PCM" :
        format.format_tag == 3 ? L"IEEE Float" : L"WAVE";
    wchar_t value[96]{};
    // The original Format placeholder is assembled as "%s %dkHz %s"; its
    // bitrate helper returns "%dK" (verified against the original 8 kHz,
    // 32,000-byte/s probe, which renders "PCM 8kHz 256K").
    swprintf_s(value, L"%s %ukHz %uK", codec.c_str(), format.sample_rate / 1000,
               format.bytes_per_second * 8 / 1000);
    return value;
}

std::wstring FormatLedTime(std::chrono::milliseconds position) {
    const bool negative = position.count() < 0;
    const auto total = std::abs(position.count() / 1000);
    const auto hours = total / 3600;
    const auto minutes = total / 60 % 60;
    const auto seconds = total % 60;
    wchar_t value[16]{};
    if (hours != 0) {
        swprintf_s(value, negative ? L"-%02lld:%02lld:%02lld"
                                   : L"%02lld:%02lld:%02lld",
                   hours, minutes, seconds);
    } else {
        swprintf_s(value, negative ? L"-%02lld:%02lld" : L"%02lld:%02lld",
                   total / 60, seconds);
    }
    return value;
}

RECT SkinLedBounds(const skin::SkinElement& led, std::wstring_view value) {
    if (!led.image || led.image_size.cx < 12 || led.image_size.cy <= 0 || value.empty()) return {};
    // 0045127C computes the number of displayed glyphs; 004512E8 then
    // resizes the actual child HWND, keeping the configured left/right edge.
    // The full XML rectangle is an anchor, not the mouse hit rectangle.
    const int width = led.image_size.cx / 12 * static_cast<int>(value.size());
    RECT bounds = led.bounds;
    if ((led.alignment & 0x0fU) == 1) bounds.right = bounds.left + width;
    else bounds.left = bounds.right - width;
    bounds.bottom = bounds.top + led.image_size.cy;
    return bounds;
}

void DrawSkinLed(HDC dc, const skin::SkinElement& led, std::wstring_view value,
                 COLORREF transparent) {
    const RECT bounds = SkinLedBounds(led, value);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    const int glyph_width = led.image_size.cx / 12;
    int x = bounds.left;
    for (const wchar_t ch : value) {
        const int glyph = ch == L':' ? 10 : ch == L'-' ? 11 : static_cast<int>(ch - L'0');
        if (glyph >= 0 && glyph < 12)
            led.image.Draw(dc, x, bounds.top, glyph_width, led.image_size.cy,
                glyph * glyph_width, 0, glyph_width, led.image_size.cy, transparent);
        x += glyph_width;
    }
}

bool ReadEqualizerProfileFile(const std::filesystem::path& path,
                              std::array<int, 11>& values) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    const auto attribute = bytes.find("Custom=\"");
    if (attribute == std::string::npos) return false;
    const auto first = attribute + 8;
    const auto last = bytes.find('"', first);
    if (last == std::string::npos) return false;
    std::string text = bytes.substr(first, last - first);
    std::replace(text.begin(), text.end(), ':', ' ');
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream values_stream(text);
    std::array<int, 11> parsed{};
    for (auto& value : parsed) {
        if (!(values_stream >> value)) return false;
        value = std::clamp(value, -12, 12);
    }
    values = parsed;
    return true;
}

bool WriteEqualizerProfileFile(const std::filesystem::path& path,
                               const std::array<int, 11>& values) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
              "<ttplayer_eq><Equalizer Custom=\"";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) output << ':';
        output << std::clamp(values[index], -12, 12);
    }
    output << "\"/></ttplayer_eq>\r\n";
    return output.good();
}

struct AutoShutdownDialogState {
    HMODULE resources{};
    int seconds{15};
};

void UpdateAutoShutdownDialog(HWND dialog, AutoShutdownDialogState& state) {
    if (const HWND progress = GetDlgItem(dialog, 0x885)) {
        SendMessageW(progress, PBM_SETRANGE32, 0, 15);
        SendMessageW(progress, PBM_SETPOS, 15 - state.seconds, 0);
    }
    const auto text = LoadResourceText(state.resources, 0x81b6);
    if (!text.empty()) SetDlgItemTextW(dialog, 0x41c, text.c_str());
}

INT_PTR CALLBACK AutoShutdownDialogProc(HWND dialog, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AutoShutdownDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<AutoShutdownDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (!state) return FALSE;
        UpdateAutoShutdownDialog(dialog, *state);
        SetTimer(dialog, 0x7b, 1000, nullptr);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_TIMER && wparam == 0x7b) {
        if (--state->seconds <= 0) {
            SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        } else {
            UpdateAutoShutdownDialog(dialog, *state);
        }
        return TRUE;
    }
    if (message == WM_COMMAND &&
        (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        KillTimer(dialog, 0x7b);
        EndDialog(dialog, LOWORD(wparam));
        return TRUE;
    }
    return FALSE;
}

bool RequestSystemPowerOff() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool have_privilege = LookupPrivilegeValueW(
        nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) != FALSE;
    if (have_privilege) {
        SetLastError(ERROR_SUCCESS);
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    }
    const bool adjusted = have_privilege && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return adjusted && ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE | EWX_POWEROFF,
                                     SHTDN_REASON_MAJOR_APPLICATION) != FALSE;
}

} // namespace detail

using namespace detail;

PlayerWindow::PlayerWindow(settings::Settings settings) : settings_(std::move(settings)) {
    lyric_editor_new_line_ = settings_.lyric.new_line_after_tag;
    // OPENFILENAMEW::lpstrInitialDir in 0048059D points at CSettings
    // Histroy/SoundPath (+0x750).
    file_dialog_initial_directory_ = settings_.history.sound_path;
    settings_.player.volume = std::clamp(settings_.player.volume, 0, 100);
    settings_.player.balance = std::clamp(settings_.player.balance, -100, 100);
    settings_.player.alpha_percent = std::clamp(settings_.player.alpha_percent, 0, 90);
    transparency_percent_ = settings_.player.alpha_percent;
    skin_window_alpha_ = static_cast<BYTE>(
        255 * (100 - transparency_percent_) / 100);
    rendered_skin_window_alpha_ = skin_window_alpha_;
    settings_.equalizer.surround = std::clamp(settings_.equalizer.surround, 0, 16);
    for (auto& value : settings_.equalizer.current)
        value = std::clamp(value, -12, 12);
    for (auto& value : settings_.equalizer.custom)
        value = std::clamp(value, -12, 12);
    if (settings_.equalizer.profile < -2 ||
        settings_.equalizer.profile >= static_cast<int>(std::size(kEqualizerPresets)))
        settings_.equalizer.profile = -2;
    if (settings_.equalizer.profile_last < -1 ||
        settings_.equalizer.profile_last >= static_cast<int>(std::size(kEqualizerPresets)))
        settings_.equalizer.profile_last = -1;
    volume_before_mute_ = std::max(1, settings_.player.volume);
    audio_.Configure({settings_.playback.file_buffer,
                      settings_.device.buffer_duration,
                      settings_.device.output_bits,
                      settings_.device.resample_rate,
                      settings_.playback.thread_priority,
                      settings_.player.balance,
                      settings_.playback.auto_gain,
                      settings_.playback.auto_scan_gain,
                      settings_.playback.skip_scan_gain,
                      settings_.equalizer.profile,
                      settings_.equalizer.surround,
                      settings_.equalizer.current,
                      settings_.device.device_type,
                      settings_.device.hardware_buffer,
                      settings_.device.create_primary,
                      settings_.device.ssrc_mode,
                      settings_.device.dither,
                      settings_.plugin.folder,
                      settings_.plugin.modules,
                      nullptr,
                      settings_.playback.sound_fade_mode,
                      settings_.playback.fade_duration,
                      settings_.playback.track_fade_duration});
    audio_.SetVolume(settings_.player.mute ? 0.0F : static_cast<float>(settings_.player.volume) / 100.0F);
    discord_presence_.Configure(settings_.general.send_title_to_msn,
                                settings_.general.discord_application_id);
}

PlayerWindow::~PlayerWindow() {
    DestroyFullScreenLyricInput();
    ShutdownMediaLibrary();
    ShutdownPlaylistInfoLoading();
    if (playlist_metadata_cancel_)
        playlist_metadata_cancel_->store(true, std::memory_order_release);
    playlist_metadata_working_.reset();
    playlist_metadata_cancel_.reset();
    desktop_lyrics_.Destroy();
    CloseOptions();
    UnregisterConfiguredHotKeys();
    ClosePlaybackOpenTip();
    RemoveTrayIcon();
    RevokeFileDropTarget(lyric_window_);
    RevokeFileDropTarget(playlist_window_);
    RevokeFileDropTarget(window_);
    StopVisualWorker();
    audio_.Stop();
    // The catalog worker only owns copied paths and borrowed module handles.
    // Stop at the next package boundary, then join it before the application
    // can release either module.
    if (skin_catalog_cancel_)
        skin_catalog_cancel_->store(true, std::memory_order_release);
    if (skin_catalog_future_.valid())
        skin_catalog_future_.wait();
    HideSkinMenuToolTip();
    if (skin_menu_tooltip_ && IsWindow(skin_menu_tooltip_))
        DestroyWindow(skin_menu_tooltip_);
    EndPopupMenuStyle();
    if (popup_menu_images_) ImageList_Destroy(popup_menu_images_);
    if (ui_font_) DeleteObject(ui_font_);
    if (title_font_) DeleteObject(title_font_);
    if (background_brush_) DeleteObject(background_brush_);
    if (panel_brush_) DeleteObject(panel_brush_);
    if (menu_resources_) FreeLibrary(menu_resources_);
    if (window_icon_small_) DestroyIcon(window_icon_small_);
    if (window_icon_big_) DestroyIcon(window_icon_big_);
}

HMODULE PlayerWindow::ResourceModule() const noexcept {
    if (skin_resources_) return skin_resources_;
    if (!menu_resources_) {
        const auto path = FindRuntimePath(L"ttpres.dll");
        if (!path.empty()) {
            menu_resources_ = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        }
    }
    return menu_resources_;
}

std::wstring PlayerWindow::ResourceText(UINT identifier) const {
    return LoadResourceText(ResourceModule(), identifier);
}

void PlayerWindow::UpdateTrayIcon() {
    if (!window_) return;
    if (!settings_.general.tray_icon) {
        RemoveTrayIcon();
        return;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = kTrayIconIdentifier;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayCallbackMessage;
    icon.hIcon = settings_.general.app_icon_file.empty() && skin_ &&
                         skin_->Icon()
        ? skin_->Icon()
        : (window_icon_small_ ? window_icon_small_
                              : LoadIconW(nullptr, IDI_APPLICATION));
    auto tip = display_title_;
    if (tip.empty()) {
        std::array<wchar_t, 128> title{};
        GetWindowTextW(window_, title.data(), static_cast<int>(title.size()));
        tip = title.data();
    }
    wcsncpy_s(icon.szTip, tip.c_str(), _TRUNCATE);

    const DWORD operation = tray_icon_added_ ? NIM_MODIFY : NIM_ADD;
    if (Shell_NotifyIconW(operation, &icon)) tray_icon_added_ = true;
}

void PlayerWindow::RemoveTrayIcon() {
    if (!tray_icon_added_ || !window_) return;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = kTrayIconIdentifier;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    tray_icon_added_ = false;
}

void PlayerWindow::ShowPlaybackOpenTip() {
    if (!window_ || !settings_.general.tips_on_open) return;
    const auto* track = PlaybackTrackForUi();
    if (!track) return;
    ClosePlaybackOpenTip();

    std::wstring title;
    std::wstring artist;
    std::wstring album;
    try { title = core::Utf8ToWide(track->title); }
    catch (const std::exception&) {}
    try { artist = core::Utf8ToWide(track->artist); }
    catch (const std::exception&) {}
    try { album = core::Utf8ToWide(track->album); }
    catch (const std::exception&) {}
    if (title.empty()) title = DisplayName(*track);
    playback_tip_title_ = ResourceText(0x81c5);
    if (playback_tip_title_.empty()) playback_tip_title_ = L"TTPlayer";
    playback_tip_body_ = FormatPlaybackTipBody(
        ResourceText(0x81cb), title, artist, album,
        FormatAudioDescription(audio_.Format()),
        audio_.Duration().count() > 0
            ? FormatInfoDuration(audio_.Duration()) : std::wstring{});
    if (playback_tip_body_.empty()) playback_tip_body_ = std::move(title);

    WNDCLASSEXW type{sizeof(type)};
    type.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    type.lpfnWndProc = PlaybackTipWindowProc;
    type.hInstance = instance_;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = nullptr;
    type.lpszClassName = kPlaybackTipWindowClass;
    if (!RegisterClassExW(&type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    HDC screen = GetDC(nullptr);
    RECT measured{0, 0, 360, 0};
    if (screen) {
        const HGDIOBJ old = SelectObject(screen,
            ui_font_ ? ui_font_ : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(screen, playback_tip_body_.c_str(), -1, &measured,
                  DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK);
        SelectObject(screen, old);
        ReleaseDC(nullptr, screen);
    }
    const int width = std::clamp<int>(
        static_cast<int>(measured.right - measured.left) + 56, 200, 380);
    const int height = std::max<int>(
        74, static_cast<int>(measured.bottom - measured.top) + 52);
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                      GetSystemMetrics(SM_CYSCREEN)};
    }
    const int left = work.right - width - 8;
    const int top = work.bottom - height;
    if (!playback_tip_window_ || !IsWindow(playback_tip_window_)) {
        playback_tip_window_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kPlaybackTipWindowClass, nullptr,
            WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            left, top, width, height, window_, nullptr, instance_, this);
        if (!playback_tip_window_) return;
    } else {
        SetWindowPos(playback_tip_window_, HWND_TOPMOST, left, top,
                     width, height, SWP_NOACTIVATE);
        InvalidateRect(playback_tip_window_, nullptr, FALSE);
    }
    SetTimer(playback_tip_window_, 1, 3000, nullptr);
    if (settings_.general.fade_windows) {
        AnimateWindow(playback_tip_window_, 120, AW_BLEND);
    } else {
        ShowWindow(playback_tip_window_, SW_SHOWNOACTIVATE);
    }
}

void PlayerWindow::ClosePlaybackOpenTip() {
    if (playback_tip_window_ && IsWindow(playback_tip_window_))
        DestroyWindow(playback_tip_window_);
    playback_tip_window_ = nullptr;
    playback_tip_title_.clear();
    playback_tip_body_.clear();
}

void PlayerWindow::PaintPlaybackOpenTip(HDC dc) const {
    if (!playback_tip_window_ || !dc) return;
    RECT client{};
    GetClientRect(playback_tip_window_, &client);
    TRIVERTEX vertices[2]{
        {client.left, client.top, 0x8000, 0xc000, 0xe000, 0xffff},
        {client.right, client.bottom, 0xffff, 0xffff, 0xffff, 0xffff}};
    GRADIENT_RECT gradient{0, 1};
    GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);
    FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    RECT title_bounds{12, 8, client.right - 10, 28};
    RECT body_bounds{34, 32, client.right - 10, client.bottom - 8};
    HICON icon = LoadIconW(nullptr, IDI_INFORMATION);
    if (icon) DrawIconEx(dc, 10, 32, icon, 16, 16, 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ old = SelectObject(dc,
        ui_font_ ? ui_font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, RGB(8, 64, 112));
    DrawTextW(dc, playback_tip_title_.c_str(), -1, &title_bounds,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SetTextColor(dc, RGB(16, 80, 128));
    DrawTextW(dc, playback_tip_body_.c_str(), -1, &body_bounds,
              DT_NOPREFIX | DT_WORDBREAK);
    SelectObject(dc, old);
}

void PlayerWindow::UnregisterConfiguredHotKeys() {
    if (window_) {
        for (const int identifier : registered_hotkey_ids_)
            UnregisterHotKey(window_, identifier);
    }
    registered_hotkey_ids_.clear();
}

void PlayerWindow::RegisterConfiguredHotKeys() {
    UnregisterConfiguredHotKeys();
    if (!window_ || !settings_.hotkey.global) return;
    for (size_t index = 0; index < settings_.hotkey.key_map.size(); ++index) {
        const auto& binding = settings_.hotkey.key_map[index];
        if (binding.command < 0 || binding.global.virtual_key == 0) continue;
        UINT modifiers{};
        if ((binding.global.modifiers & HOTKEYF_ALT) != 0) modifiers |= MOD_ALT;
        if ((binding.global.modifiers & HOTKEYF_CONTROL) != 0)
            modifiers |= MOD_CONTROL;
        if ((binding.global.modifiers & HOTKEYF_SHIFT) != 0)
            modifiers |= MOD_SHIFT;
        // CPlayerApp registers the zero-based shortcut row as the hot-key ID
        // and deliberately ignores RegisterHotKey's result.  Keep every
        // attempted ID so page activation can symmetrically unregister it.
        const int identifier = static_cast<int>(index);
        RegisterHotKey(window_, identifier, modifiers,
                       static_cast<UINT>(binding.global.virtual_key));
        registered_hotkey_ids_.push_back(identifier);
    }
}

void PlayerWindow::EnsurePopupMenuImages() {
    if (popup_menu_images_) return;
    const HMODULE resources = ResourceModule();
    if (!resources) return;
    int initial_images = 1;
    const HRSRC first_toolbar = FindResourceW(
        resources, MAKEINTRESOURCEW(0x82), MAKEINTRESOURCEW(241));
    if (first_toolbar) {
        const HGLOBAL loaded = LoadResource(resources, first_toolbar);
        const auto* words = loaded
            ? static_cast<const WORD*>(LockResource(loaded)) : nullptr;
        if (words && SizeofResource(resources, first_toolbar) >= 8 &&
            words[0] == 1 && words[1] == 16 && words[2] == 16) {
            initial_images = std::max(1, static_cast<int>(words[3]));
        }
    }
    // FUN_0048B5AB passes the first toolbar's item count to
    // ImageList_Create(..., ILC_COLOR32 | ILC_MASK, count, 1).
    popup_menu_images_ = ImageList_Create(
        16, 16, ILC_COLOR32 | ILC_MASK, initial_images, 1);
    if (!popup_menu_images_) return;

    // CCommandBarCtrl initialization in 00467B9B and 00482BAF appends these
    // three RT_TOOLBAR/RT_BITMAP pairs to one 16x16 image list.  A zero
    // toolbar command is a separator and does not consume a bitmap frame.
    for (const UINT identifier : {0x82U, 0x83U, 0x94U}) {
        const HRSRC toolbar_resource = FindResourceW(
            resources, MAKEINTRESOURCEW(identifier), MAKEINTRESOURCEW(241));
        if (!toolbar_resource) continue;
        const HGLOBAL loaded = LoadResource(resources, toolbar_resource);
        const auto* words = loaded
            ? static_cast<const WORD*>(LockResource(loaded)) : nullptr;
        const DWORD bytes = SizeofResource(resources, toolbar_resource);
        if (!words || bytes < 8 || words[0] != 1 || words[1] != 16 || words[2] != 16)
            continue;
        const size_t count = words[3];
        if (bytes < (4 + count) * sizeof(WORD)) continue;

        // FUN_0048B5AB examines the RT_BITMAP bit depth.  The recovered
        // 0x82/0x83/0x94 strips are all 24-bit, so the original takes its
        // LoadBitmapW/DDB path (the LR_CREATEDIBSECTION path is only for
        // 32-bit strips).  This also determines how the later appended icon
        // is blended by the 32-bit image list.
        const HBITMAP bitmap = LoadBitmapW(resources, MAKEINTRESOURCEW(identifier));
        if (!bitmap) continue;
        BITMAP description{};
        const int frame_count = GetObjectW(bitmap, sizeof(description), &description) ==
                sizeof(description)
            ? description.bmWidth / 16 : 0;
        // ImageList_AddMasked may replace the colour-key pixels in its input
        // bitmap. Capture transparency first, before that destructive step.
        std::vector<PopupMenuImageMask> masks;
        for (int frame = 0; frame < frame_count; ++frame)
            masks.push_back(ReadPopupMenuBitmapMask(bitmap, frame));
        const int first_image = ImageList_AddMasked(
            popup_menu_images_, bitmap, RGB(192, 192, 192));
        if (first_image >= 0 && frame_count > 0) {
            popup_menu_image_masks_.resize(static_cast<size_t>(first_image + frame_count));
            for (int frame = 0; frame < frame_count; ++frame) {
                popup_menu_image_masks_[static_cast<size_t>(first_image + frame)] =
                    masks[static_cast<size_t>(frame)];
            }
        }
        DeleteObject(bitmap);
        if (first_image < 0) continue;
        int image = first_image;
        for (size_t index = 0; index < count; ++index) {
            const UINT command = words[4 + index];
            if (command == 0) continue;
            popup_menu_image_commands_.emplace_back(command, image++);
        }
    }

    // 0045FAD8 adds the application icon separately and associates it with
    // the Options command (0xE140), before installing command aliases.
    const int small_width = GetSystemMetrics(SM_CXSMICON);
    const int small_height = GetSystemMetrics(SM_CYSMICON);
    HICON application_icon = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(0x80), IMAGE_ICON,
        small_width, small_height, LR_DEFAULTCOLOR));
    if (!application_icon) {
        application_icon = window_icon_small_ ? CopyIcon(window_icon_small_) : nullptr;
    }
    if (application_icon) {
        const int image = ImageList_AddIcon(popup_menu_images_, application_icon);
        if (image >= 0) popup_menu_image_commands_.emplace_back(kCmdOptions, image);
        DestroyIcon(application_icon);
    }

    const auto alias = [this](UINT source, UINT destination) {
        const auto found = std::find_if(popup_menu_image_commands_.begin(),
            popup_menu_image_commands_.end(), [source](const auto& value) {
                return value.first == source;
            });
        if (found != popup_menu_image_commands_.end())
            popup_menu_image_commands_.emplace_back(destination, found->second);
    };
    // Command aliases installed immediately after the command bar is built
    // in 0045FAD8 and 00482BAF.
    for (const auto [source, destination] : {
            std::pair{0x8023U, 0x8084U}, std::pair{0x8023U, 0x7efdU},
            std::pair{0x8023U, 0x7fefU}, std::pair{0x8023U, 31000U},
            std::pair{0x8023U, 0x8046U}, std::pair{0x802cU, 0x94U},
            std::pair{0xe122U, 0x8034U}, std::pair{0xe123U, 0x7f30U},
            std::pair{0xe122U, 0x7f31U}, std::pair{0xe125U, 0x7f32U},
            std::pair{0x7f12U, 0x7f13U}, std::pair{32000U, 0x8dU},
            std::pair{32000U, 0x7ef5U}, std::pair{0x7d67U, 0x7ef6U},
            std::pair{0x90U, 0x7d65U}, std::pair{0x90U, 0x7e2cU},
            std::pair{0x8fU, 0x7d64U}, std::pair{0x97U, 0x7d66U},
            std::pair{0x8070U, 0x8071U}, std::pair{0x8070U, 0x8072U},
            std::pair{0xe124U, 0x7f39U}}) {
        alias(source, destination);
    }
    // New commands need explicit aliases: their IDs are not present in the
    // original RT_TOOLBAR table. Reuse the native web-link and edit images.
    for (const auto& link : kProjectLinks)
        alias(link.image_command, link.command);
}

void PlayerWindow::BeginPopupMenuStyle(HMENU menu, bool hide_keyboard_cues) {
    EndPopupMenuStyle();
    EnsurePopupMenuImages();
    popup_menu_hide_keyboard_cues_ = hide_keyboard_cues;

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics) - sizeof(metrics.iPaddedBorderWidth);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                               &metrics, 0)) {
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                              &metrics, 0);
    }
    popup_menu_font_ = CreateFontIndirectW(&metrics.lfMenuFont);
    LOGFONTW bold = metrics.lfMenuFont;
    bold.lfWeight += 200;
    popup_menu_bold_font_ = CreateFontIndirectW(&bold);
    if (!popup_menu_font_) popup_menu_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!popup_menu_bold_font_) popup_menu_bold_font_ = popup_menu_font_;
    ApplyPopupMenuStyle(menu);
}

void PlayerWindow::ApplyPopupMenuStyle(HMENU menu) {
    if (!menu) return;
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count; ++position) {
        MENUITEMINFOW information{sizeof(information)};
        information.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU |
                            MIIM_CHECKMARKS | MIIM_DATA;
        if (!GetMenuItemInfoW(menu, position, TRUE, &information)) continue;

        // The skin submenu is populated lazily from WM_INITMENUPOPUP, after
        // this routine has already visited its resource skeleton. Preserve
        // those records and style only the newly inserted package rows.
        if ((information.fType & MFT_OWNERDRAW) != 0 &&
            FindPopupMenuItem(information.dwItemData)) {
            if (information.hSubMenu)
                ApplyPopupMenuStyle(information.hSubMenu);
            continue;
        }

        const int length = GetMenuStringW(menu, position, nullptr, 0, MF_BYPOSITION);
        std::vector<wchar_t> text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
        if (length > 0)
            GetMenuStringW(menu, position, text.data(), length + 1, MF_BYPOSITION);

        int image = -1;
        const auto icon = std::find_if(popup_menu_image_commands_.begin(),
            popup_menu_image_commands_.end(), [&](const auto& value) {
                return value.first == information.wID;
            });
        if (icon != popup_menu_image_commands_.end()) image = icon->second;
        popup_menu_items_.push_back(MenuVisualItem{
            text.data(), information.fType, information.fState, information.wID,
            information.hSubMenu != nullptr,
            information.hbmpChecked, information.hbmpUnchecked, image});
        MenuVisualItem* visual = &popup_menu_items_.back();

        MENUITEMINFOW styled{sizeof(styled)};
        styled.fMask = MIIM_FTYPE | MIIM_DATA;
        styled.fType = information.fType | MFT_OWNERDRAW;
        styled.dwItemData = reinterpret_cast<ULONG_PTR>(visual);
        SetMenuItemInfoW(menu, position, TRUE, &styled);
        if (information.hSubMenu) ApplyPopupMenuStyle(information.hSubMenu);
    }
}

void PlayerWindow::EndPopupMenuStyle() {
    popup_menu_items_.clear();
    if (popup_menu_bold_font_ && popup_menu_bold_font_ != popup_menu_font_ &&
        popup_menu_bold_font_ != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(popup_menu_bold_font_);
    }
    if (popup_menu_font_ && popup_menu_font_ != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(popup_menu_font_);
    popup_menu_font_ = nullptr;
    popup_menu_bold_font_ = nullptr;
    popup_menu_hide_keyboard_cues_ = false;
}

const PlayerWindow::MenuVisualItem* PlayerWindow::FindPopupMenuItem(
    ULONG_PTR data) const noexcept {
    const auto* candidate = reinterpret_cast<const MenuVisualItem*>(data);
    const auto found = std::find_if(popup_menu_items_.begin(), popup_menu_items_.end(),
        [candidate](const MenuVisualItem& value) { return &value == candidate; });
    return found == popup_menu_items_.end() ? nullptr : &*found;
}

bool PlayerWindow::MeasurePopupMenuItem(MEASUREITEMSTRUCT& item) const {
    if (item.CtlType != ODT_MENU) return false;
    const MenuVisualItem* visual = FindPopupMenuItem(item.itemData);
    if (!visual) return false;
    if (static_cast<SHORT>(visual->command) == -6) {
        item.itemWidth = static_cast<UINT>(
            std::max(0, 20 - GetSystemMetrics(SM_CYMENUCHECK)));
        item.itemHeight = 0;
        return true;
    }
    if ((visual->type & MFT_SEPARATOR) != 0) {
        item.itemWidth = 0;
        item.itemHeight = static_cast<UINT>(GetSystemMetrics(SM_CYMENU) / 2);
        return true;
    }

    HDC dc = GetWindowDC(nullptr);
    if (!dc) return false;
    const HFONT font = (visual->state & MFS_DEFAULT) != 0
        ? popup_menu_bold_font_ : popup_menu_font_;
    const HGDIOBJ previous = SelectObject(dc, font);
    RECT text{};
    DrawTextW(dc, visual->text.c_str(), -1, &text,
              DT_SINGLELINE | DT_VCENTER | DT_CALCRECT);
    RECT tab{};
    DrawTextW(dc, L"\t", -1, &tab, DT_SINGLELINE | DT_VCENTER | DT_CALCRECT);
    int tab_compensation = tab.right - tab.left;
    if (tab_compensation < 4) {
        SetRectEmpty(&tab);
        DrawTextW(dc, L"x", -1, &tab, DT_SINGLELINE | DT_VCENTER | DT_CALCRECT);
        tab_compensation = tab.right - tab.left;
    } else {
        tab_compensation = 0;
    }
    LOGFONTW description{};
    GetObjectW(font, sizeof(description), &description);
    SelectObject(dc, previous);
    ReleaseDC(nullptr, dc);

    item.itemHeight = static_cast<UINT>(
        std::max<LONG>(20, std::abs(description.lfHeight) + 8));
    const LONG measured_width = text.right - text.left + tab_compensation +
        20 * 2 - GetSystemMetrics(SM_CYMENUCHECK) + 6;
    item.itemWidth = static_cast<UINT>(std::max<LONG>(0, measured_width));
    return true;
}

bool PlayerWindow::DrawPopupMenuItem(const DRAWITEMSTRUCT& item) const {
    if (item.CtlType != ODT_MENU) return false;
    const MenuVisualItem* visual = FindPopupMenuItem(item.itemData);
    if (!visual) return false;
    const int saved = SaveDC(item.hDC);
    if (static_cast<SHORT>(visual->command) == -6) {
        DrawPopupMenuSideTitle(item.hDC, visual->text);
        if (saved) RestoreDC(item.hDC, saved);
        return true;
    }
    FillPopupMenuBackground(item.hDC, item.rcItem);

    if ((visual->type & MFT_SEPARATOR) != 0) {
        RECT line{item.rcItem.left + 22,
                  (item.rcItem.top + item.rcItem.bottom) / 2,
                  item.rcItem.right,
                  (item.rcItem.top + item.rcItem.bottom) / 2 + 1};
        SetDCBrushColor(item.hDC, RGB(192, 192, 192));
        FillRect(item.hDC, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (saved) RestoreDC(item.hDC, saved);
        return true;
    }

    const bool disabled = (item.itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0 && !disabled;
    const bool checked = (item.itemState & ODS_CHECKED) != 0;
    if (selected) {
        SetDCBrushColor(item.hDC, RGB(193, 210, 238));
        FillRect(item.hDC, &item.rcItem,
                 static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(item.hDC, RGB(49, 106, 197));
        FrameRect(item.hDC, &item.rcItem,
                  static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    RECT icon_bounds{item.rcItem.left, item.rcItem.top,
                     item.rcItem.left + 20, item.rcItem.top + 20};
    OffsetRect(&icon_bounds, 0,
        ((item.rcItem.bottom - item.rcItem.top) - 20) / 2);
    if (visual->image >= 0 && popup_menu_images_) {
        int x = icon_bounds.left + 2;
        int y = icon_bounds.top + 2;
        if (disabled) {
            const HICON icon = ImageList_GetIcon(popup_menu_images_, visual->image, ILD_NORMAL);
            if (icon) {
                DrawStateW(item.hDC, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0,
                           x, y, 16, 16, DST_ICON | DSS_DISABLED);
                DestroyIcon(icon);
            }
        } else {
            if (checked) {
                RECT frame = icon_bounds;
                InflateRect(&frame, -1, -1);
                SetDCBrushColor(item.hDC, RGB(193, 210, 238));
                FillRect(item.hDC, &frame,
                         static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                SetDCBrushColor(item.hDC, RGB(49, 106, 197));
                FrameRect(item.hDC, &frame,
                          static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            } else if (selected) {
                // FUN_0046D5E7: a focused, enabled, unchecked image is raised.
                // First stamp a monochrome grey copy at (+1,+1), then move the
                // coloured image to (-1,-1) before ImageList_Draw(..., ILD_TRANSPARENT).
                // The native branch tests enabled/selected/checked, not the
                // presence of a submenu or a particular command ID. Retain
                // the source mask for every toolbar image: comctl32 may lose
                // it when exporting a 32-bit image-list cell to DSS_MONO.
                if (static_cast<size_t>(visual->image) < popup_menu_image_masks_.size()) {
                    DrawPopupMenuBitmapMask(
                        item.hDC,
                        popup_menu_image_masks_[static_cast<size_t>(visual->image)],
                        x + 1, y + 1, RGB(128, 128, 128));
                } else {
                    const HICON icon = ImageList_GetIcon(
                        popup_menu_images_, visual->image, ILD_NORMAL);
                    if (icon) {
                        DrawStateW(item.hDC,
                                   static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)),
                                   nullptr, reinterpret_cast<LPARAM>(icon), 0,
                                   x + 1, y + 1, 16, 16, DST_ICON | DSS_MONO);
                        DestroyIcon(icon);
                    }
                }
                --x;
                --y;
            }
            ImageList_Draw(popup_menu_images_, visual->image, item.hDC, x, y,
                           ILD_TRANSPARENT);
        }
    } else if (checked || visual->checked_bitmap || visual->unchecked_bitmap) {
        RECT frame = icon_bounds;
        InflateRect(&frame, -1, -1);
        SetDCBrushColor(item.hDC, RGB(193, 210, 238));
        FillRect(item.hDC, &frame,
                 static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(item.hDC, RGB(49, 106, 197));
        FrameRect(item.hDC, &frame,
                  static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        HBITMAP bitmap = checked ? visual->checked_bitmap
                                 : visual->unchecked_bitmap;
        bool release_bitmap = false;
        if (!bitmap) {
            // FUN_00470B5F uses the legacy system OBM_CHECK bitmap, not
            // DrawFrameControl, then offsets it one pixel to the right.
            bitmap = LoadBitmapW(nullptr, MAKEINTRESOURCEW(0x7ff8));
            release_bitmap = bitmap != nullptr;
        }
        BITMAP description{};
        if (bitmap && GetObjectW(bitmap, sizeof(description), &description) ==
                          sizeof(description)) {
            int x = frame.left + ((frame.right - frame.left) - description.bmWidth) / 2;
            const int y = frame.top + ((frame.bottom - frame.top) - description.bmHeight) / 2;
            if (release_bitmap) ++x;
            SetBkColor(item.hDC, RGB(193, 210, 238));
            UINT state = DST_BITMAP;
            if (disabled) state |= DSS_DISABLED;
            DrawStateW(item.hDC, nullptr, nullptr,
                       reinterpret_cast<LPARAM>(bitmap), 0, x, y,
                       frame.right - frame.left, frame.bottom - frame.top,
                       state);
        }
        if (release_bitmap) DeleteObject(bitmap);
    }

    RECT text_bounds{item.rcItem.left + 23, item.rcItem.top,
                     item.rcItem.right - 20, item.rcItem.bottom};
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(192, 192, 192) : RGB(0, 0, 0));
    const HFONT font = ((item.itemState & ODS_DEFAULT) != 0 ||
                        (visual->state & MFS_DEFAULT) != 0)
        ? popup_menu_bold_font_ : popup_menu_font_;
    SelectObject(item.hDC, font);
    UINT format = DT_SINGLELINE | DT_VCENTER;
    if (popup_menu_hide_keyboard_cues_ ||
        (item.itemState & ODS_NOACCEL) != 0) {
        format |= DT_HIDEPREFIX;
    }
    const size_t tab = visual->text.find(L'\t');
    if (tab == std::wstring::npos) {
        DrawTextW(item.hDC, visual->text.c_str(), -1, &text_bounds, format | DT_LEFT);
    } else {
        const std::wstring left = visual->text.substr(0, tab);
        const std::wstring right = visual->text.substr(tab + 1);
        DrawTextW(item.hDC, left.c_str(), -1, &text_bounds, format | DT_LEFT);
        DrawTextW(item.hDC, right.c_str(), -1, &text_bounds, format | DT_RIGHT);
    }
    if (saved) RestoreDC(item.hDC, saved);
    return true;
}

bool PlayerWindow::LoadStartupSkin(HMODULE module) {
    if (window_) return false;
    const bool requested_default = settings_.skin_file.empty() ||
        _wcsicmp(settings_.skin_file.c_str(), L"<Default_Skin>") == 0;
    const bool loaded = requested_default ? LoadSkinResource(module) :
        LoadSkinPackage(skin::ResolveSkinPackagePath(
            PlayerRuntimeDirectory() / L"Skin", settings_.skin_file));
    const bool fallback = !loaded && !requested_default;
    if (!loaded && (!fallback || !LoadSkinResource(module))) return false;

    const auto global_visual = settings_.visual;
    if (fallback) {
        // FUN_0045D5FA's requested-package-missing branch replaces the
        // selector BEFORE binding 0045DDEE and reading Skin/Default.xml.
        // It is a skin change even on first startup, not an ordinary restore
        // of the last main XML. Never read the orphan .skn.xml or retain its
        // styles/rectangles already serialized into TTPlayerRebuild.xml.
        ApplyPlaylistSkinDefaults(skin_->Playlist(), settings_.playlist);
        ApplyLyricSkinDefaults(skin_->Lyric(), settings_.lyric);
        settings_.visual = settings::VisualSettings{};
        ApplySkinVisualSettings();
        // Type/FPS are global preferences, not per-skin profile fields.
        settings_.visual.type = global_visual.type;
        settings_.visual.frames_per_second = global_visual.frames_per_second;
        SetRectEmpty(&settings_.player.player_window);
        SetRectEmpty(&settings_.player.mini_player_window);
        SetRectEmpty(&settings_.player.lyric_window);
        SetRectEmpty(&settings_.player.mini_lyric_window);
        SetRectEmpty(&settings_.player.playlist_window);
        SetRectEmpty(&settings_.player.equalizer_window);
        settings_.player.lyric_visible = true;
        settings_.player.playlist_visible = true;
        settings_.player.equalizer_visible = true;
    }
    // Use the successfully loaded selector, after validation but before any
    // HWND/font/brush is created. Missing/partial profiles overlay only their
    // actual fields on the package baseline; no configuration is written here.
    static_cast<void>(settings::LoadSkinVisualProfile(CurrentSkinProfilePath(),
        settings_.player, settings_.playlist, settings_.lyric, settings_.visual));
    if (!fallback) {
        // Preserve the established normal-startup contract: main XML visual
        // globals remain active (e.g. LX-iPlay's restored #194d5c spectrum).
        // Only an actual package replacement applies its visual palette.
        settings_.visual = global_visual;
    }
    return true;
}

bool PlayerWindow::LoadSkinPackage(const std::filesystem::path& path, bool restore_profile) {
    try {
        auto package = skin::SkinPackage::Open(path);
        const auto stamp = std::filesystem::last_write_time(path).time_since_epoch().count();
        const auto cache = std::filesystem::temp_directory_path() / L"TTPlayerRebuild" /
            (path.stem().wstring() + L"-" + std::to_wstring(stamp) + L"-" +
             std::to_wstring(package.Fingerprint()));
        auto profile = path;
        profile += L".xml";
        const auto selector = skin::SkinPackageSelector(
            PlayerRuntimeDirectory() / L"Skin", path);
        return LoadSkin(std::move(package), cache, selector,
                        profile, restore_profile);
    } catch (const std::exception&) { return false; }
}

bool PlayerWindow::LoadSkinResource(HMODULE module, const wchar_t* name, bool restore_profile) {
    try {
        skin_resources_ = module;
        auto package = skin::SkinPackage::OpenResource(module, name, L"ZIP");
        const auto cache = std::filesystem::temp_directory_path() / L"TTPlayerRebuild" /
            (L"DefaultSkin-resource-" + std::to_wstring(package.Fingerprint()));
        const auto global = settings_.source_path.empty()
            ? PlayerRuntimeDirectory() / settings::kSettingsFileName
            : settings_.source_path;
        return LoadSkin(std::move(package), cache, L"<Default_Skin>",
            ResolveSkinProfilePath(global, L"<Default_Skin>"), restore_profile);
    } catch (const std::exception&) { return false; }
}

bool PlayerWindow::LoadSkin(skin::SkinPackage package,
                            const std::filesystem::path& cache,
                            const std::wstring& selector,
                            const std::filesystem::path& profile, bool restore_profile) {
    if (!package.IsLegacyCompatible()) return false;
    package.ExtractTo(cache, ttpcomm_module_);
    auto next = skin::LegacySkin::Load(cache);
    if (!next.Valid()) return false;
    const HRGN region = next.CreateWindowRegion();
    RECT bounds{};
    const bool valid_region = region && GetRgnBox(region, &bounds) > NULLREGION;
    if (region) DeleteObject(region);
    if (!valid_region) return false;

    // 004683xx/00464B6C leave mini mode before replacing the package because
    // every live child/control points into the old skin object.
    if (window_) {
        // A non-mini package switch can likewise arrive while startup,
        // activation or an auxiliary visibility fade still references HWNDs
        // backed by the old skin. The original call cannot overlap because
        // 0044EFBE is synchronous.
        CompleteSkinWindowFadeForReplacement();
        if (!window_ || !IsWindow(window_) || close_after_skin_window_fade_)
            return false;
        if (restore_profile) SaveCurrentSkinProfile();
        // 0045DDEE restores an iconic main window before rebinding it.
        if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    }
    const auto previous_player = settings_.player;
    const auto previous_playlist = settings_.playlist;
    const auto previous_lyric = settings_.lyric;
    const auto previous_visual = settings_.visual;
    const auto previous_selector = settings_.skin_file;
    if (window_ && mini_mode_) {
        ToggleMiniMode();
        // 00464B6C is synchronous in the original.  The recovered fade is a
        // timer state machine, so a skin replacement must explicitly commit
        // both its fade-out/rebuild and fade-in before releasing the old
        // LegacySkin objects referenced by the live mini controls.
        CompleteSkinWindowFadeForReplacement();
        if (!window_ || !IsWindow(window_) || mini_mode_) return false;
    }
    // Runtime switching is transactional.  The HWND can still own the old
    // skin's HICON and a synchronous style/region change can cause WM_PAINT.
    // Keep every old GDI object alive until the new skin has been completely
    // attached; a failed load leaves the visible skin untouched.
    // DeskLrcBar owns borrowed HBITMAP pointers from LegacySkin. Detach them
    // before optional::emplace destroys the package object, then rebind only
    // after the replacement has passed ApplyLoadedSkin's region validation.
    desktop_lyrics_.SetSkin(nullptr);
    auto previous = std::move(skin_);
    ResetSkinControlAnimations();
    skin_.emplace(std::move(next));
    // Load the target configuration before presenting it. The old menu path
    // applied the package, repainted/recreated its controls, then loaded the
    // sidecar and applied everything again with a second set of HWNDs.
    bool profile_loaded = false;
    if (window_) {
        if (restore_profile) {
            ApplySkinVisualSettings();
            // 0045D5FA binds the target package (0045DDEE / 0047E6FC)
            // before 004B605A overlays its saved profile. An existing but
            // partial .skn.xml must not bypass Lyric.xml/Playlist.xml and
            // retain colours or fonts from the outgoing package.
            ApplyPlaylistSkinDefaults(skin_->Playlist(), settings_.playlist);
            ApplyLyricSkinDefaults(skin_->Lyric(), settings_.lyric);
            SetRectEmpty(&settings_.player.mini_lyric_window);
            // 0045D5FA defaults the auxiliary visibility flags to one before
            // reading a runtime target profile; availability is applied by
            // each skin-window binder. An absent sidecar is not a snapshot
            // of the outgoing package's hidden windows.
            settings_.player.lyric_visible = true;
            settings_.player.playlist_visible = true;
            settings_.player.equalizer_visible = true;
            profile_loaded = settings::LoadSkinVisualProfile(profile, settings_.player,
                settings_.playlist, settings_.lyric, settings_.visual);
        } else {
            // Options Reset All supplies its own freshly built settings.
            settings_.player = previous_player;
            settings_.player.mini_mode = mini_mode_;
        }
    }
    settings_.skin_file = selector;
    if (window_ && !ApplyLoadedSkin(false, profile_loaded || !restore_profile)) {
        skin_.reset();
        skin_ = std::move(previous);
        settings_.player = previous_player;
        settings_.playlist = previous_playlist;
        settings_.lyric = previous_lyric;
        settings_.visual = previous_visual;
        settings_.skin_file = previous_selector;
        if (skin_) static_cast<void>(ApplyLoadedSkin(false));
        return false;
    }
    if (window_ && (profile_loaded || !restore_profile)) ApplySkinProfileWindowState();
    previous.reset();
    return true;
}

void PlayerWindow::ReloadApplicationIcons() {
    if (window_icon_small_) DestroyIcon(window_icon_small_);
    if (window_icon_big_) DestroyIcon(window_icon_big_);
    window_icon_small_ = nullptr;
    window_icon_big_ = nullptr;

    IconLocation location = ParseIconLocation(
        settings_.general.app_icon_file.wstring());
    if (location.path.empty())
        location.path = FindRuntimePath(L"Icons/TTPlayer.ico");

    if (!location.path.empty()) {
        HICON extracted_large{};
        HICON extracted_small{};
        if (ExtractIconExW(location.path.c_str(), location.index,
                           &extracted_large, &extracted_small, 1) != 0) {
            window_icon_big_ = extracted_large;
            window_icon_small_ = extracted_small;
        }
        if (!window_icon_small_) {
            window_icon_small_ = static_cast<HICON>(LoadImageW(
                nullptr, location.path.c_str(), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                LR_LOADFROMFILE));
        }
        if (!window_icon_big_) {
            window_icon_big_ = static_cast<HICON>(LoadImageW(
                nullptr, location.path.c_str(), IMAGE_ICON,
                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                LR_LOADFROMFILE));
        }
    }
    if (!window_icon_small_) {
        window_icon_small_ = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(128), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    }
    if (!window_icon_big_) {
        window_icon_big_ = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(128), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    }
}

bool PlayerWindow::Create(HINSTANCE instance, int show_command) {
    instance_ = instance;
    const HMODULE resources = ResourceModule();
    display_title_ = LoadResourceText(resources, 0x80);
    const auto version = LoadResourceText(resources, 0x8299);
    if (!version.empty()) {
        if (!display_title_.empty()) display_title_ += L" ";
        display_title_ += version;
    }
    if (display_title_.empty()) display_title_ = L"TTPlayer";
    ReloadApplicationIcons();
    WNDCLASSEXW type{sizeof(type)};
    // Runtime GetClassLongW(GCL_STYLE) on the original class is exactly 0x8:
    // it requests double-click messages but no automatic H/V redraw policy.
    type.style = CS_DBLCLKS;
    type.lpfnWndProc = WindowProc;
    type.hInstance = instance;
    type.hIcon = window_icon_big_ ? window_icon_big_ : LoadIconW(nullptr, IDI_APPLICATION);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = nullptr;
    type.lpszClassName = kWindowClass;
    type.hIconSm = window_icon_small_ ? window_icon_small_ : type.hIcon;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW playlist_type{sizeof(playlist_type)};
    playlist_type.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    playlist_type.lpfnWndProc = PlaylistWindowProc;
    playlist_type.hInstance = instance;
    playlist_type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    playlist_type.hbrBackground = nullptr;
    playlist_type.lpszClassName = kPlaylistWindowClass;
    if (!RegisterClassExW(&playlist_type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW lyric_type{sizeof(lyric_type)};
    lyric_type.style = CS_DBLCLKS;
    lyric_type.lpfnWndProc = LyricWindowProc;
    lyric_type.hInstance = instance;
    lyric_type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    lyric_type.hbrBackground = nullptr;
    lyric_type.lpszClassName = kLyricWindowClass;
    if (!RegisterClassExW(&lyric_type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW equalizer_type{sizeof(equalizer_type)};
    equalizer_type.style = CS_DBLCLKS;
    equalizer_type.lpfnWndProc = EqualizerWindowProc;
    equalizer_type.hInstance = instance;
    equalizer_type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    equalizer_type.hbrBackground = nullptr;
    equalizer_type.lpszClassName = kEqualizerWindowClass;
    if (!RegisterClassExW(&equalizer_type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW visual_type{sizeof(visual_type)};
    visual_type.style = CS_DBLCLKS;
    visual_type.lpfnWndProc = VisualWindowProc;
    visual_type.hInstance = instance;
    visual_type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    visual_type.hbrBackground = nullptr;
    visual_type.lpszClassName = kVisualWindowClass;
    if (!RegisterClassExW(&visual_type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // FUN_00429AAA creates real WS_CHILD/WS_TABSTOP SkinButton and SkinSlider
    // windows.  They are not merely painted rectangles: the slider HWND owns
    // capture, focus, local mouse coordinates, and the TTF_IDISHWND tooltip
    // registration used by FUN_0040EE49.
    WNDCLASSEXW equalizer_control_type{sizeof(equalizer_control_type)};
    equalizer_control_type.style = CS_DBLCLKS;
    equalizer_control_type.lpfnWndProc = EqualizerControlProc;
    equalizer_control_type.hInstance = instance;
    equalizer_control_type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    equalizer_control_type.hbrBackground = nullptr;
    equalizer_control_type.lpszClassName = kEqualizerButtonClass;
    if (!RegisterClassExW(&equalizer_control_type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    equalizer_control_type.lpszClassName = kEqualizerSliderClass;
    if (!RegisterClassExW(&equalizer_control_type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    equalizer_control_type.lpszClassName = kLyricControlClass;
    if (!RegisterClassExW(&equalizer_control_type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // FUN_00482BAF creates three real child surfaces beneath the playlist
    // popup.  Their private class names are observable as TreeCtrl/ListCtrl;
    // retaining child HWNDs is also required for native focus-dependent
    // selection paint and for the original enter/leave message boundaries.
    equalizer_control_type.lpszClassName = kPlaylistTreeClass;
    if (!RegisterClassExW(&equalizer_control_type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    equalizer_control_type.lpszClassName = kPlaylistListClass;
    if (!RegisterClassExW(&equalizer_control_type) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const bool skinned = skin_ && skin_->Valid();
    mini_mode_ = skinned && settings_.player.mini_mode &&
                 skin_->SupportsMiniMode();
    settings_.player.mini_mode = mini_mode_;
    normal_window_bounds_ = settings_.player.player_window;
    mini_window_bounds_ = settings_.player.mini_player_window;
    have_normal_window_bounds_ = normal_window_bounds_.right > normal_window_bounds_.left &&
                                 normal_window_bounds_.bottom > normal_window_bounds_.top;
    have_mini_window_bounds_ = mini_window_bounds_.right > mini_window_bounds_.left &&
                               mini_window_bounds_.bottom > mini_window_bounds_.top;

    // CPlayerApp_CreateMainWindow (004C01CD) creates a hidden 300x200 host at
    // (100,100).  CPlayerWnd's WM_CREATE path then installs the selected skin,
    // restores PlayerWnd/PlayerWnd2 and only afterwards does the caller show
    // it with SW_SHOW.  The initial extended flags are 0x100 plus the active
    // mode's TOPMOST/TOOLWINDOW bits; skin setup replaces WINDOWEDGE with
    // LAYERED before the HWND becomes visible.
    const bool active_top_most = mini_mode_ ? settings_.player.mini_top_most
                                            : settings_.player.top_most;
    const DWORD extended = skinned
        ? WS_EX_WINDOWEDGE | (active_top_most ? WS_EX_TOPMOST : 0) |
              (mini_mode_ ? WS_EX_TOOLWINDOW : 0)
        : WS_EX_APPWINDOW | (active_top_most ? WS_EX_TOPMOST : 0);
    const DWORD style = skinned
        ? WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX
        : WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    const int create_x = skinned ? 100 : CW_USEDEFAULT;
    const int create_y = skinned ? 100 : CW_USEDEFAULT;
    const int create_width = skinned ? 300 : 860;
    const int create_height = skinned ? 200 : 540;
    window_ = CreateWindowExW(extended, kWindowClass, display_title_.c_str(),
        style, create_x, create_y, create_width, create_height,
        nullptr, nullptr, instance, this);
    if (!window_) return false;
    if (skinned) {
        const LONG_PTR final_style =
            WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPSIBLINGS;
        const LONG_PTR final_extended =
            WS_EX_LAYERED | (active_top_most ? WS_EX_TOPMOST : 0) |
            (mini_mode_ ? WS_EX_TOOLWINDOW : 0);
        SetWindowLongPtrW(window_, GWL_STYLE, final_style);
        SetWindowLongPtrW(window_, GWL_EXSTYLE, final_extended);
        ApplySkinWindowAlpha(EffectiveSkinWindowAlpha(window_));
        const SIZE size = mini_mode_ ? skin_->MiniWindowSize() : skin_->WindowSize();
        const RECT& saved = mini_mode_ ? mini_window_bounds_ : normal_window_bounds_;
        const bool have_saved = mini_mode_ ? have_mini_window_bounds_
                                           : have_normal_window_bounds_;
        const int left = have_saved ? saved.left : 100;
        const int top = have_saved ? saved.top : 100;
        SetWindowPos(window_, nullptr, left, top, size.cx, size.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetWindowRgn(window_, skin_->CreateWindowRegion(mini_mode_), FALSE);
        const bool custom_icon = !settings_.general.app_icon_file.empty();
        const HICON small_icon = !custom_icon && skin_->Icon()
            ? skin_->Icon() : window_icon_small_;
        const HICON big = !custom_icon && skin_->Icon()
            ? skin_->Icon() : window_icon_big_;
        if (small_icon) SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        if (big) SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (equalizer_window_ && skin_->Equalizer().valid &&
            !(settings_.player.equalizer_window.right >
                  settings_.player.equalizer_window.left &&
              settings_.player.equalizer_window.bottom >
                  settings_.player.equalizer_window.top)) {
            RECT player{};
            GetWindowRect(window_, &player);
            const auto& equalizer = skin_->Equalizer();
            SetWindowPos(equalizer_window_, nullptr,
                player.left + equalizer.position.left,
                player.top + equalizer.position.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    ApplyWindowShadow();
    if (skinned) {
        // CPlayerApp_CreateMainWindow (004C01CD) passes the sentinel 100 to
        // 004685A1 before SW_SHOW.  The comma expression in 004685A1 turns
        // that sentinel into alpha zero without replacing DAT_00547820.
        ApplySkinWindowAlpha(0);
    }
    // 004C01CD uses SW_SHOW (5), not the CRT nCmdShow value.
    static_cast<void>(show_command);
    suppress_skin_window_activation_fade_ = skinned;
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    // Visibility commands for owned windows are posted during CPlayerWnd's
    // initialization and are dispatched by the two startup PeekMessage pumps
    // only after the main HWND has been shown.  Preserve that visible order.
    if (lyric_window_) ApplyActiveLyricWindowState();
    // 00467B9B's normal order is Lyric -> Equalizer -> PlayList, leaving
    // the playlist above the equalizer when their rectangles overlap.
    if (!mini_mode_ && settings_.player.equalizer_visible && equalizer_window_)
        ShowWindow(equalizer_window_, SW_SHOW);
    if (!mini_mode_ && settings_.player.playlist_visible && playlist_window_)
        ShowWindow(playlist_window_, SW_SHOW);
    suppress_skin_window_activation_fade_ = false;
    if (skinned) {
        // The following -1 sentinel restores DAT_00547820's configured alpha
        // through 004A47C2; OpaqueWhenActive is an activation transition and
        // is not the startup endpoint.  The original completes this fade
        // synchronously before returning to TTPlayer_wWinMain.  Keep our
        // message-driven equivalent protected from the SetForegroundWindow /
        // SetActiveWindow calls that immediately follow Create().
        startup_skin_window_fade_pending_ = true;
        BeginSkinWindowFade(window_, 0, skin_window_alpha_, true,
            settings_.general.startup_minimize
                ? kFadeCompleteStartupMinimize
                : kFadeCompleteFinishStartup);
    } else if (settings_.general.startup_minimize) {
        ShowWindow(window_, SW_MINIMIZE);
    }
    return true;
}

LRESULT CALLBACK PlayerWindow::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK PlayerWindow::PlaybackTipWindowProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (self) self->PaintPlaybackOpenTip(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_TIMER:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (self && self->playback_tip_window_ == window)
            self->playback_tip_window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PlayerWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    static const UINT taskbar_button_created = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (message == kMsgPlaylistInfoReady)
        return ApplyPlaylistInfoResult(lparam);
    if (message == kMsgMediaLibraryReady)
        return ApplyMediaLibraryIndex(lparam);
    if (message == kMsgMediaLibraryChanged)
        return HandleMediaLibraryDirectoryChange(wparam, lparam);
    // 004616BD performs its final alpha loop synchronously and pumps only
    // WM_PAINT.  While its asynchronous equivalent is pending, reject every
    // externally routable state mutation so the state already persisted by
    // WM_CLOSE cannot diverge from what WM_DESTROY observes.
    if (close_after_skin_window_fade_ &&
        (message == WM_COMMAND || message == WM_SYSCOMMAND ||
         message == WM_CONTEXTMENU || message == WM_HOTKEY ||
         message == WM_COPYDATA || message == WM_DROPFILES)) {
        return 0;
    }
    if (message == taskbar_created) {
        taskbar_playback_.Reset();
        tray_icon_added_ = false;
        UpdateTrayIcon();
        return 0;
    }
    if (message == taskbar_button_created) {
        static_cast<void>(taskbar_playback_.OnButtonCreated(
            window_, TaskbarState(), TaskbarLabels()));
        return 0;
    }
    switch (message) {
    case WM_WINDOWPOSCHANGED:
        // Mini mode uses ShowWindow(SW_HIDE); fullscreen uses
        // SetWindowPos(SWP_HIDEWINDOW). Both remove the taskbar entry without
        // destroying our HWND. A later TaskbarButtonCreated must install a
        // fresh toolbar, not update the old shell registration (which may
        // misleadingly return success even though its buttons are gone).
        if (lparam &&
            (reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_HIDEWINDOW))
            taskbar_playback_.Reset();
        // DefWindowProc must still generate WM_SIZE/WM_MOVE for skin layout.
        break;
    case WM_CREATE:
        audio_.SetDspParentWindow(window_);
        CreateControls();
        visual_window_ = CreateWindowExW(0, kVisualWindowClass, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVisualControlId)),
            instance_, this);
        UpdateVisualWindowLayout();
        UpdateVisualFrame();
        StartVisualWorker();
        CreateToolTipWindow();
        UpdateMainToolRects();
        LoadStoredPlaylist();
        // 004C038B loads Music.library during application startup whenever
        // Library/Enabled is set; entering the library view is not required.
        if (settings_.library.enabled) {
            StartMediaLibraryRefresh();
            StartMediaLibraryMonitoring();
        }
        CreateLyricWindow();
        CreateEqualizerWindow();
        CreatePlaylistWindow();
        // CPlayerWnd::OnCreate (0045FAD8) exposes files through an OLE drop
        // target.  WM_DROPFILES is not enabled on the original top-level
        // window and cannot preserve IDataObject ordering or drop effects.
        RegisterFileDropTarget(window_, FileDropSurface::player);
        UpdateTrayIcon();
        RegisterConfiguredHotKeys();
        // TTPlayer_wWinMain/CPlayerWnd installs timer 10 with 0xfa (250 ms).
        SetTimer(window_, kUiTimer, kUiRefreshIntervalMs, nullptr);
        UpdateAutoShutdownTimer();
        ResetSkinInfoScroll();
        return 0;
    case WM_ACTIVATE:
        if (!suppress_skin_window_activation_fade_ &&
            !startup_skin_window_fade_pending_ &&
            !close_after_skin_window_fade_ && !mini_mode_fade_pending_ &&
            skin_ && settings_.player.opaque_when_active &&
            transparency_percent_ > 0) {
            const BYTE alpha = LOWORD(wparam) == WA_INACTIVE
                ? static_cast<BYTE>(255 * (100 - transparency_percent_) / 100)
                : static_cast<BYTE>(255);
            AnimateSkinWindowAlpha(alpha);
        }
        return 0;
    case WM_ACTIVATEAPP:
        // Multi-monitor extension: unlike FUN_004657DF, switching to another
        // application does not tear down fullscreen. Escape/menu still exit.
        return 0;
    case WM_DISPLAYCHANGE:
        // Query the monitor topology again; lParam describes only the primary
        // resolution and cannot locate a secondary or negative-origin screen.
        if (fullscreen_mode_ != 0) {
            UpdateFullScreenLayout();
            UpdateVisualWindowLayout();
            UpdateVisualFrame();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (wparam == SPI_SETWORKAREA && fullscreen_mode_ != 0)
            UpdateFullScreenLayout();
        return 0;
    case WM_HOTKEY:
        if (static_cast<int>(wparam) == kFullscreenEscapeHotkey &&
            fullscreen_mode_ != 0) {
            SetFullScreenMode(0);
            return 0;
        }
        // 0046D21B resolves the first configured binding from lParam's
        // modifier/key pair instead of trusting the RegisterHotKey ID.
        for (const auto& binding : settings_.hotkey.key_map) {
            UINT modifiers{};
            if ((binding.global.modifiers & HOTKEYF_ALT) != 0)
                modifiers |= MOD_ALT;
            if ((binding.global.modifiers & HOTKEYF_CONTROL) != 0)
                modifiers |= MOD_CONTROL;
            if ((binding.global.modifiers & HOTKEYF_SHIFT) != 0)
                modifiers |= MOD_SHIFT;
            if (binding.command >= 0 && binding.global.virtual_key != 0 &&
                LOWORD(lparam) == modifiers &&
                HIWORD(lparam) == binding.global.virtual_key) {
                SendMessageW(window_, WM_COMMAND, binding.command, 0);
                return 0;
            }
        }
        break;
    case kTrayCallbackMessage:
        if (wparam != kTrayIconIdentifier) break;
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            POINT point{};
            GetCursorPos(&point);
            SetForegroundWindow(window_);
            SendMessageW(window_, WM_CONTEXTMENU,
                reinterpret_cast<WPARAM>(window_),
                MAKELPARAM(static_cast<short>(point.x),
                           static_cast<short>(point.y)));
            return 0;
        }
        if (lparam == WM_LBUTTONDBLCLK) {
            ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_RESTORE && fullscreen_mode_ != 0) {
            SetFullScreenMode(0);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO:
        if (!skin_) reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {760, 440};
        return 0;
    case WM_SIZE:
        LayoutControls(static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam)));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        Paint(dc);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        if (skin_) {
            // The original skin host receives WM_CONTEXTMENU for the main
            // HWND over the whole shaped surface.  Returning HTCAPTION here
            // redirects background right-clicks into the non-client system
            // menu path, so caption dragging is initiated explicitly from
            // WM_LBUTTONDOWN instead.
            return HTCLIENT;
        }
        break;
    case WM_MOUSEMOVE:
        if (skin_) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (dragging_skin_background_ && GetCapture() == window_) {
                ContinueSkinBackgroundDrag(window_, point);
                return 0;
            }
            const auto hit = HitTestSkin(point);
            if (hit != hover_skin_element_) {
                hover_skin_element_ = hit;
                InvalidateRect(window_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            if (!pressed_skin_element_.empty() && pressed_skin_element_ == L"volume") {
                SetSkinVolumeFromPoint(point);
            } else if (pressed_skin_element_ == L"progress") {
                SetSkinProgressFromPoint(point);
            }
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        hover_skin_element_.clear();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (skin_) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            pressed_skin_element_ = HitTestSkin(point);
            if (pressed_skin_element_.empty()) {
                // FUN_0044F670/FUN_0046EAAC records the client anchor and
                // uses client capture. It does not synthesize a caption
                // message, which would change double-click/right-click order.
                BeginSkinBackgroundDrag(window_, point);
            } else {
                dragging_skin_background_ = false;
                SetCapture(window_);
                if (pressed_skin_element_ == L"volume") SetSkinVolumeFromPoint(point);
                else if (pressed_skin_element_ == L"progress") SetSkinProgressFromPoint(point);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (skin_) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const auto released = HitTestSkin(point);
            const auto action = std::exchange(pressed_skin_element_, {});
            if (action == L"progress" && GetCapture() == window_) {
                // 00460AB1 commits only SB_THUMBPOSITION (4), not the
                // intermediate tracking notifications. Use the release point
                // even if it lies outside the control or no move preceded it.
                SetSkinProgressFromPoint(point);
                if (progress_tracking_position_) {
                    audio_.SeekWithoutFade(*progress_tracking_position_);
                    UpdateDiscordPresence();
                }
            }
            EndSkinMouseCapture();
            if (!action.empty() && action == released && action != L"volume" && action != L"progress") {
                InvokeSkinAction(action);
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (skin_) {
            // 00460CF5 checks only for a parsed mini_window, then dispatches
            // command 0x7DD4. Real skin buttons are child HWNDs, so a physical
            // double-click over one never reaches this main-window branch.
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (HitTestSkin(point).empty() && skin_->SupportsMiniMode())
                ToggleMiniMode();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (skin_) {
            progress_tracking_position_.reset();
            dragging_skin_background_ = false;
            skin_drag_window_ = nullptr;
            skin_drag_hit_ = 0;
            attached_drag_windows_.clear();
            pressed_skin_element_.clear();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CANCELMODE:
        if (skin_) {
            pressed_skin_element_.clear();
            EndSkinMouseCapture();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        // 00460D88 enters the complete 0x8a menu only when wParam is the
        // player HWND.  Child-window context menus take a separate 0x91 path.
        if (reinterpret_cast<HWND>(wparam) != window_) break;
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(window_, &bounds);
            point = {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        }
        if (fullscreen_mode_ != 0) SetFullScreenMode(0);
        ShowContextMenu(point);
        return 0;
    }
    case WM_INITMENUPOPUP: {
        const HMENU popup = reinterpret_cast<HMENU>(wparam);
        // CPlayerWnd_OnInitMenuPopup (00461BAE) recognizes the skin resource
        // submenu by its first command (0x7919).  The rebuild starts its scan
        // as soon as the root popup opens; this point waits for any unfinished
        // asynchronous tail and publishes the complete result into the HMENU.
        if (popup && (GetMenuItemID(popup, 0) == kCmdFirstTrack ||
                      GetMenuItemID(popup, 0) == 0x7ef4)) {
            // 00461BAE -> 004813C1. DeskLrcBar forwards this notification
            // (0041907F), so both entry points populate the same track menu.
            PopulateTrackMenu(popup);
            ApplyPopupMenuStyle(popup);
        } else if (popup && GetMenuItemID(popup, 0) == kCmdDefaultSkin) {
            PopulateSkinMenu(popup);
            ApplyPopupMenuStyle(popup);
        } else if (popup && GetMenuItemID(popup, 0) == kCmdVisualDream) {
            // CPlayerWnd_OnInitMenuPopup (00461BAE) uses CheckMenuItem with
            // flags 0x208 here.  CheckMenuRadioItem would mutate the menu
            // item type to MFT_RADIOCHECK and draw a round bullet, whereas
            // the original visual menu keeps the ordinary tick glyph.
            // The fullscreen lyric menu now starts with these same four
            // commands. Lyrics-only mode has no displayed visual to check.
            if (fullscreen_mode_ != 1)
                CheckMenuItem(popup,
                    kCmdVisualFirst + static_cast<UINT>(settings_.visual.type),
                    MF_BYCOMMAND | MF_CHECKED);
        }
        break;
    }
    case WM_MENUSELECT: {
        const UINT flags = HIWORD(wparam);
        if (flags == 0xffffU && lparam == 0) {
            HideSkinMenuToolTip();
        } else if ((flags & (MF_POPUP | MF_SEPARATOR | MF_DISABLED |
                             MF_GRAYED)) == 0) {
            QueueSkinMenuToolTip(LOWORD(wparam),
                                 reinterpret_cast<HMENU>(lparam));
        } else {
            HideSkinMenuToolTip();
        }
        break;
    }
    case WM_EXITMENULOOP:
        HideSkinMenuToolTip();
        break;
    case WM_NOTIFY:
        if (HandleToolTipNotification(window_, lparam)) return 0;
        break;
    case WM_DRAWITEM:
        if (lparam) {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (DrawPopupMenuItem(item)) return TRUE;
            if (item.CtlType == ODT_BUTTON) {
                DrawButton(item);
                return TRUE;
            }
        }
        break;
    case WM_MEASUREITEM:
        if (lparam && MeasurePopupMenuItem(
                *reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, reinterpret_cast<HWND>(lparam) == title_ ? kText : kMutedText);
        SetBkColor(dc, kBackground);
        return reinterpret_cast<LRESULT>(background_brush_);
    }
    case WM_CTLCOLORLISTBOX: {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(panel_brush_);
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == volume_) {
            const int value = static_cast<int>(SendMessageW(volume_, TBM_GETPOS, 0, 0));
            audio_.SetVolume(static_cast<float>(value) / 100.0F);
            settings_.player.volume = value;
        }
        return 0;
    case kMsgShowOptionsControl: {
        // FUN_00464900 opens the requested page and then asks the sheet to
        // reveal/focus either a control or one of its nested page templates.
        ShowOptions(static_cast<int>(lparam), static_cast<UINT>(wparam));
        return 0;
    }
    case kMsgApplyOptions:
        ApplyOptionsChangeMask(static_cast<UINT>(wparam), lparam);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == THBN_CLICKED) {
            HandleTaskbarPlaybackClick(wparam);
            return 0;
        }
        switch (LOWORD(wparam)) {
        case kOpen: ChooseFiles(); return 0;
        case kPrevious: SelectRelative(false); return 0;
        case kPlayPause:
            if (audio_.State() == audio::PlaybackState::playing) audio_.Pause();
            else if (audio_.State() == audio::PlaybackState::paused) audio_.Resume();
            else PlayCurrent();
            RefreshPlaybackUi();
            return 0;
        case kNext: SelectRelative(true); return 0;
        case kStop: Stop(); return 0;
        case kCmdShowElapsedTime:
            settings_.player.show_elapsed_time =
                !settings_.player.show_elapsed_time;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case kCmdOptions: {
            // FUN_004658A3 accepts a private page index in lParam for links
            // such as Lyric Search -> proxy settings; ordinary menus pass 0
            // and restore Histroy/LastActivePage.
            const int requested = static_cast<int>(lparam);
            LeaveFullScreen();
            // 004658A3 accepts the private range 1..15.  The sheet itself
            // rejects index 15 against its 15-page count and remains on 0.
            ShowOptions(requested > 0 && requested < 16 ? requested : -1);
            return 0;
        }
        case kPlaylist:
            if (HIWORD(wparam) == LBN_SELCHANGE) {
                const LRESULT selected = SendMessageW(
                    playlist_view_, LB_GETCURSEL, 0, 0);
                if (selected != LB_ERR)
                    SelectPlaylistRow(static_cast<size_t>(selected));
            } else if (HIWORD(wparam) == LBN_DBLCLK) {
                const LRESULT selected = SendMessageW(playlist_view_, LB_GETCURSEL, 0, 0);
                if (selected != LB_ERR)
                    SelectPlaylistRow(static_cast<size_t>(selected), false,
                        false, PlaylistSelectionTrigger::item_activated);
            }
            return 0;
        case kVisualControlId:
            // FUN_00457FAA sends control 0x7DDC with notification 1 only
            // when the Type-4 missing-cover text was hit. Every other
            // WM_LBUTTONUP advances the five-state visual cycle.
            if (HIWORD(wparam) == 1) {
                // FUN_00465057 forwards 0x17D67 through the main command
                // router. Its ordinary-focus branch exits full screen and
                // sends playlist command 0x7EF6, which acts on the selected
                // row rather than unconditionally on the playing track.
                LeaveFullScreen();
                HandlePlaylistCommand(kPlaylistProperties);
            } else {
                SetVisualType(fullscreen_mode_ != 0
                    ? (settings_.visual.type >= (fullscreen_mode_ == 3 ? 4 : 3)
                        ? 1 : settings_.visual.type + 1)
                    : (settings_.visual.type + 1) % 5);
            }
            return 0;
        default:
            // CPlayerWnd's original WM_COMMAND table owns the 0x7E/0x7F
            // playlist range even when the command originated in the owned
            // playlist window.  Keep that route available in addition to the
            // custom popup's TPM_RETURNCMD path.
            if (HandlePlaylistCommand(LOWORD(wparam))) return 0;
            if (HandleLyricCommand(LOWORD(wparam))) return 0;
            if (HandleContextCommand(LOWORD(wparam))) return 0;
            break;
        }
        break;
    case WM_COPYDATA: {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (data && data->lpData && data->cbData >= sizeof(wchar_t)) {
            const size_t count = data->cbData / sizeof(wchar_t);
            const auto* value = static_cast<const wchar_t*>(data->lpData);
            size_t length = 0;
            while (length < count && value[length] != L'\0') ++length;
            if (length != 0) {
                OpenCommandLinePath(
                    std::filesystem::path(std::wstring(value, length)),
                    data->dwData);
            }
        }
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        return TRUE;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
        std::vector<std::filesystem::path> paths;
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            path.resize(length);
            paths.emplace_back(std::move(path));
        }
        DragFinish(drop);
        static_cast<void>(ImportFiles(paths, playlists_.ActiveIndex(), 0,
            true, ImportPlayback::force));
        return 0;
    }
    case WM_TIMER:
        if (auto_shutdown_timer_ && wparam == auto_shutdown_timer_) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            if (IsAutoShutdownDue(now, settings_.general.shutdown_time)) {
                KillTimer(window_, auto_shutdown_timer_);
                auto_shutdown_timer_ = 0;
                ShowAutoShutdownDialog();
            }
        } else if (wparam == kSkinControlAnimationTimer) {
            AdvanceSkinControlAnimations();
        } else if (wparam == kSkinWindowFadeTimer) {
            AdvanceSkinWindowFade();
        } else if (wparam == kCloseAudioFadeTimer) {
            PollCloseAudioFade();
        } else if (wparam == kSkinMenuToolTipTimer) {
            ShowQueuedSkinMenuToolTip();
        } else if (wparam == kInfoItemTimer || wparam == kInfoTransitionTimer ||
            wparam == kInfoScrollTimer) {
            AdvanceSkinInfoScroll(static_cast<UINT_PTR>(wparam));
        } else if (wparam == kUiTimer) {
            // StopWithFade publishes the logical stopped state before its
            // physical volume ramp completes. Do not mistake that close-only
            // transition for natural EOF and start another playlist item.
            if (close_after_skin_window_fade_) {
                PollCloseAudioFade();
                return 0;
            }
            // A skin installed while the catalogue worker was still running
            // invalidates that worker's snapshot.  Poll it from the existing
            // 250 ms UI timer and launch the replacement only after the old
            // task has completed; neither operation may wait in the window
            // procedure.
            if (skin_catalog_result_stale_) StartSkinMenuCatalogLoad();
            PollMediaLibraryWorkers();
            playlists_.FlushDirty(false);
            if (pending_natural_play_ &&
                GetTickCount64() >= pending_natural_play_tick_) {
                pending_natural_play_ = false;
                static_cast<void>(PlayCurrent(false));
                return 0;
            }
            const auto state = audio_.State();
            if (playback_was_active_ && state == audio::PlaybackState::stopped) {
                playback_was_active_ = false;
                playback_source_open_ = false;
                // FUN_0045B69B invokes FUN_00457BCF synchronously on natural
                // completion, before it attempts to select a following item.
                UpdateVisualFrame();
                natural_completion_dispatch_ =
                    settings_.playback.track_interval > 0;
                AdvanceAfterNaturalEnd();
                natural_completion_dispatch_ = false;
                return 0;
            }
            if (state == audio::PlaybackState::failed) {
                playback_was_active_ = false;
                playback_source_open_ = false;
                opened_track_.reset();
                // 0047FEA3 calls 0047FB0C after the player's 0x7EB request
                // irrespective of its result.  The player releases its
                // failed source, but CPlayList +0x1c remains the requested
                // row and continues to gate later if-idle imports.
                ClearPersistedPlaybackIdentity();
                if (pending_failed_advance_) {
                    pending_failed_advance_ = false;
                    AdvanceAfterNaturalEnd();
                    return 0;
                }
            }
            playback_was_active_ = state == audio::PlaybackState::opening ||
                                   state == audio::PlaybackState::playing ||
                                   state == audio::PlaybackState::paused;
            RefreshPlaybackUi();
            RotateMainWindowCaption();
        }
        return 0;
    case WM_CLOSE:
        if (close_after_skin_window_fade_) return 0;
        CompleteSkinWindowFadeForReplacement();
        if (!window_ || !IsWindow(window_)) return 0;
        LeaveFullScreen();
        if (auto_shutdown_timer_) {
            KillTimer(window_, auto_shutdown_timer_);
            auto_shutdown_timer_ = 0;
        }
        if (settings_.desktop_lyric.unlock_when_close &&
            settings_.desktop_lyric.lock) {
            settings_.desktop_lyric.lock = false;
            desktop_lyrics_.ApplySettings();
        }
        // 004616BD asks the modeless sheet to execute its IDOK transaction
        // before the main window persists TTPlayerRebuild.xml. A raw DestroyWindow
        // loses edits still resident in the active/nested page controls.
        if (options_window_ && IsWindow(options_window_))
            SendMessageW(options_window_, WM_COMMAND, IDOK, 0);
        CloseOptions();
        SaveStoredPlaylist();
        PersistWindowState();
        // FUN_0045B78E starts CSoundFadeOut only when SoundFadeMode bit 3 is
        // enabled for an actively playing wave/DirectSound output.  Original
        // OnDestroy (00461ADB) keeps dispatching messages until that object
        // completes (bounded by FadeDuration[3] + 500 ms). Run the sound and
        // window fades concurrently, but do not destroy the receiver until
        // both have reached their terminal state.
        mini_mode_fade_pending_ = false;
        mini_mode_fade_continuation_ = false;
        mini_mode_fade_queued_ = false;
        close_after_skin_window_fade_ = true;
        UpdateTaskbarPlayback();
        close_skin_window_fade_finished_ = false;
        for (const HWND target : {window_, lyric_window_, playlist_window_,
                                  equalizer_window_}) {
            if (target && IsWindow(target)) EnableWindow(target, FALSE);
        }
        close_waiting_for_audio_fade_ = audio_.BeginStopFade();
        if (close_waiting_for_audio_fade_) {
            close_audio_fade_deadline_ = GetTickCount64() +
                audio::RecoveredStopFadeCloseBudgetMs(
                    settings_.playback.fade_duration[3]);
            // kUiTimer is a fallback poll if SetTimer cannot allocate a new
            // identifier, so failure never turns close into a blocking wait.
            static_cast<void>(SetTimer(
                window_, kCloseAudioFadeTimer,
                kCloseAudioFadeIntervalMs, nullptr));
        }

        // 004616BD applies the same 100-percent sentinel before destruction,
        // but deliberately skips it for an iconic main window.
        if (skin_ && !IsIconic(window_)) {
            BeginSkinWindowFade(window_, rendered_skin_window_alpha_, 0, true,
                                kFadeCompleteDestroyMain);
        } else {
            close_skin_window_fade_finished_ = true;
            FinishCloseWhenFadesComplete();
        }
        return 0;
    case WM_DESTROY:
        ClosePlaylistConverter(window_);
        taskbar_playback_.Reset();
        KillTimer(window_, kCloseAudioFadeTimer);
        close_waiting_for_audio_fade_ = false;
        close_skin_window_fade_finished_ = false;
        CancelSkinWindowFade();
        // Revoke the private-reader completion receiver and drain records
        // already queued to this HWND.  A malformed add-in may never return;
        // its independently retained snapshot must not hold the UI thread in
        // an unbounded join during WM_DESTROY.
        ShutdownPlaylistInfoLoading();
        ShutdownMediaLibrary();
        desktop_lyrics_.CaptureBounds();
        desktop_lyrics_.Destroy();
        desktop_lyric_mode_ = false;
        CloseOptions();
        UnregisterConfiguredHotKeys();
        RemoveTrayIcon();
        RevokeFileDropTarget(window_);
        StopVisualWorker();
        KillTimer(window_, kUiTimer);
        KillTimer(window_, kInfoItemTimer);
        KillTimer(window_, kInfoTransitionTimer);
        KillTimer(window_, kInfoScrollTimer);
        SaveStoredPlaylist();
        PersistWindowState();
        // Keep the source/progress object alive through the 004616BD-style
        // state capture even when destruction did not originate at WM_CLOSE.
        audio_.Stop();
        tooltip_tools_.clear();
        tooltip_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void PlayerWindow::CreateControls() {
    if (skin_) return;
    ui_font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    title_font_ = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    background_brush_ = CreateSolidBrush(kBackground);
    panel_brush_ = CreateSolidBrush(kPanel);

    title_ = CreateWindowExW(0, L"STATIC", display_title_.c_str(), WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    const auto local_media = ResourceText(0x8ca5);
    artist_ = CreateWindowExW(0, L"STATIC", local_media.c_str(), WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    const auto stopped = ResourceText(0x81bb);
    status_ = CreateWindowExW(0, L"STATIC", stopped.c_str(), WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    playlist_view_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kPlaylist), instance_, nullptr);
    progress_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH | PBS_MARQUEE,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kProgress), instance_, nullptr);
    volume_ = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kVolume), instance_, nullptr);
    SendMessageW(volume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(volume_, TBM_SETPOS, TRUE, settings_.player.volume);
    SendMessageW(progress_, PBM_SETBARCOLOR, 0, kAccent);
    SendMessageW(progress_, PBM_SETBKCOLOR, 0, kPanel);

    const struct Button { int id; UINT text; } buttons[] = {
        {kOpen, kCmdOpenFile}, {kPrevious, kCmdPrevious}, {kPlayPause, kCmdPlay},
        {kNext, kCmdNext}, {kStop, kCmdStopPlayback}
    };
    for (const auto& button : buttons) {
        const auto label = ResourceCommandLabel(ResourceModule(), button.text);
        const HWND handle = CreateWindowExW(0, L"BUTTON", label.c_str(),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)), instance_, nullptr);
        SetControlFont(handle, ui_font_);
    }
    SetControlFont(title_, title_font_);
    SetControlFont(artist_, ui_font_);
    SetControlFont(status_, ui_font_);
    SetControlFont(playlist_view_, ui_font_);
}

void PlayerWindow::CaptureWindowState() {
    // FUN_004616BD tests IsIconic before invoking FUN_004684D6, and
    // FUN_004684D6 repeats the same guard around the complete four-window
    // snapshot.  In particular, do not persist the minimized popup's iconic
    // rectangle or visibility changes caused only by owner minimization.
    if (window_ && !ShouldCaptureWindowGeometry(IsIconic(window_) != FALSE))
        return;

    RECT current{};
    if (window_ && GetWindowRect(window_, &current)) {
        if (mini_mode_) {
            mini_window_bounds_ = current;
            have_mini_window_bounds_ = true;
        } else {
            normal_window_bounds_ = current;
            have_normal_window_bounds_ = true;
        }
    }
    const bool desktop_lyric_visible =
        desktop_lyric_mode_ && desktop_lyrics_.Visible();
    CaptureActiveLyricWindowState();
    if (desktop_lyric_mode_) ActiveLyricVisible() = desktop_lyric_visible;
    desktop_lyrics_.CaptureBounds();
    RECT playlist_bounds = settings_.player.playlist_window;
    bool playlist_visible = settings_.player.playlist_visible;
    if (playlist_window_) {
        GetWindowRect(playlist_window_, &playlist_bounds);
        if (!mini_mode_)
            playlist_visible = IsWindowVisible(playlist_window_) != FALSE;
    }
    RECT equalizer_bounds = settings_.player.equalizer_window;
    bool equalizer_visible = settings_.player.equalizer_visible;
    if (equalizer_window_) {
        GetWindowRect(equalizer_window_, &equalizer_bounds);
        if (!mini_mode_)
            equalizer_visible = IsWindowVisible(equalizer_window_) != FALSE;
    }
    settings_.player.player_window = normal_window_bounds_;
    settings_.player.mini_player_window = mini_window_bounds_;
    settings_.player.mini_mode = mini_mode_;
    settings_.player.playlist_window = playlist_bounds;
    settings_.player.playlist_visible = playlist_visible;
    settings_.player.equalizer_window = equalizer_bounds;
    settings_.player.equalizer_visible = equalizer_visible;
}

std::filesystem::path PlayerWindow::CurrentSkinProfilePath() const {
    std::filesystem::path global_settings = settings_.source_path;
    if (global_settings.empty()) {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                                static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) return {};
        executable.resize(length);
        global_settings = std::filesystem::path(executable).parent_path() /
                          settings::kSettingsFileName;
    }
    return ResolveSkinProfilePath(global_settings, settings_.skin_file);
}

void PlayerWindow::SaveCurrentSkinProfile() {
    if (!skin_ || !skin_->Valid()) return;
    CaptureWindowState();
    const auto profile = CurrentSkinProfilePath();
    if (profile.empty()) return;
    // FUN_0045D5FA captures the outgoing package's windows and invokes the
    // common serializer with DAT_00547744 set before loading the replacement.
    static_cast<void>(settings::SaveSkinVisualProfile(
        profile, settings_.player, settings_.playlist, settings_.lyric,
        settings_.visual, settings_.source_path));
}

void PlayerWindow::PersistWindowState() {
    if (window_state_saved_) return;
    window_state_saved_ = true;
    CaptureWindowState();
    settings_.player.mini_mode = mini_mode_;
    settings_.player.playlist_scan_count = static_cast<int>(playlists_.Size());
    settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
    // The original keeps Player/PlayingFileName after an explicit Stop (and
    // after natural end); it is the playlist's last-playing identity, not a
    // synonym for "decoder currently open". Close-file, list replacement,
    // and failed-open paths clear it at the point of invalidation; clearing a
    // list or deleting its playing row leaves the opened item detached.
    // 004616BD samples the player/progress position unconditionally.  This is
    // also observable after natural completion, when no decoder is open but
    // the last-playing identity and terminal position are retained.
    const auto position = audio_.Position().count();
    settings_.player.playing_time = position < 0 ? 0 :
        (position > INT_MAX ? INT_MAX : static_cast<int>(position));

    // Startup applies the active package sidecar after TTPlayerRebuild.xml. Writing
    // only the root document made that stale sidecar replace the just-saved
    // PlayerWnd/LyricWnd/PlayListWnd/EqualizerWnd rectangles and visibility
    // flags on the next launch.  FUN_0045D5FA uses this same per-skin branch
    // when leaving a package; commit the active package snapshot at shutdown
    // as well, before the root state that owns mini visibility/top-most data.
    if (skin_ && skin_->Valid()) {
        const auto profile = CurrentSkinProfilePath();
        if (!profile.empty()) {
            static_cast<void>(settings::SaveSkinVisualProfile(
                profile, settings_.player, settings_.playlist,
                settings_.lyric, settings_.visual, settings_.source_path));
        }
    }
    settings::SaveWindowState(settings_.source_path, settings_);
}

void PlayerWindow::LayoutControls(int width, int height) const {
    if (skin_) return;
    constexpr int margin = 24;
    constexpr int gap = 10;
    const int playlist_width = std::clamp(width / 3, 230, 330);
    const int left_width = width - playlist_width - margin * 3;
    const int playlist_x = width - playlist_width - margin;
    MoveWindow(title_, margin, 44, left_width, 38, TRUE);
    MoveWindow(artist_, margin, 86, left_width, 28, TRUE);
    MoveWindow(progress_, margin, 142, left_width, 10, TRUE);
    MoveWindow(status_, margin, 169, left_width, 26, TRUE);
    MoveWindow(playlist_view_, playlist_x, 40, playlist_width, std::max(120, height - 80), TRUE);
    MoveWindow(volume_, margin, height - 139, std::min(left_width, 360), 28, TRUE);
    int x = margin;
    for (const auto [id, button_width] : {std::pair{kOpen, 76}, {kPrevious, 76},
            {kPlayPause, 104}, {kNext, 76}, {kStop, 66}}) {
        MoveWindow(GetDlgItem(window_, id), x, height - 92, button_width, 42, TRUE);
        x += button_width + gap;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::Paint(HDC dc) const {
    if (skin_) {
        PaintSkin(dc);
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);
    const int playlist_width = std::clamp(static_cast<int>(client.right) / 3, 230, 330);
    const HPEN pen = CreatePen(PS_SOLID, 1, RGB(51, 60, 70));
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, 24, 125, nullptr);
    LineTo(dc, client.right - playlist_width - 48, 125);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kMutedText);
    SelectObject(dc, ui_font_);
    const auto volume_label = ResourceCommandLabel(ResourceModule(), 0x7ddb);
    const auto playlist_label = ResourceCommandLabel(ResourceModule(), kCmdShowPlaylist);
    TextOutW(dc, 24, std::max(0L, client.bottom - 166), volume_label.c_str(),
             static_cast<int>(volume_label.size()));
    TextOutW(dc, client.right - playlist_width - 24, 16, playlist_label.c_str(),
             static_cast<int>(playlist_label.size()));
}

void PlayerWindow::PaintSkin(HDC dc) const {
    if (!skin_) return;
    const SIZE size = ActiveSkinSize();
    const HDC canvas = CreateCompatibleDC(dc);
    const HBITMAP buffer = CreateCompatibleBitmap(dc, size.cx, size.cy);
    const HGDIOBJ old_buffer = SelectObject(canvas, buffer);
    const RECT background_bounds{0, 0, size.cx, size.cy};
    FillRect(canvas, &background_bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    ActiveSkinBackground().Draw(canvas, 0, 0, size.cx, size.cy, 0, 0, size.cx, size.cy);

    const auto playback = audio_.State();
    const bool active = playback == audio::PlaybackState::opening ||
                        playback == audio::PlaybackState::playing;
    for (const auto& element : ActiveSkinElements()) {
        if (!element.image || !element.four_state) continue;
        if (IsSuppressedSkinControl(element.name)) continue;
        if (!IsPlayModeSkinVisible(element.name, settings_.player.play_mode)) continue;
        if (element.name == L"play" && active) continue;
        if (element.name == L"pause" && !active) continue;
        skin::SkinElement displayed = element;
        if (element.name == L"pause") {
            // The player owns one play/pause HWND (command 0x7d00).  The
            // pause bitmap replaces the play bitmap on that same HWND, so a
            // distinct <pause position> is not used by 5.7.9.
            if (const auto* play = FindActiveSkinElement(L"play")) {
                const int width = element.image_size.cx / 4;
                displayed.bounds = {play->bounds.left, play->bounds.top,
                    play->bounds.left + width,
                    play->bounds.top + element.image_size.cy};
            }
        }
        // FUN_0045A04E asks the three auxiliary HWNDs for their real visible
        // state and feeds that state to SkinButton through FUN_004455D6.
        // SkinButton::OnPaint (FUN_0040A35B) uses frame 2 while checked,
        // except that a live hover continues to use frame 1. Treat mute the
        // same way: it is another persistent SkinButton check state.
        int state = IsSkinElementEnabled(element.name)
            ? (IsSkinElementChecked(element.name) ? 2 : 0) : 3;
        if (state != 3 && pressed_skin_element_ == element.name) state = 2;
        else if (state != 3 && hover_skin_element_ == element.name) state = 1;
        DrawSkinElement(canvas, displayed, state);
    }

    if (const auto* progress = FindActiveSkinElement(L"progress"); progress && progress->thumb_image) {
        const int control_width = progress->bounds.right - progress->bounds.left;
        const int control_height = progress->bounds.bottom - progress->bounds.top;
        const auto duration = audio_.Duration().count();
        const auto position = std::clamp<int64_t>(
            progress_tracking_position_.value_or(audio_.Position()).count(), 0,
            std::max<int64_t>(0, duration));
        const int safe_duration = static_cast<int>(
            std::clamp<int64_t>(duration, 1, INT_MAX));
        const int safe_position = static_cast<int>(
            std::clamp<int64_t>(position, 0, safe_duration));
        const int width = std::max(1L, progress->thumb_size.cx / 4);
        const int height = progress->thumb_size.cy;
        // FUN_00451CEF initializes the shared legacy slider edge inset to one
        // pixel; FUN_00428F41 excludes it at both value-range endpoints.
        constexpr int slider_inset = 1;
        const int horizontal_span = std::max(
            0, control_width - slider_inset * 2 - width);
        const int vertical_span = std::max(
            0, control_height - slider_inset * 2 - height);
        const int logical_thumb_left = progress->bounds.left + slider_inset +
            (duration > 0 ? MulDiv(safe_position, horizontal_span,
                                   safe_duration) : 0);
        const int logical_thumb_top = progress->bounds.top + slider_inset +
            (duration > 0 ? MulDiv(safe_duration - safe_position,
                                   vertical_span, safe_duration)
                          : vertical_span);
        if (progress->bar_image && progress->bar_size.cx > 0 && progress->bar_size.cy > 0) {
            const int x = progress->bounds.left + (control_width - progress->bar_size.cx) / 2;
            const int y = progress->bounds.top + (control_height - progress->bar_size.cy) / 2;
            progress->bar_image.Draw(canvas, x, y, progress->bar_size.cx,
                progress->bar_size.cy, 0, 0, progress->bar_size.cx, progress->bar_size.cy);
        }
        if (duration > 0 && progress->fill_image &&
            progress->fill_size.cx > 0 && progress->fill_size.cy > 0) {
            const int saved_dc = SaveDC(canvas);
            if (saved_dc != 0) {
                IntersectClipRect(canvas, progress->bounds.left, progress->bounds.top,
                                  progress->bounds.right, progress->bounds.bottom);
            }
            const int fill_left = progress->bounds.left +
                (control_width - progress->fill_size.cx) / 2;
            const int fill_top = progress->bounds.top +
                (control_height - progress->fill_size.cy) / 2;
            if (progress->vertical) {
                // FUN_00451E07 derives the fill boundary from the already
                // rounded thumb rectangle, never from a second value ratio.
                const int fill_start = logical_thumb_top + height / 2;
                const int source_y = std::clamp(fill_start - fill_top, 0,
                                                static_cast<int>(progress->fill_size.cy));
                const int filled = progress->fill_size.cy - source_y;
                if (filled > 0) {
                    progress->fill_image.Draw(canvas, fill_left, fill_start,
                        progress->fill_size.cx, filled, 0, source_y,
                        progress->fill_size.cx, filled, skin_->TransparentColor());
                }
            } else {
                const int fill_end = logical_thumb_left + width / 2;
                const int filled = std::clamp(fill_end - fill_left, 0,
                                              static_cast<int>(progress->fill_size.cx));
                if (filled > 0) {
                    progress->fill_image.Draw(canvas, fill_left, fill_top, filled,
                        progress->fill_size.cy, 0, 0, filled,
                        progress->fill_size.cy, skin_->TransparentColor());
                }
            }
            if (saved_dc != 0) RestoreDC(canvas, saved_dc);
        }
        skin::SkinElement thumb = *progress;
        thumb.image = progress->thumb_image;
        thumb.image_size = progress->thumb_size;
        thumb.frames = 4;
        thumb.four_state = true;
        thumb.bounds.left = progress->vertical
            ? progress->bounds.left + (control_width - width) / 2
            : logical_thumb_left;
        thumb.bounds.top = progress->vertical
            ? logical_thumb_top
            : progress->bounds.top + (control_height - height) / 2;
        thumb.bounds.right = thumb.bounds.left + width;
        thumb.bounds.bottom = thumb.bounds.top + height;
        // The slider has separate disabled-fill handling; its thumb remains
        // frame 0 when seeking is unavailable (observable in ElegantLife).
        const int state = IsSkinElementEnabled(L"progress") &&
            pressed_skin_element_ == L"progress" ? 2 :
            (IsSkinElementEnabled(L"progress") && hover_skin_element_ == L"progress" ? 1 : 0);
        DrawSkinSliderThumb(canvas, thumb, state,
            playback == audio::PlaybackState::playing && duration > 0, progress->bounds);
    }
    if (const auto* volume = FindActiveSkinElement(L"volume")) {
        const int control_width = volume->bounds.right - volume->bounds.left;
        const int control_height = volume->bounds.bottom - volume->bounds.top;
        const int value = std::clamp(settings_.player.volume, 0, 100);
        const int thumb_width = volume->thumb_image && volume->thumb_size.cx > 0
            ? volume->thumb_size.cx / 4 : 0;
        const int thumb_height = volume->thumb_image ? volume->thumb_size.cy : 0;
        // FUN_00451CEF initializes the slider's edge inset to one pixel, and
        // FUN_00428F41 uses that inset on both ends of the value range.
        constexpr int slider_inset = 1;
        const int horizontal_thumb_extent = std::max(1, thumb_width);
        const int vertical_thumb_extent = std::max(1, thumb_height);
        const int horizontal_span = std::max(
            0, control_width - slider_inset * 2 - horizontal_thumb_extent);
        const int vertical_span = std::max(
            0, control_height - slider_inset * 2 - vertical_thumb_extent);
        const int logical_thumb_left = volume->bounds.left + slider_inset +
            MulDiv(horizontal_span, value, 100);
        const int logical_thumb_top = volume->bounds.top + slider_inset +
            MulDiv(vertical_span, 100 - value, 100);
        // FUN_00451E07 paints the slider background before the filled portion.
        // Volume used to skip this layer entirely, leaving only fill + thumb.
        if (volume->bar_image && volume->bar_size.cx > 0 && volume->bar_size.cy > 0) {
            const int saved = SaveDC(canvas);
            if (saved) {
                IntersectClipRect(canvas, volume->bounds.left, volume->bounds.top,
                                  volume->bounds.right, volume->bounds.bottom);
                const int x = volume->bounds.left + (control_width - volume->bar_size.cx) / 2;
                const int y = volume->bounds.top + (control_height - volume->bar_size.cy) / 2;
                volume->bar_image.Draw(canvas, x, y, volume->bar_size.cx, volume->bar_size.cy,
                    0, 0, volume->bar_size.cx, volume->bar_size.cy, skin_->TransparentColor());
                RestoreDC(canvas, saved);
            }
        }
        if (volume->fill_image && volume->fill_size.cx > 0 &&
            volume->fill_size.cy > 0 && control_width > 0 && control_height > 0) {
            const int saved_dc = SaveDC(canvas);
            if (saved_dc != 0) {
                IntersectClipRect(canvas, volume->bounds.left, volume->bounds.top,
                                  volume->bounds.right, volume->bounds.bottom);
            }
            if (volume->vertical) {
                const int fill_left = volume->bounds.left +
                    (control_width - volume->fill_size.cx) / 2;
                const int fill_top = volume->bounds.top +
                    (control_height - volume->fill_size.cy) / 2;
                const int fill_start = logical_thumb_top + vertical_thumb_extent / 2;
                const int source_y = std::clamp(fill_start - fill_top, 0,
                                                static_cast<int>(volume->fill_size.cy));
                const int filled = volume->fill_size.cy - source_y;
                if (filled > 0) {
                    volume->fill_image.Draw(canvas, fill_left, fill_start,
                        volume->fill_size.cx, filled, 0, source_y,
                        volume->fill_size.cx, filled, skin_->TransparentColor());
                }
            } else {
                const int fill_left = volume->bounds.left +
                    (control_width - volume->fill_size.cx) / 2;
                const int y = volume->bounds.top +
                    (control_height - volume->fill_size.cy) / 2;
                const int fill_end = logical_thumb_left + horizontal_thumb_extent / 2;
                const int filled = std::clamp(fill_end - fill_left, 0,
                                              static_cast<int>(volume->fill_size.cx));
                if (filled > 0) {
                    // FUN_00451E07 dispatches fill_image through FUN_00450A1C
                    // when the slider color key is not 0xffffffff.  The latter
                    // is TTPlayer's TransparentBlt wrapper; using SRCCOPY here
                    // exposed the legacy #ff00ff mask as a purple rectangle.
                    volume->fill_image.Draw(canvas, fill_left, y, filled, volume->fill_size.cy,
                        0, 0, filled, volume->fill_size.cy, skin_->TransparentColor());
                }
            }
            if (saved_dc != 0) RestoreDC(canvas, saved_dc);
        }
        if (volume->thumb_image && thumb_width > 0 && thumb_height > 0) {
            skin::SkinElement thumb = *volume;
            thumb.image = volume->thumb_image;
            thumb.image_size = volume->thumb_size;
            thumb.frames = 4;
            thumb.four_state = true;
            const int x = volume->vertical
                ? volume->bounds.left + (control_width - thumb_width) / 2
                : logical_thumb_left;
            const int y = volume->vertical
                ? logical_thumb_top
                : volume->bounds.top + (control_height - thumb_height) / 2;
            thumb.bounds = {x, y, x + thumb_width, y + thumb_height};
            const int state = pressed_skin_element_ == L"volume" ? 2 :
                (hover_skin_element_ == L"volume" ? 1 : 0);
            DrawSkinElement(canvas, thumb, state);
        }
    }

    // The scrolling info object has its own FUN_00409250 setup path and is
    // composed with the parent surface, unlike the two ordinary status
    // statics below.
    if (const auto* info = FindActiveSkinElement(L"info")) {
        const wchar_t* current = display_title_.c_str();
        const wchar_t* next = nullptr;
        if (!info_items_.empty() && info_item_index_ < info_items_.size()) {
            current = info_items_[info_item_index_].c_str();
            next = info_items_[(info_item_index_ + 1) % info_items_.size()].c_str();
        }
        DrawScrollingSkinInfo(canvas, *info, current, next,
                              info_scroll_offset_, info_vertical_offset_);
    }

    if (const auto* led = FindActiveSkinElement(L"led"); led && led->image) {
        DrawSkinLed(canvas, *led, CurrentLedText(), skin_->TransparentColor());
    }

    const HICON skin_icon = settings_.general.app_icon_file.empty() &&
                                    skin_->Icon()
        ? skin_->Icon() : window_icon_small_;
    if (skin_icon) {
        if (const auto* icon = FindActiveSkinElement(L"icon")) {
            const int width = GetSystemMetrics(SM_CXSMICON);
            const int height = GetSystemMetrics(SM_CYSMICON);
            DrawIconEx(canvas, icon->bounds.left, icon->bounds.top, skin_icon,
                width, height, 0, nullptr, DI_NORMAL);
        }
    }

    // VisualCtrl owns and presents this rectangle independently.  The
    // reconstructed 250 ms player refresh used to blit the complete parent
    // buffer across it, temporarily replacing the cover with the raw skin
    // background until CVisualCtrl's worker painted again.  Preserve the
    // child-window z-order of the original skin host by excluding the live
    // embedded visual from the parent's final screen transfer.
    const int saved_output = SaveDC(dc);
    if (visual_window_ && !fullscreen_visual_detached_ &&
        GetParent(visual_window_) == window_ &&
        IsWindowVisible(visual_window_)) {
        RECT visual_bounds{};
        GetWindowRect(visual_window_, &visual_bounds);
        MapWindowPoints(HWND_DESKTOP, window_,
                        reinterpret_cast<POINT*>(&visual_bounds), 2);
        ExcludeClipRect(dc, visual_bounds.left, visual_bounds.top,
                       visual_bounds.right, visual_bounds.bottom);
    }
    BitBlt(dc, 0, 0, size.cx, size.cy, canvas, 0, 0, SRCCOPY);

    // FUN_00408FC9/FUN_00409023 paint the stereo/status child statics after
    // copying or filling their parent background. Keeping this final screen-DC
    // pass preserves GDI's exact antialias rounding on colored backgrounds.
    if (const auto* stereo = FindActiveSkinElement(L"stereo")) {
        const auto text = ChannelText();
        DrawSkinText(dc, *stereo, text.c_str());
    }
    if (const auto* status = FindActiveSkinElement(L"status")) {
        const auto text = PlaybackStatusText();
        DrawSkinText(dc, *status, text.c_str());
    }
    if (saved_output != 0) RestoreDC(dc, saved_output);
    SelectObject(canvas, old_buffer);
    DeleteObject(buffer);
    DeleteDC(canvas);
}

const playlist::Track* PlayerWindow::PlaybackTrackForUi() const noexcept {
    return ResolvePlaybackTrackForUi(
        PlaybackPlaylist(), current_, opened_track_);
}

bool PlayerWindow::IsSkinElementEnabled(std::wstring_view name) const {
    if (IsSuppressedSkinControl(name)) return false;
    const auto state = audio_.State();
    const bool have_track = PlaybackTrackForUi() != nullptr;
    const bool active = state == audio::PlaybackState::opening ||
                        state == audio::PlaybackState::playing;
    if (name == L"play") return have_track && !active;
    if (name == L"pause") return state == audio::PlaybackState::playing;
    if (name == L"stop") return true;
    if (name == L"progress") return audio_.Duration().count() > 0 &&
                                      (state == audio::PlaybackState::playing ||
                                       state == audio::PlaybackState::paused);
    if (name == L"prev" || name == L"next") {
        const auto count = PlaybackPlaylist().Tracks().size();
        if (count == 0) return false;
        if (settings_.player.play_mode == 3 || settings_.player.play_mode == 4) return true;
        if (!current_) return false;
        return name == L"prev" ? *current_ > 0 : *current_ + 1 < count;
    }
    return true;
}

bool PlayerWindow::IsSkinElementChecked(std::wstring_view name) const noexcept {
    if (name == L"mute") return settings_.player.mute;
    if (name == L"lyric")
        return desktop_lyrics_.Visible() ||
            (lyric_window_ && IsWindowVisible(lyric_window_) != FALSE);
    if (name == L"equalizer")
        return equalizer_window_ && IsWindowVisible(equalizer_window_) != FALSE;
    if (name == L"playlist")
        return playlist_window_ && IsWindowVisible(playlist_window_) != FALSE;
    return false;
}

std::wstring PlayerWindow::PlaybackStatusText() const {
    if (!equalizer_tracking_status_.empty())
        return equalizer_tracking_status_;
    switch (audio_.State()) {
    case audio::PlaybackState::playing: return ResourceText(0x81b9);
    case audio::PlaybackState::paused: return ResourceText(0x81ba);
    case audio::PlaybackState::failed: return ResourceText(0x8285);
    case audio::PlaybackState::stopped:
        return PlaybackTrackForUi() ? ResourceText(0x81bb) : std::wstring{};
    case audio::PlaybackState::opening: return L"";
    }
    return {};
}

std::wstring PlayerWindow::ChannelText() const {
    const auto format = audio_.Format();
    if (!PlaybackTrackForUi() || format.channels == 0)
        return ResourceText(0x81b7);
    if (settings_.player.mute) return ResourceText(0x81b8);
    if (format.channels == 1) return ResourceListItem(ResourceModule(), 0x8154, 0);
    if (format.channels == 2) return ResourceListItem(ResourceModule(), 0x8154, 1);
    return ResourceListItem(ResourceModule(), 0x8154, 8);
}

void PlayerWindow::DrawSkinElement(HDC dc, const skin::SkinElement& element, int state) const {
    if (!skin_ || !element.image) return;
    const int width = element.frames > 1 ? element.image_size.cx / element.frames
                                         : element.image_size.cx;
    const int height = element.image_size.cy;
    if (width <= 0 || height <= 0) return;
    const RECT bounds{element.bounds.left, element.bounds.top,
        element.bounds.left + width, element.bounds.top + height};
    if (element.name == L"progress" || element.name == L"volume")
        DrawElementFrame(dc, element, bounds, state, skin_->TransparentColor());
    else DrawAnimatedSkinFrame(dc, element, bounds, state, window_);
}

skin::SkinImage PlayerWindow::ActiveSkinBackground() const noexcept {
    if (!skin_) return nullptr;
    return mini_mode_ && skin_->SupportsMiniMode()
        ? skin_->MiniBackground() : skin_->Background();
}

SIZE PlayerWindow::ActiveSkinSize() const noexcept {
    if (!skin_) return {};
    return mini_mode_ && skin_->SupportsMiniMode()
        ? skin_->MiniWindowSize() : skin_->WindowSize();
}

const std::vector<skin::SkinElement>& PlayerWindow::ActiveSkinElements() const noexcept {
    static const std::vector<skin::SkinElement> empty;
    if (!skin_) return empty;
    return mini_mode_ && skin_->SupportsMiniMode()
        ? skin_->MiniElements() : skin_->Elements();
}

const skin::SkinElement* PlayerWindow::FindActiveSkinElement(std::wstring_view name) const {
    if (!skin_) return nullptr;
    return mini_mode_ && skin_->SupportsMiniMode()
        ? skin_->FindMini(name) : skin_->Find(name);
}

std::wstring PlayerWindow::CurrentLedText() const {
    const auto position = progress_tracking_position_.value_or(audio_.Position());
    return FormatLedTime(settings_.player.show_elapsed_time
        ? position : position - audio_.Duration());
}

std::wstring PlayerWindow::HitTestSkin(POINT point) const {
    if (!skin_) return {};
    const auto playback = audio_.State();
    const bool active = playback == audio::PlaybackState::opening ||
                        playback == audio::PlaybackState::playing;
    const auto& elements = ActiveSkinElements();
    for (auto item = elements.rbegin(); item != elements.rend(); ++item) {
        if (!IsSkinButton(item->name)) continue;
        if (!IsPlayModeSkinVisible(item->name, settings_.player.play_mode)) continue;
        if (item->name == L"play" && active) continue;
        if (item->name == L"pause" && !active) continue;
        if (!IsSkinElementEnabled(item->name)) continue;
        RECT bounds = item->bounds;
        if (item->name == L"led") bounds = SkinLedBounds(*item, CurrentLedText());
        if (item->name == L"pause") {
            if (const auto* play = FindActiveSkinElement(L"play")) {
                const int width = item->image_size.cx / 4;
                bounds = {play->bounds.left, play->bounds.top,
                    play->bounds.left + width,
                    play->bounds.top + item->image_size.cy};
            }
        }
        if (PtInRect(&bounds, point)) return item->name;
    }
    return {};
}

void PlayerWindow::InvokeSkinAction(std::wstring_view action) {
    if (IsSuppressedSkinControl(action)) return;
    if (action == L"icon") {
        // The legacy icon child uses command 0x7DD8.  FUN_00464FBF anchors the
        // complete main popup at the skin icon rectangle's left/bottom edge;
        // it is therefore another physical entry to the 0xE140 Options item,
        // not a caption-drag surface.
        if (const auto* icon = FindActiveSkinElement(L"icon")) {
            POINT anchor{icon->bounds.left, icon->bounds.bottom};
            ClientToScreen(window_, &anchor);
            ShowContextMenu(anchor);
        }
    }
    else if (action == L"exit") PostMessageW(window_, WM_CLOSE, 0, 0);
    else if (action == L"minimize") ShowWindow(window_, SW_MINIMIZE);
    else if (action == L"minimode") ToggleMiniMode();
    else if (action == L"prev") SelectRelative(false);
    else if (action == L"next") SelectRelative(true);
    else if (action == L"stop") Stop();
    else if (action == L"open") ChooseFiles();
    else if (action == L"browser") HandleContextCommand(kCmdShowBrowser);
    else if (action == L"set") ShowOptions();
    else if (action == L"play") {
        if (audio_.State() == audio::PlaybackState::paused) audio_.Resume();
        else PlayCurrent();
    }
    else if (action.starts_with(L"mode_")) {
        settings_.player.play_mode = (settings_.player.play_mode + 1) % 5;
        ResetSkinControlAnimations();
        UpdateMainToolRects();
    }
    else if (action == L"pause") {
        if (audio_.State() == audio::PlaybackState::playing) audio_.Pause();
        else if (audio_.State() == audio::PlaybackState::paused) audio_.Resume();
    } else if (action == L"mute") {
        ToggleMute();
    } else if (action == L"playlist") {
        TogglePlaylistWindow();
    } else if (action == L"lyric") {
        ToggleLyricWindow();
    } else if (action == L"equalizer") {
        ToggleEqualizerWindow();
    } else if (action == L"led") {
        SendMessageW(window_, WM_COMMAND, kCmdShowElapsedTime, 0);
    }
    RefreshPlaybackUi();
    InvalidateRect(window_, nullptr, FALSE);
}

std::vector<HWND> PlayerWindow::RegisteredDragWindows() const {
    std::vector<HWND> result;
    for (const HWND candidate : {window_, lyric_window_, playlist_window_,
                                 equalizer_window_}) {
        if (candidate && IsWindow(candidate) &&
            std::find(result.begin(), result.end(), candidate) == result.end()) {
            result.push_back(candidate);
        }
    }
    return result;
}

void PlayerWindow::BuildAttachedDragGroup() {
    attached_drag_windows_.clear();
    if (skin_drag_window_ != window_) return;

    const auto registered = RegisteredDragWindows();
    bool added = true;
    while (added) {
        added = false;
        for (const HWND candidate : registered) {
            // FUN_0047075B's initial pair scan calls FUN_0040BFD7 before its
            // later visibility-filtered transitive pass.  Consequently an
            // already attached hidden window (LX-iPlay's embedded playlist)
            // remains in the deferred move set, while a detached hidden
            // playlist still fails the geometry test below.
            if (candidate == window_ ||
                std::find(attached_drag_windows_.begin(), attached_drag_windows_.end(),
                          candidate) != attached_drag_windows_.end()) {
                continue;
            }
            RECT candidate_rect{};
            GetWindowRect(candidate, &candidate_rect);

            bool connected = false;
            std::vector<HWND> connected_set{window_};
            connected_set.insert(connected_set.end(), attached_drag_windows_.begin(),
                                 attached_drag_windows_.end());
            for (const HWND member : connected_set) {
                RECT member_rect{};
                GetWindowRect(member, &member_rect);
                if (AreDragWindowsAttached(member_rect, candidate_rect)) {
                    connected = true;
                    break;
                }
            }
            if (connected) {
                attached_drag_windows_.push_back(candidate);
                added = true;
            }
        }
    }
}

unsigned int PlayerWindow::PlaylistDragHitTest(POINT point) const {
    if (!playlist_window_ || !skin_ || !skin_->Playlist().valid) return kDragMove;
    const RECT resize = skin_->Playlist().resize_rect;
    if (resize.right <= resize.left || resize.bottom <= resize.top) return kDragMove;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    int horizontal_margin = std::min(4, GetSystemMetrics(SM_CXFRAME));
    int vertical_margin = std::min(4, GetSystemMetrics(SM_CYFRAME));
    horizontal_margin = std::max(1, horizontal_margin);
    vertical_margin = std::max(1, vertical_margin);

    unsigned int hit = 0;
    if (point.x >= client.right - horizontal_margin && point.x <= client.right)
        hit |= kDragRight;
    else if (point.x >= client.left && point.x <= client.left + horizontal_margin)
        hit |= kDragLeft;
    if (point.y >= client.bottom - vertical_margin && point.y <= client.bottom)
        hit |= kDragBottom;
    else if (point.y >= client.top && point.y <= client.top + vertical_margin)
        hit |= kDragTop;

    // FUN_00410832 doubles the perpendicular edge band once one resize axis
    // was found, which makes all four corners practical without a native
    // non-client frame.
    if ((hit & (kDragLeft | kDragRight)) == 0 &&
        (hit & (kDragTop | kDragBottom)) != 0) {
        if (point.x >= client.right - horizontal_margin * 2 && point.x <= client.right)
            hit |= kDragRight;
        else if (point.x >= client.left && point.x <= client.left + horizontal_margin * 2)
            hit |= kDragLeft;
    } else if ((hit & (kDragLeft | kDragRight)) != 0 &&
               (hit & (kDragTop | kDragBottom)) == 0) {
        if (point.y >= client.bottom - vertical_margin * 2 && point.y <= client.bottom)
            hit |= kDragBottom;
        else if (point.y >= client.top && point.y <= client.top + vertical_margin * 2)
            hit |= kDragTop;
    }
    return hit == 0 ? kDragMove : hit;
}

void PlayerWindow::BeginSkinBackgroundDrag(HWND source, POINT point, unsigned int hit) {
    pressed_skin_element_.clear();
    dragging_skin_background_ = true;
    skin_drag_window_ = source;
    skin_drag_anchor_ = point;
    skin_drag_hit_ = hit;
    GetWindowRect(source, &skin_drag_initial_rect_);
    skin_drag_screen_anchor_ = point;
    ClientToScreen(source, &skin_drag_screen_anchor_);
    BuildAttachedDragGroup();
    SetCapture(source);
}

void PlayerWindow::ContinueSkinBackgroundDrag(HWND source, POINT point) {
    if (!dragging_skin_background_ || skin_drag_window_ != source ||
        GetCapture() != source) return;

    if (skin_drag_hit_ != kDragMove) {
        POINT screen = point;
        ClientToScreen(source, &screen);
        const int dx = screen.x - skin_drag_screen_anchor_.x;
        const int dy = screen.y - skin_drag_screen_anchor_.y;
        RECT proposed = skin_drag_initial_rect_;
        if ((skin_drag_hit_ & kDragLeft) != 0) proposed.left += dx;
        if ((skin_drag_hit_ & kDragRight) != 0) proposed.right += dx;
        if ((skin_drag_hit_ & kDragTop) != 0) proposed.top += dy;
        if ((skin_drag_hit_ & kDragBottom) != 0) proposed.bottom += dy;

        SIZE native{};
        if (skin_) {
            if (source == lyric_window_ && !mini_mode_)
                native = skin_->Lyric().background.size;
            else if (source == playlist_window_) native = skin_->Playlist().background.size;
        }
        const int minimum_width = std::max(10L, native.cx);
        const int minimum_height = std::max(10L, native.cy);
        const RECT work_area = DragWorkAreaForRect(proposed);
        const int maximum_width = std::max<int>(minimum_width,
            static_cast<int>(work_area.right - work_area.left));
        const int maximum_height = std::max<int>(minimum_height,
            static_cast<int>(work_area.bottom - work_area.top));
        auto constrain_axis = [](LONG& moving_edge, LONG fixed_edge, bool low_edge,
                                 int minimum, int maximum) {
            int length = low_edge ? static_cast<int>(fixed_edge - moving_edge)
                                  : static_cast<int>(moving_edge - fixed_edge);
            length = std::clamp(length, minimum, maximum);
            moving_edge = low_edge ? fixed_edge - length : fixed_edge + length;
        };
        if ((skin_drag_hit_ & kDragLeft) != 0)
            constrain_axis(proposed.left, proposed.right, true, minimum_width, maximum_width);
        else if ((skin_drag_hit_ & kDragRight) != 0)
            constrain_axis(proposed.right, proposed.left, false, minimum_width, maximum_width);
        if ((skin_drag_hit_ & kDragTop) != 0)
            constrain_axis(proposed.top, proposed.bottom, true, minimum_height, maximum_height);
        else if ((skin_drag_hit_ & kDragBottom) != 0)
            constrain_axis(proposed.bottom, proposed.top, false, minimum_height, maximum_height);

        // FUN_004709A2 supplies the resizing path's ten-pixel magnetic edge
        // correction.  It compares the active edge with the work area and all
        // visible registered window edges, but never translates the opposite
        // edge.
        const auto snap_option = DecodePackedRuntimeOption(
            settings_.general.snap_windows, 1, 100);
        const auto snap_edge = [snap_option](
            LONG coordinate, const std::vector<LONG>& targets) {
            if (!snap_option.enabled) return 0;
            int correction = 0;
            bool found = false;
            for (const LONG target : targets) {
                const int candidate = static_cast<int>(target - coordinate);
                if ((!found || std::abs(candidate) < std::abs(correction)) &&
                    std::abs(candidate) <= snap_option.value) {
                    correction = candidate;
                    found = true;
                }
            }
            return found ? correction : 0;
        };
        std::vector<LONG> horizontal_targets{work_area.left, work_area.right};
        std::vector<LONG> vertical_targets{work_area.top, work_area.bottom};
        for (const HWND candidate : RegisteredDragWindows()) {
            if (candidate == source || !IsWindowVisible(candidate)) continue;
            RECT bounds{};
            GetWindowRect(candidate, &bounds);
            horizontal_targets.push_back(bounds.left);
            horizontal_targets.push_back(bounds.right);
            vertical_targets.push_back(bounds.top);
            vertical_targets.push_back(bounds.bottom);
        }
        if ((skin_drag_hit_ & kDragLeft) != 0)
            proposed.left += snap_edge(proposed.left, horizontal_targets);
        else if ((skin_drag_hit_ & kDragRight) != 0)
            proposed.right += snap_edge(proposed.right, horizontal_targets);
        if ((skin_drag_hit_ & kDragTop) != 0)
            proposed.top += snap_edge(proposed.top, vertical_targets);
        else if ((skin_drag_hit_ & kDragBottom) != 0)
            proposed.bottom += snap_edge(proposed.bottom, vertical_targets);

        SetWindowPos(source, nullptr, proposed.left, proposed.top,
                     proposed.right - proposed.left, proposed.bottom - proposed.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    const int dx = point.x - skin_drag_anchor_.x;
    const int dy = point.y - skin_drag_anchor_.y;
    if (dx == 0 && dy == 0) return;

    std::vector<HWND> moving_windows{source};
    moving_windows.insert(moving_windows.end(), attached_drag_windows_.begin(),
                          attached_drag_windows_.end());
    std::vector<RECT> current_rects;
    std::vector<RECT> proposed_rects;
    current_rects.reserve(moving_windows.size());
    proposed_rects.reserve(moving_windows.size());
    for (const HWND moving : moving_windows) {
        RECT bounds{};
        GetWindowRect(moving, &bounds);
        current_rects.push_back(bounds);
        OffsetRect(&bounds, dx, dy);
        proposed_rects.push_back(bounds);
    }

    std::vector<RECT> stationary_rects;
    for (const HWND candidate : RegisteredDragWindows()) {
        if (!IsWindowVisible(candidate) ||
            std::find(moving_windows.begin(), moving_windows.end(), candidate) !=
                moving_windows.end()) continue;
        RECT bounds{};
        GetWindowRect(candidate, &bounds);
        stationary_rects.push_back(bounds);
    }
    const RECT work_area = DragWorkAreaForRect(proposed_rects.front());
    // FUN_0048AC21 constructs every skinned top-level window with
    // FUN_0044F3CF, which stores 10 in the movable block before either
    // FUN_0046EAAC or FUN_0044F670 handles WM_LBUTTONDOWN.  The threshold
    // therefore applies to the playlist as well as the primary player.
    const auto snap_option = DecodePackedRuntimeOption(
        settings_.general.snap_windows, 1, 100);
    const POINT correction = snap_option.enabled
        ? ComputeDragSnapCorrection(proposed_rects, stationary_rects,
                                    work_area, snap_option.value)
        : POINT{};
    const int final_dx = dx + correction.x;
    const int final_dy = dy + correction.y;

    HDWP batch = BeginDeferWindowPos(static_cast<int>(moving_windows.size()));
    bool deferred = batch != nullptr;
    if (batch) {
        for (size_t index = 0; index < moving_windows.size(); ++index) {
            const RECT& bounds = current_rects[index];
            batch = DeferWindowPos(batch, moving_windows[index], nullptr,
                bounds.left + final_dx, bounds.top + final_dy,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (!batch) { deferred = false; break; }
        }
        if (batch) deferred = EndDeferWindowPos(batch) != FALSE;
    }
    if (!deferred) {
        for (size_t index = 0; index < moving_windows.size(); ++index) {
            const RECT& bounds = current_rects[index];
            SetWindowPos(moving_windows[index], nullptr,
                bounds.left + final_dx, bounds.top + final_dy, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void PlayerWindow::EndSkinMouseCapture() {
    progress_tracking_position_.reset();
    HWND captured = skin_drag_window_;
    if (!captured) {
        const HWND current = GetCapture();
        if (current == window_ || current == lyric_window_ ||
            current == playlist_window_ ||
            current == equalizer_window_) captured = current;
    }
    dragging_skin_background_ = false;
    skin_drag_window_ = nullptr;
    skin_drag_hit_ = 0;
    attached_drag_windows_.clear();
    if (captured && GetCapture() == captured) ReleaseCapture();
}

void PlayerWindow::ToggleMiniMode() {
    if (!window_ || !skin_ || !skin_->SupportsMiniMode()) return;
    if (mini_mode_fade_pending_ && !mini_mode_fade_continuation_) {
        // A synchronous 00464B6C would leave the second command queued until
        // the first toggle returned. Preserve its parity instead of dropping
        // rapid double-command input merely because our fade is asynchronous.
        mini_mode_fade_queued_ = !mini_mode_fade_queued_;
        return;
    }
    const bool next_mini = !mini_mode_;
    const bool desktop_lyric_visible =
        desktop_lyric_mode_ && desktop_lyrics_.Visible();
    const HRGN next_region = skin_->CreateWindowRegion(next_mini);
    RECT region_bounds{};
    if (!next_region || GetRgnBox(next_region, &region_bounds) == NULLREGION) {
        if (next_region) DeleteObject(next_region);
        if (mini_mode_fade_continuation_) {
            mini_mode_fade_continuation_ = false;
            mini_mode_fade_pending_ = false;
            ApplySkinWindowAlpha(EffectiveSkinWindowAlpha(window_));
        }
        return;
    }

    if (!mini_mode_fade_continuation_) {
        // 00464B6C applies the 100-percent sentinel while the complete normal
        // window group is still visible, and only hides/rebuilds after alpha
        // reaches zero. Validate the destination region before beginning the
        // asynchronous equivalent so a malformed mini skin cannot strand an
        // invisible player.
        DeleteObject(next_region);
        mini_mode_fade_pending_ = true;
        BeginSkinWindowFade(window_, rendered_skin_window_alpha_, 0, true,
                            kFadeCompleteSwitchMini);
        return;
    }
    mini_mode_fade_continuation_ = false;

    EndSkinMouseCapture();
    hover_skin_element_.clear();
    ResetSkinControlAnimations();
    pressed_skin_element_.clear();

    RECT current{};
    GetWindowRect(window_, &current);
    if (!desktop_lyric_mode_) CaptureActiveLyricWindowState();
    if (mini_mode_) {
        mini_window_bounds_ = current;
        have_mini_window_bounds_ = true;
    } else {
        normal_window_bounds_ = current;
        have_normal_window_bounds_ = true;
        if (playlist_window_)
            settings_.player.playlist_visible =
                IsWindowVisible(playlist_window_) != FALSE;
        if (equalizer_window_)
            settings_.player.equalizer_visible =
                IsWindowVisible(equalizer_window_) != FALSE;
    }

    const SIZE size = next_mini ? skin_->MiniWindowSize() : skin_->WindowSize();
    int left = current.left;
    int top = current.top;
    if (next_mini && have_mini_window_bounds_) {
        left = mini_window_bounds_.left;
        top = mini_window_bounds_.top;
    } else if (!next_mini && have_normal_window_bounds_) {
        left = normal_window_bounds_.left;
        top = normal_window_bounds_.top;
    }

    // FUN_00464B6C hides the main HWND while its live control set and region
    // are replaced. Playlist/equalizer (and the original browser) are hidden
    // on entry. TTPlayer_LyricWnd is deliberately retained and rebound to its
    // independent LyricWnd2/LyricVisible2/LyricTopMost2 state.
    ShowWindow(window_, SW_HIDE);
    if (next_mini && playlist_window_ && IsWindowVisible(playlist_window_))
        ShowWindow(playlist_window_, SW_HIDE);
    if (next_mini && equalizer_window_ && IsWindowVisible(equalizer_window_))
        ShowWindow(equalizer_window_, SW_HIDE);
    mini_mode_ = next_mini;
    settings_.player.mini_mode = mini_mode_;
    const LONG_PTR desired_extended =
        WS_EX_LAYERED | (mini_mode_ ? WS_EX_TOOLWINDOW : 0) |
        (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, desired_extended);
    SetWindowPos(window_, nullptr, left, top, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ApplySkinWindowTopMost();
    if (!SetWindowRgn(window_, next_region, FALSE)) DeleteObject(next_region);
    UpdateMainToolRects();
    UpdateVisualWindowLayout();
    UpdateVisualFrame();
    ResetSkinInfoScroll();
    suppress_skin_window_activation_fade_ = true;
    ShowWindow(window_, SW_SHOW);
    if (lyric_window_) {
        if (desktop_lyric_mode_) {
            ShowWindow(lyric_window_, SW_HIDE);
            ActiveLyricVisible() = desktop_lyric_visible;
            desktop_lyrics_.Show(desktop_lyric_visible);
        } else {
            ApplyActiveLyricWindowState();
        }
    }
    if (!next_mini && settings_.player.playlist_visible && playlist_window_)
        ShowWindow(playlist_window_, SW_SHOWNOACTIVATE);
    if (!next_mini && settings_.player.equalizer_visible && equalizer_window_)
        ShowWindow(equalizer_window_, SW_SHOWNOACTIVATE);
    ApplySkinWindowTopMost();
    suppress_skin_window_activation_fade_ = false;
    // The -1 sentinel at the end of 00464B6C restores the configured group
    // alpha only after all mode-specific windows have their final visibility.
    BeginSkinWindowFade(window_, 0, EffectiveSkinWindowAlpha(window_), true,
                        kFadeCompleteFinishMini);
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void PlayerWindow::PopulateTrackMenu(HMENU menu) {
    if (!menu) return;
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    const auto& tracks = ActivePlaylist().Tracks();
    if (tracks.empty()) {
        const auto empty = ResourceText(0x7ef4);
        AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, 0x7ef4, empty.c_str());
        return;
    }
    // 004813C1 reserves commands 10000..29998, uses the playlist's display
    // formatter, pads number prefixes and appends only known durations.
    const size_t count = std::min<size_t>(tracks.size(), 19'999);
    const size_t prefix_width = std::to_wstring(count).size() + 2;
    int column_rows = 20;
    if (settings_.general.menu_bar_playlist) {
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            column_rows = std::max(20L, (work.bottom - work.top) / 22);
    }
    for (size_t index = 0; index < count; ++index) {
        std::wstring label;
        if (settings_.playlist.title_number) {
            label = std::to_wstring(index + 1) + L".";
            label.resize(prefix_width, L' ');
        }
        label += PlaylistDisplayText(tracks[index]);
        if (tracks[index].duration_ms >= 0) {
            const int seconds = tracks[index].duration_ms / 1000;
            label += L"\t[" + std::to_wstring(seconds / 60) + L":";
            if (seconds % 60 < 10) label += L"0";
            label += std::to_wstring(seconds % 60) + L"]";
        }
        UINT flags = MF_STRING;
        if (!media_library_playback_active_ && playing_playlist_index_ &&
            *playing_playlist_index_ == playlists_.ActiveIndex() && current_ == index)
            flags |= MF_CHECKED | MF_USECHECKBITMAPS;
        if (settings_.general.menu_bar_playlist &&
            index % static_cast<size_t>(column_rows) == 0)
            flags |= MF_MENUBARBREAK;
        AppendMenuW(menu, flags, kCmdFirstTrack + static_cast<UINT>(index), label.c_str());
    }
}

HMENU PlayerWindow::BuildContextMenu() {
    const HMODULE resources = ResourceModule();
    if (!resources) return nullptr;

    HMENU popup = DetachFirstPopup(LoadMenuW(resources, MAKEINTRESOURCEW(kMenuMain)));
    if (!popup) return nullptr;

    // CPlayerWnd_ShowMainContextMenu deletes 0x94 in the ordinary player
    // state and keeps 0x8f (the lyric-display menu).  The reverse branch is
    // used only while the dedicated lyric editor is active.
    DeleteMenu(popup, kMenuLyricEditor, MF_BYCOMMAND);

    // The original 0045DFBA performs this same placeholder-resource graft:
    // each command ID in menu 0x8a is tried as another RT_MENU resource ID.
    for (int index = GetMenuItemCount(popup) - 1; index >= 0; --index) {
        const UINT resource_id = GetMenuItemID(popup, index);
        if (resource_id == static_cast<UINT>(-1) || resource_id == 0) continue;
        // Supply community links even when the resource DLL has no link
        // submenu. Keep its parent caption/icon and the usual popup styling.
        HMENU child = resource_id == kMenuRelatedLinks ? CreatePopupMenu() :
            DetachFirstPopup(LoadMenuW(resources, MAKEINTRESOURCEW(resource_id)));
        if (!child) continue;

        if (resource_id == kMenuRelatedLinks) {
            for (const auto& link : kProjectLinks)
                AppendMenuW(child, MF_STRING, link.command, link.label);
        } else if (resource_id == kMenuVisual) {
            // CPlayerWnd_ShowMainContextMenu (0045E126..0045E1E3) removes
            // the embedded/full-screen-only block by deleting position 6
            // four times.  Cover, None and the final Options entry remain.
            for (int remove = 0; remove < 4 &&
                 GetMenuItemCount(child) > 6; ++remove)
                DeleteMenu(child, 6, MF_BYPOSITION);
        } else if (resource_id == kMenuTransparency) {
            // 0045E996 inserts the missing 10..90 percent entries dynamically.
            for (int percent = 10; percent <= 90; percent += 10) {
                const auto label = std::to_wstring(percent) + L"%";
                InsertMenuW(child, percent / 10, MF_BYPOSITION | MF_STRING,
                    kCmdFirstAlpha + percent / 10, label.c_str());
            }
        }

        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_SUBMENU;
        item.hSubMenu = child;
        if (!SetMenuItemInfoW(popup, resource_id, FALSE, &item)) DestroyMenu(child);
    }
    PrepareContextMenu(popup);

    // FUN_00465CB8 adds a disabled zero-height owner-draw item before the
    // resource menu, then starts the real menu in the next column.  The item
    // is drawn by FUN_004700D9 as TTPlayer's vertical branded side strip.
    auto side_title = ResourceText(0x80);
    const auto slogan = ResourceText(0x86);
    if (!side_title.empty() && !slogan.empty()) side_title += L"--" + slogan;
    if (!side_title.empty() && InsertMenuW(popup, 0,
            MF_BYPOSITION | MF_STRING | MF_DISABLED, 0xfffa,
            side_title.c_str())) {
        MENUITEMINFOW first{sizeof(first)};
        first.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(popup, 1, TRUE, &first)) {
            first.fType |= MFT_MENUBREAK;
            SetMenuItemInfoW(popup, 1, TRUE, &first);
        }
    }
    return popup;
}

std::vector<PlayerWindow::SkinMenuEntry> PlayerWindow::LoadSkinMenuCatalog(
    const std::filesystem::path& skin_directory, HMODULE skin_resources,
    HMODULE ttpcomm_module,
    const std::shared_ptr<std::atomic_bool>& cancel) {
    const auto cancelled = [&cancel] {
        return cancel && cancel->load(std::memory_order_acquire);
    };
    std::vector<SkinMenuEntry> catalog;
    if (cancelled()) return catalog;
    SkinMenuEntry embedded;
    embedded.command = kCmdDefaultSkin;
    embedded.package_name = L"<Default_Skin>";
    embedded.embedded_default = true;
    try {
        auto package = skin::SkinPackage::OpenResource(
            skin_resources, L"<Default_Skin>", L"ZIP");
        if (const auto metadata = skin::ParseLegacySkinMetadata(
                package.ReadEntry("Skin.xml", ttpcomm_module))) {
            embedded.metadata = *metadata;
        }
    } catch (const std::exception&) {
        // The fixed resource menu item remains usable even if a damaged
        // replacement resource DLL cannot provide its informational fields.
    }

    if (cancelled()) return {};
    catalog.push_back(std::move(embedded));

    std::vector<SkinMenuEntry> installed;
    for (const auto& directory : skin::SkinSearchDirectories(skin_directory)) {
        if (directory.empty()) continue;
        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            if (cancelled()) return {};
            const auto directory_entry = *iterator;
            iterator.increment(error);
            std::error_code file_error;
            if (!directory_entry.is_regular_file(file_error) || file_error)
                continue;
            const auto& path = directory_entry.path();
            const auto extension = path.extension().wstring();
            if (_wcsicmp(extension.c_str(), L".skn") != 0 &&
                _wcsicmp(extension.c_str(), L".zip") != 0) {
                continue;
            }
            try {
                const auto package = skin::SkinPackage::Open(path);
                const auto metadata = skin::ParseLegacySkinMetadata(
                    package.ReadEntry("Skin.xml", ttpcomm_module));
                if (cancelled()) return {};
                if (!metadata) continue;
                installed.push_back(SkinMenuEntry{
                    0, path, skin::SkinPackageSelector(skin_directory, path), *metadata, false});
            } catch (const std::exception&) {
                // 0045E518 simply releases a package whose ZIP/XML loader
                // failed and proceeds with the next FindNextFile result.
            }
        }
    }

    if (cancelled()) return {};
    std::sort(installed.begin(), installed.end(),
        [](const SkinMenuEntry& left, const SkinMenuEntry& right) {
            const int order = CompareSkinNames(left.metadata.name, right.metadata.name);
            return order != 0 ? order < 0
                : _wcsicmp(left.package_name.c_str(), right.package_name.c_str()) < 0;
        });

    catalog.insert(catalog.end(),
                   std::make_move_iterator(installed.begin()),
                   std::make_move_iterator(installed.end()));
    return catalog;
}

void PlayerWindow::StartSkinMenuCatalogLoad() {
    // A popup that closed before opening Skin leaves a useful result behind;
    // retain and consume that same future on the next popup instead of
    // launching overlapping scans through the legacy decompression DLL.
    if (skin_catalog_future_.valid()) {
        static_cast<void>(PublishReadySkinMenuCatalog());
        if (skin_catalog_future_.valid()) return;
    }

    const auto skin_directory = FindRuntimePath(L"Skin");
    const HMODULE resources = ResourceModule();
    const HMODULE ttpcomm = ttpcomm_module_;
    try {
        auto cancel = std::make_shared<std::atomic_bool>(false);
        skin_catalog_future_ = std::async(std::launch::async,
            [skin_directory, resources, ttpcomm, cancel] {
                return LoadSkinMenuCatalog(skin_directory, resources,
                                           ttpcomm, cancel);
            });
        skin_catalog_cancel_ = std::move(cancel);
    } catch (const std::exception&) {
        skin_catalog_cancel_.reset();
        // Resource exhaustion must not make the context menu unusable.  The
        // submenu keeps the last complete snapshot and a later root-menu open
        // retries the asynchronous scan.
    }
}

bool PlayerWindow::PublishReadySkinMenuCatalog(DWORD wait_milliseconds) {
    if (!skin_catalog_future_.valid()) return false;

    // The scan itself always runs on the asynchronous worker started when the
    // root context menu opens.  UI callers pass zero and publish only a fully
    // completed generation; the INFINITE form is retained solely for a
    // non-UI lifecycle caller should one ever need an explicit join.
    if (wait_milliseconds == INFINITE) {
        skin_catalog_future_.wait();
    } else {
        const auto status = skin_catalog_future_.wait_for(
            std::chrono::milliseconds(wait_milliseconds));
        if (status != std::future_status::ready) return false;
    }

    const bool stale = skin_catalog_result_stale_;
    try {
        auto catalog = skin_catalog_future_.get();
        if (!stale) skin_catalog_cache_ = std::move(catalog);
    } catch (const std::exception&) {
        // Keep the last complete snapshot.  A transient package or worker
        // failure must not empty a menu which was usable on the prior open.
    }
    skin_catalog_cancel_.reset();
    if (stale) {
        // The completed worker was cancelled after its directory snapshot had
        // already become obsolete (for example by deleting a skin from the
        // Options page).  Do not publish that snapshot and, importantly, do
        // not let the polling caller stop here: immediately launch the next
        // generation.  The previous implementation cleared the stale flag
        // and left the old menu cached until another unrelated menu open.
        skin_catalog_result_stale_ = false;
        StartSkinMenuCatalogLoad();
        return !skin_catalog_future_.valid();
    }
    return true;
}

void PlayerWindow::InvalidateSkinMenuCatalog() noexcept {
    // Do not destroy or replace an unfinished std::async future: its
    // destructor is permitted to join the task and would merely move the same
    // long wait into the drop callback.  Mark its snapshot stale instead; the
    // UI timer will consume it only after it is ready and start a replacement.
    skin_catalog_result_stale_ = skin_catalog_future_.valid();
    if (skin_catalog_cancel_)
        skin_catalog_cancel_->store(true, std::memory_order_release);
    skin_commands_.clear();
}

void PlayerWindow::PopulateSkinMenu(HMENU menu) {
    if (!menu) return;

    // CPlayerWnd_PopulateSkinMenu (0045E518) preserves resource positions
    // 0/1 (default + separator) and the final separator/options pair, while
    // replacing positions [2,count-3] left by the previous population.
    for (int position = GetMenuItemCount(menu) - 3; position >= 2; --position)
        DeleteMenu(menu, position, MF_BYPOSITION);
    skin_commands_.clear();

    // Root-menu construction already starts the asynchronous worker.  Menu
    // expansion must never join it: damaged packages, slow storage, or a
    // blocked decompressor otherwise freeze the player's UI thread.  Publish
    // only a complete ready generation and use the last complete snapshot for
    // this popup when scanning is still in progress.
    static_cast<void>(PublishReadySkinMenuCatalog(0));
    const bool catalog_pending = skin_catalog_future_.valid();
    const auto skin_directory = FindRuntimePath(L"Skin");
    // If launching the worker failed, retain the default-only menu for this
    // popup and retry from the next root-menu open.  A synchronous fallback
    // here would recreate the same unbounded UI wait that preload avoids.

    SkinMenuEntry embedded;
    embedded.command = kCmdDefaultSkin;
    embedded.package_name = L"<Default_Skin>";
    embedded.embedded_default = true;
    std::vector<SkinMenuEntry> installed;
    installed.reserve(skin_catalog_cache_.size());
    for (const auto& entry : skin_catalog_cache_) {
        if (entry.embedded_default)
            embedded = entry;
        else
            installed.push_back(entry);
    }

    UINT command = kCmdFirstSkin;
    int position = 2;
    bool current_found{};
    for (auto& entry : installed) {
        entry.command = command++;
        InsertMenuW(menu, position++, MF_BYPOSITION | MF_STRING,
                    entry.command, entry.metadata.name.c_str());
        if (!current_found && _wcsicmp(entry.package_name.c_str(),
                                      settings_.skin_file.c_str()) == 0) {
            CheckMenuItem(menu, entry.command, MF_BYCOMMAND | MF_CHECKED);
            current_found = true;
        }
    }

    // The command-to-object invariant used by 00465695 is index =
    // command-0x7919, so insert the resource object at vector index zero only
    // after assigning all sorted external commands.
    skin_commands_.reserve(installed.size() + 1);
    skin_commands_.push_back(std::move(embedded));
    skin_commands_.insert(skin_commands_.end(),
                          std::make_move_iterator(installed.begin()),
                          std::make_move_iterator(installed.end()));

    if (settings_.skin_file == L"<Default_Skin>" ||
        settings_.skin_file.empty()) {
        CheckMenuItem(menu, kCmdDefaultSkin, MF_BYCOMMAND | MF_CHECKED);
        current_found = true;
    }

    // The native code makes one final load attempt for a configured package
    // that was not present in the successful enumeration and appends it at
    // the end.  Restrict resolution to the executable's Skin directory, as
    // the original DAT_00547504 base path does.
    if (!catalog_pending && !skin_catalog_cache_.empty() &&
        !current_found && !skin_directory.empty() &&
        !settings_.skin_file.empty()) {
        const auto package_name = skin::NormalizeSkinPackageName(settings_.skin_file);
        const auto configured = skin::ResolveSkinPackagePath(skin_directory, settings_.skin_file);
        try {
            const auto package = skin::SkinPackage::Open(configured);
            const auto metadata = skin::ParseLegacySkinMetadata(
                package.ReadEntry("Skin.xml", ttpcomm_module_));
            if (metadata) {
                SkinMenuEntry entry{command, configured,
                    package_name.wstring(), *metadata, false};
                InsertMenuW(menu, position, MF_BYPOSITION | MF_STRING,
                            command, entry.metadata.name.c_str());
                CheckMenuItem(menu, command, MF_BYCOMMAND | MF_CHECKED);
                skin_commands_.push_back(std::move(entry));
            }
        } catch (const std::exception&) {
        }
    }
}

std::wstring PlayerWindow::SkinMenuToolTipText(UINT command) const {
    const auto found = std::find_if(skin_commands_.begin(),
        skin_commands_.end(), [command](const SkinMenuEntry& entry) {
            return entry.command == command;
        });
    if (found == skin_commands_.end() || found->metadata.version != 2)
        return {};
    const auto format = ResourceText(0x81c3);
    if (format.empty()) return {};
    const int length = _scwprintf(format.c_str(),
        found->metadata.author.c_str(), found->metadata.url.c_str(),
        found->metadata.email.c_str());
    if (length < 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    swprintf_s(text.data(), text.size(), format.c_str(),
        found->metadata.author.c_str(), found->metadata.url.c_str(),
        found->metadata.email.c_str());
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring PlayerWindow::ToolTipWithHotKey(
    UINT command, std::wstring text) const {
    if (!settings_.general.show_hotkey_in_tips) return text;
    const auto found = std::find_if(
        settings_.hotkey.key_map.begin(), settings_.hotkey.key_map.end(),
        [command](const settings::HotKeyBinding& binding) {
            return binding.command == static_cast<int>(command);
        });
    if (found == settings_.hotkey.key_map.end()) return text;
    return AppendToolTipHotKey(std::move(text), true,
                               RuntimeHotKeyText(found->application));
}

std::wstring PlayerWindow::MenuToolTipText(UINT command) const {
    if (!settings_.general.menu_tips) return {};
    if (const auto* link = FindProjectLink(command)) return link->url;
    auto text = SkinMenuToolTipText(command);
    if (text.empty()) text = CommandTipDescription(ResourceText(command));
    return ToolTipWithHotKey(command, std::move(text));
}

void PlayerWindow::QueueSkinMenuToolTip(UINT command, HMENU menu) {
    const auto text = MenuToolTipText(command);
    if (text.empty() || !menu || !window_) {
        HideSkinMenuToolTip();
        return;
    }
    if (!skin_menu_tooltip_ || !IsWindow(skin_menu_tooltip_)) {
        skin_menu_tooltip_ = CreateWindowExW(WS_EX_TOPMOST,
            TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            window_, nullptr, instance_, nullptr);
        if (!skin_menu_tooltip_) return;
        SetWindowPos(skin_menu_tooltip_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(skin_menu_tooltip_, TTM_SETMAXTIPWIDTH, 0, 600);
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        tool.hwnd = window_;
        tool.uId = 1;
        tool.lpszText = const_cast<wchar_t*>(L"");
        if (!SendMessageW(skin_menu_tooltip_, TTM_ADDTOOLW, 0,
                          reinterpret_cast<LPARAM>(&tool))) {
            DestroyWindow(skin_menu_tooltip_);
            skin_menu_tooltip_ = nullptr;
            return;
        }
    }

    KillTimer(window_, kSkinMenuToolTipTimer);
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = window_;
    tool.uId = 1;
    SendMessageW(skin_menu_tooltip_, TTM_TRACKACTIVATE, FALSE,
                 reinterpret_cast<LPARAM>(&tool));
    pending_skin_tooltip_command_ = command;
    pending_skin_tooltip_menu_ = menu;
    skin_menu_tooltip_text_ = text;
    const LRESULT configured = SendMessageW(
        skin_menu_tooltip_, TTM_GETDELAYTIME, TTDT_INITIAL, 0);
    const UINT delay = configured > 0
        ? static_cast<UINT>(configured) : 500U;
    SetTimer(window_, kSkinMenuToolTipTimer, delay, nullptr);
}

void PlayerWindow::ShowQueuedSkinMenuToolTip() {
    KillTimer(window_, kSkinMenuToolTipTimer);
    if (!skin_menu_tooltip_ || !pending_skin_tooltip_menu_ ||
        !pending_skin_tooltip_command_) return;

    int position = -1;
    const int count = GetMenuItemCount(pending_skin_tooltip_menu_);
    for (int index = 0; index < count; ++index) {
        if (GetMenuItemID(pending_skin_tooltip_menu_, index) ==
            pending_skin_tooltip_command_) {
            position = index;
            break;
        }
    }
    RECT bounds{};
    if (position < 0 ||
        (!GetMenuItemRect(nullptr, pending_skin_tooltip_menu_, position,
                          &bounds) &&
         !GetMenuItemRect(window_, pending_skin_tooltip_menu_, position,
                          &bounds))) {
        HideSkinMenuToolTip();
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    if (!PtInRect(&bounds, cursor)) {
        HideSkinMenuToolTip();
        return;
    }

    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = window_;
    tool.uId = 1;
    tool.lpszText = skin_menu_tooltip_text_.data();
    SendMessageW(skin_menu_tooltip_, TTM_UPDATETIPTEXTW, 0,
                 reinterpret_cast<LPARAM>(&tool));
    SendMessageW(skin_menu_tooltip_, TTM_TRACKPOSITION, 0,
                 MAKELPARAM(bounds.right - 1, bounds.top));
    SendMessageW(skin_menu_tooltip_, TTM_TRACKACTIVATE, TRUE,
                 reinterpret_cast<LPARAM>(&tool));
}

void PlayerWindow::HideSkinMenuToolTip() {
    if (window_) KillTimer(window_, kSkinMenuToolTipTimer);
    if (skin_menu_tooltip_ && IsWindow(skin_menu_tooltip_)) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        tool.hwnd = window_;
        tool.uId = 1;
        SendMessageW(skin_menu_tooltip_, TTM_TRACKACTIVATE, FALSE,
                     reinterpret_cast<LPARAM>(&tool));
    }
    pending_skin_tooltip_menu_ = nullptr;
    pending_skin_tooltip_command_ = 0;
    skin_menu_tooltip_text_.clear();
}

void PlayerWindow::PrepareContextMenu(HMENU menu) {
    const auto state = audio_.State();
    const bool playing = state == audio::PlaybackState::playing || state == audio::PlaybackState::opening;
    const bool paused = state == audio::PlaybackState::paused;
    const bool have_track = PlaybackTrackForUi() != nullptr;

    // Original removes Play while actively playing, otherwise removes Pause.
    if (const HMENU player = FindCommandMenu(menu, playing ? kCmdPlay : kCmdPause))
        DeleteMenu(player, playing ? kCmdPlay : kCmdPause, MF_BYCOMMAND);
    EnableCommand(menu, kCmdPause, playing || paused);
    EnableCommand(menu, kCmdStopPlayback, playing || paused);
    EnableCommand(menu, kCmdPrevious, !PlaybackPlaylist().Tracks().empty());
    EnableCommand(menu, kCmdNext, !PlaybackPlaylist().Tracks().empty());
    EnableCommand(menu, kCmdCloseFile, have_track);
    EnableCommand(menu, kCmdFileProperties, have_track);
    const bool seekable = (playing || paused) &&
        audio_.Duration() > std::chrono::milliseconds::zero();
    EnableCommand(menu, kCmdSeekBack, seekable);
    EnableCommand(menu, kCmdSeekForward, seekable);
    EnableCommand(menu, kCmdPlayCd, CanPlayCompactDisc());
    EnableCommand(menu, kCmdPlayUrl, true);
    // The shared menu updater enables 0x7DE8..0x7DEA only for engine state 2
    // (actively playing), even when one of those modes is already selected.
    const bool may_enter_fullscreen =
        state == audio::PlaybackState::playing;
    EnableCommand(menu, kCmdFullscreenLyrics, may_enter_fullscreen);
    EnableCommand(menu, kCmdFullscreenVisual, may_enter_fullscreen);
    EnableCommand(menu, kCmdFullscreenAll, may_enter_fullscreen);
    CheckCommand(menu, kCmdFullscreenLyrics, fullscreen_mode_ == 1);
    CheckCommand(menu, kCmdFullscreenVisual, fullscreen_mode_ == 2);
    CheckCommand(menu, kCmdFullscreenAll, fullscreen_mode_ == 3);
    CheckCommand(menu, kCmdMute, settings_.player.mute);
    CheckCommand(menu, kCmdAlwaysOnTop,
                 mini_mode_ ? settings_.player.mini_top_most
                            : settings_.player.top_most);
    EnableCommand(menu, kCmdMiniMode, skin_ && skin_->SupportsMiniMode());
    CheckCommand(menu, kCmdMiniMode, mini_mode_);
    CheckCommand(menu, kCmdShowEqualizer,
                 equalizer_window_ && IsWindowVisible(equalizer_window_));
    CheckCommand(menu, kCmdShowLyrics,
                 desktop_lyrics_.Visible() ||
                     (lyric_window_ && IsWindowVisible(lyric_window_)));
    CheckCommand(menu, kCmdShowPlaylist,
                 playlist_window_ && IsWindowVisible(playlist_window_));

    for (UINT command = kCmdPlayModeFirst; command <= kCmdPlayModeLast; ++command)
        CheckCommand(menu, command, static_cast<int>(command - kCmdPlayModeFirst) == settings_.player.play_mode);
    if (current_) CheckCommand(menu, kCmdFirstTrack + static_cast<UINT>(*current_), true);
    for (UINT command = kCmdFirstAlpha; command <= kCmdLastAlpha; ++command)
        CheckCommand(menu, command, static_cast<int>(command - kCmdFirstAlpha) * 10 == transparency_percent_);
    for (UINT command = kCmdVisualFirst; command <= kCmdVisualLast; ++command)
        CheckCommand(menu, command,
            static_cast<int>(command - kCmdVisualFirst) == settings_.visual.type);

    const bool default_skin = settings_.skin_file.empty() ||
        settings_.skin_file == L"<Default_Skin>";
    CheckCommand(menu, kCmdDefaultSkin, default_skin);
    for (const auto& entry : skin_commands_) {
        if (!entry.embedded_default)
            CheckCommand(menu, entry.command, !default_skin &&
                _wcsicmp(entry.package_name.c_str(),
                         settings_.skin_file.c_str()) == 0);
    }

    // Menu 0x8A grafts resource 0x8F into the player's context menu.  Its
    // dynamic label/check state is prepared by the same CLyricWnd path as the
    // dedicated lyric-window popup (FUN_0045DFBA -> FUN_004427B1).
    PrepareLyricMenu(menu);

    // 0045DFBA replaces the ordinary "desktop lyrics" action while desktop
    // mode is active.  This is the only mouse-accessible unlock route after
    // CDeskLrcCtrl has made itself transparent and hidden CDeskLrcBar.
    if (desktop_lyric_mode_ || desktop_lyrics_.Visible()) {
        // Menu 0x8A contains another 0x8039 in its grafted lyric submenu.
        // 0045DFBA calls ModifyMenuW on the root popup itself; recursively
        // locating the command changes that nested duplicate and leaves the
        // only reachable root action as "show desktop lyrics".
        int desktop_position = -1;
        for (int position = 0; position < GetMenuItemCount(menu); ++position) {
            if (GetMenuItemID(menu, position) == kCmdDesktopLyrics) {
                desktop_position = position;
                break;
            }
        }
        if (desktop_position >= 0) {
            const UINT action = settings_.desktop_lyric.lock
                ? kCmdDesktopLyricUnlock : kCmdDesktopLyricLock;
            auto label = ResourceCommandLabel(ResourceModule(), action);
            if (label.empty()) {
                std::array<wchar_t, 256> current{};
                const int length = GetMenuStringW(menu, desktop_position,
                    current.data(), static_cast<int>(current.size()),
                    MF_BYPOSITION);
                if (length > 0)
                    label.assign(current.data(), static_cast<size_t>(length));
            }
            ModifyMenuW(menu, desktop_position,
                        MF_BYPOSITION | MF_STRING, action, label.c_str());
        }
    }
}

void PlayerWindow::ShowContextMenu(POINT screen_point, HWND origin) {
    // DAT_00547858 in 0045DFBA prevents nested popup construction.  A popup
    // runs its own modal message loop, so this guard is observable under
    // accessibility tools and synthetic input as well as normal mouse use.
    if (context_menu_open_ || !IsWindowEnabled(window_)) return;
    context_menu_open_ = true;
    main_context_menu_origin_ = origin && IsWindow(origin) ? origin : window_;
    // Compatibility improvement over 00461BAE: overlap ZIP/XML catalog work
    // with the time spent navigating the already visible root popup.
    StartSkinMenuCatalogLoad();
    HMENU menu = BuildContextMenu();
    if (!menu) {
        context_menu_open_ = false;
        main_context_menu_origin_ = nullptr;
        return;
    }
    SetForegroundWindow(window_);
    BeginPopupMenuStyle(menu, true);
    UINT previous_show_delay{};
    const BOOL have_show_delay = SystemParametersInfoW(
        SPI_GETMENUSHOWDELAY, 0, &previous_show_delay, 0);
    if (have_show_delay)
        SystemParametersInfoW(SPI_SETMENUSHOWDELAY, 400, nullptr, 0);
    // Preserve FUN_0046A27D's notification route for the main window. For a
    // forwarded lyric-chrome menu, obtain the command explicitly: User32 can
    // queue WM_COMMAND until after TrackPopupMenuEx returns, losing the
    // otherwise scoped origin before the fullscreen dispatcher sees it.
    // RETURNCMD is sufficient; keep initialization notifications for the
    // shared dynamic track submenu even when this menu is forwarded.
    const bool forwarded = main_context_menu_origin_ != window_;
    const UINT selected = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON |
        (forwarded ? TPM_RETURNCMD : 0),
        screen_point.x, screen_point.y, window_, nullptr);
    if (forwarded && selected)
        SendMessageW(window_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    if (have_show_delay)
        SystemParametersInfoW(SPI_SETMENUSHOWDELAY, previous_show_delay,
                              nullptr, 0);
    HideSkinMenuToolTip();
    EndPopupMenuStyle();
    DestroyMenu(menu);
    context_menu_open_ = false;
    main_context_menu_origin_ = nullptr;
    PostMessageW(window_, WM_NULL, 0, 0);
}

bool PlayerWindow::HandleContextCommand(UINT command, HWND fullscreen_origin) {
    if (OpenProjectLink(window_, command)) return true;
    if (HandleFullScreenCommand(command, fullscreen_origin ? fullscreen_origin :
            (context_menu_open_ && main_context_menu_origin_
                ? main_context_menu_origin_ : window_))) return true;
    if (command >= kCmdFirstTrack && command < kCmdFirstTrack + 19'999 &&
        command < kCmdFirstTrack + ActivePlaylist().Tracks().size()) {
        SelectTrack(command - kCmdFirstTrack, true);
        return true;
    }
    if (command >= kCmdPlayModeFirst && command <= kCmdPlayModeLast) {
        settings_.player.play_mode = static_cast<int>(command - kCmdPlayModeFirst);
        ResetSkinControlAnimations();
        UpdateMainToolRects();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }
    if (command >= kCmdFirstAlpha && command <= kCmdLastAlpha) {
        transparency_percent_ = static_cast<int>(command - kCmdFirstAlpha) * 10;
        settings_.player.alpha_percent = transparency_percent_;
        const BYTE alpha = static_cast<BYTE>(255 * (100 - transparency_percent_) / 100);
        // DAT_00547820 stores the configured transparency percentage; the
        // temporary 255 used by OpaqueWhenActive is never written back to it.
        skin_window_alpha_ = alpha;
        const BYTE target = settings_.player.opaque_when_active &&
            GetActiveWindow() == window_ ? static_cast<BYTE>(255) : alpha;
        AnimateSkinWindowAlpha(target);
        return true;
    }
    if (command >= kCmdVisualFirst && command <= kCmdVisualLast) {
        // FUN_00465726 stores command-0x8085 and immediately rebuilds the
        // active visual surface.
        SetVisualType(static_cast<int>(command - kCmdVisualFirst));
        return true;
    }
    if (command == kCmdDefaultSkin) {
        if (!settings_.skin_file.empty() && settings_.skin_file != L"<Default_Skin>")
            static_cast<void>(LoadSkinResource(skin_resources_, L"<Default_Skin>"));
        skin_commands_.clear();
        return true;
    }
    for (const auto& entry : skin_commands_) {
        if (!entry.embedded_default && command == entry.command) {
            // 00465695 compares selectors case-sensitively, then invokes one
            // 0045D5FA transaction (save outgoing, load target, rebind in place).
            if (settings_.skin_file != entry.package_name) {
                const auto path = entry.path;
                static_cast<void>(LoadSkinPackage(path));
            }
            skin_commands_.clear();
            return true;
        }
    }
    switch (command) {
    case kCmdPlay:
        if (audio_.State() == audio::PlaybackState::paused) audio_.Resume(); else PlayCurrent();
        break;
    case kCmdPause: audio_.Pause(); break;
    case kCmdStopPlayback: Stop(); break;
    case kCmdPrevious: SelectRelative(false); break;
    case kCmdNext: SelectRelative(true); break;
    case kCmdSeekBack: {
        const auto position = audio_.Position();
        // FUN_004651D7 deliberately does nothing in the first five seconds;
        // it does not clamp that case to zero.
        if (position > std::chrono::seconds(5))
            audio_.Seek(position - std::chrono::seconds(5));
        break;
    }
    case kCmdSeekForward:
        // FUN_0046520B forwards the unclamped position + 5000 ms to the
        // player's seek adapter; the decoder/output layer owns clamping.
        audio_.Seek(audio_.Position() + std::chrono::seconds(5));
        break;
    case kCmdPlayCd: ShowPlayCdDialog(); break;
    case kCmdPlayUrl: ShowPlayUrlDialog(); break;
    case kCmdFileProperties:
        // FUN_00464A94 forwards to PlayLists/Files only when one of those
        // surfaces owns focus (or there is no live CPlayItem). A command from
        // the player chrome targets the actual playback item, not a stale
        // selection in another catalogue.
        LeaveFullScreen();
        {
            const HWND focus = GetFocus();
            const auto* playback = PlaybackTrackForUi();
            const auto target = ResolveFileInfoCommandTarget(
                playback != nullptr,
                focus == playlist_tree_control_ ||
                    focus == playlist_list_control_,
                focus == playlist_track_control_ || focus == playlist_view_);
            if (target == FileInfoCommandTarget::current_playback && playback)
                ShowPlaylistProperties(playback);
            else
                HandlePlaylistCommand(kPlaylistProperties);
        }
        break;
    case kCmdOpenFile: ChooseFiles(); break;
    case kCmdCloseFile:
        Stop();
        ClearPersistedPlaybackIdentity();
        if (playing_playlist_index_ &&
            *playing_playlist_index_ < playlists_.Size() &&
            playlists_.At(*playing_playlist_index_)
                .SetPlayingRow(std::nullopt)) {
            playlists_.MarkDirty(*playing_playlist_index_);
        }
        current_.reset();
        playing_playlist_index_.reset();
        opened_track_.reset();
        media_library_playback_active_ = false;
        media_library_playback_.Clear();
        associated_lyric_path_.clear();
        ClearLyrics();
        break;
    case kCmdVolumeUp:
        settings_.player.volume = std::min(100, settings_.player.volume + 5);
        settings_.player.mute = false;
        audio_.SetVolume(static_cast<float>(settings_.player.volume) / 100.0F);
        break;
    case kCmdVolumeDown:
        settings_.player.volume = std::max(0, settings_.player.volume - 5);
        settings_.player.mute = false;
        audio_.SetVolume(static_cast<float>(settings_.player.volume) / 100.0F);
        break;
    case kCmdMute: ToggleMute(); break;
    case kCmdAlwaysOnTop: {
        bool& top_most = mini_mode_ ? settings_.player.mini_top_most
                                    : settings_.player.top_most;
        top_most = !top_most;
        ApplySkinWindowTopMost();
        // 004A3A66 explicitly calls 0041964D after changing the main pin.
        // Unlike geometry/visibility reconciliation, this user command also
        // reorders the desktop lyric owner group using the original 0x13.
        desktop_lyrics_.RefreshTopmost(true);
        break;
    }
    case kCmdShowLyrics: ToggleLyricWindow(); break;
    case kCmdShowEqualizer: ToggleEqualizerWindow(); break;
    case kCmdShowPlaylist: TogglePlaylistWindow(); break;
    case kCmdShowBrowser: {
        // FUN_0046CCC7 uses this exact ttpres.dll warning when the optional
        // music-browser component is unavailable.  The rebuild currently has
        // no browser host, so do not misroute the skin button to Open File.
        wchar_t caption[128]{};
        GetWindowTextW(window_, caption, static_cast<int>(std::size(caption)));
        MessageBoxW(window_, ResourceText(0x827b).c_str(),
                    caption, MB_ICONWARNING);
        break;
    }
    case kCmdOptions:
        // FUN_004658A3 is the sole options entry which first exits full-screen.
        LeaveFullScreen();
        ShowOptions();
        break;
    case kCmdVisualOptions: ShowOptions(4); break;
    case kCmdPlaylistOptions: ShowOptions(5); break;
    case kCmdLibraryOptions: ShowOptions(6); break;
    case kCmdLyricOptions: ShowOptions(7); break;
    case kCmdDesktopLyricOptions:
        // FUN_00419372 posts the private route instead of entering the sheet
        // synchronously from the desktop-lyric window procedure.  The posted
        // boundary lets that source callback unwind before page 7 creates and
        // selects nested template 385.
        PostMessageW(window_, kMsgShowOptionsControl, 385, 7);
        break;
    case kCmdLibraryDownloadOptions:
        // FUN_004251C0 is the media-library/download surface's settings
        // button.  It enters the network page through the private 0x7F4
        // message so template 382 is selected after the sheet exists.
        PostMessageW(window_, kMsgShowOptionsControl, 382, 9);
        break;
    case kCmdSkinOptions: ShowOptions(12); break;
    case kCmdMiniMode: ToggleMiniMode(); break;
    case kCmdMinimize: ShowWindow(window_, SW_MINIMIZE); break;
    case kCmdExit: PostMessageW(window_, WM_CLOSE, 0, 0); break;
    default: return false;
    }
    RefreshPlaybackUi();
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool PlayerWindow::ApplyLoadedSkin(bool apply_visual_settings, bool saved_bounds) {
    if (!skin_ || !skin_->Valid()) return false;

    // FUN_0045D5FA validates and constructs the replacement skin before it
    // mutates any live window.  Do the same for the color-key region so a bad
    // bitmap cannot turn the main window into an empty/invisible region.
    const HRGN next_region = skin_->CreateWindowRegion();
    RECT region_bounds{};
    if (!next_region || GetRgnBox(next_region, &region_bounds) == NULLREGION) {
        if (next_region) DeleteObject(next_region);
        return false;
    }

    EndSkinMouseCapture();
    hover_skin_element_.clear();
    pressed_skin_element_.clear();
    if (EqualizerOwnsCapture()) ReleaseEqualizerCapture();
    equalizer_hover_ = 0;
    lyric_hover_command_ = 0;
    lyric_pressed_command_ = 0;

    // The original main skin HWND remains WS_EX_LAYERED across a package
    // change.  Clearing and restoring that bit while TrackPopupMenuEx owns a
    // modal menu can invalidate both redirected surfaces and was the source
    // of the observed apparent/actual process exit on some Windows builds.
    const LONG_PTR current_style = GetWindowLongPtrW(window_, GWL_STYLE);
    ScopedSkinRedraw redraw(window_);
    const LONG_PTR desired_style =
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPSIBLINGS |
        (current_style & (WS_VISIBLE | WS_DISABLED));
    const LONG_PTR desired_extended =
        WS_EX_LAYERED | (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST);
    const bool frame_changed = current_style != desired_style;
    if (frame_changed) SetWindowLongPtrW(window_, GWL_STYLE, desired_style);
    if (GetWindowLongPtrW(window_, GWL_EXSTYLE) != desired_extended)
        SetWindowLongPtrW(window_, GWL_EXSTYLE, desired_extended);
    // 0046D0C1 ends WM_SETREDRAW before SetWindowPos/SetWindowRgn;
    // 00468363 then rebinds the other top-level HWNDs independently. Keeping
    // the main redraw guard across those operations leaves WS_VISIBLE
    // temporarily cleared while User32 and the owner-group policy inspect
    // the window. Do not resize/reorder a temporarily invisible main HWND.
    // The final RedrawWindow below publishes the completed skin, not Resume.
    redraw.Resume(false);
    ApplySkinWindowAlpha(EffectiveSkinWindowAlpha(window_));
    const SIZE size = skin_->WindowSize();
    const RECT& saved = settings_.player.player_window;
    const bool place_main = saved_bounds && saved.right > saved.left && saved.bottom > saved.top;
    SetWindowPos(window_, nullptr, place_main ? saved.left : 0, place_main ? saved.top : 0, size.cx, size.cy,
        (place_main ? 0 : SWP_NOMOVE) | SWP_NOZORDER | SWP_NOACTIVATE |
        (frame_changed ? SWP_FRAMECHANGED : 0));
    // SetWindowRgn takes ownership on success.  The old region remains active
    // until this atomic replacement, matching the original rebuild sequence.
    if (!SetWindowRgn(window_, next_region, TRUE)) {
        DeleteObject(next_region);
        return false;
    }
    // FUN_0045DDEE forwards skin+0x498 to FUN_0045775A only after the new
    // package has been validated.  Its sparse Visual.xml fields overwrite
    // the current global visual settings at this point.
    if (apply_visual_settings) ApplySkinVisualSettings();
    const bool custom_icon = !settings_.general.app_icon_file.empty();
    const HICON small_icon = !custom_icon && skin_->Icon()
        ? skin_->Icon() : window_icon_small_;
    const HICON big = !custom_icon && skin_->Icon()
        ? skin_->Icon() : window_icon_big_;
    if (small_icon) SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (big) SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    UpdateTrayIcon();
    UpdateLyricWindowSkin(saved_bounds);
    desktop_lyrics_.SetSkin(&*skin_);
    desktop_lyrics_.ApplySettings();
    UpdatePlaylistWindowSkin(saved_bounds);
    UpdateEqualizerWindowSkin(saved_bounds);
    ApplySkinWindowTopMost();
    ApplyWindowShadow();
    UpdateVisualWindowLayout();
    UpdateVisualFrame();
    // A package may create an auxiliary HWND that the previous package did
    // not have. Apply the same stored alpha after all replacement windows
    // exist, as FUN_004A47C2 does for the original skin-window group.
    ApplySkinWindowAlpha(EffectiveSkinWindowAlpha(window_));
    UpdateMainToolRects();
    ResetSkinInfoScroll();
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return true;
}

void PlayerWindow::RaiseSkinOwnerOnActivation(HWND target, WPARAM activation, LPARAM previous) {
    // 0044F073 / 0046CD66: when an auxiliary popup is activated from
    // another GUI thread, raise its owner without activating it. Merely
    // activating the popup can otherwise leave the player and its siblings
    // behind another application. Internal focus changes and deactivation
    // must leave the existing order (including modeless options) alone.
    if (LOWORD(activation) == WA_INACTIVE) return;
    const HWND owner = GetWindow(target, GW_OWNER);
    if (!IsWindow(owner)) return;
    if (previous && GetWindowThreadProcessId(reinterpret_cast<HWND>(previous), nullptr) ==
            GetCurrentThreadId()) return;
    SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PlayerWindow::ApplySkinWindowTopMost() {
    if (!window_ || !IsWindow(window_)) return;
    const bool main_top = mini_mode_ ? settings_.player.mini_top_most
                                     : settings_.player.top_most;
    const auto apply = [](HWND target, bool topmost, bool owner_changed) {
        if (!target || !IsWindow(target)) return;
        if (!owner_changed &&
            (((GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == topmost))
            return; // Rebinding geometry must not reorder an unchanged window.
        SetWindowPos(target, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                SWP_NOOWNERZORDER);
    };
    // Recover 004A3A66/00464B6C's owner-group change, including modeless
    // sheets. Our asynchronous transitions use NOOWNERZORDER: User32 can
    // leave owned dialogs in the old band, so reconcile them explicitly.
    // Owner first; siblings back-to-front in their CURRENT Z order. Applying
    // a fixed playlist/EQ/lyric/dialog array reverses their user's stacking
    // order whenever several HWNDs change bands together.
    struct Target { HWND window; unsigned depth; unsigned rank; bool topmost; bool band_changed; };
    struct Plan { PlayerWindow* player; bool main_top; unsigned rank{}; std::vector<Target> targets; } plan{this, main_top};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Plan*>(data);
        auto& p = *context.player;
        const unsigned rank = context.rank++;
        const auto is_desktop = [&p](HWND h) {
            return h && (h == p.desktop_lyrics_.ControlHandle() ||
                h == p.desktop_lyrics_.PaintHandle() || h == p.desktop_lyrics_.BarHandle());
        };
        const bool skin_window = candidate == p.window_ || candidate == p.lyric_window_ ||
            candidate == p.playlist_window_ || candidate == p.equalizer_window_ || is_desktop(candidate);
        // Standard owned dialogs inherit their owner's effective pin. Native
        // menus, tooltips, fullscreen surfaces and notification bubbles have
        // their own band policies, and are not ordinary captioned dialogs.
        if (!skin_window && (GetWindowLongPtrW(candidate, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
            return TRUE;
        bool topmost = context.main_top;
        HWND ancestor = candidate;
        for (unsigned depth = 0; ancestor && depth < 64; ++depth) {
            if (ancestor == p.lyric_window_) topmost = topmost || p.ActiveLyricTopMost();
            if (is_desktop(ancestor)) topmost = topmost || p.settings_.desktop_lyric.topmost;
            if (ancestor == p.window_) {
                const bool changed = ((GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost;
                context.targets.push_back({candidate, depth, rank, topmost, changed});
                break;
            }
            ancestor = GetWindow(ancestor, GW_OWNER);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&plan));
    std::sort(plan.targets.begin(), plan.targets.end(), [](const Target& a, const Target& b) {
        return a.depth != b.depth ? a.depth < b.depth : a.rank > b.rank;
    });
    for (const auto& target : plan.targets) {
        bool owner_changed = target.band_changed;
        for (HWND owner = GetWindow(target.window, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER)) {
            const auto found = std::find_if(plan.targets.begin(), plan.targets.end(),
                [owner](const Target& other) { return other.window == owner; });
            if (found != plan.targets.end() && found->band_changed) owner_changed = true;
        }
        // A previously pinned lyric/dialog may retain its TOPMOST bit while
        // its owner's promotion moves that owner ABOVE it. Reassert owned
        // positions when an ancestor changed bands, even if the bit already
        // matches. Without an ancestor change, never reorder a no-op refresh.
        apply(target.window, target.topmost, owner_changed);
    }
    // Hide/show (e.g. changing CS_DROPSHADOW) can also leave an owned
    // surface below its owner without changing either TOPMOST bit. Repair
    // only that broken relation, directly above the owner, not at the front
    // of all the application's dialogs or another application's windows.
    for (const auto& target : plan.targets) {
        const HWND owner = GetWindow(target.window, GW_OWNER);
        if (!IsWindowVisible(target.window) || !IsWindowVisible(owner) || IsIconic(owner)) continue;
        bool above = false;
        for (HWND prior = GetWindow(owner, GW_HWNDPREV); prior; prior = GetWindow(prior, GW_HWNDPREV)) {
            if (prior == target.window) { above = true; break; }
        }
        if (above) continue;
        HWND insert_after = GetWindow(owner, GW_HWNDPREV);
        if (insert_after &&
            (((GetWindowLongPtrW(insert_after, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != target.topmost))
            insert_after = HWND_TOP;
        SetWindowPos(target.window, insert_after, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void PlayerWindow::ApplySkinWindowAlpha(BYTE alpha) {
    rendered_skin_window_alpha_ = alpha;
    for (const HWND target : {window_, lyric_window_, playlist_window_,
                              equalizer_window_}) {
        ApplySkinWindowAlpha(target, alpha);
    }
}

void PlayerWindow::ApplySkinWindowAlpha(HWND target, BYTE alpha) {
    if (!target || !IsWindow(target)) return;
    const LONG_PTR extended = GetWindowLongPtrW(target, GWL_EXSTYLE);
    if ((extended & WS_EX_LAYERED) == 0)
        SetWindowLongPtrW(target, GWL_EXSTYLE, extended | WS_EX_LAYERED);
    COLORREF color_key{};
    DWORD flags = LWA_ALPHA;
    if (target == lyric_window_ && settings_.lyric.transparent) {
        // Match the actual painter, including optional mini-only skin colours.
        color_key = ActiveLyricBackgroundColor();
        flags |= LWA_COLORKEY;
    }
    SetLayeredWindowAttributes(target, color_key, alpha, flags);
}

void PlayerWindow::ApplyWindowShadow() {
    // FUN_004A43E8 applies FUN_004710A5 independently to all four skinned
    // top-level classes, preserving each HWND's visibility across GCL_STYLE.
    for (const HWND target : {window_, lyric_window_, playlist_window_,
                              equalizer_window_}) {
        if (!target || !IsWindow(target)) continue;
        const LONG_PTR previous_style = GetClassLongPtrW(target, GCL_STYLE);
        LONG_PTR style = previous_style;
        if (settings_.player.window_shadow) style |= CS_DROPSHADOW;
        else style &= ~static_cast<LONG_PTR>(CS_DROPSHADOW);
        // 004A43E8/004710A5 is the shadow-option path, not the package
        // rebind path (00468363). Do not hide/show an unchanged class during
        // skin loading: that removes/recreates the taskbar button and fades
        // the owned window group even though all top-level HWNDs survived.
        if (style == previous_style) continue;
        const bool visible = IsWindowVisible(target) != FALSE;
        if (visible) ShowWindow(target, SW_HIDE);
        SetClassLongPtrW(target, GCL_STYLE, style);
        if (visible) ShowWindow(target, SW_SHOW);
    }
    ApplySkinWindowTopMost();
}

void PlayerWindow::UpdateAutoShutdownTimer() {
    if (!window_) return;
    if (!settings_.general.auto_shutdown) {
        if (auto_shutdown_timer_) KillTimer(window_, auto_shutdown_timer_);
        auto_shutdown_timer_ = 0;
        return;
    }
    if (!auto_shutdown_timer_) {
        auto_shutdown_timer_ = SetTimer(
            window_, kAutoShutdownTimer, 1000, nullptr);
    }
}

void PlayerWindow::ShowAutoShutdownDialog() {
    AutoShutdownDialogState state{ResourceModule(), 15};
    const INT_PTR result = DialogBoxParamW(
        state.resources, MAKEINTRESOURCEW(0xe3), window_,
        AutoShutdownDialogProc, reinterpret_cast<LPARAM>(&state));
    if (result == IDOK) static_cast<void>(RequestSystemPowerOff());
}

void PlayerWindow::AnimateSkinWindowAlpha(BYTE alpha) {
    BeginSkinWindowFade(window_, rendered_skin_window_alpha_, alpha, true,
                        kFadeCompleteNone);
}

BYTE PlayerWindow::EffectiveSkinWindowAlpha(HWND target) const noexcept {
    return settings_.player.opaque_when_active &&
            target && GetActiveWindow() == target
        ? static_cast<BYTE>(255) : skin_window_alpha_;
}

void PlayerWindow::SetSkinWindowVisible(HWND target, bool visible) {
    if (!target || !IsWindow(target)) return;
    CompleteSkinWindowFadeForReplacement();
    if (!target || !IsWindow(target) || close_after_skin_window_fade_) return;
    const bool currently_visible = IsWindowVisible(target) != FALSE;
    if (currently_visible == visible) return;

    const auto plan = BuildRecoveredWindowVisibilityFade(
        visible, skin_window_alpha_, settings_.player.opaque_when_active,
        GetActiveWindow() == target, settings_.general.fade_windows);
    if (visible) {
        ApplySkinWindowAlpha(target, plan.from);
        suppress_skin_window_activation_fade_ = true;
        ShowWindow(target, SW_SHOW);
        BringWindowToTop(target);
        ApplySkinWindowTopMost();
        suppress_skin_window_activation_fade_ = false;
        BeginSkinWindowFade(target, plan.from, plan.to, false,
                            kFadeCompleteNone, plan.restore);
    } else {
        BeginSkinWindowFade(target, plan.from, plan.to, false,
                            kFadeCompleteHideTarget, plan.restore);
    }
}

void PlayerWindow::BeginSkinWindowFade(HWND target, BYTE from, BYTE to,
                                       bool group, int completion,
                                       BYTE restore_alpha) {
    CompleteSkinWindowFadeForReplacement();
    if (!window_ || !IsWindow(window_) ||
        (close_after_skin_window_fade_ &&
         completion != kFadeCompleteDestroyMain)) return;
    const DWORD distance = static_cast<DWORD>(
        std::abs(static_cast<int>(to) - static_cast<int>(from)));
    const bool startup_transition =
        completion == kFadeCompleteFinishStartup ||
        completion == kFadeCompleteStartupMinimize;
    skin_window_fade_ = SkinWindowFadeState{
        target, from, to, restore_alpha, from,
        startup_transition ? 0 : GetTickCount64(), distance,
        group, completion};
    if (group) ApplySkinWindowAlpha(from);
    else ApplySkinWindowAlpha(target, from);

    // 0044EFBE always installs its exact terminal alpha, but only enters the
    // timed interpolation when Fade_Windows is enabled and at least one of
    // its five-alpha-unit samples exists.
    if (!settings_.general.fade_windows || distance < 5 ||
        !SetTimer(window_, kSkinWindowFadeTimer,
                  kSkinWindowFadeIntervalMs, nullptr)) {
        FinishSkinWindowFade();
    }
}

void PlayerWindow::AdvanceSkinWindowFade() {
    if (!skin_window_fade_) {
        if (window_) KillTimer(window_, kSkinWindowFadeTimer);
        return;
    }
    auto& fade = *skin_window_fade_;
    // Create() installs the startup timer before TTPlayer_wWinMain enters its
    // message loop.  Starting the clock there lets playlist/audio restoration
    // consume the complete (at most 255 ms) interval, so the first WM_TIMER
    // jumps straight to the endpoint.  004C01CD starts measuring only while it
    // performs the visible transition; use the first dispatched timer as the
    // equivalent boundary for the non-blocking reconstruction.
    if (fade.started == 0) fade.started = GetTickCount64();
    const ULONGLONG elapsed64 = GetTickCount64() - fade.started;
    const DWORD elapsed = static_cast<DWORD>(
        std::min<ULONGLONG>(elapsed64, fade.duration_ms));
    const BYTE alpha = InterpolateRecoveredWindowAlpha(
        fade.from, fade.to, elapsed, fade.duration_ms);
    if (alpha != fade.last) {
        fade.last = alpha;
        if (fade.group) {
            ApplySkinWindowAlpha(alpha);
            for (const HWND candidate : {window_, lyric_window_,
                                          playlist_window_, equalizer_window_}) {
                if (candidate && IsWindowVisible(candidate))
                    RedrawWindow(candidate, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
            }
        } else if (fade.target && IsWindow(fade.target)) {
            ApplySkinWindowAlpha(fade.target, alpha);
            if (IsWindowVisible(fade.target))
                RedrawWindow(fade.target, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
    if (elapsed >= fade.duration_ms) FinishSkinWindowFade();
}

void PlayerWindow::FinishSkinWindowFade() {
    if (!skin_window_fade_) return;
    const SkinWindowFadeState fade = *skin_window_fade_;
    skin_window_fade_.reset();
    if (window_) KillTimer(window_, kSkinWindowFadeTimer);
    if (fade.group) ApplySkinWindowAlpha(fade.to);
    else ApplySkinWindowAlpha(fade.target, fade.to);

    switch (fade.completion) {
    case kFadeCompleteHideTarget:
        if (fade.target && IsWindow(fade.target)) {
            suppress_skin_window_activation_fade_ = true;
            ShowWindow(fade.target, SW_HIDE);
            suppress_skin_window_activation_fade_ = false;
            // 0044E2FB restores +0x60 after the HWND is hidden.  Keep that
            // invisible write separate from the persistent configured alpha.
            ApplySkinWindowAlpha(fade.target, fade.restore_alpha);
        }
        break;
    case kFadeCompleteDestroyMain:
        close_skin_window_fade_finished_ = true;
        FinishCloseWhenFadesComplete();
        break;
    case kFadeCompleteSwitchMini:
        mini_mode_fade_continuation_ = true;
        ToggleMiniMode();
        break;
    case kFadeCompleteFinishMini:
        mini_mode_fade_pending_ = false;
        if (mini_mode_fade_queued_ && !close_after_skin_window_fade_) {
            mini_mode_fade_queued_ = false;
            ToggleMiniMode();
        }
        break;
    case kFadeCompleteStartupMinimize:
        startup_skin_window_fade_pending_ = false;
        if (window_ && IsWindow(window_)) ShowWindow(window_, SW_MINIMIZE);
        break;
    case kFadeCompleteFinishStartup:
        startup_skin_window_fade_pending_ = false;
        // TTPlayer_wWinMain activates the player only after 004C01CD has
        // restored the configured startup alpha.  Our asynchronous startup
        // phase can receive that activation early, so replay its observable
        // OpaqueWhenActive result only after the 0 -> configured transition
        // has reached its endpoint.
        if (settings_.player.opaque_when_active &&
            transparency_percent_ > 0 && window_ && IsWindow(window_) &&
            GetActiveWindow() == window_ && rendered_skin_window_alpha_ != 255) {
            AnimateSkinWindowAlpha(255);
        }
        break;
    default:
        break;
    }
}

void PlayerWindow::PollCloseAudioFade() {
    if (!close_after_skin_window_fade_ ||
        !close_waiting_for_audio_fade_) {
        if (window_) KillTimer(window_, kCloseAudioFadeTimer);
        return;
    }

    const bool pending = audio_.StopFadePending();
    const bool expired = close_audio_fade_deadline_ != 0 &&
                         GetTickCount64() >= close_audio_fade_deadline_;
    if (pending && !expired) return;

    // 00461ADB stops pumping after FadeDuration[3] + 500 ms even if a broken
    // output driver never acknowledges its final volume step. Destruction's
    // existing bounded AudioEngine::Stop path then requests hard teardown.
    close_waiting_for_audio_fade_ = false;
    close_audio_fade_deadline_ = 0;
    if (window_) KillTimer(window_, kCloseAudioFadeTimer);
    FinishCloseWhenFadesComplete();
}

void PlayerWindow::FinishCloseWhenFadesComplete() {
    if (!close_after_skin_window_fade_ ||
        close_waiting_for_audio_fade_ ||
        !close_skin_window_fade_finished_) {
        return;
    }
    if (window_) KillTimer(window_, kCloseAudioFadeTimer);
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    close_after_skin_window_fade_ = false;
}

void PlayerWindow::CancelSkinWindowFade() noexcept {
    if (window_) KillTimer(window_, kSkinWindowFadeTimer);
    skin_window_fade_.reset();
    startup_skin_window_fade_pending_ = false;
}

void PlayerWindow::CompleteSkinWindowFadeForReplacement() {
    // 0044EFBE is synchronous. The timer implementation deliberately keeps
    // the UI responsive, so a second command can arrive before its predecessor
    // would have returned in the original. Commit every resulting structural
    // stage (fade-out -> mini rebuild -> fade-in) before accepting that command.
    for (int stage = 0; skin_window_fade_ && stage < 4; ++stage) {
        FinishSkinWindowFade();
        if (!window_ || !IsWindow(window_)) break;
    }
}

void PlayerWindow::ApplySkinProfileWindowState() {
    if (!skin_ || !window_) return;
    // FUN_0045D5FA loads the target sidecar into the same global rectangles
    // consumed later by FUN_00464B6C. Keep the cached toggle rectangles in
    // lockstep; otherwise the main mini window comes from the previous skin
    // while LyricWnd2 comes from the new one.
    normal_window_bounds_ = settings_.player.player_window;
    mini_window_bounds_ = settings_.player.mini_player_window;
    have_normal_window_bounds_ = normal_window_bounds_.right > normal_window_bounds_.left &&
                                 normal_window_bounds_.bottom > normal_window_bounds_.top;
    have_mini_window_bounds_ = mini_window_bounds_.right > mini_window_bounds_.left &&
                               mini_window_bounds_.bottom > mini_window_bounds_.top;
    const RECT& active = mini_mode_ ? mini_window_bounds_ : normal_window_bounds_;
    const bool have_active = mini_mode_ ? have_mini_window_bounds_
                                        : have_normal_window_bounds_;
    if (have_active) {
        SetWindowPos(window_, nullptr, active.left, active.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const auto place = [](HWND window, const RECT& saved, SIZE native,
                          const RECT* resize) {
        if (!window || saved.right <= saved.left || saved.bottom <= saved.top ||
            native.cx <= 0 || native.cy <= 0) return;
        int width = native.cx;
        int height = native.cy;
        if (resize && resize->right > resize->left &&
            resize->bottom > resize->top) {
            width = std::max<int>(width, saved.right - saved.left);
            height = std::max<int>(height, saved.bottom - saved.top);
        }
        SetWindowPos(window, nullptr, saved.left, saved.top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };

    if (skin_->Lyric().valid && lyric_window_) {
        if (!desktop_lyric_mode_) ApplyActiveLyricWindowState();
    }
    if (skin_->Playlist().valid && playlist_window_) {
        place(playlist_window_, settings_.player.playlist_window,
              skin_->Playlist().background.size, &skin_->Playlist().resize_rect);
        LayoutPlaylistListControls();
        UpdatePlaylistWindowRegion();
        UpdatePlaylistToolRects();
        ShowWindow(playlist_window_, !mini_mode_ && settings_.player.playlist_visible
            ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (skin_->Equalizer().valid && equalizer_window_) {
        place(equalizer_window_, settings_.player.equalizer_window,
              skin_->Equalizer().background.size, nullptr);
        UpdateEqualizerWindowRegion();
        UpdateEqualizerToolRects();
        ShowWindow(equalizer_window_, !mini_mode_ && settings_.player.equalizer_visible
            ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void PlayerWindow::ToggleMute() {
    settings_.player.mute = !settings_.player.mute;
    if (settings_.player.mute) {
        if (settings_.player.volume > 0) volume_before_mute_ = settings_.player.volume;
        audio_.SetVolume(0.0F);
    } else {
        if (settings_.player.volume == 0) settings_.player.volume = volume_before_mute_;
        audio_.SetVolume(static_cast<float>(settings_.player.volume) / 100.0F);
    }
}

void PlayerWindow::SetSkinVolumeFromPoint(POINT point) {
    if (!skin_) return;
    const auto* volume = FindActiveSkinElement(L"volume");
    if (!volume) return;
    if (volume->vertical) {
        const int height = volume->bounds.bottom - volume->bounds.top;
        const int thumb = volume->thumb_image ? volume->thumb_size.cy : 0;
        const int span = height - 2 - thumb;
        if (span <= 0) return;
        const int position = volume->bounds.bottom - 1 - thumb / 2 - point.y;
        settings_.player.volume = std::clamp(MulDiv(position, 100, span), 0, 100);
    } else {
        const int width = volume->bounds.right - volume->bounds.left;
        const int thumb = volume->thumb_image ? volume->thumb_size.cx / 4 : 0;
        const int span = width - 2 - thumb;
        if (span <= 0) return;
        const int position = point.x - volume->bounds.left - 1 - thumb / 2;
        settings_.player.volume = std::clamp(MulDiv(position, 100, span), 0, 100);
    }
    audio_.SetVolume(static_cast<float>(settings_.player.volume) / 100.0F);
    settings_.player.mute = false;
    InvalidateRect(window_, &volume->bounds, FALSE);
}

void PlayerWindow::SetSkinProgressFromPoint(POINT point) {
    if (!skin_) return;
    const auto* progress = FindActiveSkinElement(L"progress");
    const auto duration = audio_.Duration();
    if (!progress || duration.count() <= 0) return;
    int value{};
    constexpr int slider_inset = 1;
    if (progress->vertical) {
        const int height = progress->bounds.bottom - progress->bounds.top;
        const int thumb = progress->thumb_image ? progress->thumb_size.cy : 0;
        const int span = height - slider_inset * 2 - thumb;
        if (span <= 0) return;
        const int first_center = progress->bounds.top + slider_inset + thumb / 2;
        value = span - std::clamp<int>(point.y - first_center, 0, span);
        progress_tracking_position_ = std::chrono::milliseconds(
            duration.count() * value / span);
    } else {
        const int width = progress->bounds.right - progress->bounds.left;
        const int thumb = progress->thumb_image ? progress->thumb_size.cx / 4 : 0;
        const int span = width - slider_inset * 2 - thumb;
        if (span <= 0) return;
        const int first_center = progress->bounds.left + slider_inset + thumb / 2;
        value = std::clamp<int>(point.x - first_center, 0, span);
        progress_tracking_position_ = std::chrono::milliseconds(
            duration.count() * value / span);
    }
    // 00428DCD ignores timer updates while tracking; 0045CE05 previews the
    // LED without moving the decoder/lyrics clock until the release seek.
    InvalidateRect(window_, &progress->bounds, FALSE);
    if (const auto* led = FindActiveSkinElement(L"led"))
        InvalidateRect(window_, &led->bounds, FALSE);
}

void PlayerWindow::DrawButton(const DRAWITEMSTRUCT& item) const {
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const HBRUSH brush = CreateSolidBrush(pressed ? RGB(34, 139, 105) : kPanel);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    const HPEN pen = CreatePen(PS_SOLID, 1, (item.itemState & ODS_FOCUS) ? kAccent : RGB(62, 72, 82));
    const HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    const HGDIOBJ old_brush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(pen);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, kText);
    SelectObject(item.hDC, ui_font_);
    RECT text_rect = item.rcItem;
    DrawTextW(item.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PlayerWindow::RefreshPlaylist() {
    if (playlist_view_) {
        SendMessageW(playlist_view_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(playlist_view_, LB_RESETCONTENT, 0, 0);
        for (const auto& track : ActivePlaylist().Tracks()) {
            const auto name = DisplayName(track);
            SendMessageW(playlist_view_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        SendMessageW(playlist_view_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(playlist_view_, nullptr, TRUE);
    }
    if (playlist_window_) {
        playlist_list_selection_ = playlists_.Size()
            ? std::optional<size_t>{playlists_.ActiveIndex()} : std::nullopt;
        playlist_list_focus_ = playlist_list_selection_;
        LayoutPlaylistListControls();
        UpdatePlaylistItemTipRects();
        InvalidateRect(playlist_window_, nullptr, FALSE);
    }
}

void PlayerWindow::RefreshPlaybackUi() {
    UpdateTaskbarPlayback();
    UpdateMainWindowCaption();
    const auto text = PlaybackStatusText();
    if (status_) SetWindowTextW(status_, text.c_str());
    const auto duration = audio_.Duration().count();
    const auto position = audio_.Position().count();
    if (progress_) {
        SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(progress_, PBM_SETRANGE32, 0, static_cast<LPARAM>(duration));
        SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(position), 0);
    }
    std::wstring current_line;
    if (lyric_control_ || desktop_lyrics_.ControlHandle()) {
        if (const auto line = lyrics_.LineAt(audio_.Position());
            line && *line < lyrics_.lines.size()) {
            try { current_line = core::Utf8ToWide(lyrics_.lines[*line].text); }
            catch (const std::exception&) {}
        } else if (lyrics_.lines.empty()) {
            if (const auto* track = PlaybackTrackForUi())
                current_line = DisplayName(*track);
        }
        if (lyric_control_) {
            SetWindowTextW(lyric_control_, current_line.c_str());
            if (audio_.State() != audio::PlaybackState::playing)
                InvalidateRect(lyric_control_, nullptr, FALSE);
        }
    }
    desktop_lyrics_.SetFallbackText(current_line);
    desktop_lyrics_.UpdatePlayback(
        audio_.Position(), audio_.State() == audio::PlaybackState::playing);
    UpdateDiscordPresence();
    if (settings_.visual.type == 4 ||
        audio_.State() == audio::PlaybackState::stopped ||
        audio_.State() == audio::PlaybackState::failed)
        UpdateVisualFrame();
    if (skin_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::UpdateDiscordPresence() {
    if (!settings_.general.send_title_to_msn) {
        discord_presence_.Clear();
        return;
    }
    const auto clock = audio_.ClockSnapshot();
    const auto state = clock.state;
    if (state != audio::PlaybackState::playing &&
        state != audio::PlaybackState::paused) {
        discord_presence_.Clear();
        return;
    }

    const auto* track = PlaybackTrackForUi();
    if (!track) {
        discord_presence_.Clear();
        return;
    }

    auto presence = integrations::BuildDiscordTrackPresence(
        *track, clock.position, clock.duration,
        state == audio::PlaybackState::playing
            ? integrations::DiscordPlaybackState::playing
            : integrations::DiscordPlaybackState::paused,
        audio::AudioEngine::IsNetworkMediaLocation(track->path));
    presence.timeline_revision = clock.timeline_revision;
    presence.seek_pending = clock.seek_pending;
    presence.lyrics_enabled = settings_.general.discord_sync_lyrics;
    integrations::ApplyDiscordLyric(presence, lyrics_);
    discord_presence_.Update(std::move(presence), clock.observed_at);
}

void PlayerWindow::UpdateMainWindowCaption() {
    if (!window_) return;
    const bool scrolling = settings_.general.scroll_title &&
        audio_.State() == audio::PlaybackState::playing;
    std::wstring source = display_title_;
    if (scrolling) source += L"  ";
    if (source == window_caption_source_ &&
        scrolling == window_caption_scrolling_) return;
    window_caption_source_ = std::move(source);
    window_caption_scrolling_ = scrolling;
    SetWindowTextW(window_, window_caption_source_.c_str());
}

void PlayerWindow::RotateMainWindowCaption() {
    if (!window_ || !window_caption_scrolling_ ||
        window_caption_source_.empty()) return;
    const int length = GetWindowTextLengthW(window_);
    if (length <= 1) return;
    std::wstring current(static_cast<size_t>(length) + 1U, L'\0');
    GetWindowTextW(window_, current.data(), length + 1);
    current.resize(static_cast<size_t>(length));
    const wchar_t* next = CharNextW(current.c_str());
    if (!next || next <= current.c_str() || *next == L'\0') return;
    const size_t first = static_cast<size_t>(next - current.c_str());
    current = current.substr(first) + current.substr(0, first);
    SetWindowTextW(window_, current.c_str());
}

void PlayerWindow::RebuildSkinInfoItems(bool include_audio_details) {
    info_items_.clear();
    info_items_.push_back(display_title_);
    const auto* playback_track = PlaybackTrackForUi();
    if (!include_audio_details || !playback_track) return;

    // FUN_0045CA57 adds the primary display title first, expands resource
    // 0x81CA, then appends each non-empty line in resource order:
    // Title, Artist, Album, Format, Duration.
    const auto& track = *playback_track;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    try { if (!track.title.empty()) title = core::Utf8ToWide(track.title); }
    catch (const std::exception&) {}
    try { if (!track.artist.empty()) artist = core::Utf8ToWide(track.artist); }
    catch (const std::exception&) {}
    try { if (!track.album.empty()) album = core::Utf8ToWide(track.album); }
    catch (const std::exception&) {}
    const auto format = FormatAudioDescription(audio_.Format());
    const auto duration = audio_.Duration().count() > 0
        ? FormatInfoDuration(audio_.Duration()) : std::wstring{};
    auto resource_template = ResourceText(0x81ca);
    size_t begin = 0;
    while (begin <= resource_template.size()) {
        const size_t end = resource_template.find(L'|', begin);
        auto line = resource_template.substr(begin,
            end == std::wstring::npos ? end : end - begin);
        ReplaceAll(line, L"%(Title)", title);
        ReplaceAll(line, L"%(Artist)", artist);
        ReplaceAll(line, L"%(Album)", album);
        ReplaceAll(line, L"%(Format)", format);
        ReplaceAll(line, L"%(Duration)", duration);
        if (line.find(L"%(") == std::wstring::npos) {
            const auto colon = line.find(L':');
            if (colon == std::wstring::npos ||
                line.find_first_not_of(L" \t", colon + 1) != std::wstring::npos)
                info_items_.push_back(std::move(line));
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
}

void PlayerWindow::ResetSkinInfoScroll() {
    if (!window_) return;
    KillTimer(window_, kInfoItemTimer);
    KillTimer(window_, kInfoTransitionTimer);
    KillTimer(window_, kInfoScrollTimer);
    if (info_items_.empty()) info_items_.push_back(display_title_);
    info_item_index_ = 0;
    info_scroll_offset_ = 0;
    info_scroll_maximum_ = 0;
    info_scroll_direction_ = 0;
    info_scroll_hold_ticks_ = 0;
    info_vertical_offset_ = 0;
    StartSkinInfoItem();
}

void PlayerWindow::StartSkinInfoItem() {
    if (!window_) return;
    KillTimer(window_, kInfoItemTimer);
    KillTimer(window_, kInfoTransitionTimer);
    KillTimer(window_, kInfoScrollTimer);
    info_scroll_offset_ = 0;
    info_scroll_maximum_ = 0;
    info_scroll_direction_ = 0;
    info_scroll_hold_ticks_ = 0;
    info_vertical_offset_ = 0;
    if (!skin_ || info_items_.empty()) return;
    if (info_item_index_ >= info_items_.size()) info_item_index_ = 0;
    const auto* info = FindActiveSkinElement(L"info");
    if (!info) return;

    const HDC dc = GetDC(window_);
    if (!dc) return;
    const HFONT font = CreateSkinFont(*info);
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    RECT measured{};
    // FUN_0040939C passes exactly 0xC24: DT_CALCRECT, DT_SINGLELINE,
    // DT_NOPREFIX and DT_VCENTER.
    DrawTextW(dc, info_items_[info_item_index_].c_str(), -1, &measured,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
    if (font) {
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    ReleaseDC(window_, dc);

    const int available = info->bounds.right - info->bounds.left;
    info_scroll_maximum_ = std::max(
        0, static_cast<int>(measured.right - measured.left) - available);
    if (info_scroll_maximum_ > 0) {
        info_scroll_direction_ = 1;
        info_scroll_hold_ticks_ = kInfoEndpointHoldTicks;
        SetTimer(window_, kInfoScrollTimer, kInfoAnimationIntervalMs, nullptr);
    } else if (info_items_.size() > 1) {
        const auto interval = DecodePackedRuntimeOption(
            settings_.general.title_slide_interval, 1, 100);
        if (interval.enabled) {
            SetTimer(window_, kInfoItemTimer,
                     static_cast<UINT>(interval.value * 1000), nullptr);
        }
    }
    InvalidateRect(window_, &info->bounds, FALSE);
}

void PlayerWindow::AdvanceSkinInfoScroll(UINT_PTR timer) {
    if (!window_ || !skin_ || info_items_.empty()) {
        if (window_) KillTimer(window_, timer);
        return;
    }
    const auto* info = FindActiveSkinElement(L"info");
    if (!info) {
        KillTimer(window_, timer);
        return;
    }

    if (timer == kInfoItemTimer) {
        KillTimer(window_, kInfoItemTimer);
        info_scroll_direction_ = 0;
        info_scroll_offset_ = 0;
        info_vertical_offset_ = 0;
        SetTimer(window_, kInfoTransitionTimer, kInfoAnimationIntervalMs, nullptr);
    } else if (timer == kInfoTransitionTimer) {
        ++info_vertical_offset_;
        const int height = info->bounds.bottom - info->bounds.top;
        if (info_vertical_offset_ == height) {
            KillTimer(window_, kInfoTransitionTimer);
            info_item_index_ = (info_item_index_ + 1) % info_items_.size();
            StartSkinInfoItem();
        }
    } else if (timer == kInfoScrollTimer) {
        if (info_scroll_hold_ticks_ > 0) {
            --info_scroll_hold_ticks_;
        } else {
            const int previous_direction = info_scroll_direction_;
            if (info_scroll_direction_ == 1) {
                if (info_scroll_offset_ > -info_scroll_maximum_)
                    --info_scroll_offset_;
                else
                    info_scroll_direction_ = -1;
            } else if (info_scroll_offset_ < 0) {
                ++info_scroll_offset_;
            } else {
                info_scroll_direction_ = 1;
            }

            if (previous_direction != info_scroll_direction_) {
                info_scroll_hold_ticks_ = kInfoEndpointHoldTicks;
                if (info_scroll_direction_ == 1 && info_scroll_offset_ == 0) {
                    KillTimer(window_, kInfoScrollTimer);
                    info_vertical_offset_ = 0;
                    if (info_items_.size() > 1) {
                        info_scroll_hold_ticks_ = 0;
                        const auto interval = DecodePackedRuntimeOption(
                            settings_.general.title_slide_interval, 1, 100);
                        if (interval.enabled) {
                            SetTimer(window_, kInfoItemTimer,
                                static_cast<UINT>(interval.value * 1000), nullptr);
                        }
                    } else {
                        SetTimer(window_, kInfoScrollTimer,
                                 kInfoAnimationIntervalMs, nullptr);
                    }
                }
            }
        }
    }
    InvalidateRect(window_, &info->bounds, FALSE);
}

bool PlayerWindow::PlayCurrent(bool report_error) {
    if (natural_completion_dispatch_) {
        // Playback/@TracksInterval is expressed in seconds by dialog 259.
        // Defer only the actual decoder request: OnPlayComplete still chooses
        // the next row/list synchronously, while the existing UI timer keeps
        // the window responsive during the silent interval.
        pending_natural_play_ = true;
        pending_natural_play_tick_ = GetTickCount64() +
            TrackIntervalMilliseconds(settings_.playback.track_interval);
        return true;
    }
    pending_natural_play_ = false;
    pending_failed_advance_ = false;
    if (!media_library_playback_active_) {
        if (!playing_playlist_index_ ||
            *playing_playlist_index_ >= playlists_.Size())
            playing_playlist_index_ = playlists_.ActiveIndex();
    }
    const auto playback_playlist_index = playing_playlist_index_;
    if (!current_ && !OpenedTrack() &&
        !PlaybackPlaylist().Tracks().empty()) current_ = 0;
    const bool indexed_playback = HasPlaybackTrack();
    const auto* requested = indexed_playback
        ? &PlaybackPlaylist().Tracks()[*current_] : OpenedTrack();
    if (!requested) return false;
    // The decoder request may replace the object which backs opened_track_.
    // Keep a stable value while the engine and metadata paths run.
    const playlist::Track requested_track = *requested;
    // 0047FEA3 sends the play request and then 0047FB0C publishes +0x1c.
    // Publish the per-list marker at request time; the failed-open path below
    // performs the later invalidation.
    if (indexed_playback && PlaybackPlaylist().SetPlayingRow(*current_) &&
        playback_playlist_index)
        playlists_.MarkDirty(*playback_playlist_index);
    if (!audio_.Play(requested_track.path, requested_track.subtrack)) {
        playback_source_open_ = false;
        opened_track_.reset();
        if (indexed_playback && PlaybackPlaylist().SetDuration(*current_, -1) &&
            playback_playlist_index)
            playlists_.MarkDirty(*playback_playlist_index);
        // FUN_0047FEA3:0047FF9E publishes +0x1c through 0047FB0C even
        // when the synchronous player request reports failure.
        ClearPersistedPlaybackIdentity();
        pending_failed_advance_ = ShouldAdvanceAfterPlaybackFailure(
            settings_.playback.stop_when_fail);
        if (report_error && settings_.playback.stop_when_fail) ShowAudioError();
        return false;
    }
    // CSettings_SerializeXml persists these adjacent Player fields at
    // 004B6BDB..004B6C7C.  Update them only after the requested source has
    // been accepted by the engine; merely moving the playlist caret must not
    // replace the last-playing identity.
    settings_.player.playing_file_name = requested_track.path.wstring();
    settings_.player.playing_file_subtrack = requested_track.subtrack;
    settings_.player.playing_time = 0;
    playback_source_open_ = true;
    opened_track_ = requested_track;
    const auto metadata = audio_.Metadata();
    if (indexed_playback) try {
        bool metadata_changed = PlaybackPlaylist().SetMetadata(
                *current_,
                metadata.title.empty() ? std::string{} :
                    core::WideToUtf8(metadata.title),
                metadata.artist.empty() ? std::string{} :
                    core::WideToUtf8(metadata.artist),
                metadata.album.empty() ? std::string{} :
                    core::WideToUtf8(metadata.album));
        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(metadata.entries.size() + 3);
        for (const auto& [name, value] : metadata.entries) {
            if (!name.empty())
                entries.emplace_back(core::WideToUtf8(name),
                                     core::WideToUtf8(value));
        }
        if (!metadata.title.empty())
            entries.emplace_back("Title", core::WideToUtf8(metadata.title));
        if (!metadata.artist.empty())
            entries.emplace_back("Artist", core::WideToUtf8(metadata.artist));
        if (!metadata.album.empty())
            entries.emplace_back("Album", core::WideToUtf8(metadata.album));
        const auto format = audio_.Format();
        const std::uint64_t bitrate =
            static_cast<std::uint64_t>(format.bytes_per_second) * 8U;
        metadata_changed |= PlaybackPlaylist().SetExtendedMetadata(
            *current_, std::move(entries),
            format.codec_name.empty() ? std::string{} :
                core::WideToUtf8(format.codec_name),
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                bitrate, std::numeric_limits<std::uint32_t>::max())),
            format.sample_rate);
        if (metadata_changed) {
            if (playback_playlist_index)
                playlists_.MarkDirty(*playback_playlist_index);
            RefreshPlaylist();
            const auto& updated = PlaybackPlaylist().Tracks()[*current_];
            display_title_ = std::to_wstring(*current_ + 1) + L"." +
                             DisplayName(updated);
            display_artist_ = ArtistName(updated, ResourceText(0x8ca5));
            if (title_) SetWindowTextW(title_, display_title_.c_str());
            if (artist_) SetWindowTextW(artist_, display_artist_.c_str());
        }
    } catch (const std::exception&) {
    }
    // The decoder-open completion in the original updates the playlist item
    // metadata before invalidating ListCtrl.  Persist the newly known WAV
    // duration so the right-aligned duration column appears immediately.
    const auto duration = audio_.Duration().count();
    if (indexed_playback && duration >= 0 && duration <= INT_MAX &&
        PlaybackPlaylist().SetDuration(*current_, static_cast<int>(duration))) {
        if (playback_playlist_index)
            playlists_.MarkDirty(*playback_playlist_index);
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
    }
    // Store the post-reader metadata as well as path/subtrack.  This copy is
    // the UI's CPlayItem once a later Clear/Remove invalidates current_.
    if (indexed_playback && HasPlaybackTrack())
        opened_track_ = PlaybackPlaylist().Tracks()[*current_];
    // FUN_0045CA57 rebuilds and starts the scrolling info immediately after
    // the decoder/open path succeeds, rather than waiting for timer 10.
    RebuildSkinInfoItems(true);
    ResetSkinInfoScroll();
    if (lyric_path_.empty()) LoadCurrentLyrics();
    playback_was_active_ = true;
    RefreshPlaybackUi();
    ShowPlaybackOpenTip();
    return true;
}

void PlayerWindow::ClearPersistedPlaybackIdentity() noexcept {
    settings::ClearPlaybackIdentity(settings_.player);
}

void PlayerWindow::RestoreStartupPlayback() {
    const auto restore_plan = settings::MakeStartupPlaybackPlan(settings_);
    const auto remembered_path = settings_.player.playing_file_name;
    const int remembered_subtrack = settings_.player.playing_file_subtrack;
    if (remembered_path.empty()) {
        ClearPersistedPlaybackIdentity();
        return;
    }

    const auto same_path = [&remembered_path](const std::filesystem::path& value) {
        if (_wcsicmp(value.c_str(), remembered_path.c_str()) == 0) return true;
        // Numbered TTBL files normally contain absolute paths, but older
        // lists can retain a relative spelling. Match their normalized
        // absolute identity without requiring the media file to exist.
        std::error_code left_error;
        std::error_code right_error;
        const auto left = std::filesystem::absolute(value, left_error);
        const auto right = std::filesystem::absolute(
            std::filesystem::path(remembered_path), right_error);
        return !left_error && !right_error &&
            _wcsicmp(left.lexically_normal().c_str(),
                     right.lexically_normal().c_str()) == 0;
    };

    std::optional<std::pair<size_t, size_t>> found;
    // Prefer the persisted active list, then search the remaining catalog.
    // This makes duplicate entries deterministic while retaining a playing
    // item that was stored in a non-active list.
    for (size_t pass = 0; pass < playlists_.Size() && !found; ++pass) {
        const size_t list_index = pass == 0 ? playlists_.ActiveIndex() :
            (pass <= playlists_.ActiveIndex() ? pass - 1 : pass);
        const auto& tracks = playlists_.At(list_index).Tracks();
        for (size_t track_index = 0; track_index < tracks.size(); ++track_index) {
            const auto& track = tracks[track_index];
            if (track.subtrack == remembered_subtrack && same_path(track.path)) {
                found = std::pair{list_index, track_index};
                break;
            }
        }
    }
    if (!found) {
        ClearPersistedPlaybackIdentity();
        return;
    }

    SelectTrackFrom(found->first, found->second, false);
    if (!restore_plan.should_play) return;
    if (!PlayCurrent()) return;
    if (restore_plan.resume_position_ms > 0) {
        audio_.Seek(std::chrono::milliseconds(restore_plan.resume_position_ms));
        // The resume offset is a one-shot startup input. The live position is
        // captured again during shutdown if this source remains open.
        settings_.player.playing_time = 0;
    }
}

void PlayerWindow::SelectTrack(size_t index, bool start_playback) {
    SelectTrackFrom(playlists_.ActiveIndex(), index, start_playback);
}

void PlayerWindow::SelectTrackFrom(size_t playlist_index, size_t index,
                                   bool start_playback) {
    if (playlist_index >= playlists_.Size() ||
        index >= playlists_.At(playlist_index).Tracks().size()) return;
    media_library_playback_active_ = false;
    media_library_playback_.Clear();
    playing_playlist_index_ = playlist_index;
    current_ = index;
    RememberPlaylistRow(playlist_index, index);
    if (playlist_index == playlists_.ActiveIndex()) {
        playlist_selection_ = index;
        playlist_selection_anchor_ = index;
        if (!playlist_selected_rows_.contains(index)) {
            playlist_selected_rows_.clear();
            playlist_selected_rows_.insert(index);
        }
        EnsurePlaylistSelectionVisible();
        if (playlist_view_) SendMessageW(playlist_view_, LB_SETCURSEL, index, 0);
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
    }
    const auto& track = playlists_.At(playlist_index).Tracks()[index];
    const auto name = DisplayName(track);
    const auto artist = ArtistName(track, ResourceText(0x8ca5));
    // FUN_0045CA57 prefixes the current one-based playlist index before it
    // writes the scrolling info control (for example "1.Track title").
    display_title_ = std::to_wstring(index + 1) + L"." + name;
    RebuildSkinInfoItems(false);
    ResetSkinInfoScroll();
    display_artist_ = artist;
    associated_lyric_path_.clear();
    LoadCurrentLyrics();
    if (title_) SetWindowTextW(title_, display_title_.c_str());
    if (artist_) SetWindowTextW(artist_, artist.c_str());
    if (skin_) InvalidateRect(window_, nullptr, FALSE);
    UpdateVisualFrame();
    UpdateMainWindowCaption();
    if (start_playback) PlayCurrent();
}

void PlayerWindow::SelectRelative(bool next) {
    if (media_library_playback_active_) {
        auto& list = media_library_playback_;
        if (list.Tracks().empty()) return;
        const size_t selected = current_.value_or(
            next ? list.Tracks().size() - 1 : 0);
        playlist::PlayMode mode = playlist::PlayMode::sequential;
        if (settings_.player.play_mode == 1)
            mode = playlist::PlayMode::repeat_one;
        else if (settings_.player.play_mode == 3)
            mode = playlist::PlayMode::repeat_all;
        else if (settings_.player.play_mode == 4)
            mode = playlist::PlayMode::shuffle;
        const auto target = next ? list.Next(selected, mode)
                                 : list.Previous(selected, mode);
        if (target) SelectMediaLibraryPlaybackTrack(*target, true);
        return;
    }
    const size_t playlist_index = playing_playlist_index_.value_or(playlists_.ActiveIndex());
    auto& list = playlists_.At(playlist_index);
    if (list.Tracks().empty()) return;
    const size_t current = current_.value_or(next ? list.Tracks().size() - 1 : 0);
    playlist::PlayMode mode = playlist::PlayMode::sequential;
    if (settings_.player.play_mode == 1) mode = playlist::PlayMode::repeat_one;
    else if (settings_.player.play_mode == 3) mode = playlist::PlayMode::repeat_all;
    else if (settings_.player.play_mode == 4) mode = playlist::PlayMode::shuffle;
    const auto selected = next ? list.Next(current, mode) : list.Previous(current, mode);
    if (selected) SelectTrackFrom(playlist_index, *selected, true);
}

void PlayerWindow::AdvanceAfterNaturalEnd() {
    // CPlayerWnd::OnPlayComplete (0045BD69) is not the same operation as a
    // user pressing Next.  In particular, single mode stops, sequential mode
    // does not wrap, and AutoSwitchList advances the catalogue with different
    // wrap rules for sequential and repeat-all.
    if (media_library_playback_active_) {
        auto& list = media_library_playback_;
        const size_t count = list.Tracks().size();
        const size_t playing = list.PlayingRow().value_or(
            current_.value_or(0));
        const size_t next = playing + 1;
        const auto restart = [this, playing]() {
            SelectMediaLibraryPlaybackTrack(playing, true);
        };
        switch (settings_.player.play_mode) {
        case 0:
            Stop();
            return;
        case 1:
            if (count != 0) restart(); else Stop();
            return;
        case 2:
            if (next < count)
                SelectMediaLibraryPlaybackTrack(next, true);
            else {
                if (fullscreen_mode_ != 0) LeaveFullScreen();
                RefreshPlaybackUi();
            }
            return;
        case 3:
            if (count != 0)
                SelectMediaLibraryPlaybackTrack(next < count ? next : 0, true);
            else
                Stop();
            return;
        case 4:
            if (count > 1) {
                const auto target = list.Next(
                    playing, playlist::PlayMode::shuffle);
                if (target) SelectMediaLibraryPlaybackTrack(*target, true);
                else Stop();
            } else if (count == 1) {
                restart();
            } else {
                Stop();
            }
            return;
        default:
            return;
        }
    }
    const size_t source = playing_playlist_index_.value_or(
        playlists_.ActiveIndex());
    if (source >= playlists_.Size()) {
        Stop();
        return;
    }
    auto& list = playlists_.At(source);
    const size_t count = list.Tracks().size();
    const int mode = settings_.player.play_mode;

    const auto restart = [this, source, &list]() {
        if (const auto playing = list.PlayingRow()) current_ = *playing;
        playing_playlist_index_ = source;
        static_cast<void>(PlayCurrent(false));
    };
    const auto exit_fullscreen_only = [this]() {
        if (fullscreen_mode_ != 0) LeaveFullScreen();
        RefreshPlaybackUi();
    };
    const auto switch_list = [this, source](bool wrap)
        -> std::optional<size_t> {
        if (playlists_.Size() == 0) return std::nullopt;
        size_t target = source + 1;
        if (target >= playlists_.Size()) {
            if (!wrap) return std::nullopt;
            target = 0;
        }
        if (target == source) return std::nullopt;
        SwitchPlaylist(target);
        playing_playlist_index_ = target;
        current_ = playlists_.At(target).Tracks().empty()
            ? std::nullopt : std::optional<size_t>{0};
        return target;
    };

    if (count == 0 &&
        (!settings_.player.auto_switch_list || mode < 2 || mode > 4)) {
        if (mode == 1 || mode == 2) restart();
        else Stop();
        return;
    }

    // When PlayFollowCursor is enabled, 0045BD69 gives a focused Files row
    // precedence over the normal end-of-track transition (except in Single
    // mode), first clearing LVIS_SELECTED just like command 0x7F36.
    if (ShouldStartPlaylistPlayback(
            PlaylistSelectionTrigger::natural_completion,
            settings_.player.play_follow_cursor) && mode != 0 &&
        source == playlists_.ActiveIndex() && playlist_selection_ &&
        *playlist_selection_ < count) {
        const size_t focused = *playlist_selection_;
        playlist_selected_rows_.clear();
        if (playlist_window_)
            InvalidateRect(playlist_window_, nullptr, FALSE);
        if (list.PlayingRow() != std::optional<size_t>{focused}) {
            SelectTrackFrom(source, focused, true);
            return;
        }
    }

    const size_t next = list.PlayingRow()
        ? *list.PlayingRow() + 1 : 0;
    switch (mode) {
    case 0: // Single
        Stop();
        return;
    case 1: // Repeat one
        restart();
        return;
    case 2: // Sequential
        if (next < count) {
            SelectTrackFrom(source, next, true);
            return;
        }
        if (!settings_.player.auto_switch_list) {
            // 0045BF3A exits full-screen without running the complete stop
            // reset a second time; the decoder has already reported stopped.
            exit_fullscreen_only();
            return;
        }
        if (switch_list(false)) {
            static_cast<void>(PlayCurrent(false));
            return;
        }
        Stop();
        return;
    case 3: // Repeat all
        if (!settings_.player.auto_switch_list || next < count) {
            if (count != 0)
                SelectTrackFrom(source, next < count ? next : 0, true);
            else
                Stop();
            return;
        }
        if (switch_list(true)) {
            static_cast<void>(PlayCurrent(false));
            return;
        }
        Stop();
        return;
    case 4: { // Shuffle
        if (count > 1) {
            const size_t current = list.PlayingRow().value_or(0);
            const auto selected = list.Next(current,
                                             playlist::PlayMode::shuffle);
            if (selected) SelectTrackFrom(source, *selected, true);
            else Stop();
            return;
        }
        if (!settings_.player.auto_switch_list) {
            if (count == 1) restart();
            else Stop();
            return;
        }
        if (const auto target = switch_list(true)) {
            auto& target_list = playlists_.At(*target);
            if (!target_list.Tracks().empty()) {
                current_ = 0;
                static_cast<void>(PlayCurrent(false));
                return;
            }
        }
        Stop();
        return;
    }
    default:
        return;
    }
}

void PlayerWindow::Stop() {
    // FUN_00465169 first leaves whichever full-screen host is active, then
    // stops the decoder and refreshes the ordinary player windows.  It does
    // not reset CPlayList +0x1c: that last-playing marker survives an explicit
    // stop and prevents 00480386 from auto-starting a subsequently appended
    // batch.  Clear/replace/failed-open paths remove it separately.
    if (fullscreen_mode_ != 0) SetFullScreenMode(0);
    playback_was_active_ = false;
    playback_source_open_ = false;
    natural_completion_dispatch_ = false;
    pending_natural_play_ = false;
    pending_failed_advance_ = false;
    audio_.StopWithFade();
    UpdateVisualFrame();
    RefreshPlaybackUi();
}

void PlayerWindow::ShowAudioError() const {
    // The decoder's internal diagnostic is not a UI string.  The original
    // maps an ordinary open failure to ttpres.dll string 0x828e (with the
    // neighbouring 0x828f/0x8290/0x8291 reserved for specific engine error
    // codes) before presenting it to the user.
    const auto message = ResourceText(0x828e);
    MessageBoxW(window_, message.c_str(), ResourceText(0x80).c_str(),
                MB_OK | MB_ICONERROR);
}
} // namespace ttplayer::ui
