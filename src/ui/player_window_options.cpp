#include "ttplayer/i18n/i18n.h"
#include "ttplayer/ui/wtl_menu.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/ui/wtl_runtime.h"
#include "ttplayer/platform/optional_windows_api.h"
#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "options_buttons.h"
#include "options_general_tabs.h"
#include "project_links.h"
#include "output_devices.h"
#include "modern_file_dialog.h"
#include "lyric_association_dialog.h"
#include "../app/resource_ids.h"
#include "ttplayer/app/worker_process.h"
#include "ttplayer/build_date.h"
#include "ttplayer/settings/file_association.h"
#include "ttplayer/ui/player_runtime_policy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <commctrl.h>
#include <dsound.h>
#include <prsht.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <vsstyle.h>

namespace ttplayer::ui {
using namespace detail;
namespace {

// FUN_0049F4AD constructs association before full-screen, but its calls to
// FUN_004A15BD add full-screen first.  This is the observable 0-based order
// confirmed by the original executable in Windows Sandbox.
constexpr std::array<UINT, 15> kOptionTemplates{
    200, 250, 251, 252, 253, 254, 255, 256,
    257, 258, 259, 260, 261, 263, 262};
constexpr int kPageAbout = 0;
constexpr int kPageGeneral = 1;
constexpr int kPagePlayback = 2;
constexpr int kPageHotkeys = 3;
constexpr int kPageVisual = 4;
constexpr int kPagePlaylist = 5;
constexpr int kPageLibrary = 6;
constexpr int kPageLyrics = 7;
constexpr int kPageLyricSearch = 8;
constexpr int kPageNetwork = 9;
constexpr int kPagePlugins = 10;
constexpr int kPageDevice = 11;
constexpr int kPageSkin = 12;
constexpr int kPageFullscreen = 13;
constexpr int kPageAssociation = 14;

// /reg has the same sheet chrome, but only logical pages 0 and 14. Keep
// page storage/notification IDs separate from the two native tab indices.
int OptionsSheetIndex(int page, bool registration) {
    return registration && page == kPageAssociation ? 1 : page;
}
thread_local PlayerWindow* registration_sheet_initializing{};

constexpr int kOptionsNavigation = 0xe910;
constexpr int kOptionsHeader = 0xe911;
constexpr int kOptionsRelated = 0xe912;
constexpr int kOptionsDiscordLyrics = 0xe913;
constexpr int kOptionsLanguage = 0xe914;
constexpr int kOptionsLanguageLabel = 0xe916;
constexpr int kOptionsLanguageNotice = 0xe917;
constexpr int kOptionsSkinKindTab = 0xe918;

std::wstring AlbumOptionText(UINT id) {
    return i18n::ResourceText(GetModuleHandleW(nullptr), id);
}

void PopulateAlbumBackgroundOptions(HWND dialog, HINSTANCE instance,
                                    const settings::FullScreenSettings& settings) {
    if (!GetDlgItem(dialog, IDC_FULLSCREEN_ALBUM_PATH)) {
        // Resource 263 has an unused lower 53-DLU strip. Keep its original
        // two groups (including the shared lyric background colour) intact.
        const auto add = [&](LPCWSTR window_class, UINT label, int id,
                             DWORD style, RECT rect, DWORD exstyle = 0) {
            MapDialogRect(dialog, &rect);
            const auto caption = label ? AlbumOptionText(label) : std::wstring{};
            const HWND control = CreateWindowExW(exstyle, window_class, caption.c_str(),
                WS_CHILD | WS_VISIBLE | style, rect.left, rect.top,
                rect.right - rect.left, rect.bottom - rect.top, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
        };
        add(WC_BUTTONW, IDS_FULLSCREEN_ALBUM_GROUP, -1, BS_GROUPBOX, {7,162,273,213});
        add(WC_STATICW, IDS_FULLSCREEN_ALBUM_FALLBACK, -1, 0, {13,176,54,189});
        add(WC_EDITW, 0, IDC_FULLSCREEN_ALBUM_PATH, WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
            {55,174,194,188}, WS_EX_CLIENTEDGE);
        add(WC_BUTTONW, IDS_FULLSCREEN_ALBUM_BROWSE, IDC_FULLSCREEN_ALBUM_BROWSE,
            WS_TABSTOP | BS_PUSHBUTTON, {198,174,230,188});
        add(WC_BUTTONW, IDS_FULLSCREEN_ALBUM_CLEAR, IDC_FULLSCREEN_ALBUM_CLEAR,
            WS_TABSTOP | BS_PUSHBUTTON, {233,174,267,188});
        add(WC_STATICW, IDS_FULLSCREEN_ALBUM_TRANSPARENCY, -1, 0, {13,195,55,208});
        add(TRACKBAR_CLASSW, 0, IDC_FULLSCREEN_ALBUM_TRANSPARENCY,
            WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, {55,192,229,210});
        add(WC_STATICW, 0, IDC_FULLSCREEN_ALBUM_PERCENT, 0, {235,195,267,208});
    }
    SetDlgItemTextW(dialog, IDC_FULLSCREEN_ALBUM_PATH, settings.album_fallback_image.c_str());
    SendDlgItemMessageW(dialog, IDC_FULLSCREEN_ALBUM_TRANSPARENCY, TBM_SETRANGE, FALSE, MAKELPARAM(0,100));
    const int percent = std::clamp(settings.album_transparency_percent, 0, 100);
    SendDlgItemMessageW(dialog, IDC_FULLSCREEN_ALBUM_TRANSPARENCY, TBM_SETPOS, TRUE, percent);
    SetDlgItemTextW(dialog, IDC_FULLSCREEN_ALBUM_PERCENT, (std::to_wstring(percent) + L"%").c_str());
}

constexpr int kPropertySheetApply = 0x3021;
constexpr UINT kSaveAllOptions = 0x04d2;
constexpr UINT kResetAllOptions = 0x04d3;
constexpr UINT_PTR kOptionsSheetSubclass = 0x54544f50;
constexpr UINT_PTR kOptionsImageButtonSubclass = 0x5454494d;
constexpr UINT_PTR kOptionsAssociationButtonSubclass = 0x54544149;
constexpr UINT_PTR kOptionsNavigationSubclass = 0x54544e41;
LRESULT CALLBACK OptionsImageButtonSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
constexpr UINT_PTR kOptionsAboutSubclass = 0x54544142;
constexpr UINT_PTR kOptionsSkinPollTimer = 0x5453;
constexpr UINT_PTR kOptionsLyricServicesTimer = 0x5454;
constexpr UINT kOptionsSkinPollMilliseconds = 40;
constexpr UINT_PTR kOptionsDspPollTimer = 0x4453;
constexpr UINT kOptionsDspPollMilliseconds = 60;
constexpr ULONGLONG kOptionsDspScanTimeoutMilliseconds = 120000;
constexpr DWORD kOptionsDeviceProbeTimeoutMilliseconds = 4000;
constexpr UINT kSwitchNestedOptionsPage = WM_APP + 0x311;
constexpr UINT kExportSkinPreview = WM_APP + 0x312;
constexpr UINT kInitializeRegistrationSheet = WM_APP + 0x313;
constexpr UINT_PTR kOptionsSkinPreviewSubclass = 0x54545056;
constexpr wchar_t kPageTemplateProperty[] = L"TTPlayer.Options.Template";
constexpr wchar_t kPageReadyProperty[] = L"TTPlayer.Options.Ready";

struct AssociationPromptState {
    bool* auto_associate{};
};

INT_PTR CALLBACK YesNoDialogProc(HWND dialog, UINT message,
                                 WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AssociationPromptState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<AssociationPromptState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        // FUN_00459908 checks "auto associate on every startup" when the
        // mismatch dialog opens and immediately updates DAT_00547870.
        if (state && state->auto_associate) {
            *state->auto_associate = true;
            CheckDlgButton(dialog, 0x8cc, BST_CHECKED);
        }
        return TRUE;
    }
    if (message == WM_COMMAND) {
        const UINT command = LOWORD(wparam);
        if (command == 0x8cc && state && state->auto_associate) {
            *state->auto_associate =
                IsDlgButtonChecked(dialog, 0x8cc) == BST_CHECKED;
            return TRUE;
        }
        if (command == IDYES || command == IDNO || command == IDCANCEL) {
            EndDialog(dialog, command);
            return TRUE;
        }
    } else if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

constexpr std::uint32_t kDspRequestMagic = 0x51534454; // TDSQ
constexpr std::uint32_t kDspResultMagic = 0x52534454;  // TDSR
constexpr std::uint32_t kDspProtocolVersion = 1;

struct DspProbeResult {
    std::filesystem::path path;
    std::wstring description;
};

constexpr std::array<int, 9> kLyricFadeValues{0,3,4,5,6,8,10,12,16};

struct OptionsChildInit {
    PlayerWindow* self{};
    UINT template_id{};
};

std::wstring DeviceKeyString(DWORD data1) {
    GUID key{};
    key.Data1 = data1;
    std::array<wchar_t, 64> text{};
    return StringFromGUID2(key, text.data(), static_cast<int>(text.size())) > 0
        ? std::wstring(text.data()) : std::wstring{};
}

int DeviceBackendFromKey(std::wstring_view text, UINT& wave_device_id) {
    GUID key{};
    const std::wstring terminated(text);
    if (terminated.empty() ||
        FAILED(CLSIDFromString(terminated.c_str(), &key))) {
        return 1;
    }
    const bool zero_tail = key.Data2 == 0 && key.Data3 == 0 &&
        std::all_of(std::begin(key.Data4), std::end(key.Data4),
                    [](BYTE value) { return value == 0; });
    if (!zero_tail) return 1;
    const WORD backend = HIWORD(key.Data1);
    if (backend == 2 || backend == 3) return backend;
    const WORD ordinal = LOWORD(key.Data1);
    wave_device_id = ordinal == 0 ? WAVE_MAPPER : ordinal - 1;
    return 0;
}

std::optional<size_t> SelectedOutputDeviceEntry(HWND dialog,
                                                size_t entry_count) {
    const HWND devices = GetDlgItem(dialog, 1059);
    const LRESULT row = devices
        ? SendMessageW(devices, CB_GETCURSEL, 0, 0) : CB_ERR;
    if (row == CB_ERR) return std::nullopt;
    COMBOBOXEXITEMW item{};
    item.mask = CBEIF_LPARAM;
    item.iItem = static_cast<int>(row);
    if (!SendMessageW(devices, CBEM_GETITEMW, 0,
                      reinterpret_cast<LPARAM>(&item)) ||
        item.lParam < 0 || static_cast<size_t>(item.lParam) >= entry_count) {
        return std::nullopt;
    }
    return static_cast<size_t>(item.lParam);
}

constexpr UINT kLinkFirst = 3000;
constexpr wchar_t kSkinDownloadTarget[] =
    L"http://ttplayer.qianqian.com/skin.htm";

int TemplateIndex(UINT template_id) {
    const auto found = std::find(kOptionTemplates.begin(),
                                 kOptionTemplates.end(), template_id);
    return found == kOptionTemplates.end()
        ? -1 : static_cast<int>(found - kOptionTemplates.begin());
}

constexpr UINT DeferredOptionsBit(UINT template_id) {
    // Main property pages occupy bits 0..13; the four nested lyric/network
    // dialogs use the remaining bits.  About/association/skin pages do not
    // have a runtime adapter and therefore need no deferred transaction.
    if (template_id >= 250 && template_id <= 263)
        return 1U << (template_id - 250U);
    switch (template_id) {
    case 381: return 1U << 14;
    case 382: return 1U << 15;
    case 384: return 1U << 16;
    case 385: return 1U << 17;
    default: return 0;
    }
}

const WORD* SkipDialogString(const WORD* cursor) {
    if (*cursor == 0) return cursor + 1;
    if (*cursor == 0xffff) return cursor + 2;
    while (*cursor != 0) ++cursor;
    return cursor + 1;
}

std::wstring DialogCaption(HMODULE module, UINT identifier) {
    return i18n::DialogCaption(module, identifier);
}

void SetChecked(HWND dialog, int control, bool checked) {
    CheckDlgButton(dialog, control, checked ? BST_CHECKED : BST_UNCHECKED);
}

bool IsChecked(HWND dialog, int control) {
    return IsDlgButtonChecked(dialog, control) == BST_CHECKED;
}

int TreeCheckState(HWND tree, HTREEITEM item) {
    TVITEMW value{};
    value.mask = TVIF_IMAGE;
    value.hItem = item;
    if (!TreeView_GetItem(tree, &value)) return 0;
    // 004A4BC0 sets TVIF_IMAGE | TVIF_SELECTEDIMAGE (0x22), not a
    // one-based state-image mask. Resource 356 is unchecked/checked/mixed.
    return std::clamp(value.iImage, 0, 2);
}

void SetTreeCheckState(HWND tree, HTREEITEM item, int state) {
    TVITEMW value{};
    value.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    value.hItem = item;
    value.iImage = value.iSelectedImage = std::clamp(state, 0, 2);
    TreeView_SetItem(tree, &value);
}

int AggregateTreeChildren(HWND tree, HTREEITEM parent) {
    HTREEITEM child = TreeView_GetChild(tree, parent);
    if (!child) return TreeCheckState(tree, parent);
    bool any_checked = false;
    bool any_unchecked = false;
    for (; child; child = TreeView_GetNextSibling(tree, child)) {
        const int state = TreeCheckState(tree, child);
        any_checked = any_checked || state != 0;
        any_unchecked = any_unchecked || state != 1;
    }
    return any_checked && any_unchecked ? 2 : any_checked ? 1 : 0;
}

std::filesystem::path CurrentExecutablePath() {
    std::vector<wchar_t> buffer(1024, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) return {};
        if (copied + 1 < buffer.size()) {
            buffer.resize(copied);
            return std::filesystem::path(buffer.data());
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min<size_t>(32768, buffer.size() * 2));
    }
}

struct IconSelection {
    std::filesystem::path path;
    int index{};
};

IconSelection ParseIconSelection(std::wstring_view value) {
    IconSelection selection;
    if (value.empty()) return selection;
    std::wstring_view suffix;
    if (value.front() == L'"') {
        const size_t close = value.find(L'"', 1);
        if (close == std::wstring_view::npos) return selection;
        selection.path = std::wstring(value.substr(1, close - 1));
        suffix = value.substr(close + 1);
    } else {
        const size_t comma = value.rfind(L',');
        if (comma == std::wstring_view::npos) {
            selection.path = std::wstring(value);
        } else {
            selection.path = std::wstring(value.substr(0, comma));
            suffix = value.substr(comma);
        }
    }
    const size_t comma = suffix.find(L',');
    if (comma != std::wstring_view::npos) {
        const std::wstring number(suffix.substr(comma + 1));
        wchar_t* end{};
        const long index = std::wcstol(number.c_str(), &end, 10);
        if (end != number.c_str()) selection.index = static_cast<int>(index);
    }
    return selection;
}

std::wstring FormatIconSelection(const IconSelection& selection) {
    if (selection.path.empty()) return {};
    // FUN_004C20F3 accepts both bare paths and the shell DefaultIcon
    // "path",index grammar.  Always retaining the index also preserves icons
    // selected from EXE/DLL containers.
    return L"\"" + selection.path.wstring() + L"\"," +
           std::to_wstring(selection.index);
}

std::optional<std::wstring> ChooseIconSelection(
    HWND owner, HMODULE resources, std::wstring_view current) {
    IconSelection selection = ParseIconSelection(current);
    if (selection.path.empty()) selection.path = CurrentExecutablePath();
    // Keep every path-selection surface on the shared Vista+ Common Item
    // Dialog.  PickIconDlg is a private legacy shell dialog and otherwise
    // bypasses the modern path even though it normally exists on Windows.
    const auto previous_path = selection.path;
    auto filter_text = LoadResourceText(resources, 0x8129);
    std::vector<wchar_t> filter(filter_text.begin(), filter_text.end());
    for (auto& character : filter)
        if (character == L'|') character = L'\0';
    if (filter.empty() || filter.back() != L'\0') filter.push_back(L'\0');
    filter.push_back(L'\0');
    ModernOpenFileOptions open;
    open.owner = owner;
    open.filters = ParseLegacyDialogFilter(
        std::span<const wchar_t>(filter.data(), filter.size()));
    open.initial_path = selection.path;
    const auto selected = ModernOpenFile(open);
    if (!selected) return std::nullopt;
    selection.path = *selected;
    // IFileOpenDialog selects the containing ICO/EXE/DLL, not an embedded
    // resource index. Preserve an already configured index when the same file
    // is confirmed; a newly selected file starts from its first icon.
    if (_wcsicmp(previous_path.c_str(), selection.path.c_str()) != 0)
        selection.index = 0;
    return FormatIconSelection(selection);
}

HIMAGELIST ButtonIconImageList(HICON icon, int width, int height) {
    if (!icon) return nullptr;
    HIMAGELIST images = ImageList_Create(
        width, height, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (!images) return nullptr;
    if (ImageList_AddIcon(images, icon) < 0) {
        ImageList_Destroy(images);
        return nullptr;
    }
    return images;
}

HIMAGELIST AssociationBitmapImageList(HMODULE resources, UINT identifier) {
    // 0049D2FA -> 00434754 -> 00434F8C: one whole bitmap, ILC_COLOR32 |
    // ILC_MASK (0x21), C0C0C0 transparent. ImageList_LoadImage's default
    // colour format loses the palette of 0x162 (the Windows program icon).
    const auto bitmap = static_cast<HBITMAP>(LoadImageW(resources,
        MAKEINTRESOURCEW(identifier), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION | LR_DEFAULTSIZE));
    if (!bitmap) return nullptr;
    BITMAP source{};
    HIMAGELIST images{};
    if (GetObjectW(bitmap, sizeof(source), &source))
        images = ImageList_Create(source.bmWidth, source.bmHeight, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (images && ImageList_AddMasked(images, bitmap, RGB(192,192,192)) < 0) {
        ImageList_Destroy(images);
        images = nullptr;
    }
    DeleteObject(bitmap);
    return images;
}

void AttachButtonImage(HWND button, HIMAGELIST images) {
    if (!button || !images) return;
    BUTTON_IMAGELIST image{};
    image.himl = images;
    image.margin = RECT{4, 0, 4, 0};
    image.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    Button_SetImageList(button, &image);
    // The original uses 0046E0C9, not the native BCM left-edge layout.
    // Ownership remains with options_association_button_images_.
    SetWindowSubclass(button, OptionsImageButtonSubclassProc, kOptionsAssociationButtonSubclass, 0);
}

bool ReadDspBytes(HANDLE file, void* destination, DWORD bytes) {
    auto* cursor = static_cast<std::byte*>(destination);
    while (bytes != 0) {
        DWORD read{};
        if (!ReadFile(file, cursor, bytes, &read, nullptr) || read == 0)
            return false;
        cursor += read;
        bytes -= read;
    }
    return true;
}

bool WriteDspBytes(HANDLE file, const void* source, DWORD bytes) {
    const auto* cursor = static_cast<const std::byte*>(source);
    while (bytes != 0) {
        DWORD written{};
        if (!WriteFile(file, cursor, bytes, &written, nullptr) || written == 0)
            return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

bool ReadDspString(HANDLE file, std::wstring& value) {
    std::uint32_t size{};
    if (!ReadDspBytes(file, &size, sizeof(size)) || size > 32768)
        return false;
    value.resize(size);
    return size == 0 || ReadDspBytes(file, value.data(),
                                     size * sizeof(wchar_t));
}

bool WriteDspString(HANDLE file, std::wstring_view value) {
    if (value.size() > 32768) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return WriteDspBytes(file, &size, sizeof(size)) &&
           (size == 0 || WriteDspBytes(file, value.data(),
                                      size * sizeof(wchar_t)));
}

std::filesystem::path DspTemporaryFile() {
    std::array<wchar_t, MAX_PATH + 1> folder{};
    std::array<wchar_t, MAX_PATH + 1> file{};
    if (!GetTempPathW(static_cast<DWORD>(folder.size()), folder.data()) ||
        !GetTempFileNameW(folder.data(), L"tdp", 0, file.data())) return {};
    return file.data();
}

std::wstring QuoteDspArgument(std::wstring_view argument) {
    return app::QuoteWorkerArgument(argument);
}

bool WriteDspRequest(const std::filesystem::path& request,
                     const std::filesystem::path& configured_folder,
                     const std::vector<std::wstring>& configured_modules,
                     const std::filesystem::path& runtime_folder) {
    const HANDLE file = CreateFileW(request.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::uint32_t count = static_cast<std::uint32_t>(
        configured_modules.size() + 2);
    bool good = WriteDspBytes(file, &kDspRequestMagic,
                              sizeof(kDspRequestMagic)) &&
                WriteDspBytes(file, &kDspProtocolVersion,
                              sizeof(kDspProtocolVersion)) &&
                WriteDspBytes(file, &count, sizeof(count));
    for (const auto& module : configured_modules) {
        std::filesystem::path path(module);
        if (!path.is_absolute()) path = configured_folder / path;
        constexpr std::uint32_t kind = 0;
        const auto value = path.wstring();
        good = good && WriteDspBytes(file, &kind, sizeof(kind)) &&
               WriteDspString(file, value);
    }
    for (const auto& directory : {configured_folder, runtime_folder}) {
        constexpr std::uint32_t kind = 1;
        const auto value = directory.wstring();
        good = good && WriteDspBytes(file, &kind, sizeof(kind)) &&
               WriteDspString(file, value);
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return good;
}

std::optional<std::vector<DspProbeResult>> ReadDspResults(
    const std::filesystem::path& output) {
    std::vector<DspProbeResult> results;
    const HANDLE file = CreateFileW(output.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    std::uint32_t magic{}, version{}, count{};
    const bool valid_header =
        ReadDspBytes(file, &magic, sizeof(magic)) &&
        ReadDspBytes(file, &version, sizeof(version)) &&
        ReadDspBytes(file, &count, sizeof(count)) &&
        magic == kDspResultMagic && version == kDspProtocolVersion &&
        count <= 4096;
    bool valid_body = valid_header;
    if (valid_header) {
        results.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            std::wstring path;
            DspProbeResult result;
            if (!ReadDspString(file, path) ||
                !ReadDspString(file, result.description)) {
                valid_body = false;
                break;
            }
            result.path = std::move(path);
            results.push_back(std::move(result));
        }
    }
    CloseHandle(file);
    if (!valid_body) return std::nullopt;
    return results;
}

std::optional<std::vector<LegacyOutputDevice>> RunOutputDeviceProbe(
    const LegacyOutputDevice* selected_device = nullptr) {
    const auto helper = app::CurrentExecutablePath();
    if (helper.empty()) return std::nullopt;
    const auto request = selected_device ? DspTemporaryFile()
                                         : std::filesystem::path{};
    const auto output = DspTemporaryFile();
    if (output.empty() || (selected_device && request.empty())) {
        if (!request.empty()) DeleteFileW(request.c_str());
        if (!output.empty()) DeleteFileW(output.c_str());
        return std::nullopt;
    }
    if (selected_device && !WriteLegacyOutputDeviceProbe(
            request, std::vector<LegacyOutputDevice>{*selected_device})) {
        DeleteFileW(request.c_str());
        DeleteFileW(output.c_str());
        return std::nullopt;
    }

    std::wstring command = app::WorkerCommandPrefix(
        helper, app::kOutputDeviceWorkerSwitch);
    if (selected_device) {
        command += L" --details " + QuoteDspArgument(request.wstring());
    }
    command += L" " + QuoteDspArgument(output.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        helper.parent_path().c_str(), &startup, &process)) {
        if (!request.empty()) DeleteFileW(request.c_str());
        DeleteFileW(output.c_str());
        return std::nullopt;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job,
                                     JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    const DWORD resumed = ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    DWORD wait = resumed == static_cast<DWORD>(-1)
        ? WAIT_FAILED
        : WaitForSingleObject(process.hProcess,
                              kOptionsDeviceProbeTimeoutMilliseconds);
    if (wait != WAIT_OBJECT_0) {
        if (job)
            TerminateJobObject(job, ERROR_TIMEOUT);
        else
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        static_cast<void>(WaitForSingleObject(process.hProcess, 1000));
    }

    std::optional<std::vector<LegacyOutputDevice>> result;
    DWORD exit_code{};
    if (wait == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0) {
        result = ReadLegacyOutputDeviceProbe(output);
    }
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    if (!request.empty()) DeleteFileW(request.c_str());
    DeleteFileW(output.c_str());
    return result;
}

std::wstring AssociationTypeLabel(std::wstring_view description) {
    const size_t pattern = description.find(L'(');
    if (pattern != std::wstring_view::npos)
        description = description.substr(0, pattern);
    // FUN_0049CC0E writes a terminator over '(' and keeps every preceding
    // code unit, including a separator space before the pattern.
    return std::wstring(description);
}

std::wstring AssociationPattern(std::wstring_view description) {
    const size_t begin = description.find(L'(');
    if (begin == std::wstring_view::npos) return {};
    const size_t end = description.find(L')', begin + 1);
    if (end == std::wstring_view::npos || end == begin + 1) return {};
    return std::wstring(description.substr(begin + 1, end - begin - 1));
}

std::vector<plugins::ReaderFormat> BuildAssociationFormatSource(
    HMODULE resources,
    const std::vector<plugins::ReaderFormat>& registered_readers) {
    // FUN_0049CC0E does not walk only the AddIn registry.  It parses
    // DAT_00547FC8, the already-materialised Open File filter built by
    // CSoundLibrary.  Consequently the association tree contains the seven
    // in-process readers as well as every successfully loaded reader DLL.
    // The individual filter entries are sorted before they are serialised;
    // the playlist entry is appended afterwards.
    constexpr std::array built_in{
        0x811cU, // CD
        0x811dU, // CUE
        0x811eU, // MP3
        0x811fU, // MIDI
        0x8120U, // Wave
        0x8121U, // AIFF
        0x8122U, // AU
    };

    std::vector<plugins::ReaderFormat> formats;
    formats.reserve(built_in.size() + registered_readers.size() + 1U);
    for (const UINT identifier : built_in) {
        auto description = LoadResourceText(resources, identifier);
        auto pattern = AssociationPattern(description);
        if (!description.empty() && !pattern.empty()) {
            formats.push_back({std::move(description), std::move(pattern), {}});
        }
    }
    for (const auto& reader : registered_readers) {
        if (!reader.description.empty() && !reader.pattern.empty())
            formats.push_back(reader);
    }
    std::ranges::sort(formats,
        [](const plugins::ReaderFormat& left,
           const plugins::ReaderFormat& right) {
            return std::wcscmp(left.description.c_str(),
                               right.description.c_str()) < 0;
        });

    auto playlist = LoadResourceText(resources, 0x8125);
    auto playlist_pattern = AssociationPattern(playlist);
    if (!playlist.empty() && !playlist_pattern.empty()) {
        formats.push_back(
            {std::move(playlist), std::move(playlist_pattern), {}});
    }
    return formats;
}

void SetInteger(HWND dialog, int control, int value) {
    SetDlgItemInt(dialog, control, static_cast<UINT>(std::max(0, value)), FALSE);
}

int GetInteger(HWND dialog, int control, int fallback, int minimum,
               int maximum) {
    BOOL valid{};
    const UINT value = GetDlgItemInt(dialog, control, &valid, FALSE);
    return valid ? std::clamp(static_cast<int>(value), minimum, maximum)
                 : fallback;
}

std::wstring GetText(HWND dialog, int control) {
    const HWND item = GetDlgItem(dialog, control);
    if (!item) return {};
    const int length = GetWindowTextLengthW(item);
    std::wstring value(static_cast<size_t>(std::max(0, length)), L'\0');
    if (length > 0) GetWindowTextW(item, value.data(), length + 1);
    return value;
}

void PopulateResourceCombo(HWND dialog, int control, HMODULE resources,
                           size_t count, int selection) {
    const HWND combo = GetDlgItem(dialog, control);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < count; ++index) {
        const auto text = ResourceListItem(resources, control, index);
        if (!text.empty()) SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
}

void PopulateTextCombo(HWND dialog, int control,
                       const std::vector<std::wstring>& values,
                       int selection) {
    const HWND combo = GetDlgItem(dialog, control);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& value : values)
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(value.c_str()));
    SendMessageW(combo, CB_SETCURSEL,
                 static_cast<WPARAM>(std::clamp(selection, 0,
                     std::max(0, static_cast<int>(values.size()) - 1))), 0);
}

void PopulateLyricFadeCombo(HWND dialog, HMODULE resources, int value) {
    const HWND combo = GetDlgItem(dialog, 1026);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    auto disabled = LoadResourceText(resources, 0x813e);
    if (disabled.empty()) disabled = L"0";
    LRESULT row = SendMessageW(combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(disabled.c_str()));
    if (row != CB_ERR && row != CB_ERRSPACE)
        SendMessageW(combo, CB_SETITEMDATA, row, 0);

    auto format = LoadResourceText(resources, 0x813f);
    if (format.empty()) format = L"1/%d";
    for (size_t index = 1; index < kLyricFadeValues.size(); ++index) {
        std::array<wchar_t, 128> text{};
        swprintf_s(text.data(), text.size(), format.c_str(),
                   kLyricFadeValues[index]);
        row = SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(text.data()));
        if (row != CB_ERR && row != CB_ERRSPACE)
            SendMessageW(combo, CB_SETITEMDATA, row,
                         kLyricFadeValues[index]);
    }
    const auto found = std::find(kLyricFadeValues.begin(),
                                 kLyricFadeValues.end(), value);
    SendMessageW(combo, CB_SETCURSEL,
        found == kLyricFadeValues.end()
            ? static_cast<WPARAM>(-1)
            : static_cast<WPARAM>(found - kLyricFadeValues.begin()), 0);
}

int LyricFadeValue(HWND dialog, int fallback) {
    const HWND combo = GetDlgItem(dialog, 1026);
    if (!combo) return fallback;
    const LRESULT row = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (row == CB_ERR) return fallback;
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, row, 0);
    return data == CB_ERR ? fallback : static_cast<int>(data);
}

void DrawAboutText(const DRAWITEMSTRUCT& item, HWND dialog,
                   std::wstring_view text, bool edition) {
    // 004920AB / 004921CE draw transparently over the already painted page.
    // Filling the hidden STATIC placeholders produces two white rectangles.
    LOGFONTW font{};
    const HFONT dialog_font = reinterpret_cast<HFONT>(
        SendMessageW(dialog, WM_GETFONT, 0, 0));
    if (!dialog_font || !GetObjectW(dialog_font, sizeof(font), &font))
        SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(font), &font, 0);
    font.lfHeight = edition ? 16 : 28;
    font.lfWeight = FW_SEMIBOLD;
    font.lfQuality = ANTIALIASED_QUALITY;
    const HFONT created = CreateFontIndirectW(&font);
    const HGDIOBJ old = created ? SelectObject(item.hDC, created) : nullptr;
    SetBkMode(item.hDC, TRANSPARENT);
    RECT bounds = item.rcItem;
    constexpr UINT flags = DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX;
    if (edition) {
        SetTextColor(item.hDC, RGB(0, 0, 128));
        DrawTextW(item.hDC, text.data(), static_cast<int>(text.size()),
                  &bounds, flags);
    } else {
        SetTextColor(item.hDC, RGB(32, 32, 32));
        OffsetRect(&bounds, 1, 1);
        DrawTextW(item.hDC, text.data(), static_cast<int>(text.size()),
                  &bounds, flags);
        SetTextColor(item.hDC, GetSysColor(COLOR_3DHIGHLIGHT));
        OffsetRect(&bounds, -2, -2);
        DrawTextW(item.hDC, text.data(), static_cast<int>(text.size()),
                  &bounds, flags);
        SetTextColor(item.hDC, RGB(255, 155, 0));
        OffsetRect(&bounds, 1, 1);
        DrawTextW(item.hDC, text.data(), static_cast<int>(text.size()),
                  &bounds, flags);
    }
    if (old) SelectObject(item.hDC, old);
    if (created) DeleteObject(created);
}

constexpr COLORREF kOptionsSelection = RGB(48,106,198);
constexpr COLORREF kOptionsAccent = RGB(192,192,255);

COLORREF OptionsBlend(COLORREF first, COLORREF second, int position, int extent) {
    if (extent <= 0) return first;
    // 00408852 uses integer division by extent, not extent-1.
    const auto component = [&](int a, int b) { return (a*(extent-position)+b*position)/extent; };
    return RGB(component(GetRValue(first),GetRValue(second)),
               component(GetGValue(first),GetGValue(second)),
               component(GetBValue(first),GetBValue(second)));
}

void FillOptionsColor(HDC dc, const RECT& rect, COLORREF color) {
    const COLORREF previous = SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, previous);
}

void DrawOptionsGradient(HDC dc, const RECT& rect, COLORREF first,
                         COLORREF last, bool symmetric = false) {
    const int width = rect.right-rect.left;
    if (width <= 0 || rect.bottom <= rect.top) return;
    std::vector<DWORD> pixels(static_cast<size_t>(width));
    const int half = width/2;
    for (int x=0; x<width; ++x) {
        const COLORREF color = !symmetric ? OptionsBlend(first,last,x,width) :
            x < half ? OptionsBlend(first,last,x,half) : OptionsBlend(last,first,x-half,width-half);
        pixels[static_cast<size_t>(x)] = (GetRValue(color)<<16) | (GetGValue(color)<<8) | GetBValue(color);
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -1;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    StretchDIBits(dc, rect.left, rect.top, width, rect.bottom-rect.top, 0,0,width,1,
                  pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

void DrawOptionsItemFrame(HDC dc, RECT rect, COLORREF first, COLORREF second) {
    // 00413320 -> 004139D7: top/left then right/bottom, including corners.
    FillOptionsColor(dc, RECT{rect.left,rect.top,rect.right-1,rect.top+1}, first);
    FillOptionsColor(dc, RECT{rect.left,rect.top,rect.left+1,rect.bottom-1}, first);
    FillOptionsColor(dc, RECT{rect.right-1,rect.top,rect.right,rect.bottom}, second);
    FillOptionsColor(dc, RECT{rect.left,rect.bottom-1,rect.right,rect.bottom}, second);
}

struct OptionsNavigationState { int hot{-1}; bool tracking{}; };

int OptionsNavigationHit(HWND list, POINT point) {
    RECT client{}; GetClientRect(list, &client);
    if (!PtInRect(&client, point)) return -1;
    const auto count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    for (int row=0; row<count; ++row) {
        RECT rect{};
        if (SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) != LB_ERR &&
            PtInRect(&rect, point)) return row;
    }
    return -1;
}

LRESULT CALLBACK OptionsNavigationSubclassProc(HWND list, UINT message, WPARAM wp, LPARAM lp,
                                               UINT_PTR subclass, DWORD_PTR data) {
    auto* state = reinterpret_cast<OptionsNavigationState*>(data);
    const auto invalidate = [&](int row) {
        RECT rect{};
        if (row >= 0 && SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) != LB_ERR)
            InvalidateRect(list, &rect, FALSE);
    };
    if (message == WM_MOUSEMOVE && GetCapture() != list) {
        const int hot = OptionsNavigationHit(list, POINT{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        if (hot != state->hot) {
            invalidate(state->hot);
            state->hot = hot;
            invalidate(hot);
        }
        if (!state->tracking) {
            TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,list,HOVER_DEFAULT};
            state->tracking = TrackMouseEvent(&event) != FALSE;
        }
        return 0;
    }
    if (message == WM_MOUSELEAVE) {
        const int previous = state->hot;
        state->hot = -1;
        state->tracking = false;
        invalidate(previous);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        const int hit = OptionsNavigationHit(list, POINT{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        // 0048D8F0 ignores blank space and the already active row.
        if (hit < 0 || hit == SendMessageW(list, LB_GETCURSEL, 0, 0)) return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(list, OptionsNavigationSubclassProc, subclass);
        delete state;
    }
    return DefSubclassProc(list, message, wp, lp);
}

void DrawOptionsNavigation(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    DWORD_PTR data{};
    GetWindowSubclass(item.hwndItem, OptionsNavigationSubclassProc, kOptionsNavigationSubclass, &data);
    const auto* state = reinterpret_cast<const OptionsNavigationState*>(data);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = state && state->hot == static_cast<int>(item.itemID);
    const COLORREF background = GetSysColor(COLOR_WINDOW);
    const COLORREF edge = OptionsBlend(kOptionsSelection,kOptionsAccent,1,2);
    const int saved = SaveDC(item.hDC);
    if (!selected && !hot) {
        FillOptionsColor(item.hDC, item.rcItem, background);
        RECT line = item.rcItem; line.top = line.bottom-1;
        DrawOptionsGradient(item.hDC, line, background, kOptionsAccent, true);
    } else {
        FillOptionsColor(item.hDC, item.rcItem,
            hot ? OptionsBlend(kOptionsSelection,background,1,2) : kOptionsSelection);
        DrawOptionsItemFrame(item.hDC,item.rcItem,selected ? edge : kOptionsAccent, selected ? kOptionsAccent : edge);
        if (selected) {
            // 0048D596: pale triangle at (7,7),(7,17),(12,12), with the
            // blended-colour silhouette first at (+1,+1). Never a black pen.
            for (const int offset : {1,0}) {
                POINT points[]{{item.rcItem.left+7+offset,item.rcItem.top+7+offset},
                               {item.rcItem.left+7+offset,item.rcItem.top+17+offset},
                               {item.rcItem.left+12+offset,item.rcItem.top+12+offset}};
                const COLORREF color = offset ? edge : kOptionsAccent;
                const auto pen = CreatePen(PS_SOLID,1,color);
                const auto brush = CreateSolidBrush(color);
                const auto old_pen = SelectObject(item.hDC,pen);
                const auto old_brush = SelectObject(item.hDC,brush);
                Polygon(item.hDC,points,3);
                SelectObject(item.hDC,old_brush); SelectObject(item.hDC,old_pen);
                DeleteObject(brush); DeleteObject(pen);
            }
        }
    }
    RECT text = item.rcItem; text.left += 22;
    SetBkMode(item.hDC,TRANSPARENT);
    SetTextColor(item.hDC, GetSysColor(selected || hot ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem,WM_GETFONT,0,0));
    if (font) SelectObject(item.hDC,font);
    const auto length = SendMessageW(item.hwndItem,LB_GETTEXTLEN,item.itemID,0);
    if (length != LB_ERR) {
        std::wstring caption(static_cast<size_t>(length)+1,L'\0');
        SendMessageW(item.hwndItem,LB_GETTEXT,item.itemID,reinterpret_cast<LPARAM>(caption.data()));
        DrawTextW(item.hDC,caption.c_str(),-1,&text,DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    RestoreDC(item.hDC,saved);
}

void DrawOptionsHeaderContent(const DRAWITEMSTRUCT& item) {
    const int saved = SaveDC(item.hDC);
    FillRect(item.hDC,&item.rcItem,GetSysColorBrush(COLOR_3DFACE));
    const HWND sheet = GetParent(item.hwndItem);
    if (const auto theme = OpenThemeData(sheet, L"Tab")) {
        // The 8px separator strip belongs to the themed right-hand pane,
        // not COLOR_3DFACE (F9F9F9 vs F0F0F0 on the default host theme).
        RECT pane = item.rcItem;
        if (const HWND page = PropSheet_GetCurrentPageHwnd(sheet)) {
            RECT bounds{}; GetWindowRect(page, &bounds);
            MapWindowPoints(nullptr, item.hwndItem, reinterpret_cast<POINT*>(&bounds), 2);
            pane.bottom = bounds.bottom;
        }
        InflateRect(&pane, 4, 4);
        IntersectClipRect(item.hDC, item.rcItem.left, item.rcItem.top,
                          item.rcItem.right, item.rcItem.bottom);
        DrawThemeBackground(theme, item.hDC, TABP_PANE, 0, &pane, nullptr);
        CloseThemeData(theme);
    }
    RECT gradient = item.rcItem;
    gradient.bottom -= 8; // 004A3209's 4px gap, 2px sunken line, 2px gap.
    DrawOptionsGradient(item.hDC,gradient,kOptionsSelection,GetSysColor(COLOR_WINDOW));
    RECT separator{gradient.left,gradient.bottom+4,gradient.right,gradient.bottom+6};
    DrawEdge(item.hDC,&separator,BDR_SUNKENOUTER,BF_RECT);
    LOGFONTW font{};
    const auto original = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem,WM_GETFONT,0,0));
    if (original) GetObjectW(original,sizeof(font),&font);
    font.lfHeight = 14; // 004A3209 overrides only height; keeps face/weight.
    const auto created = CreateFontIndirectW(&font);
    if (created) SelectObject(item.hDC,created);
    wchar_t caption[1024]{}; GetWindowTextW(item.hwndItem,caption,1024);
    gradient.left += 4;
    SetBkMode(item.hDC,TRANSPARENT);
    OffsetRect(&gradient,1,1);
    SetTextColor(item.hDC,RGB(32,32,32));
    DrawTextW(item.hDC,caption,-1,&gradient,DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    OffsetRect(&gradient,-1,-1);
    SetTextColor(item.hDC,GetSysColor(COLOR_WINDOW));
    DrawTextW(item.hDC,caption,-1,&gradient,DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(item.hDC,saved);
    if (created) DeleteObject(created);
}

void DrawOptionsHeader(const DRAWITEMSTRUCT& item) {
    // 004A3209 paints through 004086B1's compatible memory DC. Besides
    // avoiding intermediate gradient/text passes on screen, this preserves
    // GDI's text compositing rather than the STATIC's themed target format.
    const int width = item.rcItem.right - item.rcItem.left;
    const int height = item.rcItem.bottom - item.rcItem.top;
    WTL::CDC memory;
    memory.CreateCompatibleDC(item.hDC);
    WTL::CBitmap bitmap;
    bitmap.CreateCompatibleBitmap(item.hDC, width, height);
    if (memory && bitmap) {
        const auto previous = SelectObject(memory, bitmap);
        SetViewportOrgEx(memory, -item.rcItem.left, -item.rcItem.top, nullptr);
        auto buffered = item;
        buffered.hDC = memory;
        DrawOptionsHeaderContent(buffered);
        BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, width, height,
               memory, item.rcItem.left, item.rcItem.top, SRCCOPY);
        SelectObject(memory, previous);
    } else {
        DrawOptionsHeaderContent(item);
    }
}

void DrawOptionsShellFrames(HWND sheet, HDC dc, HWND navigation, RECT page) {
    if (!navigation || IsRectEmpty(&page)) return;
    RECT nav{}; GetWindowRect(navigation,&nav);
    MapWindowPoints(nullptr,sheet,reinterpret_cast<POINT*>(&nav),2);
    page.top = nav.top;
    const int saved = SaveDC(dc);
    // PrintWindow does not necessarily clip child HWNDs as GetDC does.
    // Paint only the frame rings, never over the page or owner-drawn header.
    ExcludeClipRect(dc,nav.left,nav.top,nav.right,nav.bottom);
    ExcludeClipRect(dc,page.left,page.top,page.right,page.bottom);
    InflateRect(&nav,4,4); InflateRect(&page,4,4);
    const auto theme = OpenThemeData(sheet,L"Tab");
    for (auto rect : {nav,page}) {
        if (!theme || FAILED(DrawThemeBackground(theme,dc,TABP_PANE,0,&rect,nullptr)))
            DrawEdge(dc,&rect,EDGE_ETCHED,BF_RECT);
    }
    if (theme) CloseThemeData(theme);
    RestoreDC(dc,saved);
}

void PaintAboutPage(HWND dialog, HDC dc) {
    if (!dialog || !dc) return;
    const HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(128), IMAGE_ICON,
        48, 48, LR_SHARED));
    if (icon) DrawIconEx(dc, 12, 10, icon, 48, 48, 0, nullptr, DI_NORMAL);
    for (const int control : {1009, 1020}) {
        const HWND label = GetDlgItem(dialog, control);
        RECT bounds{};
        if (!label || !GetWindowRect(label, &bounds)) continue;
        MapWindowPoints(HWND_DESKTOP, dialog,
                        reinterpret_cast<POINT*>(&bounds), 2);
        std::array<wchar_t, 256> text{};
        GetWindowTextW(label, text.data(), static_cast<int>(text.size()));
        DRAWITEMSTRUCT item{};
        item.CtlID = control;
        item.hwndItem = label;
        item.hDC = dc;
        item.rcItem = bounds;
        DrawAboutText(item, dialog, text.data(), control == 1020);
    }
}

LRESULT CALLBACK OptionsAboutSubclassProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(dialog, OptionsAboutSubclassProc, subclass);
        return DefSubclassProc(dialog, message, wparam, lparam);
    }
    if (message == WM_PAINT) {
        // 00492512 uses BeginPaint's update-region clipping. A GetDC overlay
        // on every child repaint repeatedly darkens the anti-aliased shadow.
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(dialog, &paint);
        PaintAboutPage(dialog, dc);
        EndPaint(dialog, &paint);
        return 0;
    }
    const LRESULT result = DefSubclassProc(dialog, message, wparam, lparam);
    // WM_PRINT already delegates to WM_PRINTCLIENT; do not overlay twice.
    if (message == WM_PRINTCLIENT && wparam) {
        PaintAboutPage(dialog, reinterpret_cast<HDC>(wparam));
    }
    return result;
}

int ComboSelection(HWND dialog, int control, int fallback = 0) {
    const LRESULT selected = SendDlgItemMessageW(dialog, control,
                                                  CB_GETCURSEL, 0, 0);
    return selected == CB_ERR ? fallback : static_cast<int>(selected);
}

void SetSpinRange(HWND dialog, int control, int minimum, int maximum) {
    const HWND spin = GetDlgItem(dialog, control);
    if (spin) SendMessageW(spin, UDM_SETRANGE32,
                           static_cast<WPARAM>(minimum), maximum);
}

void SetSpinPosition(HWND dialog, int control, int position) {
    const HWND spin = GetDlgItem(dialog, control);
    if (spin) SendMessageW(spin, UDM_SETPOS32, 0, position);
}

bool LayeredWindowsAvailableForOptions() noexcept {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 &&
        GetProcAddress(user32, "SetLayeredWindowAttributes") != nullptr;
}

void PopulateFullscreenLyricSizeCombo(HWND dialog,
                                      const std::wstring& format) {
    const HWND combo = GetDlgItem(dialog, 2231);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int size = 1; size <= 10; ++size) {
        std::array<wchar_t, 64> label{};
        swprintf_s(label.data(), label.size(), format.c_str(), size * 10);
        const LRESULT row = SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.data()));
        if (row != CB_ERR && row != CB_ERRSPACE)
            SendMessageW(combo, CB_SETITEMDATA, row, size);
    }
}

void SelectFullscreenProfileControls(
    HWND dialog, const settings::FullScreenSettings& fullscreen,
    int profile) {
    if (profile < 0 || profile >=
        static_cast<int>(fullscreen.position_relation.size())) return;
    SendDlgItemMessageW(dialog, 2230, CB_SETCURSEL,
        static_cast<WPARAM>(fullscreen.position_relation[
            static_cast<size_t>(profile)]), 0);
    const int lyric_size = fullscreen.lyric_size[
        static_cast<size_t>(profile)];
    // 0049F14C/0049F268 use CB_SELECTSTRING.  A malformed raw size has no
    // match and therefore leaves the previous selection intact.
    if (lyric_size >= 1 && lyric_size <= 10)
        SendDlgItemMessageW(dialog, 2231, CB_SETCURSEL,
            static_cast<WPARAM>(lyric_size - 1), 0);
}

void MakeColorButton(HWND dialog, int control) {
    const HWND button = GetDlgItem(dialog, control);
    if (!button) return;
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    SetWindowLongPtrW(button, GWL_STYLE,
        (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_PUSHBUTTON);
    // Keep native push-button/default-button state, as the original subclass
    // does. Route paint to the page's color handler without BS_OWNERDRAW.
    SetPropW(button, L"TTPlayer.OptionsColorButton", reinterpret_cast<HANDLE>(1));
    SetWindowSubclass(button, OptionsImageButtonSubclassProc,
                      kOptionsImageButtonSubclass, 0);
}

void DrawOptionsImageButton(HWND button, HDC dc, HIMAGELIST images, bool captioned,
                            const DRAWITEMSTRUCT* draw = nullptr) {
    const int saved = SaveDC(dc);
    RECT client{};
    int width{}, height{};
    if (!GetClientRect(button, &client) || !ImageList_GetIconSize(images, &width, &height)) {
        RestoreDC(dc, saved);
        return;
    }
    IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    const bool flat = (style & BS_FLAT) != 0;
    const bool default_button = (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON && (style & WS_TABSTOP);
    const LRESULT state = SendMessageW(button, BM_GETSTATE, 0, 0);
    const bool enabled = draw ? !(draw->itemState & ODS_DISABLED) : IsWindowEnabled(button) != FALSE;
    const bool pressed = enabled && (draw ? (draw->itemState & ODS_SELECTED) : (state & BST_PUSHED));
    const bool hot = enabled && !pressed &&
        GetPropW(button, L"TTPlayer.OptionsButtonHot") != nullptr;
    const int theme_state = !enabled ? PBS_DISABLED : pressed ? PBS_PRESSED : hot ? PBS_HOT :
                            default_button ? PBS_DEFAULTED : PBS_NORMAL;
    // Own the theme lookup for owner-drawn controls too. Native BUTTON can
    // retain a different theme handle after a per-window WM_THEMECHANGED.
    const HTHEME theme = OpenThemeData(button, L"Button");
    FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
    // 0046E055 composites theme transparency over COLOR_BTNFACE, including
    // rounded corners. Drawing the parent here changes the border pixels.
    if (!theme || FAILED(DrawThemeBackground(theme, dc, BP_PUSHBUTTON, theme_state, &client, nullptr))) {
        if (!flat || hot || pressed)
            DrawEdge(dc, &client, pressed ? (flat ? BDR_SUNKENOUTER : EDGE_SUNKEN) :
                hot && !flat ? EDGE_RAISED : BDR_RAISEDINNER, BF_RECT);
    }
    // Port the no-caption branch of 0046E0C9 directly. BCM image layout adds
    // theme-dependent padding/rounding, so CENTER alone is not pixel-equivalent.
    wchar_t caption[1024]{};
    const int length = captioned ? GetWindowTextW(button, caption, static_cast<int>(std::size(caption))) : 0;
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    RECT text_size{};
    if (length) DrawTextW(dc, caption, length, &text_size, DT_CALCRECT | DT_SINGLELINE);
    // Captioned horizontal branch of 0046E0C9: divide the remaining width
    // into three equal gaps (before image, between image/text, after text).
    const int gap = (client.right-width-text_size.right)/3;
    const int x = (length ? gap : (client.right-width+1)/2) + pressed;
    const int y = (client.bottom-height+1)/2 + pressed;
    const RECT image_rect{x, y, x+width, y+height};
    const bool themed_icon = theme && SUCCEEDED(DrawThemeIcon(theme, dc, BP_PUSHBUTTON, theme_state, &image_rect, images, 0));
    if (!themed_icon && !enabled) {
        if (const HICON icon = ImageList_GetIcon(images, 0, ILD_NORMAL)) {
            DrawStateW(dc, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0,
                x, y, width, height, DST_ICON | DSS_DISABLED);
            DestroyIcon(icon);
        }
    } else if (!themed_icon) ImageList_Draw(images, 0, dc, x, y, ILD_NORMAL);
    if (length) {
        const int text_x = gap*2 + width + pressed;
        const int text_y = (client.bottom-text_size.bottom+1)/2 + pressed;
        RECT text{text_x, text_y, text_x+text_size.right, text_y+text_size.bottom};
        SetBkMode(dc, TRANSPARENT);
        if (!theme || FAILED(DrawThemeText(theme, dc, BP_PUSHBUTTON, theme_state,
                                           caption, length, 0, 0, &text))) {
            if (!enabled)
                DrawStateW(dc, nullptr, nullptr, reinterpret_cast<LPARAM>(caption), length,
                    text.left, text.top, text_size.right, text_size.bottom, DST_TEXT | DSS_DISABLED);
            else {
                if (default_button && !theme) {
                    RECT shadow = text;
                    ++shadow.left;
                    ++shadow.top;
                    SetTextColor(dc, GetSysColor(COLOR_BTNSHADOW));
                    DrawTextW(dc, caption, length, &shadow, 0);
                }
                SetTextColor(dc, GetSysColor(hot ? COLOR_HIGHLIGHT : COLOR_BTNTEXT));
                DrawTextW(dc, caption, length, &text, 0);
            }
        }
        const bool focused = draw ? (draw->itemState & ODS_FOCUS) != 0
                                  : GetFocus() == button;
        if (focused && (style & WS_TABSTOP)) {
            RECT focus = client;
            InflateRect(&focus, flat ? -2 : -3, flat ? -2 : -3);
            SetTextColor(dc, RGB(0, 0, 0));
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            DrawFocusRect(dc, &focus);
        }
    }
    if (old_font) SelectObject(dc, old_font);
    if (theme) CloseThemeData(theme);
    RestoreDC(dc, saved);
}

LRESULT CALLBACK OptionsImageButtonSubclassProc(
    HWND button, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR data) {
    // 00466427 initializes +0x48 to IDC_HAND; 004664CD handles
    // WM_SETCURSOR for the shared image/color button implementation.
    if (message == WM_SETCURSOR && reinterpret_cast<HWND>(wparam) == button &&
        LOWORD(lparam) == HTCLIENT && IsWindowEnabled(button)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    if (message == WM_MOUSEMOVE) {
        RECT client{}; GetClientRect(button, &client);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const bool hot = IsWindowEnabled(button) && PtInRect(&client, point);
        if (hot != (GetPropW(button, L"TTPlayer.OptionsButtonHot") != nullptr)) {
            if (hot) {
                SetPropW(button, L"TTPlayer.OptionsButtonHot", reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
                TrackMouseEvent(&track);
            } else RemovePropW(button, L"TTPlayer.OptionsButtonHot");
            InvalidateRect(button, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE || message == WM_CANCELMODE ||
               (message == WM_ENABLE && !wparam)) {
        if (RemovePropW(button, L"TTPlayer.OptionsButtonHot"))
            InvalidateRect(button, nullptr, FALSE);
    }
    if (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_ERASEBKGND) {
        const bool captioned = GetWindowTextLengthW(button) != 0;
        BUTTON_IMAGELIST layout{};
        const bool image = SendMessageW(button, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&layout)) &&
                           layout.himl && layout.himl != BCCL_NOGLYPH;
        if (image || GetPropW(button, L"TTPlayer.OptionsColorButton")) {
            const auto draw = [&](HDC target) {
                if (image) DrawOptionsImageButton(button, target, layout.himl, captioned);
                else {
                    const LRESULT state = SendMessageW(button, BM_GETSTATE, 0, 0);
                    DRAWITEMSTRUCT item{};
                    item.CtlType = ODT_BUTTON;
                    item.CtlID = static_cast<UINT>(GetDlgCtrlID(button));
                    item.itemAction = ODA_DRAWENTIRE;
                    item.itemState = (IsWindowEnabled(button) ? 0 : ODS_DISABLED) |
                        (GetFocus() == button ? ODS_FOCUS : 0) |
                        ((state & BST_PUSHED) ? ODS_SELECTED : 0);
                    item.hwndItem = button;
                    item.hDC = target;
                    GetClientRect(button, &item.rcItem);
                    SendMessageW(GetParent(button), WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
                }
            };
            if (message == WM_ERASEBKGND) return TRUE; // Paint the full surface once.
            if (message == WM_PRINTCLIENT) {
                draw(reinterpret_cast<HDC>(wparam));
            } else {
                PAINTSTRUCT paint{};
                const HDC dc = BeginPaint(button, &paint);
                RECT client{}; GetClientRect(button, &client);
                const HDC buffer = CreateCompatibleDC(dc);
                const HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
                if (buffer && bitmap) {
                    const auto previous = SelectObject(buffer, bitmap);
                    draw(buffer);
                    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
                    SelectObject(buffer, previous);
                } else draw(dc);
                if (bitmap) DeleteObject(bitmap);
                if (buffer) DeleteDC(buffer);
                EndPaint(button, &paint);
            }
            return 0;
        }
    }
    if (message != WM_NCDESTROY)
        return DefSubclassProc(button, message, wparam, lparam);
    RemoveWindowSubclass(button, OptionsImageButtonSubclassProc, subclass);
    RemovePropW(button, L"TTPlayer.OptionsButtonHot");
    RemovePropW(button, L"TTPlayer.OptionsColorButton");
    const LRESULT result = DefSubclassProc(button, message, wparam, lparam);
    if (data) ImageList_Destroy(reinterpret_cast<HIMAGELIST>(data));
    return result;
}

bool InstallOwnedOptionsButtonImage(HWND button, HIMAGELIST images, BUTTON_IMAGELIST layout) {
    BUTTON_IMAGELIST previous_layout{};
    SendMessageW(button, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&previous_layout));
    DWORD_PTR previous{};
    GetWindowSubclass(button, OptionsImageButtonSubclassProc, kOptionsImageButtonSubclass, &previous);
    layout.himl = images;
    if (!SendMessageW(button, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&layout))) {
        ImageList_Destroy(images);
        return false;
    }
    if (!SetWindowSubclass(button, OptionsImageButtonSubclassProc,
                           kOptionsImageButtonSubclass, reinterpret_cast<DWORD_PTR>(images))) {
        SendMessageW(button, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&previous_layout));
        ImageList_Destroy(images);
        return false;
    }
    if (previous) ImageList_Destroy(reinterpret_cast<HIMAGELIST>(previous));
    return true;
}

bool InstallButtonBitmap(HWND dialog, int control, HMODULE module,
                         UINT resource, int width = 0, int height = 0) {
    const HWND button = GetDlgItem(dialog, control);
    if (!button || !module) return false;
    const HBITMAP bitmap = static_cast<HBITMAP>(LoadImageW(
        module, MAKEINTRESOURCEW(resource), IMAGE_BITMAP, width, height,
        LR_CREATEDIBSECTION));
    if (!bitmap) return false;
    // 00434754/00434F8C load the resource at its original size. In particular,
    // the local-search Add/Delete/Up/Down bitmaps are 16x15, NOT 16x16.
    BITMAP source{};
    if (!GetObjectW(bitmap, sizeof(source), &source)) {
        DeleteObject(bitmap);
        return false;
    }
    const HIMAGELIST images = ImageList_Create(
        source.bmWidth, source.bmHeight, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (!images) {
        DeleteObject(bitmap);
        return false;
    }
    const int added = ImageList_AddMasked(images, bitmap, RGB(192, 192, 192));
    DeleteObject(bitmap);
    if (added < 0) {
        ImageList_Destroy(images);
        return false;
    }
    BUTTON_IMAGELIST layout{};
    layout.himl = images;
    if (GetWindowTextLengthW(button) == 0) {
        // 0046E0C9 centers image-only controls in the client rectangle.
        layout.uAlign = BUTTON_IMAGELIST_ALIGN_CENTER;
    } else {
        layout.margin = {3, 0, 3, 0};
        layout.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    }
    return InstallOwnedOptionsButtonImage(button, images, layout);
}

bool InstallButtonIcon(HWND dialog, int control, HMODULE module,
                       UINT resource, int width, int height) {
    const HWND button = GetDlgItem(dialog, control);
    if (!button || !module) return false;
    const HICON icon = static_cast<HICON>(LoadImageW(
        module, MAKEINTRESOURCEW(resource), IMAGE_ICON, width, height,
        LR_SHARED));
    if (!icon) return false;
    const HIMAGELIST images = ImageList_Create(
        width, height, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (!images || ImageList_AddIcon(images, icon) < 0) {
        if (images) ImageList_Destroy(images);
        return false;
    }
    BUTTON_IMAGELIST layout{};
    layout.himl = images;
    layout.margin = {3, 0, 3, 0};
    layout.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    return InstallOwnedOptionsButtonImage(button, images, layout);
}

void MakeOptionsHyperlink(HWND dialog, int control) {
    const HWND link = GetDlgItem(dialog, control);
    if (!link) return;
    SetWindowLongPtrW(link, GWL_STYLE,
        GetWindowLongPtrW(link, GWL_STYLE) | SS_NOTIFY);
}

bool IsOptionsPageHyperlink(UINT template_id, UINT control) {
    if (template_id == 256)
        return control == 2087 || control == 2089 || control == 2091;
    if (template_id == 261)
        return control == 1066 || control == 1067 || control == 2109;
    return false;
}

void InsertSingleListColumn(HWND list, std::wstring_view title) {
    if (!list) return;
    ListView_SetExtendedListViewStyle(list,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_LABELTIP);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 320;
    column.pszText = const_cast<wchar_t*>(title.data());
    ListView_InsertColumn(list, 0, &column);
}

void AddListText(HWND list, int row, std::wstring_view text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(text.data());
    ListView_InsertItem(list, &item);
}

void DrawOptionsSwatchButton(const DRAWITEMSTRUCT& item, int size, int count,
                             const std::array<COLORREF, 3>& colors,
                             COLORREF transparent) {
    // 00491FEF uses a 15x15 solid swatch; 00494C9D uses 16x16 horizontal
    // color bands, not an interpolated gradient, for the desktop presets.
    WTL::CDC bitmap_dc;
    bitmap_dc.CreateCompatibleDC(item.hDC);
    WTL::CBitmap bitmap;
    bitmap.CreateCompatibleBitmap(item.hDC, size, size);
    if (!bitmap_dc || !bitmap) return;
    const auto old_bitmap = SelectObject(bitmap_dc, bitmap);
    RECT swatch{0, 0, size, size};
    FillRect(bitmap_dc, &swatch, GetSysColorBrush(COLOR_BTNFACE));
    FrameRect(bitmap_dc, &swatch, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    InflateRect(&swatch, -2, -2);
    count = std::clamp(count, 1, static_cast<int>(colors.size()));
    const int band_height = (swatch.bottom - swatch.top) / count;
    for (int band = 0; band < count; ++band) {
        RECT part{swatch.left, swatch.top + band * band_height, swatch.right,
                  band + 1 == count ? swatch.bottom : swatch.top + (band + 1) * band_height};
        FillOptionsColor(bitmap_dc, part, colors[static_cast<size_t>(band)]);
    }
    SelectObject(bitmap_dc, old_bitmap);
    const HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (!images) return;
    if (ImageList_AddMasked(images, bitmap, transparent) >= 0) {
        WTL::CDC buffer;
        buffer.CreateCompatibleDC(item.hDC);
        WTL::CBitmap surface;
        const int width = item.rcItem.right - item.rcItem.left;
        const int height = item.rcItem.bottom - item.rcItem.top;
        surface.CreateCompatibleBitmap(item.hDC, width, height);
        if (buffer && surface) {
            const auto old = SelectObject(buffer, surface);
            DrawOptionsImageButton(item.hwndItem, buffer, images, true, &item);
            BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, width, height, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
        } else DrawOptionsImageButton(item.hwndItem, item.hDC, images, true, &item);
    }
    ImageList_Destroy(images);
}

void DrawColorButton(const DRAWITEMSTRUCT& item, COLORREF color) {
    DrawOptionsSwatchButton(item, 15, 1, {color, color, color}, (~color) & 0xffffff);
}

// CColorSelectCtrl, recovered from 0048CD60..0048D0A5 and the common
// selector base at 004A0205..004A4944.  The values are the original Win32
// COLORREF dwords (00BBGGRR); the duplicate entry at index 46 is intentional.
constexpr std::array<COLORREF, 48> kLegacyPresetColors{
    0x008080ff, 0x0080ffff, 0x0080ff80, 0x0080ff00,
    0x00ffff80, 0x00ff8000, 0x00c080ff, 0x00ff80ff,
    0x000000ff, 0x0000ffff, 0x0000ff80, 0x0040ff00,
    0x00ffff00, 0x00c08000, 0x00c08080, 0x00ff00ff,
    0x00404080, 0x004080ff, 0x0000ff00, 0x00808000,
    0x00804000, 0x00ff8080, 0x00400080, 0x008000ff,
    0x00000080, 0x000080ff, 0x00008000, 0x00408000,
    0x00ff0000, 0x00a00000, 0x00800080, 0x00ff0080,
    0x00000040, 0x00004080, 0x00004000, 0x00404000,
    0x00800000, 0x00400000, 0x00400040, 0x00800040,
    0x00000000, 0x00008080, 0x00408080, 0x00808080,
    0x00808040, 0x00c0c0c0, 0x00400040, 0x00ffffff};

constexpr wchar_t kLegacyColorPopupClass[] = L"ColorSelectCtrl";
constexpr int kLegacyColorCurrent = 0x7ffe;
constexpr int kLegacyColorCustom = 0x7ffd;
constexpr int kLegacyColorOutside = 0x7fff;
constexpr int kLegacyColorPopupWidth = 156;
constexpr int kLegacyColorPopupHeight = 171;

enum class LegacyColorPopupAction { cancel, color, custom };

struct LegacyColorPopupContext {
    HWND window{};
    HWND owner{};
    HWND button{};
    HMODULE resources{};
    COLORREF initial{};
    COLORREF result{};
    std::function<void(COLORREF)> on_selected;
    LegacyColorPopupAction action{LegacyColorPopupAction::cancel};
    int hot{kLegacyColorOutside};
    int pressed{kLegacyColorOutside};
    int selected{kLegacyColorOutside};
    bool tracking{};
    bool done{};
    bool creation_pending{true};
};

HWND g_legacy_color_popup{};

std::optional<COLORREF> RunLegacyCustomColorDialog(HWND owner,
                                                    COLORREF initial);

RECT LegacyColorRawRect(int item) {
    if (item == kLegacyColorCurrent) return {3, 3, 147, 23};
    if (item == kLegacyColorCustom) return {3, 142, 147, 162};
    if (item >= 0 && item < static_cast<int>(kLegacyPresetColors.size())) {
        const int column = item % 8;
        const int row = item / 8;
        return {3 + column * 18, 26 + row * 18,
                21 + column * 18, 44 + row * 18};
    }
    return {};
}

int LegacyColorHitTest(POINT point) {
    for (int index = 0;
         index < static_cast<int>(kLegacyPresetColors.size()); ++index) {
        const RECT bounds = LegacyColorRawRect(index);
        if (PtInRect(&bounds, point)) return index;
    }
    const RECT current = LegacyColorRawRect(kLegacyColorCurrent);
    if (PtInRect(&current, point)) return kLegacyColorCurrent;
    const RECT custom = LegacyColorRawRect(kLegacyColorCustom);
    return PtInRect(&custom, point) ? kLegacyColorCustom
                                    : kLegacyColorOutside;
}

void InvalidateLegacyColorItem(LegacyColorPopupContext& context, int item) {
    if (!context.window || item == kLegacyColorOutside) return;
    RECT bounds = LegacyColorRawRect(item);
    InvalidateRect(context.window, &bounds, TRUE);
}

void PaintLegacyColorPopup(HWND window, HDC dc,
                           LegacyColorPopupContext& context) {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));

    // FUN_004A455D draws a two-pixel sunken separator immediately above the
    // custom row before forwarding its owner-draw item.
    RECT separator{3, 137, 147, 139};
    DrawEdge(dc, &separator, EDGE_SUNKEN, BF_RECT);

    const auto draw_active_background = [&](int item) {
        if (item != context.hot && item != context.pressed &&
            item != context.selected) return;
        RECT bounds = LegacyColorRawRect(item);
        FillRect(dc, &bounds, GetSysColorBrush(COLOR_HIGHLIGHT));
    };
    const auto draw_swatch = [&](int item, COLORREF color, bool current) {
        draw_active_background(item);
        RECT bounds = LegacyColorRawRect(item);
        // The common selector deflates all forwarded item rectangles by the
        // constructor defaults (2,2). CColorSelectCtrl then uses a two-pixel
        // inset for the current/screen-pick cell and one pixel for presets.
        InflateRect(&bounds, -2, -2);
        FrameRect(dc, &bounds,
                  reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        InflateRect(&bounds, current ? -2 : -1, current ? -2 : -1);
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &bounds, brush);
        DeleteObject(brush);
    };

    draw_swatch(kLegacyColorCurrent, context.initial, true);
    for (int index = 0;
         index < static_cast<int>(kLegacyPresetColors.size()); ++index) {
        draw_swatch(index, kLegacyPresetColors[static_cast<size_t>(index)],
                    false);
    }

    draw_active_background(kLegacyColorCustom);
    RECT custom = LegacyColorRawRect(kLegacyColorCustom);
    InflateRect(&custom, -2, -2);
    const bool active = context.hot == kLegacyColorCustom ||
                        context.pressed == kLegacyColorCustom ||
                        context.selected == kLegacyColorCustom;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(active ? COLOR_HIGHLIGHTTEXT
                                        : COLOR_BTNTEXT));
    const HWND font_source = context.button ? context.button : context.owner;
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(font_source, WM_GETFONT, 0, 0));
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    auto label = LoadResourceText(context.resources, 0x814b);
    if (label.empty()) label = L"Custom...";
    DrawTextW(dc, label.c_str(), -1, &custom,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (old_font) SelectObject(dc, old_font);
}

void FinishLegacyColorPopup(LegacyColorPopupContext& context,
                            LegacyColorPopupAction action,
                            COLORREF result = CLR_INVALID) {
    if (context.done) return;
    context.action = action;
    if (action == LegacyColorPopupAction::color) context.result = result;
    context.done = true;
    const HWND popup = context.window;
    const HWND owner = context.owner;
    const COLORREF initial = context.initial;
    auto on_selected = std::move(context.on_selected);
    if (GetCapture() == popup) ReleaseCapture();
    if (popup && IsWindow(popup)) DestroyWindow(popup);
    if (g_legacy_color_popup == popup) g_legacy_color_popup = nullptr;

    std::optional<COLORREF> selected;
    if (action == LegacyColorPopupAction::custom &&
        owner && IsWindow(owner)) {
        selected = RunLegacyCustomColorDialog(owner, initial);
    } else if (action == LegacyColorPopupAction::color) {
        selected = result;
    }
    if (selected && on_selected && owner && IsWindow(owner))
        on_selected(*selected);
    delete &context;
}

LRESULT CALLBACK LegacyColorPopupProc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    auto* context = reinterpret_cast<LegacyColorPopupContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        context = static_cast<LegacyColorPopupContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(context));
        if (context) context->window = window;
    }
    if (!context) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        PaintLegacyColorPopup(window, dc, *context);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintLegacyColorPopup(window, reinterpret_cast<HDC>(wparam), *context);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int hit = LegacyColorHitTest(point);
        if (hit != context->hot) {
            const int previous = context->hot;
            context->hot = hit;
            InvalidateLegacyColorItem(*context, previous);
            InvalidateLegacyColorItem(*context, hit);
        }
        if (!context->tracking) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window,
                                     HOVER_DEFAULT};
            TrackMouseEvent(&tracking);
            context->tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        context->tracking = false;
        if (GetCapture() != window) {
            const int previous = context->hot;
            context->hot = kLegacyColorOutside;
            InvalidateLegacyColorItem(*context, previous);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        context->pressed = LegacyColorHitTest(point);
        if (context->pressed != kLegacyColorOutside) {
            SetCapture(window);
            InvalidateLegacyColorItem(*context, context->pressed);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int pressed = context->pressed;
        const int hit = LegacyColorHitTest(point);
        context->pressed = kLegacyColorOutside;
        if (GetCapture() == window) ReleaseCapture();
        InvalidateLegacyColorItem(*context, pressed);
        if (pressed == kLegacyColorCurrent &&
            hit == kLegacyColorOutside) {
            POINT screen = point;
            ClientToScreen(window, &screen);
            const HDC desktop = GetDC(nullptr);
            const COLORREF sampled = desktop
                ? GetPixel(desktop, screen.x, screen.y) : CLR_INVALID;
            if (desktop) ReleaseDC(nullptr, desktop);
            if (sampled != CLR_INVALID)
                FinishLegacyColorPopup(*context,
                                       LegacyColorPopupAction::color,
                                       sampled);
            return 0;
        }
        if (hit != pressed) return 0;
        if (hit >= 0 &&
            hit < static_cast<int>(kLegacyPresetColors.size())) {
            FinishLegacyColorPopup(*context, LegacyColorPopupAction::color,
                kLegacyPresetColors[static_cast<size_t>(hit)]);
        } else if (hit == kLegacyColorCurrent) {
            FinishLegacyColorPopup(*context, LegacyColorPopupAction::color,
                                   context->initial);
        } else if (hit == kLegacyColorCustom) {
            FinishLegacyColorPopup(*context, LegacyColorPopupAction::custom);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            FinishLegacyColorPopup(*context, LegacyColorPopupAction::cancel);
            return 0;
        }
        if ((wparam == VK_RETURN || wparam == VK_SPACE) &&
            context->selected >= 0 &&
            context->selected < static_cast<int>(kLegacyPresetColors.size())) {
            FinishLegacyColorPopup(*context, LegacyColorPopupAction::color,
                kLegacyPresetColors[static_cast<size_t>(context->selected)]);
            return 0;
        }
        break;
    case WM_CANCELMODE:
        FinishLegacyColorPopup(*context, LegacyColorPopupAction::cancel);
        return 0;
    case WM_CAPTURECHANGED:
        if (!context->done && context->pressed != kLegacyColorOutside &&
            reinterpret_cast<HWND>(lparam) != window) {
            FinishLegacyColorPopup(*context,
                                   LegacyColorPopupAction::cancel);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && !context->done) {
            FinishLegacyColorPopup(*context, LegacyColorPopupAction::cancel);
            return 0;
        }
        break;
    case WM_CLOSE:
        FinishLegacyColorPopup(*context, LegacyColorPopupAction::cancel);
        return 0;
    case WM_NCDESTROY:
        context->window = nullptr;
        if (g_legacy_color_popup == window)
            g_legacy_color_popup = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (!context->done && !context->creation_pending) {
            context->done = true;
            delete context;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool EnsureLegacyColorPopupClass(HINSTANCE instance) {
    WNDCLASSEXW existing{sizeof(existing)};
    if (GetClassInfoExW(instance, kLegacyColorPopupClass, &existing))
        return true;
    WNDCLASSEXW type{sizeof(type)};
    type.style = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS;
    type.lpfnWndProc = LegacyColorPopupProc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = kLegacyColorPopupClass;
    return RegisterClassExW(&type) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::optional<COLORREF> RunLegacyCustomColorDialog(HWND owner,
                                                    COLORREF initial) {
    static std::array<COLORREF, 16> custom{};
    CHOOSECOLORW chooser{sizeof(chooser)};
    chooser.hwndOwner = owner;
    chooser.rgbResult = initial;
    chooser.lpCustColors = custom.data();
    chooser.Flags = CC_FULLOPEN;
    if (initial != 0) chooser.Flags |= CC_RGBINIT;
    return ChooseColorW(&chooser)
        ? std::optional<COLORREF>{chooser.rgbResult} : std::nullopt;
}

void FillGradientStops(HDC dc, RECT bounds, int count,
                       const std::array<COLORREF, 3>& colors) {
    count = std::clamp(count, 1, static_cast<int>(colors.size()));
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    if (count == 1) {
        const HBRUSH brush = CreateSolidBrush(colors[0]);
        FillRect(dc, &bounds, brush);
        DeleteObject(brush);
        return;
    }
    for (int segment = 0; segment + 1 < count; ++segment) {
        RECT part = bounds;
        part.top = bounds.top + MulDiv(bounds.bottom - bounds.top,
                                       segment, count - 1);
        part.bottom = bounds.top + MulDiv(bounds.bottom - bounds.top,
                                          segment + 1, count - 1);
        TRIVERTEX vertices[2]{};
        vertices[0].x = part.left;
        vertices[0].y = part.top;
        vertices[0].Red = static_cast<COLOR16>(GetRValue(colors[segment]) << 8);
        vertices[0].Green = static_cast<COLOR16>(GetGValue(colors[segment]) << 8);
        vertices[0].Blue = static_cast<COLOR16>(GetBValue(colors[segment]) << 8);
        vertices[0].Alpha = 0xff00;
        vertices[1].x = part.right;
        vertices[1].y = part.bottom;
        vertices[1].Red = static_cast<COLOR16>(GetRValue(colors[segment + 1]) << 8);
        vertices[1].Green = static_cast<COLOR16>(GetGValue(colors[segment + 1]) << 8);
        vertices[1].Blue = static_cast<COLOR16>(GetBValue(colors[segment + 1]) << 8);
        vertices[1].Alpha = 0xff00;
        GRADIENT_RECT gradient{0, 1};
        GradientFill(dc, vertices, 2, &gradient, 1,
                     GRADIENT_FILL_RECT_V);
    }
}

void DrawGradientButton(const DRAWITEMSTRUCT& item, int count,
                        const std::array<COLORREF, 3>& colors) {
    DrawOptionsSwatchButton(item, 16, count, colors, RGB(192,192,192));
}

struct GradientProfileDialogContext {
    HMODULE resources{};
    int count{3};
    std::array<COLORREF, 3> colors{};
};

void RefreshGradientProfileDialog(HWND dialog,
                                  const GradientProfileDialogContext& value) {
    SendDlgItemMessageW(dialog, 2263, CB_SETCURSEL,
                        std::clamp(value.count, 1, 3) - 1, 0);
    for (int index = 0; index < 3; ++index) {
        const HWND button = GetDlgItem(dialog, 2264 + index);
        EnableWindow(button, index < value.count);
        InvalidateRect(button, nullptr, TRUE);
    }
    for (int index = 3; index < 5; ++index)
        ShowWindow(GetDlgItem(dialog, 2264 + index), SW_HIDE);
    InvalidateRect(GetDlgItem(dialog, 1068), nullptr, TRUE);
}

INT_PTR CALLBACK GradientProfileDialogProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* value = reinterpret_cast<GradientProfileDialogContext*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        value = reinterpret_cast<GradientProfileDialogContext*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(value));
        if (!value) return FALSE;
        const HWND count = GetDlgItem(dialog, 2263);
        SendMessageW(count, CB_RESETCONTENT, 0, 0);
        for (int index = 1; index <= 3; ++index) {
            const auto text = std::to_wstring(index);
            SendMessageW(count, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(text.c_str()));
        }
        for (int index = 0; index < 3; ++index)
            MakeColorButton(dialog, 2264 + index);
        RefreshGradientProfileDialog(dialog, *value);
        return TRUE;
    }
    if (!value) return FALSE;
    switch (message) {
    case WM_COMMAND: {
        const UINT control = LOWORD(wparam);
        const UINT notification = HIWORD(wparam);
        if (control == 2263 && notification == CBN_SELCHANGE) {
            value->count = std::clamp(static_cast<int>(
                SendDlgItemMessageW(dialog, 2263, CB_GETCURSEL, 0, 0)) + 1,
                1, 3);
            RefreshGradientProfileDialog(dialog, *value);
            return TRUE;
        }
        if (control >= 2264 && control <= 2266 &&
            notification == BN_CLICKED) {
            const size_t index = static_cast<size_t>(control - 2264);
            ShowLegacyPresetColor(
                dialog, GetDlgItem(dialog, static_cast<int>(control)),
                value->resources, value->colors[index],
                [dialog, value, index](COLORREF selected) {
                if (!IsWindow(dialog)) return;
                value->colors[index] = selected;
                RefreshGradientProfileDialog(dialog, *value);
            });
            return TRUE;
        }
        if (control == IDOK) {
            EndDialog(dialog, 1);
            return TRUE;
        }
        if (control == IDCANCEL) {
            EndDialog(dialog, 2);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (!item) break;
        if (item->CtlID >= 2264 && item->CtlID <= 2266) {
            DrawColorButton(*item,
                value->colors[static_cast<size_t>(item->CtlID - 2264)]);
            return TRUE;
        }
        if (item->CtlID == 1068) {
            RECT bounds = item->rcItem;
            FillGradientStops(item->hDC, bounds, value->count, value->colors);
            FrameRect(item->hDC, &bounds,
                      reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dialog, 2);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

bool EditGradientProfile(HWND owner, HMODULE resources, int& count,
                         std::array<COLORREF, 3>& colors) {
    GradientProfileDialogContext value{resources, std::clamp(count, 1, 3),
                                       colors};
    if (ShowWtlModalDialog(resources, MAKEINTRESOURCEW(386), owner,
                        GradientProfileDialogProc,
                        reinterpret_cast<LPARAM>(&value)) != 1) return false;
    count = value.count;
    colors = value.colors;
    return true;
}

struct DesktopProfileDialogContext {
    HMODULE resources{};
    settings::DesktopLyricColorProfile working;
    settings::DesktopLyricColorProfile defaults;
};

void DrawDesktopProfileSample(const DRAWITEMSTRUCT& item,
                              const DesktopProfileDialogContext& context) {
    RECT bounds = item.rcItem;
    FillRect(item.hDC, &bounds, GetSysColorBrush(COLOR_WINDOW));
    FrameRect(item.hDC, &bounds,
              reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    InflateRect(&bounds, -5, -4);
    RECT first = bounds;
    first.bottom = first.top + (first.bottom - first.top) / 2;
    RECT second = bounds;
    second.top = first.bottom;
    const int saved = SaveDC(item.hDC);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, context.working.background_colors[0]);
    DrawTextW(item.hDC, L"TTPlayer", -1, &first,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(item.hDC, context.working.played_colors[0]);
    DrawTextW(item.hDC, L"TTPlayer", -1, &second,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(item.hDC, saved);
}

void RefreshDesktopProfileDialog(HWND dialog,
                                 const DesktopProfileDialogContext& value) {
    SetDlgItemTextW(dialog, 1005, value.working.name.c_str());
    InvalidateRect(GetDlgItem(dialog, 1155), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, 1156), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, 1068), nullptr, TRUE);
}

INT_PTR CALLBACK DesktopProfileDialogProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* value = reinterpret_cast<DesktopProfileDialogContext*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        value = reinterpret_cast<DesktopProfileDialogContext*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(value));
        if (!value) return FALSE;
        SendDlgItemMessageW(dialog, 1005, EM_SETLIMITTEXT, 12, 0);
        MakeColorButton(dialog, 1155);
        MakeColorButton(dialog, 1156);
        InstallButtonBitmap(dialog, 2094, value->resources, 1098);
        RefreshDesktopProfileDialog(dialog, *value);
        return TRUE;
    }
    if (!value) return FALSE;
    switch (message) {
    case WM_COMMAND: {
        const UINT control = LOWORD(wparam);
        const UINT notification = HIWORD(wparam);
        if ((control == 1155 || control == 1156) &&
            notification == BN_CLICKED) {
            int& count = control == 1155
                ? value->working.background_count
                : value->working.played_count;
            auto& colors = control == 1155
                ? value->working.background_colors
                : value->working.played_colors;
            if (EditGradientProfile(dialog, value->resources, count, colors))
                RefreshDesktopProfileDialog(dialog, *value);
            return TRUE;
        }
        if (control == 3 && notification == BN_CLICKED) {
            value->working = value->defaults;
            RefreshDesktopProfileDialog(dialog, *value);
            return TRUE;
        }
        if (control == 2094 || control == IDOK) {
            std::array<wchar_t, 13> name{};
            GetDlgItemTextW(dialog, 1005, name.data(),
                            static_cast<int>(name.size()));
            value->working.name = name.data();
            EndDialog(dialog, 1);
            return TRUE;
        }
        if (control == IDCANCEL) {
            EndDialog(dialog, 2);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (!item) break;
        if (item->CtlID == 1155) {
            DrawGradientButton(*item, value->working.background_count,
                               value->working.background_colors);
            return TRUE;
        }
        if (item->CtlID == 1156) {
            DrawGradientButton(*item, value->working.played_count,
                               value->working.played_colors);
            return TRUE;
        }
        if (item->CtlID == 1068) {
            DrawDesktopProfileSample(*item, *value);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dialog, 2);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

bool EditDesktopProfile(HWND owner, HMODULE resources,
                        settings::DesktopLyricColorProfile& profile,
                        const settings::DesktopLyricColorProfile& defaults) {
    DesktopProfileDialogContext value{resources, profile, defaults};
    if (ShowWtlModalDialog(resources, MAKEINTRESOURCEW(387), owner,
                        DesktopProfileDialogProc,
                        reinterpret_cast<LPARAM>(&value)) != 1) return false;
    profile = std::move(value.working);
    return true;
}

void PositionNestedDialog(HWND parent, HWND tab, HWND child,
                          UINT flags = SWP_SHOWWINDOW) {
    if (!parent || !tab || !child) return;
    RECT bounds{};
    GetClientRect(tab, &bounds);
    TabCtrl_AdjustRect(tab, FALSE, &bounds);
    MapWindowPoints(tab, parent, reinterpret_cast<POINT*>(&bounds), 2);
    // The tab and its page are siblings. The page must be in front so the
    // tab's WS_CLIPSIBLINGS excludes it from background painting.
    SetWindowPos(child, HWND_TOP, bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE | flags);
}

std::optional<std::filesystem::path> BrowseForFolder(HWND owner) {
    ModernFolderOptions dialog;
    dialog.owner = owner;
    const auto selected = ModernPickFolder(dialog);
    return selected
        ? std::optional<std::filesystem::path>{selected->path}
        : std::nullopt;
}

std::filesystem::path WithLegacyDirectoryTerminator(
    std::filesystem::path value) {
    auto text = value.wstring();
    if (!text.empty() && text.back() != L'\\') text.push_back(L'\\');
    return std::filesystem::path(std::move(text));
}

struct CheckedFolderResult {
    std::filesystem::path path;
    bool checked{};
};

// DAT_00547E88 starts cleared and is updated only after a successful folder
// selection. It deliberately is not persisted to XML.
bool checked_folder_recursive{};

std::optional<CheckedFolderResult> BrowseForCheckedFolder(
    HWND owner, std::wstring title, std::wstring checkbox_text) {
    ModernFolderOptions dialog;
    dialog.owner = owner;
    dialog.title = std::move(title);
    dialog.checkbox_label = std::move(checkbox_text);
    dialog.checkbox_checked = checked_folder_recursive;
    const auto selected = ModernPickFolder(dialog);
    if (!selected) return std::nullopt;
    checked_folder_recursive = selected->checkbox_checked;
    return CheckedFolderResult{selected->path, selected->checkbox_checked};
}

std::optional<std::filesystem::path> ChooseOptionsProfileFile(
    HWND owner, bool open, const wchar_t* extension,
    const std::filesystem::path& current, const wchar_t* filter_caption) {
    std::filesystem::path initial = current;
    if (initial.empty()) {
        initial = CurrentExecutablePath().parent_path() /
                  (std::wstring(L"1.") + extension);
    }
    std::wstring pattern = L"*.";
    pattern += extension;
    const std::wstring caption(filter_caption);
    const std::vector<ModernDialogFilter> filters{{caption, pattern}};
    if (open) {
        ModernOpenFileOptions dialog;
        dialog.owner = owner;
        dialog.filters = filters;
        dialog.initial_path = initial;
        dialog.default_extension = extension;
        return ModernOpenFile(dialog);
    }
    ModernSaveFileOptions dialog;
    dialog.owner = owner;
    dialog.filters = filters;
    dialog.initial_path = initial;
    dialog.default_extension = extension;
    return ModernSaveFile(dialog);
}

UINT TrackProfileTransferMenu(HWND dialog, HMODULE resources, int control) {
    HMENU menu = i18n::LoadMenu(resources, MAKEINTRESOURCEW(158));
    const HMENU popup = menu ? GetSubMenu(menu, 0) : nullptr;
    UINT command{};
    if (popup) {
        RECT bounds{};
        GetWindowRect(GetDlgItem(dialog, control), &bounds);
        command = TrackPlayerPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 bounds.left, bounds.bottom, 0, dialog,
                                 nullptr);
    }
    if (menu) DestroyMenu(menu);
    return command;
}

std::wstring MenuTextAt(HMENU menu, int position) {
    const int length = GetMenuStringW(menu, static_cast<UINT>(position),
                                      nullptr, 0, MF_BYPOSITION);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetMenuStringW(menu, static_cast<UINT>(position), text.data(),
                   static_cast<int>(text.size()), MF_BYPOSITION);
    text.resize(static_cast<size_t>(length));
    return text;
}

UINT TrackDesktopProfileMenu(
    HWND dialog, HMODULE resources, int control,
    const std::array<settings::DesktopLyricColorProfile, 3>& profiles,
    int selected) {
    HMENU menu = i18n::LoadMenu(resources, MAKEINTRESOURCEW(388));
    const HMENU popup = menu ? GetSubMenu(menu, 0) : nullptr;
    if (!popup) {
        if (menu) DestroyMenu(menu);
        return 0;
    }

    HMENU modify{};
    std::wstring modify_caption;
    const int original_count = GetMenuItemCount(popup);
    for (int position = 0; position < original_count; ++position) {
        if (HMENU candidate = GetSubMenu(popup, position)) {
            modify = candidate;
            modify_caption = MenuTextAt(popup, position);
            RemoveMenu(popup, position, MF_BYPOSITION);
            break;
        }
    }
    while (GetMenuItemCount(popup) > 0)
        DeleteMenu(popup, 0, MF_BYPOSITION);
    if (!modify) modify = CreatePopupMenu();
    while (GetMenuItemCount(modify) > 0)
        DeleteMenu(modify, 0, MF_BYPOSITION);

    for (size_t index = 0; index < profiles.size(); ++index) {
        const UINT flags = MF_STRING |
            (selected == static_cast<int>(index) ? MF_CHECKED : 0);
        AppendMenuW(popup, flags, 0x80a2 + static_cast<UINT>(index),
                    profiles[index].name.c_str());
        AppendMenuW(modify, MF_STRING,
                    0x80ac + static_cast<UINT>(index),
                    profiles[index].name.c_str());
    }
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_POPUP,
                reinterpret_cast<UINT_PTR>(modify), modify_caption.c_str());

    RECT bounds{};
    GetWindowRect(GetDlgItem(dialog, control), &bounds);
    const UINT command = TrackPlayerPopupMenu(
        popup, TPM_RETURNCMD, bounds.left, bounds.bottom, 0, dialog, nullptr);
    DestroyMenu(menu);
    return command;
}

bool SameDesktopProfile(const settings::DesktopLyricColorProfile& left,
                        const settings::DesktopLyricColorProfile& right) {
    if (left.background_count != right.background_count ||
        left.played_count != right.played_count) return false;
    const size_t background_count = static_cast<size_t>(
        std::clamp(left.background_count, 0, 3));
    const size_t played_count = static_cast<size_t>(
        std::clamp(left.played_count, 0, 3));
    return std::equal(left.background_colors.begin(),
                      left.background_colors.begin() + background_count,
                      right.background_colors.begin()) &&
           std::equal(left.played_colors.begin(),
                      left.played_colors.begin() + played_count,
                      right.played_colors.begin());
}

int MatchDesktopProfile(
    const settings::DesktopLyricColorProfile& current,
    const std::array<settings::DesktopLyricColorProfile, 3>& profiles) {
    for (size_t index = 0; index < profiles.size(); ++index) {
        if (SameDesktopProfile(current, profiles[index]))
            return static_cast<int>(index);
    }
    return -1;
}

std::vector<std::wstring> IntegerLabels(
    std::initializer_list<int> values, std::wstring_view suffix = {}) {
    std::vector<std::wstring> labels;
    labels.reserve(values.size());
    for (const int value : values) {
        auto label = std::to_wstring(value);
        label.append(suffix);
        labels.push_back(std::move(label));
    }
    return labels;
}

std::wstring VirtualKeyName(int virtual_key, bool extended = false) {
    LONG key_data = static_cast<LONG>(
        MapVirtualKeyW(static_cast<UINT>(virtual_key),
                       MAPVK_VK_TO_VSC) << 16);
    if (extended) key_data |= 1 << 24;
    std::array<wchar_t, 128> name{};
    if (GetKeyNameTextW(key_data, name.data(), static_cast<int>(name.size())) > 0)
        return name.data();
    return std::to_wstring(virtual_key);
}

std::wstring HotKeyText(const settings::HotKeyAccelerator& shortcut) {
    if (shortcut.virtual_key == 0) return {};
    std::wstring text;
    // FUN_0049315F resolves modifier labels through GetKeyNameText as well,
    // so localized Windows key names remain identical to the original.
    const auto append_modifier = [&text](int virtual_key) {
        text += VirtualKeyName(virtual_key);
        text += L" + ";
    };
    if ((shortcut.modifiers & HOTKEYF_CONTROL) != 0)
        append_modifier(VK_CONTROL);
    if ((shortcut.modifiers & HOTKEYF_SHIFT) != 0)
        append_modifier(VK_SHIFT);
    if ((shortcut.modifiers & HOTKEYF_ALT) != 0)
        append_modifier(VK_MENU);
    text += VirtualKeyName(shortcut.virtual_key,
        (shortcut.modifiers & HOTKEYF_EXT) != 0);
    return text;
}

void SetHotKeyControl(HWND dialog, int control,
                      const settings::HotKeyAccelerator& shortcut) {
    SendDlgItemMessageW(dialog, control, HKM_SETHOTKEY,
        MAKEWORD(shortcut.virtual_key, shortcut.modifiers), 0);
}

settings::HotKeyAccelerator GetHotKeyControl(HWND dialog, int control) {
    const WORD value = static_cast<WORD>(
        SendDlgItemMessageW(dialog, control, HKM_GETHOTKEY, 0, 0));
    return {LOBYTE(value), HIBYTE(value)};
}

void UpdateHotKeyListRow(HWND list, int row,
                         const settings::HotKeyBinding& binding) {
    if (!list || row < 0) return;
    auto application = HotKeyText(binding.application);
    auto global = HotKeyText(binding.global);
    ListView_SetItemText(list, row, 1, application.data());
    ListView_SetItemText(list, row, 2, global.data());
}

void UpdateDspButtons(HWND dialog) {
    const HWND list = GetDlgItem(dialog, 1064);
    const int selected = list
        ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const int count = list ? ListView_GetItemCount(list) : 0;
    EnableWindow(GetDlgItem(dialog, 1015), selected >= 0 &&
                 ListView_GetCheckState(list, selected));
    EnableWindow(GetDlgItem(dialog, 1042), selected > 0);
    EnableWindow(GetDlgItem(dialog, 1045),
                 selected >= 0 && selected + 1 < count);
}

bool IsProtectedLyricFolder(HWND list, int row, HMODULE resources) {
    if (!list || row < 0) return false;
    std::array<wchar_t, 32768> text{};
    ListView_GetItemText(list, row, 0, text.data(),
                         static_cast<int>(text.size()));
    const auto sound_folder = LoadResourceText(resources, 0x8139);
    const auto download_folder = LoadResourceText(resources, 0x8138);
    const std::wstring_view value{text.data()};
    return (!sound_folder.empty() && value == sound_folder) ||
           (!download_folder.empty() && value == download_folder);
}

void UpdateFolderListButtons(HWND dialog, HMODULE resources,
                             bool lyric_folders) {
    const HWND list = GetDlgItem(dialog, 1038);
    const int selected = list
        ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const int count = list ? ListView_GetItemCount(list) : 0;
    const bool protected_row = lyric_folders &&
        IsProtectedLyricFolder(list, selected, resources);
    EnableWindow(GetDlgItem(dialog, 1039),
                 selected >= 0 && !protected_row);
    if (lyric_folders) {
        EnableWindow(GetDlgItem(dialog, 1042), selected > 0);
        EnableWindow(GetDlgItem(dialog, 1045),
                     selected >= 0 && selected + 1 < count);
    }
}

LPARAM GetListItemData(HWND list, int row) {
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    return ListView_GetItem(list, &item) ? item.lParam : -1;
}

void SetListItemData(HWND list, int row, LPARAM data) {
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    item.lParam = data;
    ListView_SetItem(list, &item);
}

void SwapListRows(HWND list, int first, int second, int columns) {
    if (!list || first < 0 || second < 0 || first == second) return;
    std::vector<std::wstring> first_text(static_cast<size_t>(columns));
    std::vector<std::wstring> second_text(static_cast<size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        std::array<wchar_t, 32768> text{};
        ListView_GetItemText(list, first, column, text.data(),
                             static_cast<int>(text.size()));
        first_text[static_cast<size_t>(column)] = text.data();
        ListView_GetItemText(list, second, column, text.data(),
                             static_cast<int>(text.size()));
        second_text[static_cast<size_t>(column)] = text.data();
    }
    const bool first_checked = ListView_GetCheckState(list, first) != FALSE;
    const bool second_checked = ListView_GetCheckState(list, second) != FALSE;
    LVITEMW first_item{};
    first_item.mask = LVIF_PARAM;
    first_item.iItem = first;
    LVITEMW second_item{};
    second_item.mask = LVIF_PARAM;
    second_item.iItem = second;
    ListView_GetItem(list, &first_item);
    ListView_GetItem(list, &second_item);
    const LPARAM first_data = first_item.lParam;
    for (int column = 0; column < columns; ++column) {
        ListView_SetItemText(list, first, column,
            second_text[static_cast<size_t>(column)].data());
        ListView_SetItemText(list, second, column,
            first_text[static_cast<size_t>(column)].data());
    }
    ListView_SetCheckState(list, first, second_checked);
    ListView_SetCheckState(list, second, first_checked);
    first_item.lParam = second_item.lParam;
    second_item.lParam = first_data;
    ListView_SetItem(list, &first_item);
    ListView_SetItem(list, &second_item);
    ListView_SetItemState(list, first, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, second, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, second, FALSE);
}

void DrawPreviewSlider(HDC dc, const skin::SkinElement& element, COLORREF key) {
    // 00467F8B..00467F9E sets range 0..100 and EAX=0x50 before 00428DCD.
    // Ghidra omits the register argument: the preview volume is fixed at 80,
    // NOT the current setting or 100. Progress retains its constructor zero.
    const int width = element.bounds.right - element.bounds.left;
    const int height = element.bounds.bottom - element.bounds.top;
    if (width <= 0 || height <= 0) return;
    const int value = element.name == L"volume" ? 80 : 0;
    const int thumb_width = element.thumb_image ? element.thumb_size.cx / 4 : 0;
    const int thumb_height = element.thumb_image ? element.thumb_size.cy : 0;
    const int extent = std::max(1, element.vertical ? thumb_height : thumb_width);
    // 00428F41: one-pixel inset, value direction reversed for vertical sliders.
    const int travel = (element.vertical ? height : width) - 2 - extent;
    const int offset = 1 + MulDiv(element.vertical ? 100 - value : value, travel, 100);
    const int x = element.bounds.left + (element.vertical ? (width - thumb_width) / 2 : offset);
    const int y = element.bounds.top + (element.vertical ? offset : (height - thumb_height) / 2);
    const int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, element.bounds.left, element.bounds.top,
                      element.bounds.right, element.bounds.bottom);
    if (element.bar_image) {
        // 00451E07 copies the slider's backing, not a sprite-strip frame.
        element.bar_image.Draw(dc, element.bounds.left, element.bounds.top,
            element.bar_size.cx, element.bar_size.cy, 0, 0,
            element.bar_size.cx, element.bar_size.cy);
    }
    // 00451E07 / 0045287B derive fill from the thumb's rounded centre,
    // including the small filled part at position zero. The extra buffered
    // fill_image2 layer has length zero in the preview (004525CB).
    if (element.fill_image) {
        const int left = element.bounds.left + (width - element.fill_size.cx) / 2;
        const int top = element.bounds.top + (height - element.fill_size.cy) / 2;
        if (element.vertical) {
            const int start = element.bounds.top + offset + extent / 2;
            const int source_y = std::clamp(start - top, 0, static_cast<int>(element.fill_size.cy));
            const int filled = element.fill_size.cy - source_y;
            if (filled > 0) element.fill_image.Draw(dc, left, start,
                element.fill_size.cx, filled, 0, source_y, element.fill_size.cx, filled, key);
        } else {
            const int filled = std::clamp<int>(element.bounds.left + offset + extent / 2 - left,
                                          0, static_cast<int>(element.fill_size.cx));
            if (filled > 0) element.fill_image.Draw(dc, left, top, filled,
                element.fill_size.cy, 0, 0, filled, element.fill_size.cy, key);
        }
    }
    if (thumb_width > 0 && thumb_height > 0 && offset >= 0) {
        element.thumb_image.Draw(dc, x, y, thumb_width, thumb_height,
            0, 0, thumb_width, thumb_height, key);
    }
    RestoreDC(dc, saved);
}

HBITMAP RenderLegacySkinPreview(const skin::LegacySkin& source,
                               HMODULE resources, HICON fallback_icon) {
    if (!source.Valid()) return nullptr;
    BITMAP background{};
    if (!GetObjectW(source.Background(), sizeof(background), &background) ||
        background.bmWidth <= 0 || background.bmHeight == 0) return nullptr;

    // CopyImage is not reliable for every DIB/DDB returned by old skin
    // packages (notably the embedded resource bitmap on newer Windows).  The
    // original preview paints a temporary player HWND into its own surface;
    // create an explicit 32-bit surface and copy the skin background into it
    // before composing the child controls.  This also guarantees that the
    // preview can later be saved as a BMP regardless of the source format.
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = background.bmWidth;
    bitmap_info.bmiHeader.biHeight = -std::abs(background.bmHeight);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    const HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    const HDC target = CreateCompatibleDC(screen);
    const HDC image = CreateCompatibleDC(screen);
    void* pixels{};
    auto bitmap = CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS,
                                   &pixels, nullptr, 0);
    if (!target || !image || !bitmap || !pixels) {
        if (target) DeleteDC(target);
        if (image) DeleteDC(image);
        if (bitmap) DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    const int height = std::abs(background.bmHeight);
    // A live skin bitmap can briefly be selected by the player paint path.
    // Reading the DIB bits does not require selecting that bitmap into a
    // second DC and is the same operation used by CreateWindowRegion.  Keep a
    // BitBlt fallback for device-dependent bitmaps supplied by old packages.
    const bool alpha_background = source.Background().IsGdiPlus();
    const int copied = alpha_background ? height : GetDIBits(screen,
        source.Background(), 0, static_cast<UINT>(height), pixels,
        &bitmap_info, DIB_RGB_COLORS);
    const HGDIOBJ old_target = SelectObject(target, bitmap);
    if (!old_target || old_target == HGDI_ERROR) {
        DeleteDC(image);
        DeleteDC(target);
        DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    if (alpha_background) {
        const RECT bounds{0, 0, background.bmWidth, height};
        FillRect(target, &bounds, GetSysColorBrush(COLOR_WINDOW));
        source.Background().Draw(target, 0, 0, background.bmWidth, height,
                                  0, 0, background.bmWidth, height);
    } else if (copied != height) {
        const HGDIOBJ old_background = SelectObject(image, source.Background());
        const bool copied_by_blt = old_background && old_background != HGDI_ERROR &&
            BitBlt(target, 0, 0, background.bmWidth, height,
                   image, 0, 0, SRCCOPY) != FALSE;
        if (old_background && old_background != HGDI_ERROR)
            SelectObject(image, old_background);
        if (!copied_by_blt) {
            SelectObject(target, old_target);
            DeleteDC(image);
            DeleteDC(target);
            DeleteObject(bitmap);
            ReleaseDC(nullptr, screen);
            return nullptr;
        }
    }
    ReleaseDC(nullptr, screen);
    // Flatten the BMP backing before PNG children are composited. A final
    // colour-key pass would remove opaque magenta PNG pixels and blend
    // translucent edges against magenta rather than the preview matte.
    GdiFlush();
    if (!alpha_background) {
        const COLORREF transparent = source.TransparentColor();
        const COLORREF window_color = GetSysColor(COLOR_WINDOW);
        auto* bytes = static_cast<unsigned char*>(pixels);
        const size_t pixel_count = static_cast<size_t>(background.bmWidth) * height;
        for (size_t index = 0; index < pixel_count; ++index) {
            unsigned char* pixel = bytes + index * 4;
            if (pixel[0] == GetBValue(transparent) &&
                pixel[1] == GetGValue(transparent) &&
                pixel[2] == GetRValue(transparent)) {
                pixel[0] = GetBValue(window_color);
                pixel[1] = GetGValue(window_color);
                pixel[2] = GetRValue(window_color);
                pixel[3] = 0xff;
            }
        }
    }
    // 00467B9B creates these child windows with their initial strings.
    // Status is empty; info is the program/version string, NOT the selected
    // track or skin name. Both strings come from this executable's ttpres.
    auto caption = LoadResourceText(resources, 0x80);
    const auto version = LoadResourceText(resources, 0x8299);
    if (!caption.empty() && !version.empty()) caption += L" ";
    caption += version;
    const auto channel = LoadResourceText(resources, 0x81b7);
    for (const auto& element : source.Elements()) {
        if (IsSuppressedSkinControl(element.name)) continue;
        if (element.name == L"pause") continue;
        if (element.name.starts_with(L"mode_") && element.name != L"mode_single") continue;
        if (element.name == L"info") {
            DrawScrollingSkinInfo(target, element, caption.c_str(), nullptr, 0, 0);
            continue;
        }
        if (element.name == L"stereo" || element.name == L"status") {
            DrawSkinText(target, element, element.name == L"stereo" ? channel.c_str() : L"");
            continue;
        }
        if (element.name == L"icon") {
            const HICON icon = source.Icon() ? source.Icon() : fallback_icon;
            ICONINFO info{};
            if (icon && GetIconInfo(icon, &info)) {
                BITMAP icon_bitmap{};
                GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(icon_bitmap), &icon_bitmap);
                if (!info.hbmColor) icon_bitmap.bmHeight /= 2;
                if (info.hbmColor) DeleteObject(info.hbmColor);
                if (info.hbmMask) DeleteObject(info.hbmMask);
                // 00415D3D / 0041606F: native icon size, centred in the XML
                // child client rectangle (Let's Vista has a 16x26 icon slot).
                const int saved = SaveDC(target);
                if (saved) {
                    IntersectClipRect(target, element.bounds.left, element.bounds.top,
                        element.bounds.right, element.bounds.bottom);
                    DrawIconEx(target,
                        element.bounds.left + (element.bounds.right - element.bounds.left - icon_bitmap.bmWidth) / 2,
                        element.bounds.top + (element.bounds.bottom - element.bounds.top - icon_bitmap.bmHeight) / 2,
                        icon, icon_bitmap.bmWidth, icon_bitmap.bmHeight, 0, nullptr, DI_NORMAL);
                    RestoreDC(target, saved);
                }
            }
            continue;
        }
        if (element.name == L"progress" || element.name == L"volume") {
            DrawPreviewSlider(target, element, source.TransparentColor());
            continue;
        }
        if (element.name == L"led") {
            // The strip contains twelve glyphs, not one static decoration.
            DrawSkinLed(target, element, FormatLedTime(std::chrono::milliseconds(0)),
                        source.TransparentColor());
            continue;
        }
        if (element.image && element.image_size.cx > 0 &&
            element.image_size.cy > 0) {
            const int frames = std::max(1, element.frames);
            const int width = element.image_size.cx / frames;
            // 004710FD disables minimode when the package has no mini_window.
            const int state = element.four_state && element.name == L"minimode" &&
                !source.SupportsMiniMode() ? 3 : 0;
            element.image.Draw(target, element.bounds.left, element.bounds.top,
                width, element.image_size.cy, state * width, 0, width,
                element.image_size.cy, source.TransparentColor());
        }
    }

    SelectObject(target, old_target);
    DeleteDC(image);
    DeleteDC(target);
    return bitmap;
}

bool SaveBitmapFile(HBITMAP bitmap, const std::filesystem::path& path) {
    if (!bitmap || path.empty()) return false;
    BITMAP native{};
    if (!GetObjectW(bitmap, sizeof(native), &native) ||
        native.bmWidth <= 0 || native.bmHeight == 0) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = native.bmWidth;
    info.bmiHeader.biHeight = std::abs(native.bmHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD row_bytes = static_cast<DWORD>(native.bmWidth) * 4;
    const DWORD image_bytes = row_bytes *
        static_cast<DWORD>(std::abs(native.bmHeight));
    std::vector<std::byte> pixels(image_bytes);
    const HDC screen = GetDC(nullptr);
    const int copied = GetDIBits(screen, bitmap, 0,
        static_cast<UINT>(std::abs(native.bmHeight)), pixels.data(), &info,
        DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (copied == 0) return false;

    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + image_bytes;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool saved = WriteFile(file, &file_header, sizeof(file_header),
                                 &written, nullptr) &&
        written == sizeof(file_header) &&
        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader),
                  &written, nullptr) && written == sizeof(info.bmiHeader) &&
        WriteFile(file, pixels.data(), image_bytes, &written, nullptr) &&
        written == image_bytes;
    CloseHandle(file);
    return saved;
}

LRESULT CALLBACK OptionsSkinPreviewSubclassProc(
    HWND preview, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR) {
    const auto paint_preview = [preview](HDC dc) -> LRESULT {
        if (!dc) return 0;
        DRAWITEMSTRUCT item{};
        item.CtlType = ODT_LISTVIEW;
        item.CtlID = static_cast<UINT>(GetDlgCtrlID(preview));
        item.itemID = static_cast<UINT>(-1);
        item.itemAction = ODA_DRAWENTIRE;
        item.hwndItem = preview;
        item.hDC = dc;
        GetClientRect(preview, &item.rcItem);
        return SendMessageW(GetParent(preview), WM_DRAWITEM,
            item.CtlID, reinterpret_cast<LPARAM>(&item));
    };
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(preview, &paint);
        static_cast<void>(paint_preview(dc));
        EndPaint(preview, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        static_cast<void>(paint_preview(reinterpret_cast<HDC>(wparam)));
        return 0;
    }
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_RBUTTONUP) {
        PostMessageW(GetParent(preview), kExportSkinPreview, 0, 0);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(preview, OptionsSkinPreviewSubclassProc,
                             subclass);
    }
    return DefSubclassProc(preview, message, wparam, lparam);
}

} // namespace

void detail::DrawOptionsColorButton(const DRAWITEMSTRUCT& item, COLORREF color) {
    DrawColorButton(item, color);
}
void detail::MakeOptionsColorButton(HWND dialog, int control) {
    MakeColorButton(dialog, control);
}
bool detail::InstallOptionsBitmapButton(HWND dialog, int control, HMODULE resources,
                                       UINT identifier) {
    return InstallButtonBitmap(dialog, control, resources, identifier);
}

HBITMAP detail::RenderSkinPreview(const skin::LegacySkin& source,
                                HMODULE resources, HICON fallback_icon) {
    return RenderLegacySkinPreview(source, resources, fallback_icon);
}

bool detail::ShowLegacyPresetColor(
    HWND owner, HWND button, HMODULE resources, COLORREF initial,
    std::function<void(COLORREF)> on_selected) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!owner || !IsWindow(owner) || !on_selected) return false;
    if (!instance || !EnsureLegacyColorPopupClass(instance)) {
        if (const auto selected = RunLegacyCustomColorDialog(owner, initial))
            on_selected(*selected);
        return false;
    }

    // FUN_004A471F creates/shows the selector and returns immediately.  Keep
    // only one selector on the UI thread and let its window procedure deliver
    // the eventual colour notification; a nested GetMessage loop here would
    // keep the originating BN_CLICKED/automation call stack blocked.
    if (g_legacy_color_popup && IsWindow(g_legacy_color_popup))
        SendMessageW(g_legacy_color_popup, WM_CLOSE, 0, 0);

    auto* context = new LegacyColorPopupContext;
    context->owner = owner;
    context->button = button;
    context->resources = resources;
    context->initial = initial;
    context->result = initial;
    context->on_selected = std::move(on_selected);
    const auto match = std::find(kLegacyPresetColors.begin(),
                                 kLegacyPresetColors.end(), initial);
    if (match != kLegacyPresetColors.end())
        context->selected = static_cast<int>(
            std::distance(kLegacyPresetColors.begin(), match));

    RECT anchor{};
    if (!button || !GetWindowRect(button, &anchor)) {
        POINT cursor{};
        GetCursorPos(&cursor);
        anchor = {cursor.x, cursor.y, cursor.x, cursor.y};
    }
    int left = anchor.left;
    int top = anchor.bottom;
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        if (left + kLegacyColorPopupWidth > monitor_info.rcWork.right)
            left = monitor_info.rcWork.right - kLegacyColorPopupWidth;
        if (left < monitor_info.rcWork.left) left = monitor_info.rcWork.left;
        if (top + kLegacyColorPopupHeight > monitor_info.rcWork.bottom)
            top = anchor.top - kLegacyColorPopupHeight;
        if (top < monitor_info.rcWork.top) top = monitor_info.rcWork.top;
    }

    constexpr DWORD kOriginalStyle = 0x94400100u;
    constexpr DWORD kOriginalExtendedStyle = 0x00000100u;
    const HWND popup = CreateWindowExW(
        kOriginalExtendedStyle, kLegacyColorPopupClass, L"",
        kOriginalStyle, left, top, kLegacyColorPopupWidth,
        kLegacyColorPopupHeight, owner, nullptr, instance, context);
    if (!popup) {
        auto callback = std::move(context->on_selected);
        delete context;
        if (const auto selected = RunLegacyCustomColorDialog(owner, initial))
            callback(*selected);
        return false;
    }
    context->creation_pending = false;
    g_legacy_color_popup = popup;
    SetWindowPos(popup, HWND_TOP, left, top, kLegacyColorPopupWidth,
                 kLegacyColorPopupHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(popup);
    SetFocus(popup);
    UpdateWindow(popup);
    return true;
}

void PlayerWindow::CancelOptionsDspScan() {
    if (options_dsp_scan_process_) {
        DWORD exit_code{};
        if (GetExitCodeProcess(options_dsp_scan_process_, &exit_code) &&
            exit_code == STILL_ACTIVE) {
            if (options_dsp_scan_job_)
                TerminateJobObject(options_dsp_scan_job_, ERROR_CANCELLED);
            else
                TerminateProcess(options_dsp_scan_process_, ERROR_CANCELLED);
        }
        CloseHandle(options_dsp_scan_process_);
        options_dsp_scan_process_ = nullptr;
    }
    if (options_dsp_scan_job_) {
        CloseHandle(options_dsp_scan_job_);
        options_dsp_scan_job_ = nullptr;
    }
    if (!options_dsp_scan_request_.empty())
        DeleteFileW(options_dsp_scan_request_.c_str());
    if (!options_dsp_scan_output_.empty())
        DeleteFileW(options_dsp_scan_output_.c_str());
    options_dsp_scan_request_.clear();
    options_dsp_scan_output_.clear();
    options_dsp_scan_started_ = 0;
}

void PlayerWindow::StartOptionsDspScan(
    HWND dialog, const std::filesystem::path& folder) {
    CancelOptionsDspScan();
    options_dsp_scan_complete_ = false;
    options_dsp_paths_.clear();
    options_dsp_scan_request_ = DspTemporaryFile();
    options_dsp_scan_output_ = DspTemporaryFile();
    if (options_dsp_scan_request_.empty() ||
        options_dsp_scan_output_.empty()) {
        CancelOptionsDspScan();
        return;
    }
    const auto runtime_plugins = FindRuntimePath(L"Plugins");
    if (!WriteDspRequest(options_dsp_scan_request_, folder,
                         settings_.plugin.modules, runtime_plugins)) {
        CancelOptionsDspScan();
        return;
    }

    const auto helper = app::CurrentExecutablePath();
    if (helper.empty()) {
        CancelOptionsDspScan();
        return;
    }
    std::wstring command = app::WorkerCommandPrefix(helper, app::kDspWorkerSwitch) +
                           L" --scan " +
                           QuoteDspArgument(options_dsp_scan_request_.wstring()) +
                           L" " +
                           QuoteDspArgument(options_dsp_scan_output_.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        helper.parent_path().c_str(), &startup, &process)) {
        CancelOptionsDspScan();
        return;
    }
    options_dsp_scan_process_ = process.hProcess;
    options_dsp_scan_job_ = CreateJobObjectW(nullptr, nullptr);
    if (options_dsp_scan_job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(options_dsp_scan_job_,
                                     JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(options_dsp_scan_job_, process.hProcess)) {
            CloseHandle(options_dsp_scan_job_);
            options_dsp_scan_job_ = nullptr;
        }
    }
    const DWORD resumed = ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    if (resumed == static_cast<DWORD>(-1)) {
        CancelOptionsDspScan();
        return;
    }
    options_dsp_scan_started_ = GetTickCount64();
    SetTimer(dialog, kOptionsDspPollTimer, kOptionsDspPollMilliseconds,
             nullptr);
}

void PlayerWindow::PollOptionsDspScan(HWND dialog) {
    if (!options_dsp_scan_process_) {
        KillTimer(dialog, kOptionsDspPollTimer);
        return;
    }
    const DWORD wait = WaitForSingleObject(options_dsp_scan_process_, 0);
    if (wait == WAIT_TIMEOUT && GetTickCount64() - options_dsp_scan_started_ <
                                    kOptionsDspScanTimeoutMilliseconds)
        return;

    std::optional<std::vector<DspProbeResult>> results;
    DWORD exit_code{};
    if (wait == WAIT_OBJECT_0 &&
        GetExitCodeProcess(options_dsp_scan_process_, &exit_code) &&
        exit_code == 0) {
        results = ReadDspResults(options_dsp_scan_output_);
    }
    KillTimer(dialog, kOptionsDspPollTimer);
    CancelOptionsDspScan();

    // A crashed, timed-out, or malformed helper result is not an empty scan.
    // Keep the persisted DSP chain untouched; CommitOptionsPage(259) is only
    // allowed to replace it after a completely validated result stream.
    if (!results) {
        options_dsp_scan_complete_ = false;
        UpdateDspButtons(dialog);
        return;
    }
    options_dsp_scan_complete_ = true;

    const HWND list = GetDlgItem(dialog, 1064);
    if (!list || !IsWindow(list)) return;
    RemovePropW(dialog, kPageReadyProperty);
    ListView_DeleteAllItems(list);
    options_dsp_paths_.clear();
    for (const auto& result : *results) {
        const size_t index = options_dsp_paths_.size();
        options_dsp_paths_.push_back(result.path);
        const auto file = result.path.filename().wstring();
        AddListText(list, static_cast<int>(index), L"");
        SetListItemData(list, static_cast<int>(index),
                        static_cast<LPARAM>(index));
        auto description = result.description;
        ListView_SetItemText(list, static_cast<int>(index), 1,
                             description.data());
        ListView_SetItemText(list, static_cast<int>(index), 2,
                             const_cast<wchar_t*>(file.c_str()));
        const auto full = result.path.wstring();
        const bool active = std::any_of(settings_.plugin.modules.begin(),
            settings_.plugin.modules.end(), [&file, &full](const auto& module) {
                return _wcsicmp(module.c_str(), full.c_str()) == 0 ||
                       _wcsicmp(std::filesystem::path(module).filename().c_str(),
                               file.c_str()) == 0;
            });
        ListView_SetCheckState(list, static_cast<int>(index), active);
    }
    SetPropW(dialog, kPageReadyProperty, reinterpret_cast<HANDLE>(1));
    UpdateDspButtons(dialog);
}

void PlayerWindow::LaunchOptionsDspConfiguration(
    HWND dialog, const std::filesystem::path& module) {
    const auto helper = app::CurrentExecutablePath();
    if (helper.empty()) return;
    std::wstring command = app::WorkerCommandPrefix(helper, app::kDspWorkerSwitch) +
                           L" --configure " +
                           QuoteDspArgument(module.wstring()) + L" " +
                           std::to_wstring(reinterpret_cast<ULONG_PTR>(dialog));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const auto working = module.parent_path().wstring();
    if (CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr,
                       working.empty() ? nullptr : working.c_str(),
                       &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void PlayerWindow::ShowVisualOptions() {
    ShowOptions(kPageVisual);
}

void detail::InstallLyricAssociationImages(HWND dialog, HMODULE resources, bool all_associations) {
    // 00447A2E / 00445B30 use the same 00434754 button bitmap adapter as
    // the options sheet. Reuse its hover/disabled rendering and destruction.
    if (all_associations) InstallButtonBitmap(dialog, 0x802e, resources, 0x40f);
    else {
        InstallButtonBitmap(dialog, 2115, resources, 1);
        InstallButtonBitmap(dialog, 2120, resources, 7);
    }
}

void PlayerWindow::InstallLyricServiceEditorButtons(HWND dialog) {
    // Same resources, image-list mask, native button style and cleanup as
    // the local-search directory controls on options page 257.
    const HMODULE resources = ResourceModule();
    InstallButtonBitmap(dialog, IDC_LYRIC_SERVICES_ADD, resources, 1027);
    InstallButtonBitmap(dialog, IDC_LYRIC_SERVICES_DELETE, resources, 1039);
    InstallButtonBitmap(dialog, IDC_LYRIC_SERVICES_UP, resources, 1042);
    InstallButtonBitmap(dialog, IDC_LYRIC_SERVICES_DOWN, resources, 1045);
}

void PlayerWindow::ShowOptions(int page, UINT focus_control) {
    if (lyric_service_editor_ && IsWindow(lyric_service_editor_)) {
        ShowLyricServiceEditor();
        return; // Do not replace or activate the sheet behind its editor.
    }
    const HMODULE resources = ResourceModule();
    if (!resources) return;
    for (const UINT identifier : kOptionTemplates) {
        if (!FindResourceW(resources, MAKEINTRESOURCEW(identifier), RT_DIALOG))
            return;
    }

    // FUN_0045D531 deliberately destroys the previous sheet object even when
    // it is already visible, then rebuilds it with the requested active page.
    CloseOptions();
    options_registration_mode_ = false;
    RefreshLyricServices();
    const bool targeted_entry = page >= 0;
    if (page < 0) page = settings_.history.last_active_page;
    // FUN_0049F4AD only selects LastActivePage when it is inside the sheet's
    // actual page count.  A corrupt/high value leaves PropertySheet on page 0;
    // clamping it to System Association changes the original recovery path.
    if (page < 0 || page >= static_cast<int>(kOptionTemplates.size())) page = 0;
    if (targeted_entry) settings_.history.last_active_page = page;
    options_page_index_ = page;
    options_focus_control_ = focus_control;
    options_deferred_apply_mask_ = 0;
    options_pages_.fill(nullptr);
    options_lyric_child_ = nullptr;
    options_lyric_pages_.fill(nullptr);
    options_network_child_ = nullptr;
    options_network_pages_.fill(nullptr);
    options_hotkey_selection_ = -1;
    options_dsp_paths_.clear();
    options_dsp_scan_complete_ = false;

    std::array<PROPSHEETPAGEW, kOptionTemplates.size()> pages{};
    for (size_t index = 0; index < pages.size(); ++index) {
        pages[index].dwSize = sizeof(PROPSHEETPAGEW);
        pages[index].hInstance = resources;
        pages[index].pszTemplate = MAKEINTRESOURCEW(kOptionTemplates[index]);
        pages[index].pfnDlgProc = OptionsPageDialogProc;
        pages[index].lParam = reinterpret_cast<LPARAM>(this);
    }

    auto title = ResourceText(0x80);
    const auto suffix = ResourceText(0x813b);
    if (!suffix.empty()) {
        if (!title.empty()) title += L" - ";
        title += suffix;
    }
    PROPSHEETHEADERW header{sizeof(header)};
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_MODELESS | PSH_NOAPPLYNOW;
    header.hwndParent = window_;
    header.hInstance = resources;
    header.pszCaption = title.c_str();
    header.nPages = static_cast<UINT>(pages.size());
    header.nStartPage = static_cast<UINT>(page);
    header.ppsp = pages.data();

    const INT_PTR result = ShowWtlPropertySheet(header);
    if (result <= 0 || result == -1) {
        options_window_ = nullptr;
        return;
    }
    options_window_ = reinterpret_cast<HWND>(result);
    SetTimer(options_window_, kOptionsLyricServicesTimer, 100, nullptr);
    InitializeOptionsShell();
    SelectOptionsPage(page, focus_control);
    BringWindowToTop(options_window_);
    SetForegroundWindow(options_window_);
}

int PlayerWindow::ShowRegistrationOptions(HINSTANCE instance) {
    const HMODULE resources = ResourceModule();
    if (!resources ||
        !FindResourceW(resources, MAKEINTRESOURCEW(200), RT_DIALOG) ||
        !FindResourceW(resources, MAKEINTRESOURCEW(262), RT_DIALOG)) {
        return -1;
    }
    instance_ = instance;
    options_registration_mode_ = true;
    ReloadApplicationIcons();
    options_pages_.fill(nullptr);
    options_page_index_ = kPageAssociation;
    options_association_nodes_.clear();

    constexpr std::array<UINT, 2> templates{200, 262};
    std::array<PROPSHEETPAGEW, templates.size()> pages{};
    for (size_t index = 0; index < pages.size(); ++index) {
        pages[index].dwSize = sizeof(PROPSHEETPAGEW);
        pages[index].hInstance = resources;
        pages[index].pszTemplate = MAKEINTRESOURCEW(templates[index]);
        pages[index].pfnDlgProc = OptionsPageDialogProc;
        pages[index].lParam = reinterpret_cast<LPARAM>(this);
    }
    auto title = ResourceText(0x80);
    const auto suffix = ResourceText(0x813b);
    if (!suffix.empty()) {
        if (!title.empty()) title += L" - ";
        title += suffix;
    }
    PROPSHEETHEADERW header{sizeof(header)};
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_USECALLBACK;
    header.hInstance = resources;
    header.pszCaption = title.c_str();
    header.nPages = static_cast<UINT>(pages.size());
    // TTPlayer_RunApplicationSession's /reg branch selects page one of the
    // two-page About + System Association sheet constructed by 0049F4AD.
    header.nStartPage = 1;
    header.ppsp = pages.data();
    // 00434572 keeps this a native modal property sheet. 004A2F66 still
    // installs the same custom shell as the modeless options entry.
    header.pfnCallback = [](HWND sheet, UINT message, LPARAM) -> int {
        if (message == PSCB_INITIALIZED && registration_sheet_initializing) {
            auto* self = registration_sheet_initializing;
            self->options_window_ = sheet;
            SetWindowSubclass(sheet, OptionsSheetSubclassProc, kOptionsSheetSubclass,
                              reinterpret_cast<DWORD_PTR>(self));
            // Comctl32 still sizes/repositions buttons after this callback.
            // Install the common chrome once its native initialization has
            // unwound, inside the modal loop, or it overwrites our layout.
            PostMessageW(sheet, kInitializeRegistrationSheet, 0, 0);
        }
        return 0;
    };
    struct InitializingScope {
        PlayerWindow* previous{registration_sheet_initializing};
        ~InitializingScope() { registration_sheet_initializing = previous; }
    } initializing;
    registration_sheet_initializing = this;
    const INT_PTR result = ShowWtlPropertySheet(header);
    options_window_ = nullptr;
    CloseOptions();
    options_registration_mode_ = false;
    // 004C0900 serializes settings after the modal /reg sheet returns; do
    // not capture nonexistent player HWNDs over the stored window geometry.
    if (result != -1) settings::SaveWindowState(settings_.source_path, settings_);
    return static_cast<int>(result);
}

bool PlayerWindow::UnregisterAllAssociations() {
    settings::FileAssociationBackendOptions options;
    options.notify_shell = false;
    settings::FileAssociationBackend backend(
        CurrentExecutablePath(), ResourceText(0x80), options);
    const auto formats = BuildAssociationFormatSource(
        ResourceModule(), reader_formats_);
    const auto extensions = settings::BuildAssociableExtensions(formats);
    bool succeeded = backend.UnregisterApplication(extensions).Succeeded();
    for (const auto target : {settings::ShellIntegrationTarget::audio_cd,
                              settings::ShellIntegrationTarget::directory}) {
        const auto result = backend.SetShellIntegration(target, false);
        succeeded = succeeded && result.Succeeded();
    }
    // FUN_0049CD41 issues SHCNE_ASSOCCHANGED once after the complete sweep,
    // even when every queried association was already absent.  Keep that
    // observable /unreg idempotency boundary instead of suppressing the
    // notification on a no-op pass.
    settings::FileAssociationBackend::NotifyShellAssociationsChanged();
    return succeeded;
}

void PlayerWindow::CheckStartupAssociations() {
    if (!settings_.player.check_association) return;
    const auto formats = BuildAssociationFormatSource(
        ResourceModule(), reader_formats_);
    const auto extensions = settings::BuildAssociableExtensions(formats);
    settings::FileAssociationBackend backend(
        CurrentExecutablePath(), ResourceText(0x80));
    bool mismatch = false;
    for (const wchar_t* required : {L"mp3", L"wma"}) {
        const auto found = std::ranges::find_if(extensions,
            [required](const auto& value) {
                return _wcsicmp(value.extension.c_str(), required) == 0;
            });
        if (found == extensions.end()) continue;
        const auto query = backend.QueryExtension(required);
        if (!query.result || !query.effective) mismatch = true;
    }
    if (!mismatch) return;

    const bool user_confirmed = !settings_.player.auto_associate;
    if (!settings_.player.auto_associate) {
        AssociationPromptState prompt{&settings_.player.auto_associate};
        const INT_PTR answer = ShowWtlModalDialog(
            ResourceModule(), MAKEINTRESOURCEW(0x17a), window_,
            YesNoDialogProc, reinterpret_cast<LPARAM>(&prompt));
        if (answer == -1) return;
        if (answer != IDYES) {
            settings_.player.check_association = false;
            return;
        }
    }

    settings::FileAssociationBackendOptions options;
    options.notify_shell = false;
    settings::FileAssociationBackend batch(
        CurrentExecutablePath(), ResourceText(0x80), options);
    const settings::ShellVerbLabels labels{
        ResourceText(0x81a8), ResourceText(0x81a9)};
    std::set<std::wstring> visited;
    bool changed = false;
    bool requires_choice = false;
    for (const auto& extension : extensions) {
        auto normalized = extension.extension;
        std::ranges::transform(normalized, normalized.begin(), towlower);
        if (!visited.insert(normalized).second) continue;
        const auto result = batch.SetExtensionAssociation(
            extension.extension, true,
            AssociationTypeLabel(extension.description), {}, labels);
        changed = changed || (result && result.changed);
        requires_choice = requires_choice || (result && result.requires_user_choice);
    }
    for (const auto target : {settings::ShellIntegrationTarget::audio_cd,
                              settings::ShellIntegrationTarget::directory}) {
        const auto result = batch.SetShellIntegration(target, true, labels);
        changed = changed || (result && result.changed);
    }
    if (changed)
        settings::FileAssociationBackend::NotifyShellAssociationsChanged();
    // Do not launch Settings on every startup just because another app is
    // the current default. Only follow a fresh affirmative user response.
    if (requires_choice && user_confirmed) {
        const auto result = batch.OpenDefaultPrograms(window_);
        if (!result) {
            const auto error_text = result.operation + L"\n" + result.message;
            MessageBoxW(window_, error_text.c_str(), ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
        }
    }
}

bool PlayerWindow::CommitOptionsAssociations(HWND dialog, bool show_errors, bool open_requested_defaults) {
    if (options_association_committing_) return false;
    struct CommitScope {
        bool& busy;
        explicit CommitScope(bool& value) : busy(value) { busy = true; }
        ~CommitScope() { busy = false; }
    } commit_scope(options_association_committing_);
    options_association_commit_pending_ = false;
    settings::FileAssociationBackendOptions options;
    options.notify_shell = false;
    settings::FileAssociationBackend backend(CurrentExecutablePath(), ResourceText(0x80), options);
    const settings::ShellVerbLabels labels{ResourceText(0x81a8), ResourceText(0x81a9)};
    bool changed = false, requires_choice = false, success = true;
    std::wstring errors;
    for (auto& node : options_association_nodes_) {
        if (node->current == node->desired && !node->icon_dirty) continue;
        settings::FileAssociationResult result;
        if (node->current != node->desired) {
            result = backend.SetExtensionAssociation(
                node->extension, node->desired, node->description,
                node->desired ? node->icon : std::wstring{}, labels);
        }
        if (result && node->icon_dirty) {
            auto update = backend.RegisterApplication({{node->extension, node->description, {}}});
            if (update) {
                const auto icon = backend.SetExtensionIcon(node->extension, node->icon);
                update.changed = update.changed || icon.changed;
                if (!icon) update = icon;
            }
            result.changed = result.changed || update.changed;
            if (!update) result = update;
        }
        if (!result) {
            success = false;
            if (errors.empty()) errors = result.operation + L"\n" + result.message;
            continue;
        }
        changed = changed || result.changed;
        requires_choice = requires_choice || result.requires_user_choice;
        const auto query = backend.QueryExtension(node->extension);
        // Never show 'checked' merely because candidate registration worked.
        node->current = query.result && query.effective;
        node->desired = node->current;
        node->icon_dirty = false;
    }
    if (changed) settings::FileAssociationBackend::NotifyShellAssociationsChanged();
    RefreshOptionsAssociations(dialog);
    if (!success) {
        if (show_errors) MessageBoxW(dialog, errors.c_str(), ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
        else OutputDebugStringW((L"TTPlayer association: " + errors + L"\n").c_str());
    } else if (requires_choice && show_errors && open_requested_defaults &&
               ShowOptionsAssociationReminder(dialog)) {
        const auto launched = backend.OpenDefaultPrograms(dialog);
        if (!launched) {
            const auto error = launched.operation + L"\n" + launched.message;
            MessageBoxW(dialog, error.c_str(), ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
            return false;
        }
    }
    return success;
}

bool PlayerWindow::ShowOptionsAssociationReminder(HWND dialog) {
    if (settings_.player.suppress_association_reminder) return false;
    const auto title = ResourceText(0x80);
    const auto content = AlbumOptionText(IDS_ASSOCIATION_USER_CHOICE);
    const auto suppress = AlbumOptionText(IDS_ASSOCIATION_SUPPRESS);
    const auto open = AlbumOptionText(IDS_ASSOCIATION_OPEN_SETTINGS);
    const TASKDIALOG_BUTTON button{IDOK, open.c_str()};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = GetAncestor(dialog, GA_ROOT);
    config.hInstance = instance_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszContent = content.c_str();
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszVerificationText = suppress.c_str();
    config.cButtons = 1; config.pButtons = &button; config.nDefaultButton = IDOK;
    int selected{}; BOOL checked{};
    const HRESULT result = platform::TaskDialogIndirect(&config, &selected, nullptr, &checked);
    if (FAILED(result)) {
        // The permanent page checkbox remains available even if the common
        // controls task dialog cannot be created on the user's installation.
        return MessageBoxW(config.hwndParent, content.c_str(), title.c_str(),
                           MB_OKCANCEL | MB_ICONINFORMATION) == IDOK;
    }
    settings_.player.suppress_association_reminder = checked != FALSE;
    SetChecked(dialog, IDC_ASSOCIATION_SUPPRESS, checked != FALSE);
    return selected == IDOK;
}

void PlayerWindow::InitializeOptionsAssociationActions(HWND dialog) {
    SetChecked(dialog, IDC_ASSOCIATION_SUPPRESS, settings_.player.suppress_association_reminder);
    if (GetDlgItem(dialog, IDC_ASSOCIATION_SUPPRESS)) return;
    const HWND tree = GetDlgItem(dialog, 2038);
    if (!tree) return;
    const auto bounds = [dialog](HWND control) {
        RECT rect{}; GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
        return rect;
    };
    const auto move = [](HWND control, const RECT& rect) {
        SetWindowPos(control, nullptr, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    RECT units{0,0,14,12}; MapDialogRect(dialog, &units);
    const auto tree_bounds = bounds(tree);
    const auto label_bounds = bounds(GetDlgItem(dialog,2282));
    const int row = units.bottom + 2;
    auto shortened = tree_bounds; shortened.bottom -= row; move(tree, shortened);
    for (const int id : {2281,2282}) {
        auto rect = bounds(GetDlgItem(dialog,id)); OffsetRect(&rect,0,-row);
        move(GetDlgItem(dialog,id),rect);
    }
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(dialog,WM_GETFONT,0,0));
    const auto suppress = AlbumOptionText(IDS_ASSOCIATION_SUPPRESS);
    const HWND checkbox = CreateWindowExW(0, WC_BUTTONW, suppress.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        tree_bounds.left,label_bounds.top,tree_bounds.right-tree_bounds.left,units.bottom,
        dialog,reinterpret_cast<HMENU>(IDC_ASSOCIATION_SUPPRESS),instance_,nullptr);
    SendMessageW(checkbox,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    SetChecked(dialog,IDC_ASSOCIATION_SUPPRESS,settings_.player.suppress_association_reminder);
    const int side = std::max(20L,units.right);
    const RECT refresh_bounds{tree_bounds.right-side,tree_bounds.top-side-1,tree_bounds.right,tree_bounds.top-1};
    // Original heading statics have anonymous IDs. Shorten only the label
    // immediately above the tree, leaving room for the refresh glyph.
    for (HWND child = GetWindow(dialog,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) {
        wchar_t type[32]{}; GetClassNameW(child,type,32);
        auto rect = bounds(child);
        if (_wcsicmp(type,L"Static")==0 && rect.left==tree_bounds.left &&
            rect.top<tree_bounds.top && rect.bottom<=tree_bounds.top && rect.right>refresh_bounds.left) {
            rect.right=refresh_bounds.left-2; move(child,rect);
        }
    }
    const HWND refresh = CreateWindowExW(0,WC_BUTTONW,L"",WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        refresh_bounds.left,refresh_bounds.top,side,side,dialog,
        reinterpret_cast<HMENU>(IDC_ASSOCIATION_REFRESH),instance_,nullptr);
    SendMessageW(refresh,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    // Reuse the original lyric Reload toolbar glyph and the common options
    // button renderer instead of introducing a different icon style.
    EnsurePopupMenuImages();
    const auto found = std::ranges::find_if(popup_menu_image_commands_,[](const auto& entry) {
        return entry.first==kCmdLyricReload;
    });
    HICON icon = found!=popup_menu_image_commands_.end() && popup_menu_images_
        ? ImageList_GetIcon(popup_menu_images_,found->second,ILD_NORMAL) : nullptr;
    const auto images = ButtonIconImageList(icon,16,16);
    if (icon) DestroyIcon(icon);
    if (images) {
        BUTTON_IMAGELIST layout{}; layout.himl=images; layout.uAlign=BUTTON_IMAGELIST_ALIGN_CENTER;
        if (Button_SetImageList(refresh,&layout))
            SetWindowSubclass(refresh,OptionsImageButtonSubclassProc,kOptionsImageButtonSubclass,
                              reinterpret_cast<DWORD_PTR>(images));
        else ImageList_Destroy(images);
    } else SetWindowTextW(refresh,AlbumOptionText(IDS_ASSOCIATION_REFRESH).c_str());
    const HWND tips = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,
        dialog,nullptr,instance_,nullptr);
    TTTOOLINFOW tip{sizeof(tip)}; tip.hwnd=dialog; tip.uFlags=TTF_IDISHWND | TTF_SUBCLASS;
    tip.uId=reinterpret_cast<UINT_PTR>(refresh); tip.hinst=instance_;
    tip.lpszText=MAKEINTRESOURCEW(IDS_ASSOCIATION_REFRESH);
    SendMessageW(tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
}

bool PlayerWindow::ToggleOptionsAssociationAt(HWND dialog, POINT point) {
    if (options_association_committing_) return true;
    const HWND tree = GetDlgItem(dialog,2038);
    TVHITTESTINFO hit{}; hit.pt=point; TreeView_HitTest(tree,&hit);
    if (!hit.hItem || !(hit.flags & TVHT_ONITEMICON)) return false;
    const int requested = TreeCheckState(tree,hit.hItem)==1 ? 0 : 1;
    std::vector<HTREEITEM> pending{hit.hItem};
    while (!pending.empty()) {
        const auto item=pending.back(); pending.pop_back();
        SetTreeCheckState(tree,item,requested);
        TVITEMW value{}; value.mask=TVIF_PARAM; value.hItem=item;
        if (TreeView_GetItem(tree,&value) && value.lParam)
            reinterpret_cast<AssociationOptionNode*>(value.lParam)->desired=requested!=0;
        for (auto child=TreeView_GetChild(tree,item); child; child=TreeView_GetNextSibling(tree,child))
            pending.push_back(child);
    }
    for (auto parent=TreeView_GetParent(tree,hit.hItem); parent; parent=TreeView_GetParent(tree,parent))
        SetTreeCheckState(tree,parent,AggregateTreeChildren(tree,parent));
    // One commit/prompt per click (including category/root recursion), after
    // the native click handler has released its internal TreeView state.
    if (!options_association_commit_pending_) {
        options_association_commit_pending_=true;
        if (!PostMessageW(dialog,kApplyOptionsAssociations,0,0)) {
            options_association_commit_pending_=false;
            CommitOptionsAssociations(dialog,true);
        }
    }
    return true;
}

void PlayerWindow::RefreshOptionsAssociations(HWND dialog) {
    const HWND tree = GetDlgItem(dialog, 2038);
    if (!tree || !GetPropW(dialog, kPageReadyProperty)) return;
    settings::FileAssociationBackend backend(CurrentExecutablePath(), ResourceText(0x80));
    for (auto& node : options_association_nodes_) {
        if (node->current != node->desired || node->icon_dirty) continue; // Keep unsaved edits.
        const auto query = backend.QueryExtension(node->extension);
        if (query.result) node->current = node->desired = query.effective;
    }
    const HTREEITEM root = TreeView_GetRoot(tree);
    for (HTREEITEM category = TreeView_GetChild(tree, root); category;
         category = TreeView_GetNextSibling(tree, category)) {
        for (HTREEITEM item = TreeView_GetChild(tree, category); item;
             item = TreeView_GetNextSibling(tree, item)) {
            TVITEMW value{};
            value.hItem = item; value.mask = TVIF_PARAM;
            if (TreeView_GetItem(tree, &value) && value.lParam) {
                const auto* node = reinterpret_cast<const AssociationOptionNode*>(value.lParam);
                SetTreeCheckState(tree, item, node->desired ? 1 : 0);
            }
        }
        SetTreeCheckState(tree, category, AggregateTreeChildren(tree, category));
    }
    if (root) SetTreeCheckState(tree, root, AggregateTreeChildren(tree, root));
}

std::wstring PlayerWindow::OptionsAssociationIcon(HWND dialog) const {
    const HWND tree = GetDlgItem(dialog, 2038);
    const auto selected = tree ? TreeView_GetSelection(tree) : nullptr;
    if (!selected) return {};
    // 0049D011: a category/root only has a preview when its descendant
    // extensions have a common icon location (case-insensitive comparison).
    const auto common = [&](auto&& self, HTREEITEM item, std::wstring& icon) -> bool {
        const auto child = TreeView_GetChild(tree, item);
        if (!child) {
            TVITEMW value{}; value.mask = TVIF_PARAM; value.hItem = item;
            if (!TreeView_GetItem(tree, &value) || !value.lParam) return false;
            icon = reinterpret_cast<const AssociationOptionNode*>(value.lParam)->icon;
            return true;
        }
        for (auto next = child; next; next = TreeView_GetNextSibling(tree, next)) {
            std::wstring candidate;
            if (!self(self, next, candidate)) continue;
            if (icon.empty()) icon = candidate;
            if (_wcsicmp(icon.c_str(), candidate.c_str()) != 0) { icon.clear(); break; }
        }
        return true;
    };
    std::wstring icon;
    common(common, selected, icon);
    return icon;
}

void PlayerWindow::RefreshOptionsAssociationIcon(HWND dialog) {
    const HWND button = GetDlgItem(dialog, 2108);
    if (!button) return;
    // 0049EB86 -> 0049D1A2: replace the previous large icon on every
    // selection change. Detach before freeing the caller-owned image list.
    BUTTON_IMAGELIST empty{};
    empty.himl = BCCL_NOGLYPH;
    Button_SetImageList(button, &empty);
    auto& images = options_association_button_images_[4];
    if (images) ImageList_Destroy(images);
    images = nullptr;
    const auto selection = ParseIconSelection(OptionsAssociationIcon(dialog));
    HICON icon{};
    if (!selection.path.empty()) {
        ExtractIconExW(selection.path.c_str(), selection.index, &icon, nullptr, 1);
        if (icon) {
            images = ButtonIconImageList(icon, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
            DestroyIcon(icon); // ImageList_AddIcon copied it.
        }
    }
    if (images) AttachButtonImage(button, images);
    // Mixed/invalid paths hide the old image rather than leaving a stale
    // preview. The icon picker remains enabled for category/root selections.
    InvalidateRect(button, nullptr, FALSE);
}

void PlayerWindow::SetOptionsAssociationIcon(HWND dialog, const std::wstring& icon) {
    const HWND tree = GetDlgItem(dialog, 2038);
    const auto selected = tree ? TreeView_GetSelection(tree) : nullptr;
    if (!selected) return;
    // 0049D125 recursively applies a chosen icon to category/root leaves.
    std::vector<HTREEITEM> pending{selected};
    while (!pending.empty()) {
        const auto item = pending.back(); pending.pop_back();
        const auto child = TreeView_GetChild(tree, item);
        if (child) {
            for (auto next = child; next; next = TreeView_GetNextSibling(tree, next)) pending.push_back(next);
        } else {
            TVITEMW value{}; value.mask = TVIF_PARAM; value.hItem = item;
            if (TreeView_GetItem(tree, &value) && value.lParam) {
                auto& node = *reinterpret_cast<AssociationOptionNode*>(value.lParam);
                if (node.icon != icon) { node.icon = icon; node.icon_dirty = true; }
            }
        }
    }
    RefreshOptionsAssociationIcon(dialog);
}

void PlayerWindow::CloseOptions() {
    CancelOptionsDspScan();
    if (options_window_ && IsWindow(options_window_)) {
        FlushDeferredOptionsRuntime();
        if (options_page_index_ == kPageHotkeys)
            RegisterConfiguredHotKeys();
        DestroyWindow(options_window_);
    }
    options_window_ = nullptr;
    options_navigation_ = nullptr;
    options_header_ = nullptr;
    options_page_bounds_ = {};
    options_pages_.fill(nullptr);
    options_lyric_child_ = nullptr;
    options_lyric_pages_.fill(nullptr);
    options_network_child_ = nullptr;
    options_network_pages_.fill(nullptr);
    options_hotkey_selection_ = -1;
    options_deferred_apply_mask_ = 0;
    options_skin_entries_.clear();
    if (options_skin_preview_) DeleteObject(options_skin_preview_);
    options_skin_preview_ = nullptr;
    options_dsp_paths_.clear();
    options_dsp_scan_complete_ = false;
    options_device_entries_.clear();
    if (options_device_images_)
        ImageList_Destroy(options_device_images_);
    options_device_images_ = nullptr;
    options_association_nodes_.clear();
    options_association_commit_pending_ = false;
    if (options_association_images_)
        ImageList_Destroy(options_association_images_);
    options_association_images_ = nullptr;
    for (auto& images : options_association_button_images_) {
        if (images) ImageList_Destroy(images);
        images = nullptr;
    }
}

INT_PTR CALLBACK PlayerWindow::OptionsPageDialogProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        const auto* page = reinterpret_cast<const PROPSHEETPAGEW*>(lparam);
        self = page ? reinterpret_cast<PlayerWindow*>(page->lParam) : nullptr;
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(self));
        if (page) SetPropW(dialog, kPageTemplateProperty,
            reinterpret_cast<HANDLE>(const_cast<wchar_t*>(page->pszTemplate)));
        if (self && page) {
            const UINT identifier = LOWORD(page->pszTemplate);
            const int index = TemplateIndex(identifier);
            if (index >= 0) {
                self->options_pages_[static_cast<size_t>(index)] = dialog;
                // COptionsSheet exposes each page's long descriptive resource
                // through its child #32770 caption; the compact dialog caption
                // is used only by the owner-drawn selector at the left.
                auto description = self->ResourceText(identifier);
                // Template 255 (media library) has no same-numbered string
                // resource.  COptionsPage falls back to the dialog caption.
                if (description.empty())
                    description = DialogCaption(self->ResourceModule(),
                                                identifier);
                SetWindowTextW(dialog, description.c_str());
            }
            if (!self->options_window_) self->options_window_ = GetParent(dialog);
        }
    }
    if (!self) return FALSE;
    const UINT template_id = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(
        GetPropW(dialog, kPageTemplateProperty)));
    if (template_id == 253) {
        if (message == WM_NOTIFY) {
            const auto* header = reinterpret_cast<const NMHDR*>(lparam);
            if (header && header->code == PSN_SETACTIVE) {
                self->options_page_index_ = kPageVisual;
                if (self->options_navigation_)
                    SendMessageW(self->options_navigation_, LB_SETCURSEL,
                                 kPageVisual, 0);
                if (self->options_header_) {
                    const auto description = self->ResourceText(253);
                    SetWindowTextW(self->options_header_, description.c_str());
                    InvalidateRect(self->options_header_, nullptr, TRUE);
                }
                self->PositionOptionsPage(dialog);
            }
        }
        const INT_PTR result = self->HandleVisualOptionsDialog(
            dialog, message, wparam, lparam);
        if (message == WM_DESTROY) {
            RemovePropW(dialog, kPageTemplateProperty);
            if (self->options_pages_[kPageVisual] == dialog)
                self->options_pages_[kPageVisual] = nullptr;
        }
        return result;
    }
    return self->HandleOptionsPageDialog(dialog, message, wparam, lparam);
}

INT_PTR CALLBACK PlayerWindow::OptionsChildDialogProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        const auto* init = reinterpret_cast<const OptionsChildInit*>(lparam);
        self = init ? init->self : nullptr;
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(self));
        if (init) SetPropW(dialog, kPageTemplateProperty,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(init->template_id)));
    }
    return self ? self->HandleOptionsPageDialog(
                      dialog, message, wparam, lparam)
                : FALSE;
}

LRESULT CALLBACK PlayerWindow::OptionsSheetSubclassProc(
    HWND sheet, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR data) {
    auto* self = reinterpret_cast<PlayerWindow*>(data);
    if (!self) return DefSubclassProc(sheet, message, wparam, lparam);
    const LRESULT handled = self->HandleOptionsSheetMessage(
        sheet, message, wparam, lparam);
    if (handled != -1) return handled;
    const LRESULT result = DefSubclassProc(sheet, message, wparam, lparam);
    if (message == WM_PAINT) {
        const HDC dc = GetDC(sheet);
        DrawOptionsShellFrames(sheet, dc, self->options_navigation_, self->options_page_bounds_);
        ReleaseDC(sheet, dc);
    } else if ((message == WM_PRINT || message == WM_PRINTCLIENT) && wparam) {
        DrawOptionsShellFrames(sheet, reinterpret_cast<HDC>(wparam),
                               self->options_navigation_, self->options_page_bounds_);
    }
    if (message == PSM_SETCURSEL || message == PSM_SETCURSELID) {
        const HWND current = PropSheet_GetCurrentPageHwnd(sheet);
        if (current) self->PositionOptionsPage(current);
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(sheet, OptionsSheetSubclassProc, subclass);
    return result;
}

LRESULT PlayerWindow::HandleOptionsSheetMessage(
    HWND sheet, UINT message, WPARAM wparam, LPARAM lparam) {
    if (options_association_committing_ && (message == WM_COMMAND || message == WM_CLOSE ||
        (message == WM_SYSCOMMAND && (wparam & 0xfff0U) == SC_CLOSE))) return 0;
    if (sheet == lyric_service_disabled_owner_ && IsWindow(lyric_service_editor_) &&
        (message == WM_COMMAND || message == WM_CLOSE ||
         (message == WM_SYSCOMMAND && (wparam & 0xfff0U) == SC_CLOSE))) {
        ShowLyricServiceEditor();
        return 0; // Includes close/apply commands queued before disabling.
    }
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE && options_pages_[kPageAssociation])
            RefreshOptionsAssociations(options_pages_[kPageAssociation]);
        break;
    case WM_NCCALCSIZE:
        if (lparam && !IsRectEmpty(&options_page_bounds_)) {
            auto* rect = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                                : reinterpret_cast<RECT*>(lparam);
            const LONG proposed_top = rect->top;
            const LRESULT result = DefSubclassProc(sheet, message, wparam, lparam);
            // The 5.7.9 PE targets subsystem 4.0: Windows excludes the newer
            // padded border from its fixed-dialog caption. Our modern PE adds
            // it to the title bar. Adapt only this sheet, not the executable's
            // subsystem/compatibility mode (which also affects other windows).
            rect->top = std::min(rect->top, proposed_top +
                GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYDLGFRAME));
            return result;
        }
        break;
    case kInitializeRegistrationSheet:
        if (options_registration_mode_ && !options_navigation_) {
            InitializeOptionsShell();
            SelectOptionsPage(kPageAssociation);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        const HWND control = reinterpret_cast<HWND>(lparam);
        const UINT identifier = control ? GetDlgCtrlID(control) : 0;
        if (identifier >= kLinkFirst &&
            identifier < kLinkFirst + kProjectLinks.size()) {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0, 0, 255));
            return reinterpret_cast<LRESULT>(
                GetSysColorBrush(COLOR_3DFACE));
        }
        break;
    }
    case WM_SETCURSOR: {
        const HWND control = reinterpret_cast<HWND>(wparam);
        const UINT identifier = control ? GetDlgCtrlID(control) : 0;
        if (LOWORD(lparam) == HTCLIENT && IsWindowEnabled(control) &&
            GetParent(control) == sheet && identifier >= kLinkFirst &&
            identifier < kLinkFirst + kProjectLinks.size()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        const UINT control = LOWORD(wparam);
        if (control == kOptionsNavigation && HIWORD(wparam) == LBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(options_navigation_,
                                                   LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR) SelectOptionsPage(static_cast<int>(
                SendMessageW(options_navigation_, LB_GETITEMDATA, selected, 0)));
            return 0;
        }
        if (control >= kLinkFirst && control < kLinkFirst + kProjectLinks.size() &&
            HIWORD(wparam) == STN_CLICKED) {
            OpenProjectLink(sheet, kProjectLinks[control - kLinkFirst].command);
            return 0;
        }
        // 0049FE39's command overrides are guarded by PSH_MODELESS. /reg
        // must let PropertySheet end its own modal loop (IDOK/Cancel/Esc),
        // and must not post player-window messages or DestroyWindow it.
        if (options_registration_mode_) break;
        if (control == kSaveAllOptions) {
            // FUN_0049FE39/0x4D2 only invokes the settings serializer.  Every
            // page has already updated the shared settings object from its
            // individual notification handlers.
            CaptureWindowState();
            settings::SaveWindowState(settings_.source_path, settings_);
            return 0;
        }
        if (control == kResetAllOptions) {
            const auto prompt = ResourceText(0x8143);
            auto caption = ResourceText(0x80);
            if (MessageBoxW(sheet, prompt.c_str(), caption.c_str(),
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                const int reopen = options_page_index_;
                const auto source = settings_.source_path;
                // FUN_0049FE39 first leaves the property-sheet callback and
                // destroys every page.  The all-subsystem reset and E140
                // reopen are then queued to the player in that order.
                options_deferred_apply_mask_ = 0;
                DestroyWindow(sheet);
                settings_ = settings::Settings{};
                settings_.source_path = source;
                settings_.history.last_active_page = reopen;
                PostMessageW(window_, kMsgApplyOptions, 0xffff,
                             static_cast<LPARAM>(-2));
                PostMessageW(window_, WM_COMMAND, kCmdOptions,
                             0);
            }
            return 0;
        }
        if (control == IDOK || control == IDCANCEL) {
            // FUN_0049FE39 treats both buttons as IDOK: it records the page,
            // posts the close notification, passes the rewritten command to
            // the original property-sheet procedure (thereby delivering
            // PSN_KILLACTIVE/PSN_APPLY), and only then destroys the modeless
            // sheet.  Bypassing that procedure silently skipped every page's
            // final validation/commit notification.
            PostMessageW(window_, kMsgApplyOptions, 0x100,
                         static_cast<LPARAM>(-1));
            settings_.history.last_active_page = options_page_index_;
            const WPARAM ok = MAKEWPARAM(IDOK, HIWORD(wparam));
            const LRESULT result = DefSubclassProc(sheet, message, ok, lparam);
            if (IsWindow(sheet)) DestroyWindow(sheet);
            return result;
        }
        break;
    }
    case WM_SYSCOMMAND:
        if (!options_registration_mode_ && (wparam & 0xfff0U) == SC_CLOSE) {
            // COptionsSheet::FUN_0049FF9B does not let the stock modeless
            // property sheet process its disabled/hidden Cancel path.  A
            // title-bar close is translated asynchronously to IDCANCEL, and
            // FUN_0049FE39 then rewrites both IDCANCEL and IDOK to the same
            // IDOK/apply/destroy transaction.  Posting (rather than sending)
            // is significant: the non-client command must unwind before the
            // property-sheet pages receive PSN_KILLACTIVE/PSN_APPLY.
            PostMessageW(sheet, WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (!item) break;
        if (item->CtlID == kOptionsHeader) {
            DrawOptionsHeader(*item);
            return TRUE;
        }
        if (item->CtlID == kOptionsNavigation) {
            DrawOptionsNavigation(*item);
            return TRUE;
        }
        break;
    }
    case WM_TIMER:
        if (wparam == kOptionsLyricServicesTimer) {
            PollLyricServices();
            if (!lyric_catalog_job_ && !lyric_catalog_save_job_)
                KillTimer(sheet, kOptionsLyricServicesTimer);
            return 0;
        }
        if (wparam == kOptionsSkinPollTimer) {
            if (PublishReadySkinMenuCatalog(0)) {
                KillTimer(sheet, kOptionsSkinPollTimer);
                if (options_pages_[kPageSkin])
                    PopulateOptionsSkinPage(options_pages_[kPageSkin]);
            }
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (lyric_service_disabled_owner_ == sheet) lyric_service_disabled_owner_ = nullptr;
        KillTimer(sheet, kOptionsLyricServicesTimer);
        KillTimer(sheet, kOptionsSkinPollTimer);
        if (options_window_ == sheet) {
            options_window_ = nullptr;
            options_navigation_ = nullptr;
            options_header_ = nullptr;
            options_page_bounds_ = {};
            options_pages_.fill(nullptr);
            options_lyric_child_ = nullptr;
            options_lyric_pages_.fill(nullptr);
            options_network_child_ = nullptr;
            options_network_pages_.fill(nullptr);
            options_deferred_apply_mask_ = 0;
            options_skin_entries_.clear();
            if (options_skin_preview_) DeleteObject(options_skin_preview_);
            options_skin_preview_ = nullptr;
            CancelOptionsDspScan();
            options_dsp_paths_.clear();
            options_dsp_scan_complete_ = false;
            options_device_entries_.clear();
            if (options_device_images_)
                ImageList_Destroy(options_device_images_);
            options_device_images_ = nullptr;
            options_association_nodes_.clear();
            if (options_association_images_)
                ImageList_Destroy(options_association_images_);
            options_association_images_ = nullptr;
            for (auto& images : options_association_button_images_) {
                if (images) ImageList_Destroy(images);
                images = nullptr;
            }
        }
        break;
    default:
        break;
    }
    return -1;
}

void PlayerWindow::InitializeOptionsShell() {
    if (!options_window_ || options_navigation_) return;
    // COptionsSheet::004A2F66 adds 0x02000000 through 004053CD.
    // The sheet background must not paint over the navigation/page HWNDs.
    SetWindowLongPtrW(options_window_, GWL_STYLE,
        GetWindowLongPtrW(options_window_, GWL_STYLE) | WS_CLIPCHILDREN);
    SetWindowSubclass(options_window_, OptionsSheetSubclassProc,
                      kOptionsSheetSubclass,
                      reinterpret_cast<DWORD_PTR>(this));

    const HWND tab = PropSheet_GetTabControl(options_window_);
    if (!tab) return;
    // 004A2F66 extends the actual native property sheet, not a hard-coded
    // 558x458 rectangle. In particular, the stock tab can have several rows.
    RECT bounds{}, tab_bounds{};
    GetWindowRect(options_window_, &bounds);
    POINT client_origin{};
    ClientToScreen(options_window_, &client_origin);
    const LONG caption_padding = std::max(0L, client_origin.y - bounds.top -
        GetSystemMetrics(SM_CYCAPTION) - GetSystemMetrics(SM_CYDLGFRAME));
    GetWindowRect(tab, &tab_bounds);
    MapWindowPoints(nullptr, options_window_, reinterpret_cast<POINT*>(&tab_bounds), 2);
    RECT page = tab_bounds;
    TabCtrl_AdjustRect(tab, FALSE, &page);
    const int dy = (page.bottom - page.top) - (tab_bounds.bottom - tab_bounds.top) + 50;
    constexpr int dx = 110;
    OffsetRect(&page, dx, dy);
    options_page_bounds_ = page;
    EnableWindow(tab, FALSE);
    ShowWindow(tab, SW_HIDE);
    for (HWND child = GetWindow(options_window_, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, options_window_, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(child, nullptr, rect.left + dx, rect.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    SetWindowPos(options_window_, nullptr, 0, 0,
                 bounds.right - bounds.left + dx, bounds.bottom - bounds.top + dy - caption_padding,
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    // 0049F9BE removes the help-button extended style only after the native
    // dialog has been laid out. PSH_NOCONTEXTHELP selects a different system
    // template/non-client frame and changes the usable size on Windows 11.
    SetWindowLongPtrW(options_window_, GWL_EXSTYLE,
        GetWindowLongPtrW(options_window_, GWL_EXSTYLE) & ~WS_EX_CONTEXTHELP);
    // FUN_004A1427 sends PSM_CANCELTOCLOSE before hiding Cancel.  Besides
    // matching the stock sheet state this prevents hidden-tab keyboard paths
    // from reviving Apply/Cancel semantics behind the custom shell.
    SendMessageW(options_window_, PSM_CANCELTOCLOSE, 0, 0);

    const HWND close = GetDlgItem(options_window_, IDOK);
    const HWND cancel = GetDlgItem(options_window_, IDCANCEL);
    RECT close_bounds{};
    GetWindowRect(cancel ? cancel : close, &close_bounds);
    MapWindowPoints(nullptr, options_window_, reinterpret_cast<POINT*>(&close_bounds), 2);
    const int button_width = close_bounds.right - close_bounds.left;
    const int button_height = close_bounds.bottom - close_bounds.top;
    const HFONT font = close ? reinterpret_cast<HFONT>(
        SendMessageW(close, WM_GETFONT, 0, 0)) : nullptr;
    for (const int identifier : {IDCANCEL, kPropertySheetApply, IDHELP}) {
        if (const HWND control = GetDlgItem(options_window_, identifier))
            ShowWindow(control, SW_HIDE);
    }
    if (close) {
        const auto text = ResourceText(8);
        if (!text.empty()) SetWindowTextW(close, text.c_str());
        SetWindowPos(close, nullptr, close_bounds.left, close_bounds.top,
                     button_width, button_height, SWP_NOACTIVATE | SWP_NOZORDER);
        InstallButtonBitmap(options_window_, IDOK, ResourceModule(), 1);
    }

    const auto make_button = [&](UINT id, UINT text_id, int x) {
        const auto text = ResourceText(text_id);
        const HWND button = CreateWindowExW(
            0, WC_BUTTONW, text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, close_bounds.top, button_width, button_height, options_window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (button && font) SendMessageW(button, WM_SETFONT,
                                         reinterpret_cast<WPARAM>(font), TRUE);
    };
    make_button(kSaveAllOptions, 0x8141, close_bounds.left - 2 * (button_width + 6));
    make_button(kResetAllOptions, 0x8140, close_bounds.left - button_width - 6);

    options_navigation_ = CreateWindowExW(
        0, WC_LISTBOXW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        14, 14, page.left - 12 - 14, page.bottom - 14, options_window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOptionsNavigation)),
        instance_, nullptr);
    if (options_navigation_) {
        auto state = std::make_unique<OptionsNavigationState>();
        if (SetWindowSubclass(options_navigation_, OptionsNavigationSubclassProc,
                kOptionsNavigationSubclass, reinterpret_cast<DWORD_PTR>(state.get()))) state.release();
        if (font) SendMessageW(options_navigation_, WM_SETFONT,
                               reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(options_navigation_, LB_SETITEMHEIGHT, 0, 24);
        for (const UINT identifier : kOptionTemplates) {
            if (options_registration_mode_ && identifier != 200 && identifier != 262) continue;
            const auto caption = DialogCaption(ResourceModule(), identifier);
            const auto row = SendMessageW(options_navigation_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(caption.c_str()));
            if (row != LB_ERR && row != LB_ERRSPACE)
                SendMessageW(options_navigation_, LB_SETITEMDATA, row, TemplateIndex(identifier));
        }
    }
    options_header_ = CreateWindowExW(
        0, WC_STATICW, nullptr, WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        page.left, 14, page.right - page.left, page.top - 14, options_window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOptionsHeader)),
        instance_, nullptr);
    if (options_header_ && font) SendMessageW(options_header_, WM_SETFONT,
        reinterpret_cast<WPARAM>(font), TRUE);

    const auto related = ResourceText(0x8142);
    const HDC dc = GetDC(options_window_);
    const auto previous_font = font ? SelectObject(dc, font) : nullptr;
    const auto text_width = [dc](std::wstring_view text) {
        SIZE size{};
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
        return size.cx;
    };
    const int related_width = text_width(related);
    const HWND related_label = CreateWindowExW(
        0, WC_STATICW, related.c_str(), WS_CHILD | WS_VISIBLE,
        8, close_bounds.top + 6, related_width, button_height - 6, options_window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOptionsRelated)),
        instance_, nullptr);
    if (related_label && font) SendMessageW(related_label, WM_SETFONT,
        reinterpret_cast<WPARAM>(font), TRUE);
    int link_x = 8 + related_width + 4;
    for (size_t index = 0; index < kProjectLinks.size(); ++index) {
        const std::wstring_view label{ProjectLinkLabel(kProjectLinks[index])};
        const int width = text_width(label);
        const HWND link = CreateWindowExW(
            0, WC_STATICW, label.data(),
            WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            link_x, close_bounds.top + 6, width, button_height - 6, options_window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkFirst + index)),
            instance_, nullptr);
        if (link && font) SendMessageW(link, WM_SETFONT,
                                       reinterpret_cast<WPARAM>(font), TRUE);
        link_x += width + 8;
    }
    if (previous_font) SelectObject(dc, previous_font);
    ReleaseDC(options_window_, dc);

    if (!options_registration_mode_) {
        StartSkinMenuCatalogLoad();
        SetTimer(options_window_, kOptionsSkinPollTimer,
                 kOptionsSkinPollMilliseconds, nullptr);
    }

    GetWindowRect(options_window_, &bounds);
    HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;
    SetWindowPos(options_window_, HWND_TOP, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    RedrawWindow(options_window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void PlayerWindow::PositionOptionsPage(HWND page) {
    if (!options_window_ || !page || IsRectEmpty(&options_page_bounds_)) return;
    const RECT target = options_page_bounds_;
    RECT current{};
    GetWindowRect(page, &current);
    MapWindowPoints(nullptr, options_window_, reinterpret_cast<POINT*>(&current), 2);
    if (EqualRect(&current, &target) &&
        (GetWindowLongPtrW(page, GWL_STYLE) & WS_VISIBLE)) return;
    // Comctl32 can first place a page at the hidden tab's stock origin.
    // Repaint its new client area rather than copying pixels from that
    // overlapping location (which can contain the navigation selection).
    SetWindowPos(page, nullptr, target.left, target.top,
                 target.right - target.left, target.bottom - target.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

void PlayerWindow::SelectOptionsPage(int page, UINT focus_control) {
    if (!options_window_) return;
    page = std::clamp(page, 0, static_cast<int>(kOptionTemplates.size()) - 1);
    if (options_registration_mode_ && page != kPageAbout && page != kPageAssociation) return;
    options_focus_control_ = focus_control;
    const HWND active = PropSheet_GetCurrentPageHwnd(options_window_);
    // 004A3651 compares LB_GETCURSEL with the active property-sheet index
    // before 004345B4/PSM_SETCURSEL. A second click on the same item must not
    // send PSN_KILLACTIVE/PSN_SETACTIVE or move the live page through the
    // stock tab origin. Still honour 0049FD89's targeted nested-page focus.
    const bool different_page = !active ||
        active != options_pages_[static_cast<size_t>(page)];
    if (different_page && !PropSheet_SetCurSel(options_window_, nullptr,
            OptionsSheetIndex(page, options_registration_mode_))) {
        const HWND current = PropSheet_GetCurrentPageHwnd(options_window_);
        const auto found = std::find(options_pages_.begin(),
                                     options_pages_.end(), current);
        if (found != options_pages_.end())
            page = static_cast<int>(found - options_pages_.begin());
        focus_control = 0;
        options_focus_control_ = 0;
    }
    options_page_index_ = page;
    const int row = OptionsSheetIndex(page, options_registration_mode_);
    if (options_navigation_ &&
        SendMessageW(options_navigation_, LB_GETCURSEL, 0, 0) != row)
        SendMessageW(options_navigation_, LB_SETCURSEL, row, 0);
    const HWND current = PropSheet_GetCurrentPageHwnd(options_window_);
    if (current) PositionOptionsPage(current);
    if (options_header_) {
        const UINT template_id =
            kOptionTemplates[static_cast<size_t>(page)];
        auto description = ResourceText(template_id);
        if (description.empty())
            description = DialogCaption(ResourceModule(), template_id);
        std::wstring previous(static_cast<size_t>(
            GetWindowTextLengthW(options_header_)) + 1U, L'\0');
        previous.resize(static_cast<size_t>(GetWindowTextW(options_header_,
            previous.data(), static_cast<int>(previous.size()))));
        if (previous != description) {
            SetWindowTextW(options_header_, description.c_str());
            InvalidateRect(options_header_, nullptr, TRUE);
        }
    }

    HWND target = nullptr;
    if (focus_control == 384 || focus_control == 385) {
        if (current) {
            const HWND tab = GetDlgItem(current, 2256);
            if (tab) {
                TabCtrl_SetCurSel(tab, focus_control == 385 ? 1 : 0);
                const UINT current_child = static_cast<UINT>(
                    reinterpret_cast<ULONG_PTR>(GetPropW(
                        options_lyric_child_, kPageTemplateProperty)));
                if (current_child != focus_control)
                    SendMessageW(current, kSwitchNestedOptionsPage,
                                 focus_control, 0);
            }
        }
        target = options_lyric_child_;
    } else if (focus_control == 381 || focus_control == 382) {
        if (current) {
            const HWND tab = GetDlgItem(current, 1181);
            if (tab) {
                TabCtrl_SetCurSel(tab, focus_control == 382 ? 1 : 0);
                const UINT current_child = static_cast<UINT>(
                    reinterpret_cast<ULONG_PTR>(GetPropW(
                        options_network_child_, kPageTemplateProperty)));
                if (current_child != focus_control)
                    SendMessageW(current, kSwitchNestedOptionsPage,
                                 focus_control, 0);
            }
        }
        target = options_network_child_;
    } else if (current && focus_control) {
        target = GetDlgItem(current, static_cast<int>(focus_control));
        if (!target && options_lyric_child_)
            target = GetDlgItem(options_lyric_child_,
                                static_cast<int>(focus_control));
        if (!target && options_network_child_)
            target = GetDlgItem(options_network_child_,
                                static_cast<int>(focus_control));
    }
    if (target && IsWindow(target)) {
        if (current == options_pages_[kPageGeneral]) GeneralOptionsTabs::ShowControl(current, target);
        SetFocus(target);
    }
    options_focus_control_ = 0;
}

void PlayerWindow::SelectOptionsLyricMode(HWND dialog, UINT template_id) {
    const bool desktop = template_id == 385;
    const HWND next = options_lyric_pages_[desktop ? 1 : 0];
    if (!next || !IsWindow(next)) return;
    if (options_lyric_child_ && options_lyric_child_ != next) {
        const UINT previous = options_lyric_child_ == options_lyric_pages_[1] ? 385 : 384;
        CommitOptionsPage(options_lyric_child_, previous);
        FlushDeferredOptionsRuntime(previous);
    }
    // 004974F0 creates both pages once; 00497779 only changes visibility and
    // the two outer window-mode options. Keep each page's controls alive.
    const HWND previous = options_lyric_pages_[desktop ? 0 : 1];
    if (previous) ShowWindow(previous, SW_HIDE);
    options_lyric_child_ = next;
    ShowWindow(next, SW_SHOW);
    TabCtrl_SetCurSel(GetDlgItem(dialog, 2256), desktop ? 1 : 0);
    EnableWindow(GetDlgItem(dialog, 2021), !desktop);
    EnableWindow(GetDlgItem(dialog, 2024), !desktop);
}

void PlayerWindow::SelectOptionsNetworkMode(HWND dialog, UINT template_id) {
    const bool download = template_id == 382;
    const HWND next = options_network_pages_[download ? 1 : 0];
    if (!next || !IsWindow(next)) return;
    if (options_network_child_ && options_network_child_ != next) {
        const UINT previous = options_network_child_ == options_network_pages_[1] ? 382 : 381;
        CommitOptionsPage(options_network_child_, previous);
        FlushDeferredOptionsRuntime(previous);
    }
    // 0049885A creates both resource-sized pages; 00498BD1 only shows/hides them.
    const HWND previous = options_network_pages_[download ? 0 : 1];
    if (previous) ShowWindow(previous, SW_HIDE);
    options_network_child_ = next;
    ShowWindow(next, SW_SHOW);
    TabCtrl_SetCurSel(GetDlgItem(dialog, 1181), download ? 1 : 0);
}

void PlayerWindow::InitializeOptionsPage(HWND dialog, UINT template_id) {
    const HMODULE resources = ResourceModule();
    const auto ready = [&] {
        SetPropW(dialog, kPageReadyProperty, reinterpret_cast<HANDLE>(1));
    };
    switch (template_id) {
    case 200: {
        const auto raw_about = ResourceText(129);
        std::wstring about;
        about.reserve(raw_about.size() + 32);
        for (size_t index = 0; index < raw_about.size(); ++index) {
            if (raw_about[index] == L'\n' &&
                (index == 0 || raw_about[index - 1] != L'\r')) {
                about.push_back(L'\r');
            }
            about.push_back(raw_about[index]);
        }
        SetDlgItemTextW(dialog, 1073, about.c_str());
        auto version = ResourceText(0x8299);
        version += L" (Unicode)";
        SetDlgItemTextW(dialog, 1011, version.c_str());
        SetDlgItemTextW(dialog, 1005, i18n::Literal(L"nanling与社区"));
        SetDlgItemTextW(dialog, 1040, build::kCompletionDate);
        // RT_DIALOG 200 uses the shared IDC_STATIC (-1) for its captions.
        // Replace only the date caption, not the other anonymous labels.
        EnumChildWindows(dialog, [](HWND control, LPARAM) -> BOOL {
            wchar_t caption[32]{}, klass[16]{};
            GetClassNameW(control, klass, static_cast<int>(std::size(klass)));
            GetWindowTextW(control, caption, static_cast<int>(std::size(caption)));
            if (_wcsicmp(klass, L"Static") == 0 &&
                (std::wstring_view(caption) == L"完成日期:" ||
                 std::wstring_view(caption) == i18n::Text(L"完成日期:",
                    "ttpres/dialog/200/control/4294967295/3")))
                SetWindowTextW(control, i18n::Literal(L"构建日期:"));
            return TRUE;
        }, 0);
        SetDlgItemTextW(dialog, 1009, ResourceText(0x80).c_str());
        SetDlgItemTextW(dialog, 1020, i18n::Literal(L"社区版"));
        // The resource placeholders are intentionally hidden.  AboutPage's
        // window paint hook uses their rectangles just like FUN_0049227D,
        // so the logo/icon are part of the page rather than child controls.
        ShowWindow(GetDlgItem(dialog, 1009), SW_HIDE);
        ShowWindow(GetDlgItem(dialog, 1020), SW_HIDE);
        SetWindowSubclass(dialog, OptionsAboutSubclassProc,
                          kOptionsAboutSubclass, 0);
        break;
    }
    case 250: {
        const auto& value = settings_.general;
        // Keep the resource controls alive across the three General tabs.
        // All coordinates use the loaded resource's font and dialog units.
        if (!GetDlgItem(dialog, kOptionsDiscordLyrics)) {
            const bool language_available = !FindRuntimePath(L"AddIn/ttp_i18n.dll").empty();
            const auto place = [dialog](HWND control, RECT rect, HWND after) {
                MapDialogRect(dialog, &rect);
                SetWindowPos(control, after, rect.left, rect.top,
                             rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
            };
            const auto add = [&](LPCWSTR klass, LPCWSTR text, int id, DWORD style,
                                 RECT rect, HWND after) {
                const HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
                    0, 0, 0, 0, dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    instance_, nullptr);
                SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
                place(control, rect, after);
                return control;
            };
            const HWND discord = GetDlgItem(dialog, 2188);
            SetWindowTextW(discord, i18n::Literal(L"向 Discord 发送播放的歌曲信息"));
            // The date/time picker is last in the original template. Move it
            // beside its checkbox in tab order before inserting the new rows.
            const HWND shutdown_time = GetDlgItem(dialog, 2183);
            SetWindowPos(shutdown_time, GetDlgItem(dialog, 2182), 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            place(discord, {13, 124, 267, 134}, shutdown_time);
            const HWND lyrics = add(WC_BUTTONW, i18n::Literal(L"向 Discord 发送歌词"),
                kOptionsDiscordLyrics, WS_TABSTOP | BS_AUTOCHECKBOX, {13, 139, 267, 149}, discord);
            if (language_available) {
                const HWND label = add(WC_STATICW, i18n::Literal(L"界面语言"), kOptionsLanguageLabel,
                    0, {13, 160, 83, 172}, lyrics);
                const HWND languages = add(WC_COMBOBOXW, L"", kOptionsLanguage,
                    WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                    {85, 156, 197, 276}, label);
                std::vector<std::wstring> choices{L"auto", L"source"};
                const auto available = i18n::Languages(PlayerRuntimeDirectory());
                for (const auto& locale : available)
                    if (locale != L"auto" && locale != L"source") choices.push_back(locale);
                const auto selected = value.language.empty() ? L"auto" : value.language;
                if (std::find(choices.begin(), choices.end(), selected) == choices.end())
                    choices.push_back(selected); // Preserve unavailable or aliased configured locales.
                for (size_t index = 0; index < choices.size(); ++index) {
                    const auto& locale = choices[index];
                    const auto text = index == 0 ? i18n::Text(L"自动（系统语言）") :
                        index == 1 ? i18n::Text(L"原始文本") :
                        locale == L"chs" ? i18n::Text(L"简体中文") :
                        locale == L"cht" ? i18n::Text(L"繁体中文") :
                        locale == L"en_US" ? i18n::Text(L"英文") : locale;
                    const LRESULT item = SendMessageW(languages, CB_ADDSTRING, 0,
                                                      reinterpret_cast<LPARAM>(text.c_str()));
                    if (item == CB_ERR || item == CB_ERRSPACE) continue;
                    // Store stable locale identifiers separately from translated
                    // labels. These string literals live for the entire session.
                    const wchar_t* code = locale == L"chs" ? L"chs" :
                        locale == L"cht" ? L"cht" : locale == L"en_US" ? L"en_US" : nullptr;
                    SendMessageW(languages, CB_SETITEMDATA, item, reinterpret_cast<LPARAM>(code));
                    if (locale == selected) SendMessageW(languages, CB_SETCURSEL, item, 0);
                }
                add(WC_STATICW, i18n::Literal(L"界面语言将在下次启动播放器时生效。"),
                    kOptionsLanguageNotice, 0, {13, 175, 267, 193}, languages);
            }
        }
        SetChecked(dialog, 2088, value.startup_minimize);
        SetChecked(dialog, 2085, value.tray_icon);
        SetChecked(dialog, 2127, value.show_hotkey_in_tips);
        SetChecked(dialog, 2086, value.tips_on_open);
        SetChecked(dialog, 1079, value.menu_tips);
        SetChecked(dialog, 2153, value.menu_bar_playlist);
        SetChecked(dialog, 2189, value.scroll_title);
        SetChecked(dialog, 2188, value.send_title_to_msn);
        SetChecked(dialog, kOptionsDiscordLyrics, value.discord_sync_lyrics);
        EnableWindow(GetDlgItem(dialog, kOptionsDiscordLyrics),
                     value.send_title_to_msn);
        SetChecked(dialog, 1075, value.fade_windows);
        SetChecked(dialog, 1076, (value.snap_windows & 0x10000) != 0);
        SetInteger(dialog, 1077, value.snap_windows & 0xffff);
        SetChecked(dialog, 1083,
                   (value.title_slide_interval & 0x10000) != 0);
        SetInteger(dialog, 1084, value.title_slide_interval & 0xffff);
        SetChecked(dialog, 2182, value.auto_shutdown);
        SYSTEMTIME shutdown{};
        GetLocalTime(&shutdown);
        shutdown.wHour = static_cast<WORD>(std::clamp(value.shutdown_time[0], 0, 23));
        shutdown.wMinute = static_cast<WORD>(std::clamp(value.shutdown_time[1], 0, 59));
        shutdown.wSecond = static_cast<WORD>(std::clamp(value.shutdown_time[2], 0, 59));
        SendDlgItemMessageW(dialog, 2183, DTM_SETFORMATW, 0,
                            reinterpret_cast<LPARAM>(L"HH:mm:ss"));
        SendDlgItemMessageW(dialog, 2183, DTM_SETSYSTEMTIME,
                            GDT_VALID, reinterpret_cast<LPARAM>(&shutdown));
        SetChecked(dialog, 2139, value.clear_list_on_command);
        SetChecked(dialog, 2137, value.default_list_on_command);
        SetDlgItemTextW(dialog, 2138, value.default_list.c_str());
        PopulateResourceCombo(dialog, 1047, resources, 4,
            value.check_update_days < 0 ? 0 :
            value.check_update_days <= 1 ? 1 :
            value.check_update_days <= 7 ? 2 : 3);
        SetSpinRange(dialog, 1078, 1, 100);
        SetSpinRange(dialog, 1044, 1, 100);
        EnableWindow(GetDlgItem(dialog, 1077), IsChecked(dialog, 1076));
        EnableWindow(GetDlgItem(dialog, 1078), IsChecked(dialog, 1076));
        EnableWindow(GetDlgItem(dialog, 1084), IsChecked(dialog, 1083));
        EnableWindow(GetDlgItem(dialog, 1044), IsChecked(dialog, 1083));
        EnableWindow(GetDlgItem(dialog, 2138), IsChecked(dialog, 2137));
        EnableWindow(GetDlgItem(dialog, 2183), IsChecked(dialog, 2182));
        GeneralOptionsTabs::Attach(dialog);
        break;
    }
    case 251: {
        const auto& value = settings_.playback;
        SetChecked(dialog, 1070, value.auto_play);
        SetChecked(dialog, 2092, value.continue_play);
        SetChecked(dialog, 2093, value.stop_when_fail);
        SetInteger(dialog, 1087, value.track_interval);
        PopulateResourceCombo(dialog, 1100, resources, 3,
            value.thread_priority >= 15 ? 0 : value.thread_priority >= 2 ? 1 : 2);
        SetInteger(dialog, 2195, value.file_buffer / 1024);
        for (int index = 0; index < 4; ++index) {
            SetChecked(dialog, 2070 + index,
                       (value.sound_fade_mode & (1 << index)) != 0);
            SetInteger(dialog, 2075 + index, value.fade_duration[index]);
            SetSpinRange(dialog, 2080 + index, 100, 10000);
        }
        SetChecked(dialog, 2074, (value.sound_fade_mode & 0x10) != 0);
        SetInteger(dialog, 2079, value.track_fade_duration);
        SetSpinRange(dialog, 2084, 100, 10000);
        SetChecked(dialog, 2129, value.auto_gain);
        SetChecked(dialog, 2130, value.auto_scan_gain);
        SetChecked(dialog, 2131, value.skip_scan_gain);
        SetSpinRange(dialog, 1096, 0, 10);
        SetSpinRange(dialog, 1097, 1, 1024 * 16);
        EnableWindow(GetDlgItem(dialog, 2092), IsChecked(dialog, 1070));
        for (int index = 0; index < 5; ++index) {
            const bool enabled = IsChecked(dialog, 2070 + index);
            EnableWindow(GetDlgItem(dialog, 2075 + index), enabled);
            EnableWindow(GetDlgItem(dialog, 2080 + index), enabled);
        }
        break;
    }
    case 252: {
        const HWND list = GetDlgItem(dialog, 1064);
        if (list) {
            ListView_SetExtendedListViewStyle(list,
                LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT |
                LVS_EX_LABELTIP | LVS_EX_FLATSB);
            for (int column_index = 0; column_index < 3; ++column_index) {
                auto title = ResourceListItem(resources, 33114, column_index);
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.cx = column_index == 0 ? 90 : 145;
                column.pszText = title.data();
                ListView_InsertColumn(list, column_index, &column);
            }
            for (size_t row = 0; row < settings_.hotkey.key_map.size(); ++row) {
                const auto& binding = settings_.hotkey.key_map[row];
                auto label = binding.command >= 0
                    ? ResourceCommandLabel(resources,
                        static_cast<UINT>(binding.command))
                    : binding.raw_text;
                if (const auto end = label.find_first_of(L"\n|");
                    end != std::wstring::npos) {
                    label.erase(0, end + 1);
                }
                if (binding.command == 32000) {
                    auto alternate = ResourceCommandLabel(resources, 32001);
                    if (const auto end = alternate.find_first_of(L"\n|");
                        end != std::wstring::npos) {
                        alternate.erase(0, end + 1);
                    }
                    if (!alternate.empty()) {
                        label += L"/";
                        label += alternate;
                    }
                }
                if (label.empty()) label = std::to_wstring(binding.command);
                AddListText(list, static_cast<int>(row), label);
                UpdateHotKeyListRow(list, static_cast<int>(row), binding);
            }
            if (!settings_.hotkey.key_map.empty()) {
                ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                SetHotKeyControl(dialog, 2054,
                                 settings_.hotkey.key_map[0].application);
                SetHotKeyControl(dialog, 2055,
                                 settings_.hotkey.key_map[0].global);
                options_hotkey_selection_ = 0;
            }
            ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
        }
        SetChecked(dialog, 2053, settings_.hotkey.global);
        const BOOL has_selection = options_hotkey_selection_ >= 0;
        EnableWindow(GetDlgItem(dialog, 2054), has_selection);
        EnableWindow(GetDlgItem(dialog, 2055), has_selection);
        EnableWindow(GetDlgItem(dialog, 1098), has_selection);
        InstallButtonBitmap(dialog, 1098, resources, 1098);
        break;
    }
    case 254: {
        const auto& value = settings_.playlist;
        SetChecked(dialog, 2010, value.enable_drag_drop);
        SetChecked(dialog, 2128, value.disable_delete_file);
        SetChecked(dialog, 2125, value.save_relative_path);
        SetChecked(dialog, 2045, value.ignore_bad_files);
        SetChecked(dialog, 2008, value.item_tips);
        SetChecked(dialog, 2148, value.save_tags);
        PopulateResourceCombo(dialog, 2009, resources, 3,
                              std::clamp(value.read_info_mode, 0, 2));
        SetChecked(dialog, 1086, value.title_number);
        SetChecked(dialog, 1099, value.tag_format != 0);
        SetDlgItemTextW(dialog, 1097, value.tag_title_format.c_str());
        SetDlgItemTextW(dialog, 1089, value.default_title_format.c_str());
        EnableWindow(GetDlgItem(dialog, 1097), IsChecked(dialog, 1099));
        for (const int control : {1155,1156,1159,1160,1158,1162,1161})
            MakeColorButton(dialog, control);
        InstallButtonBitmap(dialog, 1036, resources, 0x161);
        InstallButtonBitmap(dialog, 2154, resources, 0x160);
        break;
    }
    case 255: {
        SetChecked(dialog, 2212, settings_.library.enabled);
        SetChecked(dialog, 2210, settings_.library.monitor_directories);
        const HWND list = GetDlgItem(dialog, 1038);
        InsertSingleListColumn(list, L"");
        for (size_t index = 0; list &&
             index < settings_.library.directories.size(); ++index) {
            const auto& directory = settings_.library.directories[index];
            AddListText(list, static_cast<int>(index),
                        directory.path.wstring());
            ListView_SetCheckState(list, static_cast<int>(index),
                                   directory.recursive);
        }
        if (list && ListView_GetItemCount(list) > 0)
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        InstallButtonBitmap(dialog, 1027, resources, 1027);
        InstallButtonBitmap(dialog, 1039, resources, 1039);
        EnableWindow(list, settings_.library.monitor_directories);
        EnableWindow(GetDlgItem(dialog, 1027),
                     settings_.library.monitor_directories);
        EnableWindow(GetDlgItem(dialog, 1039),
                     settings_.library.monitor_directories);
        UpdateFolderListButtons(dialog, resources, false);
        if (!settings_.library.monitor_directories)
            EnableWindow(GetDlgItem(dialog, 1039), FALSE);
        break;
    }
    case 256: {
        SetChecked(dialog, 2062, settings_.lyric.auto_load_lyric);
        SetChecked(dialog, 2063, settings_.lyric.trim_spaces);
        SetChecked(dialog, 2126, settings_.lyric.auto_save_lyric_tag);
        SetChecked(dialog, 2176, settings_.lyric.dont_load_lyric_tag);
        SetChecked(dialog, 2021, settings_.lyric.auto_visible);
        SetChecked(dialog, 2024, settings_.lyric.drag_lyric);
        SetChecked(dialog, 2066, settings_.lyric.save_compress);
        PopulateResourceCombo(dialog, 2196, resources, 3,
                              std::clamp(settings_.lyric.lyric_save_mode, 0, 2));
        const HWND tab = GetDlgItem(dialog, 2256);
        if (tab) {
            for (int index = 0; index < 2; ++index) {
                auto title = ResourceText(32770 + index);
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = title.data();
                TabCtrl_InsertItem(tab, index, &item);
            }
            const UINT child_id = options_focus_control_ == 385 ? 385 : 384;
            // Match 004974F0's desktop-then-window creation order. Each
            // child's 00495F5C/00496A32 initialization positions it at HWND_TOP
            // with SWP_NOSIZE, retaining its resource-defined dimensions.
            for (const UINT identifier : {385U, 384U}) {
                OptionsChildInit init{this, identifier};
                auto& child = options_lyric_pages_[identifier - 384];
                child = CreateWtlDialog(resources, MAKEINTRESOURCEW(identifier),
                    dialog, OptionsChildDialogProc, reinterpret_cast<LPARAM>(&init));
                PositionNestedDialog(dialog, tab, child, SWP_NOSIZE);
            }
            SelectOptionsLyricMode(dialog, child_id);
        }
        for (const int control : {2087, 2089, 2091})
            MakeOptionsHyperlink(dialog, control);
        break;
    }
    case 257: {
        const HWND list = GetDlgItem(dialog, 1038);
        InsertSingleListColumn(list, L"");
        for (size_t index = 0; list && index < settings_.lyric.folders.size(); ++index) {
            auto folder = settings_.lyric.folders[index];
            if (!folder.empty() && folder.front() == L'*')
                folder.erase(folder.begin());
            if (folder == L"<Sound Folder>")
                folder = ResourceText(0x8139);
            else if (folder == L"<Lyrics Download Folder>")
                folder = ResourceText(0x8138);
            AddListText(list, static_cast<int>(index), folder);
            ListView_SetCheckState(list, static_cast<int>(index),
                !settings_.lyric.folders[index].empty() &&
                settings_.lyric.folders[index].front() == L'*');
        }
        if (list && ListView_GetItemCount(list) > 0)
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        InstallButtonBitmap(dialog, 1027, resources, 1027);
        InstallButtonBitmap(dialog, 1039, resources, 1039);
        InstallButtonBitmap(dialog, 1042, resources, 1042);
        InstallButtonBitmap(dialog, 1045, resources, 1045);
        InstallButtonBitmap(dialog, 1023, resources, 1023);
        SetChecked(dialog, 2065, settings_.lyric.auto_download);
        SetChecked(dialog, 2180, settings_.lyric.download_when_full_info);
        SetChecked(dialog, 2067, settings_.lyric.auto_select_download);
        SetChecked(dialog, 2064, settings_.lyric.auto_associate);
        SetChecked(dialog, 2016, settings_.lyric.overwrite);
        SetChecked(dialog, 2147, settings_.lyric.same_file_title);
        SetChecked(dialog, 2146, settings_.lyric.save_to_sound_folder);
        SetDlgItemTextW(dialog, 1028,
                        settings_.lyric.download_folder.c_str());
        PopulateLyricServices(dialog);
        InstallLyricServiceEditorButton(dialog);
        UpdateFolderListButtons(dialog, resources, true);
        break;
    }
    case 258: {
        const HWND tab = GetDlgItem(dialog, 1181);
        if (tab) {
            for (int index = 0; index < 2; ++index) {
                auto title = ResourceText(33262 + index);
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = title.data();
                TabCtrl_InsertItem(tab, index, &item);
            }
            const UINT child_id = options_focus_control_ == 382 ? 382 : 381;
            for (const UINT identifier : {381U, 382U}) {
                OptionsChildInit init{this, identifier};
                auto& child = options_network_pages_[identifier - 381];
                child = CreateWtlDialog(resources, MAKEINTRESOURCEW(identifier), dialog,
                    OptionsChildDialogProc, reinterpret_cast<LPARAM>(&init));
                PositionNestedDialog(dialog, tab, child, SWP_NOSIZE);
            }
            SelectOptionsNetworkMode(dialog, child_id);
        }
        CheckRadioButton(dialog, 2190, 2192,
                         2190 + std::clamp(settings_.network.proxy_type, 0, 2));
        SetDlgItemTextW(dialog, 2143,
                        settings_.network.proxy_server.c_str());
        SetInteger(dialog, 2144, settings_.network.proxy_port);
        SetDlgItemTextW(dialog, 2184,
                        settings_.network.proxy_username.c_str());
        SetDlgItemTextW(dialog, 2185,
                        settings_.network.proxy_password.c_str());
        SetChecked(dialog, 2173, settings_.network.freedb_auto_query);
        SetChecked(dialog, 2176, settings_.network.show_info_when_fail);
        const HWND freedb = GetDlgItem(dialog, 2177);
        if (freedb) {
            for (const auto& server : settings_.network.server_list)
                SendMessageW(freedb, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(server.c_str()));
            if (!settings_.network.freedb_server.empty() &&
                SendMessageW(freedb, CB_FINDSTRINGEXACT,
                    static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(
                        settings_.network.freedb_server.c_str())) == CB_ERR)
                SendMessageW(freedb, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(
                        settings_.network.freedb_server.c_str()));
            SetWindowTextW(freedb, settings_.network.freedb_server.c_str());
        }
        const bool custom_proxy = settings_.network.proxy_type == 2;
        for (const int control : {2143,2144,2184,2185})
            EnableWindow(GetDlgItem(dialog, control), custom_proxy);
        break;
    }
    case 259: {
        auto folder = settings_.plugin.folder;
        if (folder.empty()) folder = FindRuntimePath(L"Plugins");
        auto folder_text = folder.wstring();
        if (!folder_text.empty() && folder_text.back() != L'\\' &&
            folder_text.back() != L'/') folder_text.push_back(L'\\');
        SetDlgItemTextW(dialog, 1028, folder_text.c_str());
        InstallButtonBitmap(dialog, 1023, resources, 1023);
        InstallButtonBitmap(dialog, 1042, resources, 1042);
        InstallButtonBitmap(dialog, 1045, resources, 1045);
        InstallButtonBitmap(dialog, 1015, resources, 0x160);
        const HWND list = GetDlgItem(dialog, 1064);
        if (list) {
            ListView_SetExtendedListViewStyle(list,
                LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_FLATSB);
            constexpr std::array<int, 3> widths{20,256,100};
            for (int index = 0; index < 3; ++index) {
                auto title = index == 0 ? std::wstring{}
                    : ResourceListItem(resources, 0x815b,
                                       static_cast<size_t>(index - 1));
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = title.data();
                column.cx = widths[static_cast<size_t>(index)];
                ListView_InsertColumn(list, index, &column);
            }

            EnableWindow(GetDlgItem(dialog, 1015), FALSE);
            EnableWindow(GetDlgItem(dialog, 1042), FALSE);
            EnableWindow(GetDlgItem(dialog, 1045), FALSE);
            // FUN_00428626/FUN_0042822F do not publish a candidate until the
            // DLL loads, exports winampDSPGetHeader2, and reports version >
            // 0x1f.  Run that legacy code in a bounded same-bitness helper;
            // loading arbitrary DllMain code on the property-sheet thread can
            // otherwise reproduce the old application's long UI hangs.
            StartOptionsDspScan(dialog, folder);
        }
        break;
    }
    case 260: {
        options_device_entries_.clear();

        // FUN_004991F1 performs waveOut, DirectSound, KS and ASIO discovery in
        // that order.  Preserve the resulting synchronous page snapshot, but
        // isolate every driver call in a same-bitness kill-on-close helper:
        // SetupDi/CreateFile/DirectSound callbacks themselves have no usable
        // in-process cancellation contract if a legacy driver stalls.
        auto discovered = RunOutputDeviceProbe();
        const bool complete_catalog = discovered.has_value();
        const auto append_devices = [this](
                std::vector<LegacyOutputDevice> sources) {
            for (auto& source : sources) {
                DeviceOptionEntry entry;
                entry.backend = source.backend;
                entry.wave_device_id = source.wave_device_id;
                entry.key = std::move(source.key);
                entry.name = std::move(source.name);
                entry.module = std::move(source.module);
                entry.identifier = source.class_id;
                entry.has_identifier = source.has_class_id;
                entry.details = std::move(source.details);
                entry.details_resolved = entry.backend == 0 ||
                    entry.backend == 2 || std::any_of(
                        entry.details.begin(), entry.details.end(),
                        [](const std::wstring& value) {
                            return !value.empty();
                        });
                options_device_entries_.push_back(std::move(entry));
            }
        };
        if (discovered) append_devices(std::move(*discovered));

        // A timed-out/missing helper must not erase the safe first half of
        // FUN_004991F1.  These calls enumerate names only and never activate
        // a DirectSound, KS or ASIO endpoint.
        if (options_device_entries_.empty())
            append_devices(EnumerateWaveAndDirectSoundOutputDevices());

        const bool configured_present = settings_.device.device_type.empty() ||
            std::any_of(options_device_entries_.begin(),
                        options_device_entries_.end(),
                        [this](const DeviceOptionEntry& entry) {
                return _wcsicmp(entry.key.c_str(),
                                settings_.device.device_type.c_str()) == 0;
            });
        if (!complete_catalog && !configured_present) {
            // The isolated full scan failed after settings had selected a
            // KS/ASIO (or now-unplugged DirectSound) key.  Keep that choice
            // visible and inert rather than silently rewriting it to row 0
            // when the user closes the page.
            DeviceOptionEntry configured;
            configured.key = settings_.device.device_type;
            configured.backend = DeviceBackendFromKey(
                configured.key, configured.wave_device_id);
            configured.name = configured.key;
            configured.details_resolved = true;
            options_device_entries_.push_back(std::move(configured));
        }

        if (options_device_entries_.empty()) {
            // A missing/timed-out/malformed helper is not evidence that the
            // configured device became Wave Mapper.  Publish one inert row
            // carrying the existing key so Close/Apply cannot silently
            // overwrite Device/@DeviceType after a discovery failure.
            DeviceOptionEntry fallback;
            fallback.key = settings_.device.device_type;
            fallback.backend = DeviceBackendFromKey(
                fallback.key, fallback.wave_device_id);
            fallback.name = fallback.key;
            if (fallback.name.empty()) fallback.name = i18n::Literal(L"Unavailable device");
            fallback.details_resolved = true;
            options_device_entries_.push_back(std::move(fallback));
        }
        const HWND devices = GetDlgItem(dialog, 1059);
        if (devices) {
            SendMessageW(devices, CB_RESETCONTENT, 0, 0);
            if (options_device_images_) {
                ImageList_Destroy(options_device_images_);
                options_device_images_ = nullptr;
            }
            options_device_images_ = ImageList_LoadImageW(
                resources, MAKEINTRESOURCEW(0x167), 16, 1,
                RGB(192, 192, 192), IMAGE_BITMAP, LR_CREATEDIBSECTION);
            if (options_device_images_)
                SendMessageW(devices, CBEM_SETIMAGELIST, 0,
                             reinterpret_cast<LPARAM>(options_device_images_));
            if (const HWND details = GetDlgItem(dialog, 1064);
                details && options_device_images_) {
                ListView_SetImageList(details, options_device_images_,
                                      LVSIL_SMALL);
            }
            int selected = 0;
            for (size_t index = 0; index < options_device_entries_.size();
                 ++index) {
                auto& entry = options_device_entries_[index];
                COMBOBOXEXITEMW item{};
                item.mask = CBEIF_TEXT | CBEIF_LPARAM;
                if (options_device_images_) {
                    item.mask |= CBEIF_IMAGE | CBEIF_SELECTEDIMAGE;
                    item.iImage = entry.backend;
                    item.iSelectedImage = entry.backend;
                }
                item.iItem = static_cast<int>(index);
                item.pszText = entry.name.data();
                item.lParam = static_cast<LPARAM>(index);
                const LRESULT inserted = SendMessageW(
                    devices, CBEM_INSERTITEMW, 0,
                    reinterpret_cast<LPARAM>(&item));
                if (_wcsicmp(entry.key.c_str(),
                             settings_.device.device_type.c_str()) == 0) {
                    selected = inserted >= 0
                        ? static_cast<int>(inserted)
                        : static_cast<int>(index);
                }
            }
            SendMessageW(devices, CB_SETCURSEL, selected, 0);
        }
        SetInteger(dialog, 1071, settings_.device.buffer_duration);
        SetSpinRange(dialog, 1072, 100, 10000);
        auto bit_labels = IntegerLabels({8,16,24,32}, L" Bits");
        bit_labels.insert(bit_labels.begin(), ResourceText(0x8151));
        PopulateTextCombo(dialog, 2041, bit_labels,
            std::clamp(settings_.device.output_bits / 8, 0, 4));
        SetChecked(dialog, 2037, settings_.device.dither != 0);
        PopulateResourceCombo(dialog, 2043, resources, 4,
            std::clamp(settings_.device.dither - 1, 0, 3));
        SetChecked(dialog, 2036, settings_.device.resample_rate != 0);
        const std::array<int, 13> rates{8000,11025,16000,22050,24000,32000,
            44100,48000,64000,88200,96000,176400,192000};
        PopulateTextCombo(dialog, 2042,
            IntegerLabels({8000,11025,16000,22050,24000,32000,44100,48000,
                           64000,88200,96000,176400,192000}, L" Hz"),
            settings_.device.resample_rate == 0 ? 7 : static_cast<int>(
                std::find(rates.begin(), rates.end(),
                          settings_.device.resample_rate) - rates.begin()));
        PopulateResourceCombo(dialog, 2186, resources, 3,
                              std::clamp(settings_.device.ssrc_mode, 0, 2));
        SetChecked(dialog, 2033, settings_.device.hardware_buffer);
        SetChecked(dialog, 2034, settings_.device.create_primary);
        EnableWindow(GetDlgItem(dialog, 2043), settings_.device.dither != 0);
        EnableWindow(GetDlgItem(dialog, 2042),
                     settings_.device.resample_rate != 0);
        EnableWindow(GetDlgItem(dialog, 2186),
                     settings_.device.resample_rate != 0);
        UpdateOptionsDeviceDetails(dialog);
        break;
    }
    case 261:
        if (const HWND preview = GetDlgItem(dialog, 1068)) {
            // Resource 261 declares 1068 as SysListView32.  The original
            // subclasses that HWND (FUN_0049B010); changing its low style
            // bits to SS_OWNERDRAW merely changes LVS_* flags and never emits
            // WM_DRAWITEM.
            SetWindowSubclass(preview, OptionsSkinPreviewSubclassProc,
                              kOptionsSkinPreviewSubclass, 0);
        }
        InstallButtonBitmap(dialog, 1098, resources, 1098);
        InstallButtonBitmap(dialog, 1039, resources, 1039);
        for (const int control : {1066, 1067, 2109})
            MakeOptionsHyperlink(dialog, control);
        InitializeOptionsSkinTabs(dialog);
        PopulateOptionsSkinPage(dialog);
        break;
    case 263: {
        const bool unified = settings_.fullscreen.visual_type == 0;
        // FUN_0049ECD0 checks 2233 and synchronously dispatches 0049F14C.
        // A non-unified page therefore always starts by editing Goom; the
        // disabled effect combo has no selection while unified settings are
        // active.
        if (!unified) settings_.fullscreen.visual_type = 1;
        PopulateResourceCombo(dialog, 2232, resources, 3,
                              unified ? -1 : 0);
        const auto album_label = AlbumOptionText(IDS_FULLSCREEN_ALBUM);
        SendDlgItemMessageW(dialog, 2232, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(album_label.c_str()));
        PopulateAlbumBackgroundOptions(dialog, instance_, settings_.fullscreen);
        SetChecked(dialog, 2233, unified);
        PopulateResourceCombo(dialog, 2230, resources, 2, -1);
        const bool layered = LayeredWindowsAvailableForOptions();
        if (!layered)
            SendDlgItemMessageW(dialog, 2230, CB_DELETESTRING, 1, 0);
        auto size_format = ResourceText(0x8214);
        if (size_format.empty()) size_format = L"%d %%";
        PopulateFullscreenLyricSizeCombo(dialog, size_format);
        SelectFullscreenProfileControls(
            dialog, settings_.fullscreen, unified ? 0 : 1);
        PopulateResourceCombo(dialog, 1033, resources, 2,
                              settings_.lyric.fullscreen_scroll_mode);
        PopulateResourceCombo(dialog, 1037, resources, 3,
                              settings_.lyric.fullscreen_text_align);
        PopulateLyricFadeCombo(dialog, resources,
                               settings_.lyric.fullscreen_fade_index);
        SetSpinRange(dialog, 1044, 0, 100);
        SetSpinPosition(dialog, 1044,
                        settings_.lyric.fullscreen_row_interval);
        SetChecked(dialog, 2145, settings_.lyric.fullscreen_fade_highlight);
        SetChecked(dialog, 2150, settings_.lyric.fullscreen_karaoke_mode);
        SetChecked(dialog, 2151, settings_.lyric.fullscreen_transparent);
        SetChecked(dialog, 2022, settings_.lyric.fullscreen_auto_font);
        if (!GetDlgItem(dialog, IDC_FULLSCREEN_LYRIC_DRAG)) {
            // Resource 263: the spare cell beside the font picker remains
            // inside "lyrics fullscreen", above the album-background group.
            RECT bounds{213, 139, 271, 149};
            MapDialogRect(dialog, &bounds);
            const auto label = AlbumOptionText(IDS_FULLSCREEN_LYRIC_DRAG);
            const HWND checkbox = CreateWindowExW(0, WC_BUTTONW, label.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FULLSCREEN_LYRIC_DRAG)),
                instance_, nullptr);
            SendMessageW(checkbox, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
            SetWindowPos(checkbox, GetDlgItem(dialog, 1036), 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SetChecked(dialog, IDC_FULLSCREEN_LYRIC_DRAG, settings_.lyric.fullscreen_drag_lyric);
        for (const int control : {1155,1156,1158})
            MakeColorButton(dialog, control);
        EnableWindow(GetDlgItem(dialog, 2232), !unified);
        EnableWindow(GetDlgItem(dialog, 2151), layered);
        InstallButtonBitmap(dialog, 1036, resources, 0x161);
        break;
    }
    case 262: {
        SetDlgItemTextW(dialog, 2282, AlbumOptionText(IDS_ASSOCIATION_VIEW_SYSTEM).c_str());
        InitializeOptionsAssociationActions(dialog);
        const HWND tree = GetDlgItem(dialog, 2038);
        if (tree) {
            TreeView_DeleteAllItems(tree);
            options_association_nodes_.clear();
            if (options_association_images_) {
                ImageList_Destroy(options_association_images_);
                options_association_images_ = nullptr;
            }
            options_association_images_ = ImageList_LoadImageW(
                ResourceModule(), MAKEINTRESOURCEW(0x164), 16, 0,
                RGB(255, 255, 255), IMAGE_BITMAP, 0);
            if (options_association_images_)
                TreeView_SetImageList(tree, options_association_images_,
                                      TVSIL_NORMAL);
            TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER,
                                      TVS_EX_DOUBLEBUFFER);
            TVINSERTSTRUCTW root{};
            root.hParent = TVI_ROOT;
            root.hInsertAfter = TVI_LAST;
            root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            auto all = ResourceText(0x8123);
            root.item.pszText = all.data();
            const HTREEITEM parent = TreeView_InsertItem(tree, &root);

            settings::FileAssociationBackend backend(
                CurrentExecutablePath(), ResourceText(0x80));
            const auto association_formats = BuildAssociationFormatSource(
                ResourceModule(), reader_formats_);
            for (size_t format_index = 0;
                 format_index < association_formats.size(); ++format_index) {
                const auto& source = association_formats[format_index];
                const auto extensions = settings::BuildAssociableExtensions(
                    std::vector<plugins::ReaderFormat>{source});
                if (extensions.empty()) continue;

                const auto description = AssociationTypeLabel(source.description);
                TVINSERTSTRUCTW category_item{};
                category_item.hParent = parent;
                // 0049D2FA inserts ordinary reader categories with TVI_SORT;
                // only the final playlist filter is pinned at the bottom.
                category_item.hInsertAfter =
                    format_index + 1U == association_formats.size()
                        ? TVI_LAST : TVI_SORT;
                category_item.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
                category_item.item.pszText =
                    const_cast<wchar_t*>(description.c_str());
                const HTREEITEM category =
                    TreeView_InsertItem(tree, &category_item);

                for (const auto& format : extensions) {
                    const auto query = backend.QueryExtension(format.extension);
                    auto node = std::make_unique<AssociationOptionNode>();
                    node->extension = format.extension;
                    // 0049D2FA -> 00433B2E (_wcsupr) before TVI_SORT insertion.
                    // Keep the reader/filter spelling untouched; only the
                    // association page's leaf labels are uppercased.
                    std::ranges::transform(node->extension, node->extension.begin(), towupper);
                    node->description = description;
                    node->current = query.result && query.effective;
                    node->desired = node->current;
                    // 0049D2FA discards the queried DefaultIcon when this
                    // executable is not the active handler. In particular,
                    // do not borrow another installed rebuild's ProgID icon
                    // when inspecting a portable/test copy of the player.
                    if (node->current) node->icon = query.icon;
                    if (node->icon.empty()) {
                        const auto candidate =
                            CurrentExecutablePath().parent_path() / L"Icons" /
                            (node->extension + L".ico");
                        std::error_code error;
                        if (std::filesystem::is_regular_file(candidate, error))
                            node->icon = candidate.wstring();
                    }
                    if (node->icon.empty()) {
                        // 00491213 builds "%s",1 for audio and "%s",2 for
                        // playlists; these original group icons are embedded
                        // in the rebuild too, so TTPlayer.exe is not needed.
                        node->icon = FormatIconSelection({CurrentExecutablePath(),
                            format_index + 1U == association_formats.size() ? 2 : 1});
                    }

                    TVINSERTSTRUCTW child{};
                    child.hParent = category;
                    child.hInsertAfter = TVI_SORT;
                    child.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
                    child.item.pszText = node->extension.data();
                    child.item.lParam = reinterpret_cast<LPARAM>(node.get());
                    child.item.iImage = child.item.iSelectedImage = node->current ? 1 : 0;
                    TreeView_InsertItem(tree, &child);
                    options_association_nodes_.push_back(std::move(node));
                }
                SetTreeCheckState(tree, category,
                                  AggregateTreeChildren(tree, category));
            }
            if (parent)
                SetTreeCheckState(tree, parent,
                                  AggregateTreeChildren(tree, parent));
            if (parent) {
                TreeView_SelectItem(tree, parent);
                TreeView_Expand(tree, parent, TVE_EXPAND);
            }
        }
        settings::FileAssociationBackend backend(
            CurrentExecutablePath(), ResourceText(0x80));
        const auto cd = backend.QueryShellIntegration(
            settings::ShellIntegrationTarget::audio_cd);
        const auto directory = backend.QueryShellIntegration(
            settings::ShellIntegrationTarget::directory);
        SetChecked(dialog, 1069, cd.result && cd.associated);
        SetChecked(dialog, 2120, directory.result && directory.associated);
        SetChecked(dialog, 2121, settings_.player.check_association);

        for (auto& images : options_association_button_images_) {
            if (images) ImageList_Destroy(images);
            images = nullptr;
        }
        const HICON shortcut_icon = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(128), IMAGE_ICON, 16, 16, LR_SHARED));
        options_association_button_images_[0] =
            ButtonIconImageList(shortcut_icon, 16, 16);
        options_association_button_images_[1] = AssociationBitmapImageList(resources, 0x162);
        options_association_button_images_[2] = AssociationBitmapImageList(resources, 0x163);
        options_association_button_images_[3] = ButtonIconImageList(
            window_icon_big_ ? window_icon_big_ : shortcut_icon, 32, 32);
        AttachButtonImage(GetDlgItem(dialog, 2030),
                          options_association_button_images_[0]);
        AttachButtonImage(GetDlgItem(dialog, 2031),
                          options_association_button_images_[1]);
        AttachButtonImage(GetDlgItem(dialog, 2032),
                          options_association_button_images_[2]);
        AttachButtonImage(GetDlgItem(dialog, 2106),
                          options_association_button_images_[3]);
        // 0049DC89 / 0049D125 also allow assigning a common icon to all
        // descendants of a selected category/root.
        EnableWindow(GetDlgItem(dialog, 2106), TRUE);
        EnableWindow(GetDlgItem(dialog, 2108), TRUE);
        RefreshOptionsAssociationIcon(dialog);
        break;
    }
    case 384: {
        PopulateResourceCombo(dialog, 1033, resources, 2,
                              settings_.lyric.scroll_mode);
        PopulateResourceCombo(dialog, 1037, resources, 3,
                              settings_.lyric.text_align);
        PopulateLyricFadeCombo(dialog, resources, settings_.lyric.fade_index);
        SetInteger(dialog, 1043, settings_.lyric.row_interval);
        SetSpinRange(dialog, 1044, 0, 100);
        SetChecked(dialog, 2145, settings_.lyric.fade_highlight);
        SetChecked(dialog, 2150, settings_.lyric.karaoke_mode);
        SetChecked(dialog, 2151, settings_.lyric.transparent);
        SetChecked(dialog, 2152, settings_.lyric.transparent_skin);
        SetChecked(dialog, 2022, settings_.lyric.auto_width);
        SetChecked(dialog, 2023, settings_.lyric.auto_width_only_vertical);
        for (const int control : {1155,1156,1158})
            MakeColorButton(dialog, control);
        const bool layered = LayeredWindowsAvailableForOptions();
        EnableWindow(GetDlgItem(dialog, 2151), layered);
        EnableWindow(GetDlgItem(dialog, 2152),
                     settings_.lyric.transparent);
        InstallButtonBitmap(dialog, 1036, resources, 0x161);
        InstallButtonBitmap(dialog, 2154, resources, 0x160);
        break;
    }
    case 385:
        // FUN_00402BCD initializes the three mutable preset names from
        // ttpres string IDs 0x80A2..0x80A4 before menu 388 is expanded.
        for (size_t index = 0;
             index < settings_.desktop_lyric.profiles.size(); ++index) {
            if (settings_.desktop_lyric.profiles[index].name.empty())
                settings_.desktop_lyric.profiles[index].name =
                    ResourceText(0x80a2 + static_cast<UINT>(index));
        }
        PopulateTextCombo(dialog, 2257, {L"1",L"2"},
                          std::clamp(settings_.desktop_lyric.lines - 1, 0, 1));
        PopulateResourceCombo(dialog, 1037, resources, 3,
                              std::clamp(settings_.desktop_lyric.align, 0, 2));
        SetChecked(dialog, 2258, settings_.desktop_lyric.shadow);
        SetChecked(dialog, 2150, settings_.desktop_lyric.karaoke_mode);
        SetChecked(dialog, 2259, settings_.desktop_lyric.smooth);
        SetChecked(dialog, 2022, settings_.desktop_lyric.auto_width);
        SetChecked(dialog, 2260, settings_.desktop_lyric.border);
        SetChecked(dialog, 2261, settings_.desktop_lyric.background_show);
        SetChecked(dialog, 2023, settings_.desktop_lyric.unlock_when_close);
        for (const int control : {1155,1156,1038,1158})
            MakeColorButton(dialog, control);
        EnableWindow(GetDlgItem(dialog, 1038),
                     settings_.desktop_lyric.border);
        EnableWindow(GetDlgItem(dialog, 1158),
                     settings_.desktop_lyric.background_show);
        for (const auto [control, value] : {
                std::pair{2262, settings_.desktop_lyric.text_alpha},
                std::pair{2263, settings_.desktop_lyric.background_alpha}}) {
            if (const HWND slider = GetDlgItem(dialog, control)) {
                SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
                // FUN_00495F5C presents opacity as the inverse percentage of
                // the persisted 0..255 alpha value.
                SendMessageW(slider, TBM_SETPOS, TRUE,
                             MulDiv(255 - std::clamp(value, 0, 255),
                                    100, 255));
            }
        }
        InstallButtonBitmap(dialog, 1036, resources, 0x161);
        InstallButtonBitmap(dialog, 2154, resources, 0x160);
        break;
    case 381:
        SetChecked(dialog, 2249,
                   settings_.network.accept_recommendation_list);
        // COptionsNetworkCache::OnInitDialog forces DAT_00547D70 on before
        // hiding 0x8CA; the remaining cache controls therefore stay active.
        settings_.network.cache_enabled = true;
        SetChecked(dialog, 2250, true);
        ShowWindow(GetDlgItem(dialog, 2250), SW_HIDE);
        SetInteger(dialog, 2252, settings_.network.cache_space_size);
        SetSpinRange(dialog, 2253, 600, 2000);
        SetDlgItemTextW(dialog, 2251,
                        settings_.network.cache_folder.c_str());
        CheckRadioButton(dialog, 1165, 1166,
                         settings_.network.speed_mode == 0 ? 1165 : 1166);
        InstallButtonBitmap(dialog, 1023, resources, 1023);
        EnableWindow(GetDlgItem(dialog, 2252), TRUE);
        EnableWindow(GetDlgItem(dialog, 2253), TRUE);
        EnableWindow(GetDlgItem(dialog, 2251), TRUE);
        EnableWindow(GetDlgItem(dialog, 1023), TRUE);
        break;
    case 382:
        SetChecked(dialog, 1175, settings_.network.create_folder_by_artist);
        SetChecked(dialog, 1176, settings_.network.replace_file);
        // The shipped 5.7.9 build creates this legacy replacement option from
        // template 382 but hides it during COptionsDownload initialization.
        ShowWindow(GetDlgItem(dialog, 1176), SW_HIDE);
        SetInteger(dialog, 1177, settings_.network.max_download_tasks);
        SetSpinRange(dialog, 1178, 1, 5);
        SetDlgItemTextW(dialog, 1179,
                        settings_.network.download_folder.c_str());
        SetChecked(dialog, 1180, settings_.network.download_when_listen);
        SetChecked(dialog, 1182, settings_.network.download_lyric);
        InstallButtonBitmap(dialog, 1023, resources, 1023);
        break;
    default:
        break;
    }
    ready();
}

void PlayerWindow::CommitOptionsPage(HWND dialog, UINT template_id) {
    if (!dialog || !IsWindow(dialog) ||
        !GetPropW(dialog, kPageReadyProperty)) return;
    switch (template_id) {
    case 250: {
        auto& value = settings_.general;
        value.startup_minimize = IsChecked(dialog, 2088);
        value.tray_icon = IsChecked(dialog, 2085);
        value.show_hotkey_in_tips = IsChecked(dialog, 2127);
        value.tips_on_open = IsChecked(dialog, 2086);
        value.menu_tips = IsChecked(dialog, 1079);
        value.menu_bar_playlist = IsChecked(dialog, 2153);
        value.scroll_title = IsChecked(dialog, 2189);
        value.send_title_to_msn = IsChecked(dialog, 2188);
        if (GetDlgItem(dialog, kOptionsDiscordLyrics))
            value.discord_sync_lyrics = IsChecked(dialog, kOptionsDiscordLyrics);
        if (GetDlgItem(dialog, kOptionsLanguage))
            CommitOptionsControl(dialog, template_id, kOptionsLanguage);
        value.fade_windows = IsChecked(dialog, 1075);
        value.snap_windows = (IsChecked(dialog, 1076) ? 0x10000 : 0) |
            GetInteger(dialog, 1077, value.snap_windows & 0xffff, 1, 100);
        value.title_slide_interval = (IsChecked(dialog, 1083) ? 0x10000 : 0) |
            GetInteger(dialog, 1084, value.title_slide_interval & 0xffff, 1, 100);
        value.auto_shutdown = IsChecked(dialog, 2182);
        SYSTEMTIME shutdown{};
        if (SendDlgItemMessageW(dialog, 2183, DTM_GETSYSTEMTIME, 0,
                reinterpret_cast<LPARAM>(&shutdown)) == GDT_VALID) {
            value.shutdown_time = {shutdown.wHour, shutdown.wMinute,
                                   shutdown.wSecond};
        }
        value.clear_list_on_command = IsChecked(dialog, 2139);
        value.default_list_on_command = IsChecked(dialog, 2137);
        value.default_list = GetText(dialog, 2138);
        constexpr std::array<int, 4> days{-1,1,7,30};
        value.check_update_days = days[static_cast<size_t>(std::clamp(
            ComboSelection(dialog, 1047), 0, 3))];
        break;
    }
    case 251: {
        auto& value = settings_.playback;
        value.auto_play = IsChecked(dialog, 1070);
        value.continue_play = IsChecked(dialog, 2092);
        value.stop_when_fail = IsChecked(dialog, 2093);
        value.track_interval = GetInteger(dialog, 1087,
                                           value.track_interval, 0, 10);
        constexpr std::array<int, 3> priorities{15,2,0};
        value.thread_priority = priorities[static_cast<size_t>(std::clamp(
            ComboSelection(dialog, 1100), 0, 2))];
        value.file_buffer = GetInteger(dialog, 2195,
            value.file_buffer / 1024, 1, 16384) * 1024;
        value.sound_fade_mode = 0;
        for (int index = 0; index < 4; ++index) {
            if (IsChecked(dialog, 2070 + index))
                value.sound_fade_mode |= 1 << index;
            value.fade_duration[index] = GetInteger(dialog, 2075 + index,
                value.fade_duration[index], 100, 10000);
        }
        if (IsChecked(dialog, 2074)) value.sound_fade_mode |= 0x10;
        value.track_fade_duration = GetInteger(dialog, 2079,
            value.track_fade_duration, 100, 10000);
        value.auto_gain = IsChecked(dialog, 2129);
        value.auto_scan_gain = IsChecked(dialog, 2130);
        value.skip_scan_gain = IsChecked(dialog, 2131);
        break;
    }
    case 252: {
        settings_.hotkey.global = IsChecked(dialog, 2053);
        break;
    }
    case 254: {
        auto& value = settings_.playlist;
        value.enable_drag_drop = IsChecked(dialog, 2010);
        value.disable_delete_file = IsChecked(dialog, 2128);
        value.save_relative_path = IsChecked(dialog, 2125);
        value.ignore_bad_files = IsChecked(dialog, 2045);
        value.item_tips = IsChecked(dialog, 2008);
        value.read_info_mode = ComboSelection(dialog, 2009,
                                               value.read_info_mode);
        value.save_tags = IsChecked(dialog, 2148);
        value.title_number = IsChecked(dialog, 1086);
        value.tag_format = IsChecked(dialog, 1099) ? 1 : 0;
        value.tag_title_format = GetText(dialog, 1097);
        value.default_title_format = GetText(dialog, 1089);
        break;
    }
    case 255: {
        settings_.library.enabled = IsChecked(dialog, 2212);
        settings_.library.monitor_directories = IsChecked(dialog, 2210);
        const HWND list = GetDlgItem(dialog, 1038);
        settings_.library.directories.clear();
        const int count = list ? ListView_GetItemCount(list) : 0;
        for (int row = 0; row < count; ++row) {
            std::array<wchar_t, 32768> text{};
            ListView_GetItemText(list, row, 0, text.data(),
                                 static_cast<int>(text.size()));
            settings_.library.directories.push_back({
                std::filesystem::path(text.data()),
                ListView_GetCheckState(list, row) != FALSE});
        }
        break;
    }
    case 256:
        settings_.lyric.auto_load_lyric = IsChecked(dialog, 2062);
        settings_.lyric.trim_spaces = IsChecked(dialog, 2063);
        settings_.lyric.auto_save_lyric_tag = IsChecked(dialog, 2126);
        settings_.lyric.dont_load_lyric_tag = IsChecked(dialog, 2176);
        settings_.lyric.auto_visible = IsChecked(dialog, 2021);
        settings_.lyric.drag_lyric = IsChecked(dialog, 2024);
        settings_.lyric.save_compress = IsChecked(dialog, 2066);
        settings_.lyric.lyric_save_mode = ComboSelection(dialog, 2196,
            settings_.lyric.lyric_save_mode);
        for (size_t index = 0; index < options_lyric_pages_.size(); ++index)
            if (const HWND child = options_lyric_pages_[index]; child && IsWindow(child))
                CommitOptionsPage(child, 384 + static_cast<UINT>(index));
        break;
    case 257: {
        settings_.lyric.auto_download = IsChecked(dialog, 2065);
        settings_.lyric.download_when_full_info = IsChecked(dialog, 2180);
        settings_.lyric.auto_select_download = IsChecked(dialog, 2067);
        settings_.lyric.auto_associate = IsChecked(dialog, 2064);
        settings_.lyric.overwrite = IsChecked(dialog, 2016);
        settings_.lyric.same_file_title = IsChecked(dialog, 2147);
        settings_.lyric.save_to_sound_folder = IsChecked(dialog, 2146);
        settings_.lyric.download_folder = GetText(dialog, 1028);
        SelectLyricService(ComboSelection(dialog, 2090, settings_.lyric.add_in_index));
        const HWND list = GetDlgItem(dialog, 1038);
        if (list) {
            settings_.lyric.folders.clear();
            const int count = ListView_GetItemCount(list);
            for (int row = 0; row < count; ++row) {
                std::array<wchar_t, 32768> text{};
                ListView_GetItemText(list, row, 0, text.data(),
                                     static_cast<int>(text.size()));
                std::wstring folder = text.data();
                if (!folder.empty() && folder.front() == L'*')
                    folder.erase(folder.begin());
                if (folder == ResourceText(0x8139))
                    folder = L"<Sound Folder>";
                else if (folder == ResourceText(0x8138))
                    folder = L"<Lyrics Download Folder>";
                if (ListView_GetCheckState(list, row)) folder.insert(folder.begin(), L'*');
                settings_.lyric.folders.push_back(std::move(folder));
            }
        }
        break;
    }
    case 258:
        if (IsChecked(dialog, 2192)) settings_.network.proxy_type = 2;
        else if (IsChecked(dialog, 2191)) settings_.network.proxy_type = 1;
        else settings_.network.proxy_type = 0;
        settings_.network.proxy_server = GetText(dialog, 2143);
        settings_.network.proxy_port = GetInteger(dialog, 2144,
            settings_.network.proxy_port, 0, 65535);
        settings_.network.proxy_username = GetText(dialog, 2184);
        settings_.network.proxy_password = GetText(dialog, 2185);
        settings_.network.freedb_auto_query = IsChecked(dialog, 2173);
        settings_.network.show_info_when_fail = IsChecked(dialog, 2176);
        settings_.network.freedb_server = GetText(dialog, 2177);
        settings_.network.server_list.clear();
        if (const HWND servers = GetDlgItem(dialog, 2177)) {
            const int count = static_cast<int>(
                SendMessageW(servers, CB_GETCOUNT, 0, 0));
            for (int index = 0; index < count; ++index) {
                const int length = static_cast<int>(
                    SendMessageW(servers, CB_GETLBTEXTLEN, index, 0));
                if (length < 0) continue;
                std::wstring server(static_cast<size_t>(length) + 1, L'\0');
                SendMessageW(servers, CB_GETLBTEXT, index,
                             reinterpret_cast<LPARAM>(server.data()));
                server.resize(static_cast<size_t>(length));
                settings_.network.server_list.push_back(std::move(server));
            }
        }
        for (size_t index = 0; index < options_network_pages_.size(); ++index)
            if (const HWND child = options_network_pages_[index]; child && IsWindow(child))
                CommitOptionsPage(child, static_cast<UINT>(381 + index));
        break;
    case 259: {
        settings_.plugin.folder = GetText(dialog, 1028);
        // An isolated scan can still be running when the user immediately
        // closes the sheet.  An empty in-progress ListView is not a request
        // to erase the persisted module order.
        if (!options_dsp_scan_complete_) break;
        settings_.plugin.modules.clear();
        const HWND list = GetDlgItem(dialog, 1064);
        const int count = list ? ListView_GetItemCount(list) : 0;
        for (int row = 0; row < count; ++row) {
            if (!ListView_GetCheckState(list, row)) continue;
            const LPARAM data = GetListItemData(list, row);
            if (data >= 0 && static_cast<size_t>(data) < options_dsp_paths_.size())
                settings_.plugin.modules.push_back(
                    options_dsp_paths_[static_cast<size_t>(data)].wstring());
        }
        break;
    }
    case 260: {
        if (const auto device = SelectedOutputDeviceEntry(
                dialog, options_device_entries_.size())) {
            settings_.device.device_type =
                options_device_entries_[*device].key;
        }
        settings_.device.buffer_duration = GetInteger(dialog, 1071,
            settings_.device.buffer_duration, 100, 10000);
        settings_.device.output_bits =
            std::clamp(ComboSelection(dialog, 2041), 0, 4) * 8;
        settings_.device.dither = IsChecked(dialog, 2037)
            ? std::clamp(ComboSelection(dialog, 2043), 0, 3) + 1 : 0;
        constexpr std::array<int, 13> rates{8000,11025,16000,22050,24000,32000,
            44100,48000,64000,88200,96000,176400,192000};
        settings_.device.resample_rate = IsChecked(dialog, 2036)
            ? rates[static_cast<size_t>(std::clamp(
                  ComboSelection(dialog, 2042), 0, 12))] : 0;
        settings_.device.ssrc_mode = ComboSelection(dialog, 2186,
                                                     settings_.device.ssrc_mode);
        settings_.device.hardware_buffer = IsChecked(dialog, 2033);
        settings_.device.create_primary = IsChecked(dialog, 2034);
        break;
    }
    case 263:
        // COptionsFullScreen has no deferred DDX pass.  FUN_00491571 writes
        // each setting from its own WM_COMMAND handler, so leaving or closing
        // the page must not normalize untouched combo/edit values.
        break;
    case 262:
        settings_.player.check_association = IsChecked(dialog, 2121);
        settings_.player.suppress_association_reminder = IsChecked(dialog, IDC_ASSOCIATION_SUPPRESS);
        break;
    case 384:
        settings_.lyric.scroll_mode = ComboSelection(dialog, 1033,
                                                      settings_.lyric.scroll_mode);
        settings_.lyric.text_align = ComboSelection(dialog, 1037,
                                                     settings_.lyric.text_align);
        settings_.lyric.fade_index = LyricFadeValue(
            dialog, settings_.lyric.fade_index);
        settings_.lyric.row_interval = GetInteger(dialog, 1043,
            settings_.lyric.row_interval, 0, 100);
        settings_.lyric.fade_highlight = IsChecked(dialog, 2145);
        settings_.lyric.karaoke_mode = IsChecked(dialog, 2150);
        settings_.lyric.transparent = IsChecked(dialog, 2151);
        settings_.lyric.transparent_skin = IsChecked(dialog, 2152);
        settings_.lyric.auto_width = IsChecked(dialog, 2022);
        settings_.lyric.auto_width_only_vertical = IsChecked(dialog, 2023);
        break;
    case 385:
        settings_.desktop_lyric.lines = ComboSelection(dialog, 2257,
            settings_.desktop_lyric.lines - 1) + 1;
        settings_.desktop_lyric.align = ComboSelection(dialog, 1037,
            settings_.desktop_lyric.align);
        settings_.desktop_lyric.shadow = IsChecked(dialog, 2258);
        settings_.desktop_lyric.karaoke_mode = IsChecked(dialog, 2150);
        settings_.desktop_lyric.smooth = IsChecked(dialog, 2259);
        settings_.desktop_lyric.auto_width = IsChecked(dialog, 2022);
        settings_.desktop_lyric.border = IsChecked(dialog, 2260);
        settings_.desktop_lyric.background_show = IsChecked(dialog, 2261);
        settings_.desktop_lyric.unlock_when_close = IsChecked(dialog, 2023);
        if (const HWND slider = GetDlgItem(dialog, 2262))
            settings_.desktop_lyric.text_alpha = MulDiv(
                100 - static_cast<int>(SendMessageW(
                    slider, TBM_GETPOS, 0, 0)), 255, 100);
        if (const HWND slider = GetDlgItem(dialog, 2263))
            settings_.desktop_lyric.background_alpha = MulDiv(
                100 - static_cast<int>(SendMessageW(
                    slider, TBM_GETPOS, 0, 0)), 255, 100);
        break;
    case 381:
        settings_.network.accept_recommendation_list = IsChecked(dialog, 2249);
        settings_.network.cache_enabled = IsChecked(dialog, 2250);
        settings_.network.cache_space_size = GetInteger(dialog, 2252,
            settings_.network.cache_space_size, 600, 2000);
        settings_.network.cache_folder = WithLegacyDirectoryTerminator(
            std::filesystem::path(GetText(dialog, 2251)));
        settings_.network.speed_mode = IsChecked(dialog, 1166) ? 1 : 0;
        break;
    case 382:
        settings_.network.create_folder_by_artist = IsChecked(dialog, 1175);
        settings_.network.replace_file = IsChecked(dialog, 1176);
        settings_.network.max_download_tasks = GetInteger(dialog, 1177,
            settings_.network.max_download_tasks, 1, 5);
        settings_.network.download_folder = WithLegacyDirectoryTerminator(
            std::filesystem::path(GetText(dialog, 1179)));
        settings_.network.download_when_listen = IsChecked(dialog, 1180);
        settings_.network.download_lyric = IsChecked(dialog, 1182);
        break;
    default:
        break;
    }
}

bool PlayerWindow::CommitOptionsControl(
    HWND dialog, UINT template_id, UINT control) {
    if (!dialog || !IsWindow(dialog) ||
        !GetPropW(dialog, kPageReadyProperty)) {
        return false;
    }

    // The original option classes do not run a page-wide DDX transaction for
    // every WM_COMMAND.  Each command handler copies only the value belonging
    // to the control that generated the notification.  Apart from matching
    // that behaviour, this preserves legacy values which a modern combo may
    // be unable to represent (notably an unavailable output device/rate).
    switch (template_id) {
    case 250: {
        auto& value = settings_.general;
        switch (control) {
        case 2088: value.startup_minimize = IsChecked(dialog, 2088); break;
        case 2085: value.tray_icon = IsChecked(dialog, 2085); break;
        case 2127: value.show_hotkey_in_tips = IsChecked(dialog, 2127); break;
        case 2086: value.tips_on_open = IsChecked(dialog, 2086); break;
        case 1079: value.menu_tips = IsChecked(dialog, 1079); break;
        case 2153: value.menu_bar_playlist = IsChecked(dialog, 2153); break;
        case 2189: value.scroll_title = IsChecked(dialog, 2189); break;
        case 2188: value.send_title_to_msn = IsChecked(dialog, 2188); break;
        case kOptionsDiscordLyrics:
            value.discord_sync_lyrics = IsChecked(dialog, kOptionsDiscordLyrics);
            break;
        case kOptionsLanguage: {
            const LRESULT selected = SendDlgItemMessageW(dialog, kOptionsLanguage, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR) {
                const LRESULT code = SendDlgItemMessageW(dialog, kOptionsLanguage, CB_GETITEMDATA, selected, 0);
                value.language = code && code != CB_ERR ? reinterpret_cast<const wchar_t*>(code) :
                    selected == 0 ? L"auto" : selected == 1 ? L"source" : GetText(dialog, kOptionsLanguage);
            }
            break;
        }
        case 1075: value.fade_windows = IsChecked(dialog, 1075); break;
        case 1076:
            value.snap_windows = (value.snap_windows & 0xffff) |
                (IsChecked(dialog, 1076) ? 0x10000 : 0);
            break;
        case 1077:
            value.snap_windows = (value.snap_windows & 0x10000) |
                GetInteger(dialog, 1077, value.snap_windows & 0xffff, 1, 100);
            break;
        case 1083:
            value.title_slide_interval =
                (value.title_slide_interval & 0xffff) |
                (IsChecked(dialog, 1083) ? 0x10000 : 0);
            break;
        case 1084:
            value.title_slide_interval =
                (value.title_slide_interval & 0x10000) |
                GetInteger(dialog, 1084,
                           value.title_slide_interval & 0xffff, 1, 100);
            break;
        case 2182: value.auto_shutdown = IsChecked(dialog, 2182); break;
        case 2139:
            value.clear_list_on_command = IsChecked(dialog, 2139);
            break;
        case 2137:
            value.default_list_on_command = IsChecked(dialog, 2137);
            break;
        case 2138: value.default_list = GetText(dialog, 2138); break;
        case 1047: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 1047, CB_GETCURSEL, 0, 0);
            constexpr std::array<int, 4> days{-1, 1, 7, 30};
            if (selected >= 0 && selected < static_cast<LRESULT>(days.size()))
                value.check_update_days = days[static_cast<size_t>(selected)];
            break;
        }
        default: return false;
        }
        return true;
    }
    case 251: {
        auto& value = settings_.playback;
        switch (control) {
        case 1070: value.auto_play = IsChecked(dialog, 1070); break;
        case 2092: value.continue_play = IsChecked(dialog, 2092); break;
        case 2093: value.stop_when_fail = IsChecked(dialog, 2093); break;
        case 1087:
            value.track_interval = GetInteger(
                dialog, 1087, value.track_interval, 0, 10);
            break;
        case 1100: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 1100, CB_GETCURSEL, 0, 0);
            constexpr std::array<int, 3> priorities{15, 2, 0};
            if (selected >= 0 &&
                selected < static_cast<LRESULT>(priorities.size())) {
                value.thread_priority = priorities[static_cast<size_t>(selected)];
            }
            break;
        }
        case 2195:
            value.file_buffer = GetInteger(
                dialog, 2195, value.file_buffer / 1024, 1, 16384) * 1024;
            break;
        case 2070: case 2071: case 2072: case 2073: case 2074: {
            const int bit = control == 2074
                ? 0x10 : 1 << static_cast<int>(control - 2070);
            if (IsChecked(dialog, static_cast<int>(control)))
                value.sound_fade_mode |= bit;
            else
                value.sound_fade_mode &= ~bit;
            break;
        }
        case 2075: case 2076: case 2077: case 2078: {
            const size_t index = static_cast<size_t>(control - 2075);
            value.fade_duration[index] = GetInteger(
                dialog, static_cast<int>(control), value.fade_duration[index],
                100, 10000);
            break;
        }
        case 2079:
            value.track_fade_duration = GetInteger(
                dialog, 2079, value.track_fade_duration, 100, 10000);
            break;
        case 2129: value.auto_gain = IsChecked(dialog, 2129); break;
        case 2130: value.auto_scan_gain = IsChecked(dialog, 2130); break;
        case 2131: value.skip_scan_gain = IsChecked(dialog, 2131); break;
        default: return false;
        }
        return true;
    }
    case 252:
        if (control != 2053) return false;
        settings_.hotkey.global = IsChecked(dialog, 2053);
        return true;
    case 254: {
        auto& value = settings_.playlist;
        switch (control) {
        case 2010: value.enable_drag_drop = IsChecked(dialog, 2010); break;
        case 2128: value.disable_delete_file = IsChecked(dialog, 2128); break;
        case 2125: value.save_relative_path = IsChecked(dialog, 2125); break;
        case 2045: value.ignore_bad_files = IsChecked(dialog, 2045); break;
        case 2008: value.item_tips = IsChecked(dialog, 2008); break;
        case 2009: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2009, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR) value.read_info_mode = static_cast<int>(selected);
            break;
        }
        case 2148: value.save_tags = IsChecked(dialog, 2148); break;
        case 1086: value.title_number = IsChecked(dialog, 1086); break;
        case 1099: value.tag_format = IsChecked(dialog, 1099) ? 1 : 0; break;
        case 1097: value.tag_title_format = GetText(dialog, 1097); break;
        case 1089: value.default_title_format = GetText(dialog, 1089); break;
        default: return false;
        }
        return true;
    }
    case 255:
        if (control == 2212) {
            settings_.library.enabled = IsChecked(dialog, 2212);
            return true;
        }
        if (control == 2210) {
            settings_.library.monitor_directories = IsChecked(dialog, 2210);
            return true;
        }
        return false;
    case 256:
        switch (control) {
        case 2062: settings_.lyric.auto_load_lyric = IsChecked(dialog, 2062); break;
        case 2063: settings_.lyric.trim_spaces = IsChecked(dialog, 2063); break;
        case 2126: settings_.lyric.auto_save_lyric_tag = IsChecked(dialog, 2126); break;
        case 2176: settings_.lyric.dont_load_lyric_tag = IsChecked(dialog, 2176); break;
        case 2021: settings_.lyric.auto_visible = IsChecked(dialog, 2021); break;
        case 2024: settings_.lyric.drag_lyric = IsChecked(dialog, 2024); break;
        case 2066: settings_.lyric.save_compress = IsChecked(dialog, 2066); break;
        case 2196: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2196, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                settings_.lyric.lyric_save_mode = static_cast<int>(selected);
            break;
        }
        default: return false;
        }
        return true;
    case 257:
        switch (control) {
        case 2065: settings_.lyric.auto_download = IsChecked(dialog, 2065); break;
        case 2180: settings_.lyric.download_when_full_info = IsChecked(dialog, 2180); break;
        case 2067: settings_.lyric.auto_select_download = IsChecked(dialog, 2067); break;
        case 2064: settings_.lyric.auto_associate = IsChecked(dialog, 2064); break;
        case 2016: settings_.lyric.overwrite = IsChecked(dialog, 2016); break;
        case 2147: settings_.lyric.same_file_title = IsChecked(dialog, 2147); break;
        case 2146: settings_.lyric.save_to_sound_folder = IsChecked(dialog, 2146); break;
        case 1028: settings_.lyric.download_folder = GetText(dialog, 1028); break;
        case 2090: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2090, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                SelectLyricService(static_cast<int>(selected));
            break;
        }
        default: return false;
        }
        return true;
    case 258:
        switch (control) {
        case 2190: case 2191: case 2192:
            settings_.network.proxy_type = static_cast<int>(control - 2190);
            break;
        case 2143: settings_.network.proxy_server = GetText(dialog, 2143); break;
        case 2144:
            settings_.network.proxy_port = GetInteger(
                dialog, 2144, settings_.network.proxy_port, 0, 65535);
            break;
        case 2184: settings_.network.proxy_username = GetText(dialog, 2184); break;
        case 2185: settings_.network.proxy_password = GetText(dialog, 2185); break;
        case 2173: settings_.network.freedb_auto_query = IsChecked(dialog, 2173); break;
        case 2176: settings_.network.show_info_when_fail = IsChecked(dialog, 2176); break;
        case 2177:
            // FUN_00490AB9 dispatches both CBN_SELCHANGE (1) and
            // CBN_EDITCHANGE (5) to FUN_00498B95.  The selected/typed value is
            // independent of the persistent drop-down history.
            settings_.network.freedb_server = GetText(dialog, 2177);
            break;
        default: return false;
        }
        return true;
    case 259:
        if (control != 1028) return false;
        settings_.plugin.folder = GetText(dialog, 1028);
        return true;
    case 260: {
        switch (control) {
        case 1059: {
            if (const auto selected = SelectedOutputDeviceEntry(
                    dialog, options_device_entries_.size())) {
                settings_.device.device_type =
                    options_device_entries_[*selected].key;
            }
            break;
        }
        case 1071:
            settings_.device.buffer_duration = GetInteger(
                dialog, 1071, settings_.device.buffer_duration, 100, 10000);
            break;
        case 2041: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2041, CB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected <= 4)
                settings_.device.output_bits = static_cast<int>(selected) * 8;
            break;
        }
        case 2037: {
            if (!IsChecked(dialog, 2037)) {
                settings_.device.dither = 0;
            } else {
                const LRESULT selected = SendDlgItemMessageW(
                    dialog, 2043, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected <= 3)
                    settings_.device.dither = static_cast<int>(selected) + 1;
            }
            break;
        }
        case 2043: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2043, CB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected <= 3)
                settings_.device.dither = static_cast<int>(selected) + 1;
            break;
        }
        case 2036: {
            if (!IsChecked(dialog, 2036)) {
                settings_.device.resample_rate = 0;
            } else {
                constexpr std::array<int, 13> rates{
                    8000,11025,16000,22050,24000,32000,44100,
                    48000,64000,88200,96000,176400,192000};
                const LRESULT selected = SendDlgItemMessageW(
                    dialog, 2042, CB_GETCURSEL, 0, 0);
                if (selected >= 0 &&
                    selected < static_cast<LRESULT>(rates.size())) {
                    settings_.device.resample_rate =
                        rates[static_cast<size_t>(selected)];
                }
            }
            break;
        }
        case 2042: {
            constexpr std::array<int, 13> rates{
                8000,11025,16000,22050,24000,32000,44100,
                48000,64000,88200,96000,176400,192000};
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2042, CB_GETCURSEL, 0, 0);
            if (selected >= 0 &&
                selected < static_cast<LRESULT>(rates.size())) {
                settings_.device.resample_rate =
                    rates[static_cast<size_t>(selected)];
            }
            break;
        }
        case 2186: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2186, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                settings_.device.ssrc_mode = static_cast<int>(selected);
            break;
        }
        case 2033:
            settings_.device.hardware_buffer = IsChecked(dialog, 2033);
            break;
        case 2034:
            settings_.device.create_primary = IsChecked(dialog, 2034);
            break;
        default: return false;
        }
        return true;
    }
    case 262:
        if (control == IDC_ASSOCIATION_SUPPRESS) {
            settings_.player.suppress_association_reminder = IsChecked(dialog, IDC_ASSOCIATION_SUPPRESS);
            return true;
        }
        if (control != 2121) return false;
        settings_.player.check_association = IsChecked(dialog, 2121);
        return true;
    case 384:
        switch (control) {
        case 1033: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 1033, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                settings_.lyric.scroll_mode = static_cast<int>(selected);
            break;
        }
        case 1037: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 1037, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                settings_.lyric.text_align = static_cast<int>(selected);
            break;
        }
        case 1026:
            settings_.lyric.fade_index = LyricFadeValue(
                dialog, settings_.lyric.fade_index);
            break;
        case 1043:
            settings_.lyric.row_interval = GetInteger(
                dialog, 1043, settings_.lyric.row_interval, 0, 100);
            break;
        case 2145: settings_.lyric.fade_highlight = IsChecked(dialog, 2145); break;
        case 2150: settings_.lyric.karaoke_mode = IsChecked(dialog, 2150); break;
        case 2151: settings_.lyric.transparent = IsChecked(dialog, 2151); break;
        case 2152: settings_.lyric.transparent_skin = IsChecked(dialog, 2152); break;
        case 2022: settings_.lyric.auto_width = IsChecked(dialog, 2022); break;
        case 2023: settings_.lyric.auto_width_only_vertical = IsChecked(dialog, 2023); break;
        default: return false;
        }
        return true;
    case 385:
        switch (control) {
        case 2257: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 2257, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                settings_.desktop_lyric.lines = static_cast<int>(selected) + 1;
            break;
        }
        case 1037: {
            const LRESULT selected = SendDlgItemMessageW(
                dialog, 1037, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                settings_.desktop_lyric.align = static_cast<int>(selected);
            break;
        }
        case 2258: settings_.desktop_lyric.shadow = IsChecked(dialog, 2258); break;
        case 2150: settings_.desktop_lyric.karaoke_mode = IsChecked(dialog, 2150); break;
        case 2259: settings_.desktop_lyric.smooth = IsChecked(dialog, 2259); break;
        case 2022: settings_.desktop_lyric.auto_width = IsChecked(dialog, 2022); break;
        case 2260: settings_.desktop_lyric.border = IsChecked(dialog, 2260); break;
        case 2261: settings_.desktop_lyric.background_show = IsChecked(dialog, 2261); break;
        case 2023: settings_.desktop_lyric.unlock_when_close = IsChecked(dialog, 2023); break;
        case 2262:
            settings_.desktop_lyric.text_alpha = MulDiv(
                100 - static_cast<int>(SendDlgItemMessageW(
                    dialog, 2262, TBM_GETPOS, 0, 0)), 255, 100);
            break;
        case 2263:
            settings_.desktop_lyric.background_alpha = MulDiv(
                100 - static_cast<int>(SendDlgItemMessageW(
                    dialog, 2263, TBM_GETPOS, 0, 0)), 255, 100);
            break;
        default: return false;
        }
        return true;
    case 381:
        switch (control) {
        case 2249:
            settings_.network.accept_recommendation_list =
                IsChecked(dialog, 2249);
            break;
        case 2250:
            settings_.network.cache_enabled = IsChecked(dialog, 2250);
            break;
        case 2252:
            settings_.network.cache_space_size = GetInteger(
                dialog, 2252, settings_.network.cache_space_size, 600, 2000);
            break;
        case 2251:
            settings_.network.cache_folder = WithLegacyDirectoryTerminator(
                std::filesystem::path(GetText(dialog, 2251)));
            break;
        case 1165: case 1166:
            settings_.network.speed_mode = control == 1166 ? 1 : 0;
            break;
        default: return false;
        }
        return true;
    case 382:
        switch (control) {
        case 1175:
            settings_.network.create_folder_by_artist = IsChecked(dialog, 1175);
            break;
        case 1176:
            settings_.network.replace_file = IsChecked(dialog, 1176);
            break;
        case 1177:
            settings_.network.max_download_tasks = GetInteger(
                dialog, 1177, settings_.network.max_download_tasks, 1, 5);
            break;
        case 1179:
            settings_.network.download_folder = WithLegacyDirectoryTerminator(
                std::filesystem::path(GetText(dialog, 1179)));
            break;
        case 1180:
            settings_.network.download_when_listen = IsChecked(dialog, 1180);
            break;
        case 1182:
            settings_.network.download_lyric = IsChecked(dialog, 1182);
            break;
        default: return false;
        }
        return true;
    default:
        return false;
    }
}

void PlayerWindow::FlushDeferredOptionsRuntime(UINT template_id) {
    UINT requested = options_deferred_apply_mask_;
    if (template_id != 0) {
        requested = DeferredOptionsBit(template_id);
        // The normal/desktop lyric and network subpages are child dialogs of
        // templates 256/258.  A property-page deactivation commits the whole
        // visible logical page, including whichever nested page owns focus.
        if (template_id == 256)
            requested |= DeferredOptionsBit(384) | DeferredOptionsBit(385);
        else if (template_id == 258)
            requested |= DeferredOptionsBit(381) | DeferredOptionsBit(382);
        requested &= options_deferred_apply_mask_;
    }
    if (requested == 0) return;
    options_deferred_apply_mask_ &= ~requested;

    constexpr std::array<UINT, 18> templates{
        250,251,252,253,254,255,256,257,258,259,260,261,262,263,
        381,382,384,385};
    for (const UINT current : templates) {
        if ((requested & DeferredOptionsBit(current)) != 0)
            ApplyOptionsPageRuntime(current);
    }
}

void PlayerWindow::ApplyOptionsPageRuntime(UINT template_id) {
    const UINT change_mask = OptionsPageRuntimeChangeMask(template_id);
    if (change_mask != 0) {
        // FUN_0046228D records Device's 0x400 transition, then the next
        // FUN_0045BF4B transaction issues player command 0x7EB. Keeping the
        // routing here also prevents ApplyOptionsRuntime(260) from running
        // twice for one page notification.
        ApplyOptionsChangeMask(change_mask, 0);
        return;
    }
    ApplyOptionsRuntime(template_id);
}

void PlayerWindow::ApplyOptionsRuntime(UINT template_id) {
    const bool all = template_id == 0;
    if (all) {
        // AppIconFile belongs to General settings but is edited from the
        // association page.  A full reset/save must refresh the same global
        // icon path as command 33000/33001.
        ReloadApplicationIcons();
        const bool custom = !settings_.general.app_icon_file.empty();
        const HICON small_icon = !custom && skin_ && skin_->Icon()
            ? skin_->Icon() : window_icon_small_;
        const HICON large_icon = !custom && skin_ && skin_->Icon()
            ? skin_->Icon() : window_icon_big_;
        if (window_) {
            SendMessageW(window_, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(small_icon));
            SendMessageW(window_, WM_SETICON, ICON_BIG,
                         reinterpret_cast<LPARAM>(large_icon));
        }
    }
    if (all || template_id == 251 || template_id == 259 ||
        template_id == 260) {
        audio_->Configure({settings_.playback.file_buffer,
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
        audio_->SetVolume(settings_.player.mute ? 0.0F :
            static_cast<float>(settings_.player.volume) / 100.0F);
    }
    if (all || template_id == 250) {
        discord_presence_.Configure(settings_.general.send_title_to_msn,
                                    settings_.general.discord_application_id);
        UpdateTrayIcon();
        if (!settings_.general.menu_tips) HideSkinMenuToolTip();
        if (!settings_.general.tips_on_open) ClosePlaybackOpenTip();
        ApplyWindowShadow();
        UpdateAutoShutdownTimer();
        UpdateMainWindowCaption();
        ResetSkinInfoScroll();
    }
    if (all || template_id == 252) RegisterConfiguredHotKeys();
    if ((all || template_id == 254) && playlist_window_) {
        UpdatePlaylistWindowSkin();
        RefreshPlaylist();
    }
    if (all || template_id == 255) ApplyMediaLibraryConfiguration();
    if ((all || template_id == 256 || template_id == 384 ||
         template_id == 385 || template_id == 263) && lyric_window_) {
        RebuildLyricFont(false);
        UpdateLyricEditorStyle();
        UpdateLyricScrollTimer();
        ApplyFullScreenLyricTransparency();
        LayoutLyricControls();
        InvalidateRect(lyric_window_, nullptr, FALSE);
    }
    if (all || template_id == 256) {
        // Lyric/AutoLoadLyric and Folders_* are consumed by the next track
        // selection.  A live page apply also re-evaluates the current item,
        // while an explicitly loaded/embedded lyric remains authoritative.
        if (!lyric_path_.empty()) {
            const bool associated = !associated_lyric_path_.empty() &&
                _wcsicmp(lyric_path_.c_str(),
                          associated_lyric_path_.c_str()) == 0;
            LoadLyricsFrom(lyric_path_, associated);
        } else if (lyrics_.lines.empty() &&
                   settings_.lyric.auto_load_lyric) {
            LoadCurrentLyrics();
        } else {
            ApplyAutoLyricVisibility();
        }
    }
    if (all || template_id == 385) desktop_lyrics_.ApplySettings();
    if (all || template_id == 253 || template_id == 263) {
        if (fullscreen_mode_ != 0) UpdateFullScreenLayout();
        UpdateVisualWindowLayout();
        UpdateVisualFrame();
    }
    if (all || template_id == 250 || template_id == 251 ||
        template_id == 260) RefreshPlaybackUi();
}

void PlayerWindow::ApplyOptionsChangeMask(UINT mask, LPARAM source_control) {
    // FUN_0046228D is the central 0x7F0 dispatcher.  Most page notifications
    // already call the narrow runtime adapter synchronously; lifecycle posts
    // (notably 0x100/-1 and 0xFFFF/-2) arrive here after the page callback has
    // unwound, matching the original ordering.
    const bool all = mask == 0xffff;
    const auto playback_state = audio_->State();
    const auto output_restart = PlanOutputRestart(
        mask, playback_source_open_,
        playback_state == audio::PlaybackState::playing,
        playback_state == audio::PlaybackState::paused);
    const auto output_restart_position = audio_->Position();
    if (all || (mask & 0x800U) != 0) {
        const auto plugin_skin = settings_.plugin_skin_file;
        const bool use_default = settings_.skin_file.empty() ||
            _wcsicmp(settings_.skin_file.c_str(), L"<Default_Skin>") == 0;
        if (use_default) {
            // Rebind once using the freshly constructed defaults. Do not
            // replace them with an outgoing capture or a saved skin profile.
            static_cast<void>(LoadSkinResource(ResourceModule(), L"<Default_Skin>", false));
        } else {
            const auto skin_path = skin::ResolveSkinPackagePath(
                CurrentExecutablePath().parent_path() / L"Skin", settings_.skin_file);
            static_cast<void>(LoadSkinPackage(skin_path, false));
        }
        if (!plugin_skin.empty()) {
            settings_.plugin_skin_file = plugin_skin;
            static_cast<void>(LoadPluginSkin(skin::ResolveSkinPackagePath(
                CurrentExecutablePath().parent_path() / L"Skin", plugin_skin), false));
        }
    }

    if (all) {
        transparency_percent_ = std::clamp(
            settings_.player.alpha_percent, 0, 90);
        skin_window_alpha_ = static_cast<BYTE>(
            255 * (100 - transparency_percent_) / 100);
        ApplySkinWindowAlpha(EffectiveSkinWindowAlpha(window_));
        ApplySkinWindowTopMost();
        if (playlist_window_)
            ShowWindow(playlist_window_,
                       settings_.player.playlist_visible ? SW_SHOWNOACTIVATE
                                                         : SW_HIDE);
        if (equalizer_window_)
            ShowWindow(equalizer_window_,
                       settings_.player.equalizer_visible ? SW_SHOWNOACTIVATE
                                                          : SW_HIDE);
        if (lyric_window_) {
            if (desktop_lyric_mode_) {
                ShowWindow(lyric_window_, SW_HIDE);
                desktop_lyrics_.Show(settings_.player.lyric_visible);
            } else {
                ShowWindow(lyric_window_,
                           settings_.player.lyric_visible ? SW_SHOWNOACTIVATE
                                                          : SW_HIDE);
            }
        }
        ApplyOptionsRuntime();
        if (output_restart != OutputRestartState::none &&
            PlayCurrent()) {
            audio_->RestoreAfterOutputRestart(
                output_restart_position,
                output_restart == OutputRestartState::paused);
            settings_.player.playing_time = static_cast<int>(std::clamp<int64_t>(
                output_restart_position.count(), 0, INT_MAX));
            RefreshPlaybackUi();
        }
        return;
    }

    if ((mask & 0x0001U) != 0) ApplyOptionsRuntime(253);
    if ((mask & 0x0002U) != 0) ApplyOptionsRuntime(250);
    if ((mask & 0x0004U) != 0) ApplyOptionsRuntime(251);
    if ((mask & 0x0008U) != 0) ApplyOptionsRuntime(252);
    if ((mask & 0x0010U) != 0) ApplyOptionsRuntime(384);
    if ((mask & 0x0040U) != 0) ApplyOptionsRuntime(254);
    if ((mask & 0x0080U) != 0) ApplyOptionsRuntime(255);
    if ((mask & 0x0100U) != 0) ApplyOptionsRuntime(258);
    if ((mask & 0x0200U) != 0) ApplyOptionsRuntime(259);
    if ((mask & 0x0400U) != 0) ApplyOptionsRuntime(260);
    if ((mask & 0x4000U) != 0) ApplyOptionsRuntime(385);
    if (output_restart != OutputRestartState::none &&
        PlayCurrent()) {
        audio_->RestoreAfterOutputRestart(
            output_restart_position,
            output_restart == OutputRestartState::paused);
        settings_.player.playing_time = static_cast<int>(std::clamp<int64_t>(
            output_restart_position.count(), 0, INT_MAX));
        RefreshPlaybackUi();
    }
    static_cast<void>(source_control);
}

void PlayerWindow::UpdateOptionsDeviceDetails(HWND dialog) {
    const HWND devices = GetDlgItem(dialog, 1059);
    const HWND details = GetDlgItem(dialog, 1064);
    if (!devices || !details) return;
    const auto selected = SelectedOutputDeviceEntry(
        dialog, options_device_entries_.size());
    if (!selected) return;

    ListView_SetExtendedListViewStyle(details,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const HWND header = ListView_GetHeader(details);
    if (!header || Header_GetItemCount(header) == 0) {
        for (int column_index = 0; column_index < 2; ++column_index) {
            LVCOLUMNW column{};
            column.mask = LVCF_WIDTH;
            column.cx = column_index == 0 ? 100 : 200;
            ListView_InsertColumn(details, column_index, &column);
        }
    }
    ListView_DeleteAllItems(details);

    auto& entry = options_device_entries_[*selected];
    if (!entry.details_resolved) {
        // 0049989D probes DirectSound/ASIO only for the selected row.  Keep
        // the call in the isolated helper and remember both success and
        // failure so merely repainting the page cannot repeatedly load a
        // broken legacy driver.
        entry.details_resolved = true;
        LegacyOutputDevice request;
        request.backend = entry.backend;
        request.wave_device_id = entry.wave_device_id;
        request.key = entry.key;
        request.name = entry.name;
        request.module = entry.module;
        request.class_id = entry.identifier;
        request.has_class_id = entry.has_identifier;
        request.details = entry.details;
        auto resolved = RunOutputDeviceProbe(&request);
        if (resolved && resolved->size() == 1 &&
            (*resolved)[0].backend == entry.backend &&
            _wcsicmp((*resolved)[0].key.c_str(), entry.key.c_str()) == 0) {
            entry.details = std::move((*resolved)[0].details);
        }
    }
    const UINT detail_resource = entry.backend == 0 ? 0x812a
        : entry.backend == 1 ? 0x812b
        : entry.backend == 2 ? 0x812c : 0x812d;
    std::array<std::wstring, 5> labels{ResourceText(0x813c)};
    for (size_t index = 0; index < 4; ++index)
        labels[index + 1] = ResourceListItem(
            ResourceModule(), detail_resource, index);
    std::array<std::wstring, 5> values;
    values[0] = entry.backend == 0 ? L"WaveOut"
        : entry.backend == 1 ? L"DirectSound"
        : entry.backend == 2 ? L"Kernel Streaming" : L"ASIO";
    const auto yes_no = [this](bool value) {
        return ResourceText(value ? 6 : 7);
    };

    if (entry.backend == 0) {
        values[1] = entry.details[0];
        for (size_t index = 1; index < entry.details.size(); ++index) {
            if (!entry.details[index].empty())
                values[index + 1] = yes_no(entry.details[index] == L"1");
        }
    } else if (entry.backend == 1) {
        if (!entry.details[0].empty())
            values[1] = yes_no(entry.details[0] == L"1");
        values[2] = entry.details[1];
        values[3] = entry.details[2];
        values[4] = entry.details[3];
    } else {
        values[1] = entry.details[0];
        values[2] = entry.details[1];
        values[3] = entry.details[2];
        values[4] = entry.details[3];
        if (entry.backend == 2 && !values[4].empty())
            values[4] = yes_no(values[4] == L"1");
    }

    const int show_direct_sound = entry.backend == 1 ? SW_SHOW : SW_HIDE;
    ShowWindow(GetDlgItem(dialog, 2033), show_direct_sound);
    ShowWindow(GetDlgItem(dialog, 2034), show_direct_sound);
    for (int row = 0; row < static_cast<int>(labels.size()); ++row) {
        AddListText(details, row, labels[static_cast<size_t>(row)]);
        ListView_SetItemText(details, row, 1,
            values[static_cast<size_t>(row)].data());
        // Once an image list is attached, a zero-initialized LVITEM makes
        // every inserted row inherit image 0.  COptionsDevice draws the
        // backend glyph only beside the first "device type" property; the
        // capability rows intentionally have no image.
        LVITEMW image_item{};
        image_item.mask = LVIF_IMAGE;
        image_item.iItem = row;
        image_item.iImage = -1;
        ListView_SetItem(details, &image_item);
    }
    if (options_device_images_) {
        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = 0;
        item.iImage = entry.backend;
        ListView_SetItem(details, &item);
    }
}

void PlayerWindow::InitializeOptionsSkinTabs(HWND dialog) {
    // Discover validates the ABI and all required entry points. A missing or
    // unusable DLL leaves the original resource layout completely intact.
    if (skin_plugins_.empty() || GetDlgItem(dialog, kOptionsSkinKindTab)) return;
    RECT tab_bounds{};
    GetClientRect(dialog, &tab_bounds);
    const HWND tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
        tab_bounds.left, tab_bounds.top,
        tab_bounds.right - tab_bounds.left, tab_bounds.bottom - tab_bounds.top,
        dialog, reinterpret_cast<HMENU>(kOptionsSkinKindTab),
        GetModuleHandleW(nullptr), nullptr);
    if (!tab) return;
    SendMessageW(tab, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
    TCITEMW native{};native.mask=TCIF_TEXT;native.pszText=const_cast<wchar_t*>(L"\u539f\u751f");
    TabCtrl_InsertItem(tab,0,&native);
    int selected=0;
    for(size_t index=0;index<skin_plugins_.size();++index) {
        TCITEMW item{};item.mask=TCIF_TEXT;
        item.pszText=const_cast<wchar_t*>(skin_plugins_[index]->Name().c_str());
        const int inserted=TabCtrl_InsertItem(tab,static_cast<int>(index+1),&item);
        if(external_skin_ && external_skin_->Provider()==skin_plugins_[index].get()) selected=inserted;
    }
    TabCtrl_SetCurSel(tab,selected);

    // A tab control draws a complete pane, not just its header. Let that pane
    // contain the existing controls instead of drawing an empty shallow box
    // above them. Use its actual font/theme metrics for the header height.
    RECT content = tab_bounds;
    TabCtrl_AdjustRect(tab, FALSE, &content);
    const int offset = content.top - tab_bounds.top;
    // Keep the original heading margin and the metadata/bottom buttons in
    // place; only the list and preview sacrifice vertical space.
    RECT list_bounds{};
    GetWindowRect(GetDlgItem(dialog, 1064), &list_bounds);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&list_bounds), 2);
    for (HWND child = GetWindow(dialog, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == tab) continue;
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bounds), 2);
        if (bounds.top > list_bounds.top) continue;
        const int id = GetDlgCtrlID(child);
        const int shrink = id == 1064 || id == 1068 ? offset : 0;
        SetWindowPos(child, nullptr, bounds.left, bounds.top + offset,
            bounds.right - bounds.left, bounds.bottom - bounds.top - shrink,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // The pane and controls are siblings. Keep the pane behind them so its
    // WS_CLIPSIBLINGS excludes their pixels during selection/focus repaint.
    SetWindowPos(tab, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PlayerWindow::PopulateOptionsSkinPage(HWND dialog) {
    if (!dialog || !IsWindow(dialog)) return;
    const HWND list = GetDlgItem(dialog, 1064);
    if (!list) return;
    std::wstring previous;
    const auto old_row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    const auto old_data = old_row == LB_ERR ? LB_ERR
        : SendMessageW(list, LB_GETITEMDATA, old_row, 0);
    if (old_data >= 0 && static_cast<size_t>(old_data) < options_skin_entries_.size())
        previous = options_skin_entries_[static_cast<size_t>(old_data)].package_name;
    static_cast<void>(PublishReadySkinMenuCatalog(0));
    options_skin_entries_ = skin_catalog_cache_;
    if (options_skin_entries_.empty()) {
        SkinMenuEntry embedded;
        embedded.command = kCmdDefaultSkin;
        embedded.package_name = L"<Default_Skin>";
        embedded.embedded_default = true;
        embedded.metadata.name = ResourceText(0x81a6);
        if (embedded.metadata.name.empty())
            embedded.metadata.name = ResourceText(33190);
        embedded.metadata.author = L"TTplayer";
        embedded.metadata.url = L"http://www.ttplayer.com";
        embedded.metadata.email = L"none";
        options_skin_entries_.push_back(std::move(embedded));
    }
    const HWND tab = GetDlgItem(dialog, kOptionsSkinKindTab);
    const int selection=tab?TabCtrl_GetCurSel(tab):0;
    const auto provider=selection>0 && static_cast<size_t>(selection)<=skin_plugins_.size()
        ? skin_plugins_[static_cast<size_t>(selection-1)] : nullptr;
    std::erase_if(options_skin_entries_,[&](const SkinMenuEntry& entry) {
        return provider ? !entry.external || entry.provider!=provider : entry.external;
    });
    const bool retain_selection = std::any_of(options_skin_entries_.begin(),
        options_skin_entries_.end(), [&](const SkinMenuEntry& entry) {
            return _wcsicmp(entry.package_name.c_str(), previous.c_str()) == 0;
        });

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    int selected = options_skin_entries_.empty() ? -1 : 0;
    for (size_t index = 0; index < options_skin_entries_.size(); ++index) {
        auto& entry = options_skin_entries_[index];
        auto label = entry.embedded_default
            ? ResourceText(0x81a6) : entry.metadata.name;
        if (label.empty()) label = entry.package_name;
        const LRESULT row = SendMessageW(list, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (row != LB_ERR && row != LB_ERRSPACE)
            SendMessageW(list, LB_SETITEMDATA, row, index);
        const auto& selector = entry.external ? settings_.plugin_skin_file : settings_.skin_file;
        const bool active = entry.embedded_default
            ? settings_.skin_file.empty() ||
              _wcsicmp(settings_.skin_file.c_str(), L"<Default_Skin>") == 0
            : _wcsicmp(selector.c_str(),
                       entry.package_name.c_str()) == 0;
        const bool select = retain_selection
            ? _wcsicmp(previous.c_str(), entry.package_name.c_str()) == 0 : active;
        if (select && row >= 0) selected = static_cast<int>(row);
    }
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    UpdateOptionsSkinDetails(dialog);
}

void PlayerWindow::UpdateOptionsSkinDetails(HWND dialog) {
    const HWND list = GetDlgItem(dialog, 1064);
    const LRESULT row = list ? SendMessageW(list, LB_GETCURSEL, 0, 0) : LB_ERR;
    const LRESULT data = row == LB_ERR ? LB_ERR
        : SendMessageW(list, LB_GETITEMDATA, row, 0);
    const SkinMenuEntry* entry = data == LB_ERR || data < 0 ||
        static_cast<size_t>(data) >= options_skin_entries_.size()
        ? nullptr : &options_skin_entries_[static_cast<size_t>(data)];
    const auto author = entry ? entry->metadata.author : std::wstring{};
    const auto url = entry ? entry->metadata.url : std::wstring{};
    const auto email = entry ? entry->metadata.email : std::wstring{};
    SetDlgItemTextW(dialog, 1005, author.c_str());
    SetDlgItemTextW(dialog, 1066, url.c_str());
    SetDlgItemTextW(dialog, 1067, email.c_str());
    SetDlgItemTextW(dialog, 2001,
        entry ? entry->package_name.c_str() : L"");
    EnableWindow(GetDlgItem(dialog, 1039), entry && !entry->embedded_default);
    EnableWindow(GetDlgItem(dialog, 1098), entry != nullptr);

    if (options_skin_preview_) DeleteObject(options_skin_preview_);
    options_skin_preview_ = nullptr;
    // RenderLegacySkinPreview has already replaced the package colour key
    // with COLOR_WINDOW, just as FUN_0049A6EF/FUN_00445709 do.  The cached
    // bitmap must therefore be copied normally, not keyed a second time.
    options_skin_preview_transparent_ = CLR_INVALID;
    if (entry) {
        try {
            if (entry->external) {
                if(entry->provider) {
                    auto preview=skin::SkinPluginInstance::Create(entry->provider,entry->path,nullptr);
                    if(preview) options_skin_preview_=preview->Preview();
                }
                InvalidateRect(GetDlgItem(dialog, 1068), nullptr, TRUE);
                return;
            }
            // FUN_0049A372 owns an independent CSkinManager for every list
            // item.  FUN_0049B010 lazily loads that object and FUN_0049A9A3
            // renders a temporary hidden player; even the active package does
            // not borrow the live player's GDI objects.  Keep the same
            // lifetime boundary so page painting cannot race a live skin DC.
            auto package = entry->embedded_default
                ? skin::SkinPackage::OpenResource(
                    ResourceModule(), L"<Default_Skin>", L"ZIP")
                : skin::SkinPackage::Open(entry->path);
            const auto cache = std::filesystem::temp_directory_path() /
                L"TTPlayerRebuild" /
                (L"OptionsPreview-" +
                 std::to_wstring(package.Fingerprint()));
            package.ExtractTo(cache, ttpcomm_module_);
            auto preview = skin::LegacySkin::Load(cache);
            if (preview.Valid()) {
                options_skin_preview_ = RenderSkinPreview(preview,
                    ResourceModule(), window_icon_small_);
            }
        } catch (const std::exception&) {
            options_skin_preview_ = nullptr;
        }
    }
    InvalidateRect(GetDlgItem(dialog, 1068), nullptr, TRUE);
}

INT_PTR PlayerWindow::HandleOptionsPageDialog(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    const UINT template_id = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(
        GetPropW(dialog, kPageTemplateProperty)));
    const auto color_target = [this, template_id](UINT control) -> COLORREF* {
        if (template_id == 254) {
            switch (control) {
            case 1155: return &settings_.playlist.text_color;
            case 1156: return &settings_.playlist.highlight_color;
            case 1159: return &settings_.playlist.number_color;
            case 1160: return &settings_.playlist.duration_color;
            case 1158: return &settings_.playlist.background_color;
            case 1162: return &settings_.playlist.alternate_background_color;
            case 1161: return &settings_.playlist.selected_color;
            default: break;
            }
        } else if (template_id == 384) {
            switch (control) {
            case 1155: return &settings_.lyric.text_color;
            case 1156: return &settings_.lyric.highlight_color;
            case 1158: return &settings_.lyric.background_color;
            default: break;
            }
        } else if (template_id == 385) {
            switch (control) {
            case 1155:
                return &settings_.desktop_lyric.current.background_colors[0];
            case 1156:
                return &settings_.desktop_lyric.current.played_colors[0];
            case 1038: return &settings_.desktop_lyric.border_color;
            case 1158: return &settings_.desktop_lyric.background_color;
            default: break;
            }
        } else if (template_id == 263) {
            switch (control) {
            case 1155: return &settings_.lyric.fullscreen_text_color;
            case 1156: return &settings_.lyric.fullscreen_highlight_color;
            case 1158: return &settings_.lyric.fullscreen_background_color;
            default: break;
            }
        }
        return nullptr;
    };
    const auto switch_nested = [this, dialog, template_id](UINT requested) {
        if (template_id == 256) {
            const HWND tab = GetDlgItem(dialog, 2256);
            UINT child_id = requested;
            if (child_id != 384 && child_id != 385)
                child_id = TabCtrl_GetCurSel(tab) == 1 ? 385 : 384;
            SelectOptionsLyricMode(dialog, child_id);
        } else if (template_id == 258) {
            const HWND tab = GetDlgItem(dialog, 1181);
            UINT child_id = requested;
            if (child_id != 381 && child_id != 382)
                child_id = TabCtrl_GetCurSel(tab) == 1 ? 382 : 381;
            SelectOptionsNetworkMode(dialog, child_id);
        }
    };

    switch (message) {
    case WM_INITDIALOG:
        InitializeOptionsPage(dialog, template_id);
        return TRUE;
    case kApplyOptionsAssociations:
        if (template_id == 262 && options_association_commit_pending_)
            CommitOptionsAssociations(dialog,true);
        return TRUE;
    case kExportSkinPreview:
        if (template_id == 261 && options_skin_preview_) {
            const HWND list = GetDlgItem(dialog, 1064);
            const LRESULT row = list
                ? SendMessageW(list, LB_GETCURSEL, 0, 0) : LB_ERR;
            const LRESULT data = row == LB_ERR ? LB_ERR
                : SendMessageW(list, LB_GETITEMDATA, row, 0);
            const SkinMenuEntry* entry = data == LB_ERR || data < 0 ||
                static_cast<size_t>(data) >= options_skin_entries_.size()
                ? nullptr
                : &options_skin_entries_[static_cast<size_t>(data)];
            if (!entry) return TRUE;
            std::filesystem::path suggested = entry->embedded_default
                ? CurrentExecutablePath().parent_path() / L"Skin" /
                      L"Default_Skin.bmp"
                : entry->path;
            suggested.replace_extension(L".bmp");
            const wchar_t filter[] =
                L"BMP Files(*.bmp)\0*.bmp\0All files (*.*)\0*.*\0\0";
            ModernSaveFileOptions save;
            save.owner = dialog;
            save.filters = ParseLegacyDialogFilter(filter);
            save.initial_path = std::move(suggested);
            save.default_extension = L"bmp";
            if (const auto selected = ModernSaveFile(save))
                SaveBitmapFile(options_skin_preview_, *selected);
        }
        return TRUE;
    case WM_TIMER:
        if (template_id == 259 && wparam == kOptionsDspPollTimer) {
            PollOptionsDspScan(dialog);
            return TRUE;
        }
        break;
    case kSwitchNestedOptionsPage:
        switch_nested(static_cast<UINT>(wparam));
        return TRUE;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item && template_id == 261 && item->CtlID == 1068) {
            FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_WINDOW));
            if (options_skin_preview_) {
                BITMAP bitmap{};
                if (GetObjectW(options_skin_preview_, sizeof(bitmap), &bitmap) &&
                    bitmap.bmWidth > 0 && bitmap.bmHeight > 0) {
                    RECT content = item->rcItem;
                    // FUN_0049B010 reserves one client pixel on every side;
                    // the SysListView32 keeps drawing its own non-client edge.
                    InflateRect(&content, -1, -1);
                    const int available_width = std::max(
                        1L, content.right - content.left);
                    const int available_height = std::max(
                        1L, content.bottom - content.top);
                    // FUN_0049A9A3 only enters its resize branch when the
                    // fitted ratio is below 1.0.  Small skins remain at their
                    // native dimensions; the preview never enlarges them.
                    const double scale = std::min(1.0, std::min(
                        static_cast<double>(available_width) / bitmap.bmWidth,
                        static_cast<double>(available_height) / bitmap.bmHeight));
                    const int width = std::max(1, static_cast<int>(
                        bitmap.bmWidth * scale));
                    const int height = std::max(1, static_cast<int>(
                        bitmap.bmHeight * scale));
                    const int left = content.left +
                        (content.right - content.left - width) / 2;
                    const int top = content.top +
                        (content.bottom - content.top - height) / 2;
                    const HDC source = CreateCompatibleDC(item->hDC);
                    const HGDIOBJ old = SelectObject(source,
                                                     options_skin_preview_);
                    SetStretchBltMode(item->hDC, HALFTONE);
                    SetBrushOrgEx(item->hDC, 0, 0, nullptr);
                    if (options_skin_preview_transparent_ != CLR_INVALID) {
                        TransparentBlt(item->hDC, left, top, width, height,
                            source, 0, 0, bitmap.bmWidth, bitmap.bmHeight,
                            options_skin_preview_transparent_);
                    } else {
                        StretchBlt(item->hDC, left, top, width, height, source,
                            0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
                    }
                    SelectObject(source, old);
                    DeleteDC(source);
                }
            }
            return TRUE;
        }
        if (item && template_id == 385 && item->CtlID == 1155) {
            DrawGradientButton(*item,
                settings_.desktop_lyric.current.background_count,
                settings_.desktop_lyric.current.background_colors);
            return TRUE;
        }
        if (item && template_id == 385 && item->CtlID == 1156) {
            DrawGradientButton(*item,
                settings_.desktop_lyric.current.played_count,
                settings_.desktop_lyric.current.played_colors);
            return TRUE;
        }
        COLORREF* color = item ? color_target(item->CtlID) : nullptr;
        if (!item || !color) break;
        if (template_id == 385)
            DrawGradientButton(*item, 1, {*color, *color, *color});
        else DrawColorButton(*item, *color);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const HWND control = reinterpret_cast<HWND>(lparam);
        const UINT identifier = control ? GetDlgCtrlID(control) : 0;
        if (IsOptionsPageHyperlink(template_id, identifier)) {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0, 0, 255));
            return reinterpret_cast<INT_PTR>(
                GetSysColorBrush(COLOR_3DFACE));
        }
        break;
    }
    case WM_SETCURSOR: {
        const HWND control = reinterpret_cast<HWND>(wparam);
        const UINT identifier = control ? GetDlgCtrlID(control) : 0;
        if (LOWORD(lparam) == HTCLIENT && IsWindowEnabled(control) &&
            GetParent(control) == dialog && IsOptionsPageHyperlink(template_id, identifier)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            // DLGPROC's TRUE means "handled", not the cursor result. The
            // parent must return TRUE to the Static's default procedure or
            // it immediately overwrites our hand with its class arrow.
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    }
    case WM_HSCROLL: {
        const HWND source = reinterpret_cast<HWND>(lparam);
        const UINT control = source ? static_cast<UINT>(GetDlgCtrlID(source)) : 0;
        if (template_id == 263 && control == IDC_FULLSCREEN_ALBUM_TRANSPARENCY) {
            settings_.fullscreen.album_transparency_percent = std::clamp(
                static_cast<int>(SendMessageW(source, TBM_GETPOS, 0, 0)), 0, 100);
            SetDlgItemTextW(dialog, IDC_FULLSCREEN_ALBUM_PERCENT,
                (std::to_wstring(settings_.fullscreen.album_transparency_percent) + L"%").c_str());
            ApplyOptionsPageRuntime(template_id);
            return TRUE;
        }
        if (!control || !CommitOptionsControl(dialog, template_id, control))
            CommitOptionsPage(dialog, template_id);
        options_deferred_apply_mask_ &= ~DeferredOptionsBit(template_id);
        ApplyOptionsPageRuntime(template_id);
        return TRUE;
    }
    case WM_COMMAND: {
        const UINT control = LOWORD(wparam);
        const UINT notification = HIWORD(wparam);
        if (template_id == 256 && notification == STN_CLICKED) {
            if (control == 2087) {
                HandleLyricCommand(kCmdLyricAssociate);
                return TRUE;
            }
            if (control == 2089 || control == 2091) {
                SelectOptionsPage(kPageLyricSearch,
                                  control == 2089 ? 1038 : 2090);
                return TRUE;
            }
        }
        if (template_id == 250) {
            if (control == 2188)
                EnableWindow(GetDlgItem(dialog, kOptionsDiscordLyrics),
                             IsChecked(dialog, 2188));
            else if (control == 1076) {
                EnableWindow(GetDlgItem(dialog, 1077), IsChecked(dialog, 1076));
                EnableWindow(GetDlgItem(dialog, 1078), IsChecked(dialog, 1076));
            } else if (control == 1083) {
                EnableWindow(GetDlgItem(dialog, 1084), IsChecked(dialog, 1083));
                EnableWindow(GetDlgItem(dialog, 1044), IsChecked(dialog, 1083));
            } else if (control == 2137)
                EnableWindow(GetDlgItem(dialog, 2138), IsChecked(dialog, 2137));
            else if (control == 2182)
                EnableWindow(GetDlgItem(dialog, 2183), IsChecked(dialog, 2182));
            else if (control == 2197 && notification == BN_CLICKED) {
                SendMessageW(window_, WM_TIMER, 13, 0);
                return TRUE;
            }
        }
        if (template_id == 251) {
            if (control == 1070)
                EnableWindow(GetDlgItem(dialog, 2092), IsChecked(dialog, 1070));
            else if (control >= 2070 && control <= 2074) {
                const int offset = static_cast<int>(control - 2070);
                const bool enabled = IsChecked(dialog, static_cast<int>(control));
                EnableWindow(GetDlgItem(dialog, 2075 + offset), enabled);
                EnableWindow(GetDlgItem(dialog, 2080 + offset), enabled);
            }
        }
        if (template_id == 252) {
            if (control == 2053 && notification == BN_CLICKED) {
                settings_.hotkey.global = IsChecked(dialog, 2053);
                return TRUE;
            }
            if (control == 1098 && notification == BN_CLICKED) {
                const int row = options_hotkey_selection_;
                if (row >= 0 && static_cast<size_t>(row) <
                        settings_.hotkey.key_map.size()) {
                    auto& binding = settings_.hotkey.key_map[
                        static_cast<size_t>(row)];
                    binding.application = GetHotKeyControl(dialog, 2054);
                    binding.global = GetHotKeyControl(dialog, 2055);
                    binding.raw_text.clear();
                    UpdateHotKeyListRow(GetDlgItem(dialog, 1064), row, binding);
                }
                return TRUE;
            }
        }
        if (template_id == 254 && control == 1099)
            EnableWindow(GetDlgItem(dialog, 1097), IsChecked(dialog, 1099));
        if ((template_id == 254 || template_id == 384) && control == 2154 &&
            notification == BN_CLICKED) {
            const UINT command = TrackProfileTransferMenu(
                dialog, ResourceModule(), 2154);
            if (command == 0x7d14) {
                const bool playlist = template_id == 254;
                auto& history = playlist ? settings_.history.playlist_profile
                                         : settings_.history.lyric_profile;
                const auto selected = ChooseOptionsProfileFile(
                    dialog, true, playlist ? L"ttpl_cfg" : L"ttlr_cfg",
                    history, playlist
                        ? i18n::Literal(L"Playlist Profile (*.ttpl_cfg)")
                        : i18n::Literal(L"Lyrics Profile (*.ttlr_cfg)"));
                if (selected) {
                    // FUN_0042918D passes the history string as lpstrFile, so
                    // a chosen path is retained even when profile parsing
                    // subsequently fails.
                    history = *selected;
                    const bool loaded = playlist
                        ? settings::LoadPlaylistOptionsProfile(
                              *selected, settings_.playlist)
                        : settings::LoadLyricOptionsProfile(
                              *selected, settings_.lyric);
                    if (loaded) {
                        if (playlist) {
                            for (const int color : {
                                     1155,1156,1159,1160,1158,1162,1161}) {
                                InvalidateRect(GetDlgItem(dialog, color),
                                               nullptr, TRUE);
                            }
                        } else {
                            for (const int color : {1155,1156,1158}) {
                                InvalidateRect(GetDlgItem(dialog, color),
                                               nullptr, TRUE);
                            }
                        }
                        ApplyOptionsPageRuntime(template_id);
                    }
                }
            } else if (command == 0x7d15) {
                CommitOptionsPage(dialog, template_id);
                const bool playlist = template_id == 254;
                auto& history = playlist ? settings_.history.playlist_profile
                                         : settings_.history.lyric_profile;
                const auto selected = ChooseOptionsProfileFile(
                    dialog, false, playlist ? L"ttpl_cfg" : L"ttlr_cfg",
                    history, playlist
                        ? i18n::Literal(L"Playlist Profile (*.ttpl_cfg)")
                        : i18n::Literal(L"Lyrics Profile (*.ttlr_cfg)"));
                if (selected) {
                    history = *selected;
                    if (playlist)
                        settings::SavePlaylistOptionsProfile(
                            *selected, settings_.playlist);
                    else
                        settings::SaveLyricOptionsProfile(
                            *selected, settings_.lyric);
                }
            }
            return TRUE;
        }
        if (template_id == 385 && control == 2154 &&
            notification == BN_CLICKED) {
            auto& desktop = settings_.desktop_lyric;
            const UINT command = TrackDesktopProfileMenu(
                dialog, ResourceModule(), 2154, desktop.profiles,
                desktop.profile);
            if (command >= 0x80a2 && command <= 0x80a4) {
                const size_t index = static_cast<size_t>(command - 0x80a2);
                desktop.profile = static_cast<int>(index);
                desktop.current = desktop.profiles[index];
                desktop.current.name.clear();
                InvalidateRect(GetDlgItem(dialog, 1155), nullptr, TRUE);
                InvalidateRect(GetDlgItem(dialog, 1156), nullptr, TRUE);
                ApplyOptionsPageRuntime(template_id);
            } else if (command >= 0x80ac && command <= 0x80ae) {
                const size_t index = static_cast<size_t>(command - 0x80ac);
                settings::DesktopLyricSettings original_defaults;
                auto defaults = original_defaults.profiles[index];
                defaults.name = ResourceText(
                    0x80a2 + static_cast<UINT>(index));
                if (EditDesktopProfile(dialog, ResourceModule(),
                                       desktop.profiles[index], defaults)) {
                    if (desktop.profile == static_cast<int>(index)) {
                        desktop.current = desktop.profiles[index];
                        desktop.current.name.clear();
                        ApplyOptionsPageRuntime(template_id);
                    }
                    InvalidateRect(GetDlgItem(dialog, 1155), nullptr, TRUE);
                    InvalidateRect(GetDlgItem(dialog, 1156), nullptr, TRUE);
                }
            }
            return TRUE;
        }
        if (template_id == 255) {
            const bool monitoring = IsChecked(dialog, 2210);
            if (control == 2212 || control == 2210) {
                EnableWindow(GetDlgItem(dialog, 1038), monitoring);
                EnableWindow(GetDlgItem(dialog, 1027), monitoring);
                UpdateFolderListButtons(dialog, ResourceModule(), false);
                if (!monitoring) EnableWindow(GetDlgItem(dialog, 1039), FALSE);
            }
            const HWND list = GetDlgItem(dialog, 1038);
            if (control == 1027 && notification == BN_CLICKED) {
                if (const auto folder = BrowseForFolder(dialog)) {
                    const int row = ListView_GetItemCount(list);
                    AddListText(list, row, folder->wstring());
                    ListView_SetCheckState(list, row, TRUE);
                    ListView_SetItemState(list, row,
                        LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
                }
                UpdateFolderListButtons(dialog, ResourceModule(), false);
                return TRUE;
            }
            if (control == 1039 && notification == BN_CLICKED) {
                const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                if (row >= 0) ListView_DeleteItem(list, row);
                UpdateFolderListButtons(dialog, ResourceModule(), false);
                return TRUE;
            }
        }
        if (template_id == 258 && control >= 2190 && control <= 2192 &&
            notification == BN_CLICKED) {
            const bool custom = IsChecked(dialog, 2192);
            for (const int target : {2143,2144,2184,2185})
                EnableWindow(GetDlgItem(dialog, target), custom);
        }
        if (template_id == 381 && control == 2250 &&
            notification == BN_CLICKED) {
            const bool enabled = IsChecked(dialog, 2250);
            for (const int target : {2252,2253,2251,1023})
                EnableWindow(GetDlgItem(dialog, target), enabled);
        }
        if (template_id == 385 && notification == BN_CLICKED) {
            // FUN_00496534/FUN_004965D3 toggle the independent colour
            // buttons immediately; 1038 and 1158 are pushbuttons, while
            // 2260 and 2261 are their enabling check boxes.
            if (control == 2260)
                EnableWindow(GetDlgItem(dialog, 1038),
                             IsChecked(dialog, 2260));
            else if (control == 2261)
                EnableWindow(GetDlgItem(dialog, 1158),
                             IsChecked(dialog, 2261));
            else if (control == 1155 || control == 1156) {
                auto& current = settings_.desktop_lyric.current;
                int& count = control == 1155 ? current.background_count
                                             : current.played_count;
                auto& colors = control == 1155 ? current.background_colors
                                                : current.played_colors;
                if (EditGradientProfile(dialog, ResourceModule(), count,
                                        colors)) {
                    settings_.desktop_lyric.profile = MatchDesktopProfile(
                        current, settings_.desktop_lyric.profiles);
                    InvalidateRect(GetDlgItem(dialog,
                                             static_cast<int>(control)),
                                   nullptr, TRUE);
                    ApplyOptionsPageRuntime(template_id);
                }
                return TRUE;
            }
        }
        if (template_id == 257) {
            const HWND list = GetDlgItem(dialog, 1038);
            if (control == 1027 && notification == BN_CLICKED) {
                if (const auto folder = BrowseForCheckedFolder(
                        dialog, ResourceText(0x8152),
                        ResourceText(0x8153))) {
                    const std::wstring path = folder->path.wstring();
                    bool duplicate = false;
                    for (int row = 0; row < ListView_GetItemCount(list);
                         ++row) {
                        std::array<wchar_t, 32768> existing{};
                        ListView_GetItemText(list, row, 0, existing.data(),
                                             static_cast<int>(existing.size()));
                        if (_wcsicmp(existing.data(), path.c_str()) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        const int selected = ListView_GetNextItem(
                            list, -1, LVNI_SELECTED);
                        const int row = selected >= 0
                            ? selected : ListView_GetItemCount(list);
                        AddListText(list, row, path);
                        ListView_SetCheckState(list, row, folder->checked);
                        ListView_SetItemState(list, row,
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
                    }
                }
                UpdateFolderListButtons(dialog, ResourceModule(), true);
                return TRUE;
            }
            if (control == 1039 && notification == BN_CLICKED) {
                const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                if (row >= 0 &&
                    !IsProtectedLyricFolder(list, row, ResourceModule()))
                    ListView_DeleteItem(list, row);
                UpdateFolderListButtons(dialog, ResourceModule(), true);
                return TRUE;
            }
            if ((control == 1042 || control == 1045) &&
                notification == BN_CLICKED) {
                const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                const int target = row + (control == 1042 ? -1 : 1);
                const int count = ListView_GetItemCount(list);
                if (row >= 0 && target >= 0 && target < count) {
                    std::array<wchar_t, 32768> first{};
                    std::array<wchar_t, 32768> second{};
                    ListView_GetItemText(list, row, 0, first.data(),
                                         static_cast<int>(first.size()));
                    ListView_GetItemText(list, target, 0, second.data(),
                                         static_cast<int>(second.size()));
                    const bool first_checked = ListView_GetCheckState(list, row);
                    const bool second_checked = ListView_GetCheckState(list, target);
                    ListView_SetItemText(list, row, 0, second.data());
                    ListView_SetItemText(list, target, 0, first.data());
                    ListView_SetCheckState(list, row, second_checked);
                    ListView_SetCheckState(list, target, first_checked);
                    ListView_SetItemState(list, target, LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                }
                UpdateFolderListButtons(dialog, ResourceModule(), true);
                return TRUE;
            }
            if (control == 1023 && notification == BN_CLICKED) {
                if (const auto folder = BrowseForFolder(dialog))
                    SetDlgItemTextW(dialog, 1028, folder->c_str());
                CommitOptionsPage(dialog, template_id);
                return TRUE;
            }
            if (control == 2185 && notification == BN_CLICKED) {
                ShowLyricServiceEditor(options_window_);
                return TRUE;
            }
        }
        if (template_id == 259) {
            const HWND list = GetDlgItem(dialog, 1064);
            if (control == 1023 && notification == BN_CLICKED) {
                CommitOptionsPage(dialog, template_id);
                if (const auto folder = BrowseForFolder(dialog)) {
                    settings_.plugin.folder = *folder;
                    ListView_DeleteAllItems(list);
                    while (ListView_DeleteColumn(list, 0)) {}
                    RemovePropW(dialog, kPageReadyProperty);
                    InitializeOptionsPage(dialog, template_id);
                }
                return TRUE;
            }
            if ((control == 1042 || control == 1045) &&
                notification == BN_CLICKED) {
                const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                const int target = row + (control == 1042 ? -1 : 1);
                if (row >= 0 && target >= 0 &&
                    target < ListView_GetItemCount(list)) {
                    SwapListRows(list, row, target, 3);
                    CommitOptionsPage(dialog, template_id);
                    ApplyOptionsPageRuntime(template_id);
                }
                UpdateDspButtons(dialog);
                return TRUE;
            }
            if (control == 1015 && notification == BN_CLICKED) {
                const int row = ListView_GetNextItem(list, -1,
                                                     LVNI_SELECTED);
                if (row >= 0 && ListView_GetCheckState(list, row)) {
                    const LPARAM data = GetListItemData(list, row);
                    if (data >= 0 && static_cast<size_t>(data) <
                            options_dsp_paths_.size()) {
                        // Configuration is isolated too: the DSP ABI has no
                        // cancellation contract and many plug-ins implement a
                        // modal Config callback.  The player never waits for
                        // that third-party callback on its UI thread.
                        LaunchOptionsDspConfiguration(dialog,
                            options_dsp_paths_[static_cast<size_t>(data)]);
                    }
                }
                UpdateDspButtons(dialog);
                return TRUE;
            }
        }
        if ((template_id == 381 || template_id == 382) && control == 1023 &&
            notification == BN_CLICKED) {
            if (const auto folder = BrowseForFolder(dialog)) {
                const auto value = WithLegacyDirectoryTerminator(*folder);
                SetDlgItemTextW(dialog, template_id == 381 ? 2251 : 1179,
                                value.c_str());
            }
            return TRUE;
        }
        if (template_id == 260) {
            if (control == 1059 && notification == CBN_SELCHANGE)
                UpdateOptionsDeviceDetails(dialog);
            else if (control == 2036) {
                const bool enabled = IsChecked(dialog, 2036);
                EnableWindow(GetDlgItem(dialog, 2042), enabled);
                EnableWindow(GetDlgItem(dialog, 2186), enabled);
            }
            else if (control == 2037)
                EnableWindow(GetDlgItem(dialog, 2043), IsChecked(dialog, 2037));
        }
        if (template_id == 262) {
            if (control == IDC_ASSOCIATION_SUPPRESS && notification == BN_CLICKED) {
                settings_.player.suppress_association_reminder = IsChecked(dialog, IDC_ASSOCIATION_SUPPRESS);
                return TRUE;
            }
            if (control == IDC_ASSOCIATION_REFRESH && notification == BN_CLICKED) {
                RefreshOptionsAssociations(dialog);
                RefreshOptionsAssociationIcon(dialog);
                return TRUE;
            }
            const auto executable = CurrentExecutablePath();
            settings::FileAssociationBackend backend(
                executable, ResourceText(0x80));
            const settings::ShellVerbLabels labels{
                ResourceText(0x81a8), ResourceText(0x81a9)};
            if ((control == 1069 || control == 2120) &&
                notification == BN_CLICKED) {
                const auto target = control == 1069
                    ? settings::ShellIntegrationTarget::audio_cd
                    : settings::ShellIntegrationTarget::directory;
                const bool enabled = IsChecked(dialog, static_cast<int>(control));
                const auto result = backend.SetShellIntegration(
                    target, enabled, labels);
                if (!result) {
                    SetChecked(dialog, static_cast<int>(control), !enabled);
                    MessageBoxW(dialog, result.message.c_str(),
                                ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
                }
                return TRUE;
            }
            if (control == 2121 && notification == BN_CLICKED) {
                settings_.player.check_association = IsChecked(dialog, 2121);
                return TRUE;
            }
            if (control >= 2030 && control <= 2032 &&
                notification == BN_CLICKED) {
                settings::ShortcutOptions shortcut;
                shortcut.display_name = ResourceText(0x80);
                shortcut.description = ResourceText(0x80f2);
                shortcut.working_directory = executable.parent_path();
                shortcut.icon_path = executable;
                const auto location = control == 2030
                    ? settings::ShortcutLocation::desktop
                    : control == 2031
                        ? settings::ShortcutLocation::programs
                        : settings::ShortcutLocation::quick_launch;
                const auto result = backend.CreateShortcut(location, shortcut);
                if (!result)
                    MessageBoxW(dialog, result.message.c_str(),
                                ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (control == 2216 && notification == BN_CLICKED) {
                if (const auto folder = BrowseForFolder(dialog)) {
                    for (auto& node : options_association_nodes_) {
                        const auto candidate = *folder /
                            (node->extension + L".ico");
                        std::error_code error;
                        if (std::filesystem::is_regular_file(candidate, error) &&
                            node->icon != candidate.wstring()) {
                            node->icon = candidate.wstring();
                            node->icon_dirty = true;
                        }
                    }
                    RefreshOptionsAssociationIcon(dialog);
                }
                return TRUE;
            }
            if (control == 2106 && notification == BN_CLICKED) {
                HMENU menu = i18n::LoadMenu(ResourceModule(), MAKEINTRESOURCEW(154));
                const HMENU popup = menu ? GetSubMenu(menu, 0) : nullptr;
                UINT command{};
                if (popup) {
                    CheckMenuRadioItem(
                        popup, 33000, 33001,
                        settings_.general.app_icon_file.empty() ? 33000 : 33001,
                        MF_BYCOMMAND);
                    RECT bounds{};
                    GetWindowRect(GetDlgItem(dialog, 2106), &bounds);
                    command = TrackPlayerPopupMenu(
                        popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                        bounds.left, bounds.bottom, 0, dialog, nullptr);
                }
                if (menu) DestroyMenu(menu);

                bool changed = false;
                if (command == 33000) {
                    changed = !settings_.general.app_icon_file.empty();
                    settings_.general.app_icon_file.clear();
                } else if (command == 33001) {
                    if (const auto selected = ChooseIconSelection(
                            dialog, ResourceModule(),
                            settings_.general.app_icon_file.wstring())) {
                        changed = settings_.general.app_icon_file.wstring() !=
                                  *selected;
                        settings_.general.app_icon_file = *selected;
                    }
                }
                if (changed) {
                    ReloadApplicationIcons();
                    const bool custom =
                        !settings_.general.app_icon_file.empty();
                    const HICON small_icon = !custom && skin_ && skin_->Icon()
                        ? skin_->Icon() : window_icon_small_;
                    const HICON large_icon = !custom && skin_ && skin_->Icon()
                        ? skin_->Icon() : window_icon_big_;
                    if (window_) {
                        SendMessageW(window_, WM_SETICON, ICON_SMALL,
                                     reinterpret_cast<LPARAM>(small_icon));
                        SendMessageW(window_, WM_SETICON, ICON_BIG,
                                     reinterpret_cast<LPARAM>(large_icon));
                    }
                    UpdateTrayIcon();
                    BUTTON_IMAGELIST empty{};
                    Button_SetImageList(GetDlgItem(dialog, 2106), &empty);
                    if (options_association_button_images_[3])
                        ImageList_Destroy(
                            options_association_button_images_[3]);
                    options_association_button_images_[3] =
                        ButtonIconImageList(window_icon_big_, 32, 32);
                    AttachButtonImage(GetDlgItem(dialog, 2106),
                        options_association_button_images_[3]);
                }
                return TRUE;
            }
            if (control == 2108 && notification == BN_CLICKED) {
                if (const auto icon = ChooseIconSelection(
                        dialog, ResourceModule(), OptionsAssociationIcon(dialog)))
                    SetOptionsAssociationIcon(dialog, *icon);
                return TRUE;
            }
            if (control == 2281 && notification == BN_CLICKED) {
                // 0049DFB4 used ttpsvr.exe / FileTypeAsso. Register a real
                // Default Programs candidate before handing user choice to
                // the version-appropriate Windows UI, without the old EXE.
                const auto formats = settings::BuildAssociableExtensions(
                    BuildAssociationFormatSource(ResourceModule(), reader_formats_));
                auto result = backend.RegisterApplication(formats);
                if (result && !CommitOptionsAssociations(dialog, true, false)) return TRUE;
                if (result) result = backend.OpenDefaultPrograms(dialog);
                if (!result) {
                    const auto error_text = result.operation + L"\n" + result.message;
                    MessageBoxW(dialog, error_text.c_str(), ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
                }
                RefreshOptionsAssociations(dialog);
                return TRUE;
            }
        }
        if (template_id == 384 && control == 2151 &&
            notification == BN_CLICKED) {
            EnableWindow(GetDlgItem(dialog, 2152),
                         IsChecked(dialog, 2151));
        }
        if (template_id == 263 &&
            GetPropW(dialog, kPageReadyProperty)) {
            if (control == IDC_FULLSCREEN_LYRIC_DRAG && notification == BN_CLICKED) {
                settings_.lyric.fullscreen_drag_lyric = IsChecked(dialog, IDC_FULLSCREEN_LYRIC_DRAG);
                if (fullscreen_lyric_detached_ && lyric_control_ && lyric_line_dragging_ &&
                    !settings_.lyric.fullscreen_drag_lyric)
                    HandleLyricControlMessage(lyric_control_, WM_CANCELMODE, 0, 0);
                UpdateFullScreenLyricInput();
                return TRUE;
            }
            if ((control == IDC_FULLSCREEN_ALBUM_BROWSE ||
                 control == IDC_FULLSCREEN_ALBUM_CLEAR) && notification == BN_CLICKED) {
                if (control == IDC_FULLSCREEN_ALBUM_BROWSE) {
                    ModernOpenFileOptions options;
                    options.owner = dialog;
                    options.title = AlbumOptionText(IDS_FULLSCREEN_ALBUM_PICKER);
                    options.filters = {{AlbumOptionText(IDS_FULLSCREEN_ALBUM_FILTER),
                                        L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico"}};
                    options.initial_path = settings_.fullscreen.album_fallback_image;
                    const auto selected = ModernOpenFile(options);
                    if (!selected) return TRUE;
                    settings_.fullscreen.album_fallback_image = selected->wstring();
                } else {
                    settings_.fullscreen.album_fallback_image.clear();
                }
                SetDlgItemTextW(dialog, IDC_FULLSCREEN_ALBUM_PATH,
                               settings_.fullscreen.album_fallback_image.c_str());
                ApplyOptionsPageRuntime(template_id);
                return TRUE;
            }
            const auto selection = [dialog](UINT id) {
                return SendDlgItemMessageW(dialog, static_cast<int>(id),
                                           CB_GETCURSEL, 0, 0);
            };
            if (control == 2233 && notification == BN_CLICKED) {
                const bool unified = IsChecked(dialog, 2233);
                EnableWindow(GetDlgItem(dialog, 2232), !unified);
                settings_.fullscreen.visual_type = unified ? 0 : 1;
                if (!unified)
                    SendDlgItemMessageW(dialog, 2232, CB_SETCURSEL, 0, 0);
                SelectFullscreenProfileControls(
                    dialog, settings_.fullscreen,
                    settings_.fullscreen.visual_type);
                return TRUE;
            }
            if (control == 2232 && notification == CBN_SELCHANGE) {
                settings_.fullscreen.visual_type =
                    static_cast<int>(selection(2232)) + 1;
                SelectFullscreenProfileControls(
                    dialog, settings_.fullscreen,
                    settings_.fullscreen.visual_type);
                return TRUE;
            }
            if (control == 2230 && notification == CBN_SELCHANGE) {
                const int profile = settings_.fullscreen.visual_type;
                if (profile >= 0 && profile < static_cast<int>(
                        settings_.fullscreen.position_relation.size())) {
                    settings_.fullscreen.position_relation[
                        static_cast<size_t>(profile)] =
                        static_cast<int>(selection(2230));
                }
                return TRUE;
            }
            if (control == 2231 && notification == CBN_SELCHANGE) {
                const int profile = settings_.fullscreen.visual_type;
                if (profile >= 0 && profile < static_cast<int>(
                        settings_.fullscreen.lyric_size.size())) {
                    settings_.fullscreen.lyric_size[
                        static_cast<size_t>(profile)] =
                        static_cast<int>(selection(2231)) + 1;
                }
                return TRUE;
            }
            if (control == 1033 && notification == CBN_SELCHANGE) {
                const LRESULT selected = selection(1033);
                settings_.lyric.fullscreen_scroll_mode =
                    selected == CB_ERR ? 0 : static_cast<int>(selected);
                return TRUE;
            }
            if (control == 1037 && notification == CBN_SELCHANGE) {
                settings_.lyric.fullscreen_text_align =
                    static_cast<int>(selection(1037));
                return TRUE;
            }
            if (control == 1026 && notification == CBN_SELCHANGE) {
                const LRESULT selected = selection(1026);
                settings_.lyric.fullscreen_fade_index = selected == CB_ERR
                    ? 0 : static_cast<int>(SendDlgItemMessageW(
                        dialog, 1026, CB_GETITEMDATA,
                        static_cast<WPARAM>(selected), 0));
                return TRUE;
            }
            if (control == 1043 && notification == EN_CHANGE) {
                BOOL translated{};
                const UINT value = GetDlgItemInt(
                    dialog, 1043, &translated, FALSE);
                if (translated)
                    settings_.lyric.fullscreen_row_interval =
                        static_cast<int>(value);
                return TRUE;
            }
            if (control == 2145) {
                settings_.lyric.fullscreen_fade_highlight =
                    IsChecked(dialog, 2145);
                return TRUE;
            }
            if (control == 2150) {
                settings_.lyric.fullscreen_karaoke_mode =
                    IsChecked(dialog, 2150);
                return TRUE;
            }
            if (control == 2151) {
                settings_.lyric.fullscreen_transparent =
                    IsChecked(dialog, 2151);
                return TRUE;
            }
            if (control == 2022) {
                settings_.lyric.fullscreen_auto_font =
                    IsChecked(dialog, 2022);
                return TRUE;
            }
        }
        if (template_id == 261) {
            if (control == 1064 && notification == LBN_SELCHANGE) {
                UpdateOptionsSkinDetails(dialog);
                return TRUE;
            }
            if ((control == 1098 && notification == BN_CLICKED) ||
                (control == 1064 && notification == LBN_DBLCLK)) {
                const HWND list = GetDlgItem(dialog, 1064);
                const LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
                const LRESULT data = row == LB_ERR ? LB_ERR
                    : SendMessageW(list, LB_GETITEMDATA, row, 0);
                if (data != LB_ERR && data >= 0 &&
                    static_cast<size_t>(data) < options_skin_entries_.size()) {
                    skin_commands_ = options_skin_entries_;
                    UINT next = kCmdFirstSkin;
                    UINT command{};
                    for (auto& entry : skin_commands_) {
                        entry.command = entry.embedded_default
                            ? kCmdDefaultSkin : next++;
                        if (&entry - skin_commands_.data() == data)
                            command = entry.command;
                    }
                    if (command) HandleContextCommand(command);
                    PopulateOptionsSkinPage(dialog);
                }
                return TRUE;
            }
            if (control == 1039 && notification == BN_CLICKED) {
                const HWND list = GetDlgItem(dialog, 1064);
                const LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
                const LRESULT data = row == LB_ERR ? LB_ERR
                    : SendMessageW(list, LB_GETITEMDATA, row, 0);
                if (data != LB_ERR && data >= 0 &&
                    static_cast<size_t>(data) < options_skin_entries_.size()) {
                    const auto& entry = options_skin_entries_[static_cast<size_t>(data)];
                    if (!entry.embedded_default) {
                        wchar_t prompt[512]{};
                        const auto format = ResourceText(33191);
                        swprintf_s(prompt, format.c_str(),
                                   entry.metadata.name.c_str());
                        if (MessageBoxW(dialog, prompt, ResourceText(0x80).c_str(),
                                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            DeleteFileW(entry.path.c_str());
                            InvalidateSkinMenuCatalog();
                            StartSkinMenuCatalogLoad();
                            SetTimer(options_window_, kOptionsSkinPollTimer,
                                     kOptionsSkinPollMilliseconds, nullptr);
                        }
                    }
                }
                return TRUE;
            }
            if (control == 2109 && notification == STN_CLICKED) {
                ShellExecuteW(dialog, L"open", kSkinDownloadTarget, nullptr,
                              nullptr, SW_SHOWNORMAL);
                return TRUE;
            }
            if ((control == 1066 || control == 1067) &&
                notification == STN_CLICKED) {
                auto target = GetText(dialog, static_cast<int>(control));
                if (!target.empty() && _wcsicmp(target.c_str(), L"none") != 0) {
                    if (control == 1067 && target.find(L':') == std::wstring::npos)
                        target.insert(0, L"mailto:");
                    ShellExecuteW(dialog, L"open", target.c_str(), nullptr,
                                  nullptr, SW_SHOWNORMAL);
                }
                return TRUE;
            }
        }

        if (COLORREF* target = color_target(control);
            target && notification == BN_CLICKED) {
            ShowLegacyPresetColor(
                dialog, GetDlgItem(dialog, static_cast<int>(control)),
                ResourceModule(), *target,
                [this, dialog, control, template_id,
                 target](COLORREF selected) {
                if (!IsWindow(dialog)) return;
                *target = selected;
                InvalidateRect(GetDlgItem(dialog, static_cast<int>(control)),
                               nullptr, TRUE);
                ApplyOptionsPageRuntime(template_id);
            });
            return TRUE;
        }
        if (control == 1036 && notification == BN_CLICKED &&
            (template_id == 254 || template_id == 384 ||
             template_id == 385 || template_id == 263)) {
            LOGFONTW font{};
            bool* valid{};
            if (template_id == 254) {
                if (settings_.playlist.font_descriptor_valid) {
                    font = settings_.playlist.font_descriptor;
                } else {
                    font.lfHeight = settings_.playlist.font_height;
                    font.lfWeight = FW_NORMAL;
                    font.lfCharSet = DEFAULT_CHARSET;
                    wcsncpy_s(font.lfFaceName, settings_.playlist.font.c_str(),
                              _TRUNCATE);
                }
            } else if (template_id == 384) {
                font = settings_.lyric.font;
                valid = &settings_.lyric.font_valid;
            } else if (template_id == 385) {
                font = settings_.desktop_lyric.font;
            } else {
                font = settings_.lyric.fullscreen_font;
                valid = &settings_.lyric.fullscreen_font_valid;
            }
            CHOOSEFONTW chooser{sizeof(chooser)};
            chooser.hwndOwner = dialog;
            chooser.lpLogFont = &font;
            chooser.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
            if (template_id == 263) chooser.Flags |= CF_NOVERTFONTS;
            if (ChooseFontW(&chooser)) {
                if (template_id == 254) {
                    settings_.playlist.font = font.lfFaceName;
                    settings_.playlist.font_height = font.lfHeight;
                    settings_.playlist.font_descriptor = font;
                    settings_.playlist.font_descriptor_valid = true;
                } else if (template_id == 384) {
                    settings_.lyric.font = font;
                } else if (template_id == 385) {
                    settings_.desktop_lyric.font = font;
                    settings_.desktop_lyric.font_valid = true;
                } else {
                    // FUN_00499ED4 normalizes fullscreen lyric rendering to
                    // antialiased text after the common chooser returns.
                    font.lfQuality = ANTIALIASED_QUALITY;
                    settings_.lyric.fullscreen_font = font;
                }
                if (valid) *valid = true;
                ApplyOptionsPageRuntime(template_id);
            }
            return TRUE;
        }

        const bool edit_change = notification == EN_CHANGE ||
            (template_id == 258 && control == 2177 &&
             notification == CBN_EDITCHANGE);
        const bool edit_commit = notification == EN_KILLFOCUS ||
            (template_id == 258 && control == 2177 &&
             notification == CBN_KILLFOCUS);
        const bool immediate = notification == BN_CLICKED ||
            notification == CBN_SELCHANGE;
        if (template_id != 263 && GetPropW(dialog, kPageReadyProperty) &&
            (immediate || edit_change || edit_commit)) {
            if (!CommitOptionsControl(dialog, template_id, control))
                CommitOptionsPage(dialog, template_id);

            const UINT dirty = DeferredOptionsBit(template_id);
            if (edit_change) {
                // Match the per-control writes in the original handlers, but
                // coalesce their expensive player transaction.  This is most
                // important for playback/device numeric edits, where a
                // Configure call for every intermediate character can stall
                // both interactive use and automation.
                options_deferred_apply_mask_ |= dirty;
            } else if (edit_commit) {
                FlushDeferredOptionsRuntime(template_id);
            } else {
                options_deferred_apply_mask_ &= ~dirty;
                ApplyOptionsPageRuntime(template_id);
            }
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (!header) break;
        if (template_id == 261 && header->idFrom == kOptionsSkinKindTab &&
            header->code == TCN_SELCHANGE) {
            PopulateOptionsSkinPage(dialog);
            return TRUE;
        }
        if (template_id == 250 && header->idFrom == 2183 &&
            header->code == DTN_DATETIMECHANGE &&
            GetPropW(dialog, kPageReadyProperty)) {
            // FUN_00492C1B copies all three time fields from
            // NMDATETIMECHANGE before dispatching the General (mask 2)
            // notification.  Relying on PSN_APPLY loses a time-only edit when
            // the original-style Close button destroys the modeless sheet.
            const auto* changed =
                reinterpret_cast<const NMDATETIMECHANGE*>(lparam);
            if (changed->dwFlags == GDT_VALID) {
                settings_.general.shutdown_time = {
                    changed->st.wHour, changed->st.wMinute,
                    changed->st.wSecond};
                ApplyOptionsPageRuntime(template_id);
            }
            return TRUE;
        }
        if (template_id == 262 && header->idFrom == 2038 &&
            GetPropW(dialog, kPageReadyProperty)) {
            const HWND tree = GetDlgItem(dialog, 2038);
            if (header->code == NM_CLICK) {
                const DWORD packed = GetMessagePos();
                POINT point{GET_X_LPARAM(packed), GET_Y_LPARAM(packed)};
                ScreenToClient(tree, &point);
                if (ToggleOptionsAssociationAt(dialog,point)) return TRUE;
            }
            if (header->code == TVN_SELCHANGEDW ||
                header->code == TVN_SELCHANGEDA) {
                // FUN_0049DDD9 changes the application-wide AppIconFile and
                // does not depend on a file-type leaf selection.  Original
                // template 262 also keeps 2108 enabled for the root row.
                EnableWindow(GetDlgItem(dialog, 2106), TRUE);
                EnableWindow(GetDlgItem(dialog, 2108), TRUE);
                RefreshOptionsAssociationIcon(dialog);
                return TRUE;
            }
        }
        if ((template_id == 255 || template_id == 257) &&
            header->idFrom == 1038 && header->code == LVN_ITEMCHANGED &&
            GetPropW(dialog, kPageReadyProperty)) {
            UpdateFolderListButtons(dialog, ResourceModule(),
                                    template_id == 257);
            return TRUE;
        }
        if (template_id == 252 && header->idFrom == 1064 &&
            header->code == LVN_ITEMCHANGED &&
            GetPropW(dialog, kPageReadyProperty)) {
            const HWND list = GetDlgItem(dialog, 1064);
            options_hotkey_selection_ = ListView_GetNextItem(
                list, -1, LVNI_SELECTED);
            const BOOL selected = options_hotkey_selection_ >= 0;
            EnableWindow(GetDlgItem(dialog, 2054), selected);
            EnableWindow(GetDlgItem(dialog, 2055), selected);
            EnableWindow(GetDlgItem(dialog, 1098), selected);
            if (selected && static_cast<size_t>(options_hotkey_selection_) <
                    settings_.hotkey.key_map.size()) {
                const auto& next = settings_.hotkey.key_map[
                    static_cast<size_t>(options_hotkey_selection_)];
                SetHotKeyControl(dialog, 2054, next.application);
                SetHotKeyControl(dialog, 2055, next.global);
            } else {
                SetHotKeyControl(dialog, 2054, {});
                SetHotKeyControl(dialog, 2055, {});
            }
            return TRUE;
        }
        if (template_id == 259 && header->idFrom == 1064 &&
            GetPropW(dialog, kPageReadyProperty)) {
            const HWND list = GetDlgItem(dialog, 1064);
            if (header->code == LVN_ITEMCHANGED) {
                const auto* changed =
                    reinterpret_cast<const NMLISTVIEW*>(lparam);
                const UINT old_check = changed->uOldState &
                    LVIS_STATEIMAGEMASK;
                const UINT new_check = changed->uNewState &
                    LVIS_STATEIMAGEMASK;
                // FUN_0049911C reacts only when the state-image (checkbox)
                // bits changed. Selection/focus changes merely update the
                // Configure button and must not tear down/rebuild the active
                // processor chain.
                if ((changed->uChanged & LVIF_STATE) != 0 &&
                    old_check != new_check) {
                    CommitOptionsPage(dialog, template_id);
                    ApplyOptionsPageRuntime(template_id);
                }
                UpdateDspButtons(dialog);
                return TRUE;
            }
            if (header->code == NM_DBLCLK ||
                header->code == LVN_ITEMACTIVATE) {
                if (IsWindowEnabled(GetDlgItem(dialog, 1015)))
                    SendMessageW(dialog, WM_COMMAND,
                        MAKEWPARAM(1015, BN_CLICKED),
                        reinterpret_cast<LPARAM>(GetDlgItem(dialog, 1015)));
                return TRUE;
            }
            if (header->code == static_cast<UINT>(-197)) {
                const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lparam);
                if (changed->iItem >= 0 && changed->iSubItem >= 0 &&
                    changed->iItem < ListView_GetItemCount(list) &&
                    changed->iSubItem < ListView_GetItemCount(list)) {
                    SwapListRows(list, changed->iItem, changed->iSubItem, 3);
                    CommitOptionsPage(dialog, template_id);
                    ApplyOptionsPageRuntime(template_id);
                }
                UpdateDspButtons(dialog);
                return TRUE;
            }
        }
        if ((template_id == 256 && header->idFrom == 2256) ||
            (template_id == 258 && header->idFrom == 1181)) {
            if (header->code == TCN_SELCHANGE) {
                switch_nested(0);
                return TRUE;
            }
        }
        if (header->code == PSN_SETACTIVE) {
            // The original removes configured global registrations while the
            // shortcut editor owns the two HOTKEY controls.  They are rebuilt
            // only when this page loses activation.
            if (template_id == 252) UnregisterConfiguredHotKeys();
            const int index = TemplateIndex(template_id);
            if (index >= 0) {
                options_page_index_ = index;
                if (options_navigation_)
                    SendMessageW(options_navigation_, LB_SETCURSEL,
                        OptionsSheetIndex(index, options_registration_mode_), 0);
                if (options_header_) {
                    auto description = ResourceText(template_id);
                    if (description.empty())
                        description = DialogCaption(ResourceModule(),
                                                    template_id);
                    SetWindowTextW(options_header_, description.c_str());
                    InvalidateRect(options_header_, nullptr, TRUE);
                }
            }
            PositionOptionsPage(dialog);
            return TRUE;
        }
        if (header->code == PSN_KILLACTIVE || header->code == PSN_APPLY ||
            header->code == PSN_RESET) {
            if (header->code == PSN_KILLACTIVE && template_id == 252)
                RegisterConfiguredHotKeys();
            if (header->code == PSN_APPLY) {
                if (template_id == 262) CommitOptionsAssociations(dialog, true);
                CommitOptionsPage(dialog, template_id);
                FlushDeferredOptionsRuntime(template_id);
            } else if (header->code == PSN_KILLACTIVE) {
                // FUN_00494750/FUN_00497831 rebuild the two directory
                // vectors from their ListViews on notification -0xC9.  Item
                // changes themselves only update button state/dirty status.
                if (template_id == 255 || template_id == 257)
                    CommitOptionsPage(dialog, template_id);
                // Text controls already copied their value on EN_CHANGE.
                // Losing the page is the original logical commit boundary for
                // the coalesced player notification.
                FlushDeferredOptionsRuntime(template_id);
            }
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT,
                              header->code == PSN_KILLACTIVE ? FALSE
                                                             : PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    case WM_DESTROY: {
        if (template_id == 381 || template_id == 382) {
            auto& child = options_network_pages_[template_id - 381];
            if (child == dialog) child = nullptr;
            if (options_network_child_ == dialog) options_network_child_ = nullptr;
        }
        if (template_id == 384 || template_id == 385) {
            auto& child = options_lyric_pages_[template_id - 384];
            if (child == dialog) child = nullptr;
            if (options_lyric_child_ == dialog) options_lyric_child_ = nullptr;
        }
        if (template_id == 259) {
            KillTimer(dialog, kOptionsDspPollTimer);
            CancelOptionsDspScan();
        }
        if (template_id == 262 && !options_association_nodes_.empty()) {
            // Fallback for direct teardown; never open a modal UI while the
            // property sheet is destroying HWNDs. Normal Close/Save already
            // committed through PSN_APPLY and reported errors there.
            CommitOptionsAssociations(dialog, false);
        }
        RemovePropW(dialog, kPageReadyProperty);
        RemovePropW(dialog, kPageTemplateProperty);
        const int index = TemplateIndex(template_id);
        if (index >= 0 && options_pages_[static_cast<size_t>(index)] == dialog)
            options_pages_[static_cast<size_t>(index)] = nullptr;
        break;
    }
    default:
        break;
    }
    return FALSE;
}

} // namespace ttplayer::ui
