#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "modern_file_dialog.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/core/text.h"
#include "ttplayer/skin/skin_package.h"
#include "ttplayer/ui/dialog_history_policy.h"
#include "ttplayer/ui/player_control_packet.h"
#include "ttplayer/ui/player_runtime_policy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <commctrl.h>
#include <objidl.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <wrl/client.h>

namespace ttplayer::ui {
using namespace detail;
namespace {

struct ScopedStgMedium {
    STGMEDIUM value{};
    bool owns{};

    ScopedStgMedium() = default;
    ScopedStgMedium(const ScopedStgMedium&) = delete;
    ScopedStgMedium& operator=(const ScopedStgMedium&) = delete;
    ~ScopedStgMedium() {
        if (owns) ReleaseStgMedium(&value);
    }
};

struct ScopedGlobalLock {
    explicit ScopedGlobalLock(HGLOBAL handle) noexcept
        : handle(handle), value(GlobalLock(handle)) {}
    ScopedGlobalLock(const ScopedGlobalLock&) = delete;
    ScopedGlobalLock& operator=(const ScopedGlobalLock&) = delete;
    ~ScopedGlobalLock() {
        if (value) GlobalUnlock(handle);
    }

    HGLOBAL handle{};
    void* value{};
};

struct ScopedFindHandle {
    explicit ScopedFindHandle(HANDLE value) noexcept : value(value) {}
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;
    ~ScopedFindHandle() {
        if (value != INVALID_HANDLE_VALUE) FindClose(value);
    }

    HANDLE value{INVALID_HANDLE_VALUE};
};

struct ScopedKernelHandle {
    explicit ScopedKernelHandle(HANDLE value) noexcept : value(value) {}
    ScopedKernelHandle(const ScopedKernelHandle&) = delete;
    ScopedKernelHandle& operator=(const ScopedKernelHandle&) = delete;
    ~ScopedKernelHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }

    HANDLE value{INVALID_HANDLE_VALUE};
};

constexpr int kPlayUrlEdit = 2001;
constexpr int kPlayUrlBrowse = 1023;
constexpr int kPlayCdList = 1064;
constexpr int kPlayCdAddToPlaylist = 2017;
constexpr int kControlPacketPlaylistList = 1064;

struct PlayUrlDialogState {
    std::wstring value{L"http://"};
};

struct ControlPacketPlaylistDialogState {
    const playlist::PlaylistStore* store{};
    size_t selected{playlist::PlaylistStore::npos};
};

INT_PTR CALLBACK ControlPacketPlaylistDialogProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ControlPacketPlaylistDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<ControlPacketPlaylistDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state || !state->store) return FALSE;
        const HWND list = GetDlgItem(dialog, kControlPacketPlaylistList);
        for (const auto& entry : state->store->Entries()) {
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(entry.playlist.Title().c_str()));
        }
        if (state->store->Size() != 0) {
            SendMessageW(list, LB_SETCURSEL,
                         state->store->ActiveIndex(), 0);
        }
        return TRUE;
    }
    if (message != WM_COMMAND || !state) return FALSE;
    switch (LOWORD(wparam)) {
    case IDOK: {
        const LRESULT selected = SendDlgItemMessageW(
            dialog, kControlPacketPlaylistList, LB_GETCURSEL, 0, 0);
        if (selected == LB_ERR) return TRUE;
        state->selected = static_cast<size_t>(selected);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    case kPlaylistNewList:
        state->selected = state->store->Size();
        EndDialog(dialog, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK PlayUrlDialogProc(HWND dialog, UINT message,
                                   WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PlayUrlDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PlayUrlDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state) return FALSE;
        SetDlgItemTextW(dialog, kPlayUrlEdit, state->value.c_str());
        const HWND edit = GetDlgItem(dialog, kPlayUrlEdit);
        if (edit) {
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return FALSE;
        }
        return TRUE;
    }
    if (message != WM_COMMAND || !state) return FALSE;
    switch (LOWORD(wparam)) {
    case IDOK: {
        const int length = GetWindowTextLengthW(GetDlgItem(dialog, kPlayUrlEdit));
        if (length <= 0) return TRUE;
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        GetDlgItemTextW(dialog, kPlayUrlEdit, value.data(), length + 1);
        value.resize(static_cast<size_t>(length));
        const auto first = value.find_first_not_of(L" \t\r\n");
        const auto last = value.find_last_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return TRUE;
        state->value = value.substr(first, last - first + 1);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case kPlayUrlBrowse:
        // CPlayList::OpenURL returns the private result 0x3ff for the
        // resource dialog's Browse button.  The owner performs the ordinary
        // file-open transaction after the modal stack has unwound.
        EndDialog(dialog, kPlayUrlBrowse);
        return TRUE;
    default:
        return FALSE;
    }
}

std::vector<std::filesystem::path> OpticalDrives() {
    std::vector<std::filesystem::path> result;
    for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
        wchar_t root[]{drive, L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) == DRIVE_CDROM) result.emplace_back(root);
    }
    return result;
}

std::vector<std::filesystem::path> EnumeratePattern(
    const std::filesystem::path& directory, const wchar_t* pattern) {
    std::vector<std::filesystem::path> result;
    WIN32_FIND_DATAW data{};
    const ScopedFindHandle find(FindFirstFileW(
        (directory / pattern).c_str(), &data));
    if (find.value == INVALID_HANDLE_VALUE) return result;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            result.push_back(directory / data.cFileName);
    } while (FindNextFileW(find.value, &data));
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
    });
    return result;
}

std::vector<std::filesystem::path> DiscMediaPaths(
    const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    const auto name = root.root_name().wstring();
    if (name.size() == 2 && name[1] == L':') {
        std::wstring device = L"\\\\.\\";
        device += static_cast<wchar_t>(towupper(name[0]));
        device += L":";
        const ScopedKernelHandle handle(CreateFileW(
            device.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle.value != INVALID_HANDLE_VALUE) {
            CDROM_TOC toc{};
            DWORD returned{};
            if (DeviceIoControl(handle.value, IOCTL_CDROM_READ_TOC, nullptr, 0,
                                &toc, sizeof(toc), &returned, nullptr) &&
                toc.FirstTrack != 0 && toc.LastTrack >= toc.FirstTrack) {
                for (unsigned track = toc.FirstTrack;
                     track <= toc.LastTrack; ++track) {
                    const size_t index = track - toc.FirstTrack;
                    if ((toc.TrackData[index].Control & 4U) != 0) continue;
                    wchar_t filename[32]{};
                    swprintf_s(filename, L"Track%02u.cda", track);
                    result.push_back(root / filename);
                }
            }
        }
    }
    // FUN_0047D023 falls back to the shell's synthetic CDA files and then
    // VCD MPEGAV/*.dat when the TOC reader produced no playable item.
    if (result.empty()) result = EnumeratePattern(root, L"*.cda");
    if (result.empty()) result = EnumeratePattern(root / L"MPEGAV", L"*.dat");
    return result;
}

struct PlayCdDialogState {
    std::vector<std::filesystem::path> drives;
    std::vector<std::filesystem::path> selected;
    std::function<void(const std::vector<std::filesystem::path>&)> add;
};

std::optional<size_t> SelectedCdDrive(HWND dialog,
                                      const PlayCdDialogState& state) {
    const HWND list = GetDlgItem(dialog, kPlayCdList);
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) return std::nullopt;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(list, &item) || item.lParam < 0 ||
        static_cast<size_t>(item.lParam) >= state.drives.size())
        return std::nullopt;
    return static_cast<size_t>(item.lParam);
}

void InitializeCdDriveList(HWND dialog, PlayCdDialogState& state) {
    const HWND list = GetDlgItem(dialog, kPlayCdList);
    if (!list) return;
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    RECT client{};
    GetClientRect(list, &client);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = std::max(1L, client.right - client.left - 4);
    ListView_InsertColumn(list, 0, &column);

    HIMAGELIST images{};
    for (size_t index = 0; index < state.drives.size(); ++index) {
        SHFILEINFOW info{};
        const DWORD_PTR image_list = SHGetFileInfoW(
            state.drives[index].c_str(), 0, &info, sizeof(info),
            SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (!images && image_list) {
            images = reinterpret_cast<HIMAGELIST>(image_list);
            ListView_SetImageList(list, images, LVSIL_SMALL);
        }
        std::wstring title = info.szDisplayName;
        if (title.empty()) title = state.drives[index].wstring();
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = static_cast<int>(index);
        item.pszText = title.data();
        item.lParam = static_cast<LPARAM>(index);
        item.iImage = info.iIcon;
        ListView_InsertItem(list, &item);
    }
    if (!state.drives.empty()) {
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, 0, FALSE);
    }
    EnableWindow(GetDlgItem(dialog, IDOK), !state.drives.empty());
    EnableWindow(GetDlgItem(dialog, kPlayCdAddToPlaylist),
                 !state.drives.empty());
}

INT_PTR CALLBACK PlayCdDialogProc(HWND dialog, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PlayCdDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PlayCdDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state) return FALSE;
        InitializeCdDriveList(dialog, *state);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_NOTIFY) {
        const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
        if (notification && notification->idFrom == kPlayCdList &&
            notification->code == NM_DBLCLK) {
            SendMessageW(dialog, WM_COMMAND, IDOK, 0);
            return TRUE;
        }
    }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wparam)) {
    case IDOK:
    case kPlayCdAddToPlaylist: {
        const auto selected = SelectedCdDrive(dialog, *state);
        if (!selected) return TRUE;
        auto paths = DiscMediaPaths(state->drives[*selected]);
        if (paths.empty()) {
            MessageBeep(MB_ICONEXCLAMATION);
            return TRUE;
        }
        if (LOWORD(wparam) == kPlayCdAddToPlaylist) {
            if (state->add) state->add(paths);
        } else {
            state->selected = std::move(paths);
            EndDialog(dialog, IDOK);
        }
        return TRUE;
    }
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), towlower);
    return extension;
}

std::string ImportedTrackTitle(const std::filesystem::path& path) {
    // FUN_0041EA0C constructs every row accepted by 00474051 from the
    // logical archive-member path.  At import time that constructor stores
    // the file stem as the title; CUE TITLE/PERFORMER and decoder metadata
    // are not copied into the TTBL row by the archive enumerator.
    try {
        return core::WideToUtf8(path.stem().wstring());
    } catch (const std::exception&) {
        return {};
    }
}

bool IsUrl(std::wstring_view value) {
    const size_t delimiter = value.find(L"://");
    return delimiter > 0 && delimiter != std::wstring_view::npos;
}

bool PatternMatchesPath(std::wstring_view patterns,
                        const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    if (extension.empty()) return false;
    const std::wstring needle = L"*" + extension;
    size_t begin{};
    while (begin <= patterns.size()) {
        const size_t end = patterns.find(L';', begin);
        auto pattern = patterns.substr(begin, end == std::wstring_view::npos
            ? patterns.size() - begin : end - begin);
        while (!pattern.empty() && iswspace(pattern.front())) pattern.remove_prefix(1);
        while (!pattern.empty() && iswspace(pattern.back())) pattern.remove_suffix(1);
        if (!pattern.empty() &&
            (_wcsicmp(std::wstring(pattern).c_str(), needle.c_str()) == 0 ||
             _wcsicmp(std::wstring(pattern).c_str(), L"*.*") == 0)) {
            return true;
        }
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return false;
}

bool IsBuiltInAudioPath(const std::filesystem::path& path) {
    // FUN_004CB1DB's seven built-in reader descriptions are the only formats
    // which remain available without a successfully registered reader DLL.
    // AAC/AC3/DTS are supplied by add-ins and must stay plugin-gated.
    static constexpr std::array<std::wstring_view, 16> extensions{
        L".cda", L".mp3", L".mp2", L".mp1", L".mpa", L".mp3pro",
        L".mid", L".midi", L".rmi", L".wav", L".wave", L".aif",
        L".aifc", L".aiff", L".au", L".snd"};
    const auto extension = LowerExtension(path);
    return std::ranges::find(extensions, extension) != extensions.end();
}

std::filesystem::path RuntimeDirectory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::wstring DirectoryIdentity(const std::filesystem::path& directory) {
    const ScopedKernelHandle handle(CreateFileW(
        directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    std::wstring identity;
    if (handle.value != INVALID_HANDLE_VALUE) {
        const DWORD required = GetFinalPathNameByHandleW(
            handle.value, nullptr, 0, FILE_NAME_NORMALIZED);
        if (required != 0) {
            identity.resize(required);
            const DWORD written = GetFinalPathNameByHandleW(
                handle.value, identity.data(), required, FILE_NAME_NORMALIZED);
            if (written != 0 && written < required) identity.resize(written);
            else identity.clear();
        }
    }
    if (identity.empty()) {
        std::error_code error;
        identity = std::filesystem::absolute(directory, error)
            .lexically_normal().wstring();
        if (error) identity = directory.lexically_normal().wstring();
    }
    std::ranges::transform(identity, identity.begin(), towlower);
    return identity;
}

std::optional<std::filesystem::path> ResolveShortcut(
    const std::filesystem::path& shortcut) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.GetAddressOf()))) || !link) {
        return std::nullopt;
    }
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    HRESULT result = link->QueryInterface(IID_PPV_ARGS(persist.GetAddressOf()));
    if (SUCCEEDED(result) && persist)
        result = persist->Load(shortcut.c_str(), STGM_READ);
    if (SUCCEEDED(result))
        result = link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK);
    std::wstring target(32768, L'\0');
    WIN32_FIND_DATAW data{};
    if (SUCCEEDED(result))
        result = link->GetPath(target.data(), static_cast<int>(target.size()),
                               &data, SLGP_RAWPATH);
    if (FAILED(result) || target.front() == L'\0') return std::nullopt;
    target.resize(std::wcslen(target.c_str()));
    return std::filesystem::path(std::move(target));
}

bool QueryDropFormat(IDataObject* data, CLIPFORMAT format) {
    if (!data) return false;
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data->QueryGetData(&request) == S_OK;
}

std::vector<std::filesystem::path> ExtractDropPaths(IDataObject* data) {
    std::vector<std::filesystem::path> paths;
    if (!data) return paths;
    FORMATETC request{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    ScopedStgMedium file_medium;
    if (SUCCEEDED(data->GetData(&request, &file_medium.value))) {
        file_medium.owns = true;
        const HDROP drop = static_cast<HDROP>(file_medium.value.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            // 0045A8BE, 00481F2A and 0044AC70 each pass cch=0x104.
            // Preserve that observable MAX_PATH truncation instead of using
            // the preliminary length query as an accidental modern extension.
            std::array<wchar_t, MAX_PATH> value{};
            if (DragQueryFileW(drop, index, value.data(),
                               static_cast<UINT>(value.size())) != 0)
                paths.emplace_back(value.data());
        }
        if (!paths.empty()) return paths;
    }

    const CLIPFORMAT url_w = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"UniformResourceLocatorW"));
    request.cfFormat = url_w;
    ScopedStgMedium url_medium;
    if (url_w && SUCCEEDED(data->GetData(&request, &url_medium.value))) {
        url_medium.owns = true;
        ScopedGlobalLock lock(url_medium.value.hGlobal);
        const auto* value = static_cast<const wchar_t*>(lock.value);
        const SIZE_T bytes = GlobalSize(url_medium.value.hGlobal);
        if (value && bytes >= sizeof(wchar_t)) {
            const size_t maximum = bytes / sizeof(wchar_t);
            size_t length{};
            while (length < maximum && value[length] != L'\0') ++length;
            if (length) paths.emplace_back(std::wstring(value, length));
        }
        if (!paths.empty()) return paths;
    }

    return paths;
}

bool HasExternalDropData(IDataObject* data, bool accept_url) {
    if (QueryDropFormat(data, CF_HDROP)) return true;
    if (!accept_url) return false;
    const auto url_w = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"UniformResourceLocatorW"));
    return url_w && QueryDropFormat(data, url_w);
}

struct ExternalDropPaths {
    std::vector<std::filesystem::path> ordinary;
    std::vector<std::filesystem::path> playlists;
};

ExternalDropPaths ClassifyExternalDropPaths(
    const std::vector<std::filesystem::path>& supplied_paths) {
    ExternalDropPaths result;
    result.ordinary.reserve(supplied_paths.size());
    result.playlists.reserve(supplied_paths.size());
    for (const auto& supplied : supplied_paths) {
        auto path = supplied;
        if (LowerExtension(path) == L".lnk") {
            if (const auto resolved = ResolveShortcut(path)) path = *resolved;
        }
        std::error_code error;
        const bool regular = std::filesystem::is_regular_file(path, error) &&
                             !error;
        if (regular && IsPlaylistFile(path)) result.playlists.push_back(path);
        else result.ordinary.push_back(path);
    }
    return result;
}

std::vector<std::filesystem::path> OriginalExternalImportOrder(
    ExternalDropPaths paths) {
    // 00481F2A accumulates ordinary inputs immediately, queues every type-5
    // playlist path separately, and 004822AD expands those queued lists only
    // after the ordinary temporary playlist has been inserted.
    paths.ordinary.insert(paths.ordinary.end(),
                          std::make_move_iterator(paths.playlists.begin()),
                          std::make_move_iterator(paths.playlists.end()));
    return std::move(paths.ordinary);
}

LRESULT OriginalListDropHit(HWND control, POINT point) {
    if (!control) return -1;
    LVHITTESTINFO hit{};
    hit.pt = point;
    LRESULT row = SendMessageW(control, LVM_HITTEST, 0,
                               reinterpret_cast<LPARAM>(&hit));
    if (row != -1) return row;

    // FUN_0041CB83 extends the stock ListView hit test to the two clipped
    // rows exposed by 0048218C's Y-only rectangle inflation: the row just
    // above the top index, and the synthetic insertion row after the final
    // item.
    const LRESULT top = SendMessageW(control, LVM_GETTOPINDEX, 0, 0);
    RECT bounds{};
    bounds.left = LVIR_BOUNDS;
    if (SendMessageW(control, LVM_GETITEMRECT,
                     static_cast<WPARAM>(top - 1),
                     reinterpret_cast<LPARAM>(&bounds)) &&
        PtInRect(&bounds, point)) {
        return top - 1;
    }

    const LRESULT count = SendMessageW(control, LVM_GETITEMCOUNT, 0, 0);
    bounds = {};
    bounds.left = LVIR_BOUNDS;
    if (SendMessageW(control, LVM_GETITEMRECT,
                     static_cast<WPARAM>(count - 1),
                     reinterpret_cast<LPARAM>(&bounds))) {
        OffsetRect(&bounds, 0, bounds.bottom - bounds.top);
        if (PtInRect(&bounds, point)) return count;
    }
    return -1;
}

std::wstring DefaultImportedListTitle(HMODULE resources, size_t ordinal) {
    auto format = LoadResourceText(resources, 0x8192);
    wchar_t title[128]{};
    if (!format.empty()) swprintf_s(title, format.c_str(), ordinal);
    return title[0] ? std::wstring(title) : LoadResourceText(resources, 0x813a);
}

} // namespace

class PlayerWindow::FileDropTarget final : public IDropTarget {
public:
    FileDropTarget(PlayerWindow& owner, HWND window, FileDropSurface surface)
        : owner_(owner), window_(window), surface_(surface) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *result = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD,
                                        POINTL point, DWORD* effect) override {
        const DWORD incoming = effect ? *effect : DROPEFFECT_NONE;
        DWORD selected = DROPEFFECT_NONE;
        try {
            // 0045A8BE and 0044AC70 request CF_HDROP only.  The playlist's
            // 00481F2A additionally falls back to UniformResourceLocatorW.
            accepted_ = HasExternalDropData(
                data, surface_ == FileDropSurface::playlist);
            EnsureHelper();
            if (helper_) {
                POINT screen{point.x, point.y};
                // 00468A8A/0048AF29 relay the source's unmodified allowed
                // mask to IDropTargetHelper before the real target decides
                // which effect to publish.
                helper_->DragEnter(window_, data, &screen, incoming);
            }
            point_accepted_ = accepted_ && AcceptsPoint(point);
            if (point_accepted_) {
                selected = surface_ == FileDropSurface::playlist
                    ? incoming : DROPEFFECT_LINK;
            }
        } catch (...) {
            accepted_ = false;
            point_accepted_ = false;
            owner_.ClearPlaylistDropCue();
            if (helper_) helper_->DragLeave();
            selected = DROPEFFECT_NONE;
        }
        if (effect) *effect = selected;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point,
                                       DWORD* effect) override {
        const DWORD incoming = effect ? *effect : DROPEFFECT_NONE;
        DWORD selected = DROPEFFECT_NONE;
        try {
            if (helper_) {
                POINT screen{point.x, point.y};
                helper_->DragOver(&screen, incoming);
            }
            point_accepted_ = accepted_ && AcceptsPoint(point);
            if (point_accepted_) {
                selected = owner_.FileDropEffect(surface_, point, incoming);
            }
        } catch (...) {
            accepted_ = false;
            point_accepted_ = false;
            owner_.ClearPlaylistDropCue();
            if (helper_) helper_->DragLeave();
            selected = DROPEFFECT_NONE;
        }
        if (effect) *effect = selected;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        accepted_ = false;
        point_accepted_ = false;
        try { owner_.ClearPlaylistDropCue(); }
        catch (...) {}
        if (helper_) helper_->DragLeave();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD key_state,
                                   POINTL point, DWORD* effect) override {
        const DWORD incoming = effect ? *effect : DROPEFFECT_NONE;
        DWORD selected = incoming;
        try {
            if (helper_) {
                POINT screen{point.x, point.y};
                helper_->Drop(data, &screen, incoming);
            }
            // An owned popup without its own IDropTarget can resolve to the
            // owner's registered target.  CPlayerWnd's 00468A8A/00468B09
            // rejects such points outside the actual player rectangle.  Do
            // the same check again at Drop so an effect-NONE release cannot
            // accidentally run the main-window import path.
            point_accepted_ = accepted_ && AcceptsPoint(point);
            if (point_accepted_)
                owner_.HandleDroppedFiles(surface_, data, key_state, point,
                                          &selected);
            else
                selected = DROPEFFECT_NONE;
        } catch (...) {
            if (helper_) helper_->DragLeave();
            selected = DROPEFFECT_NONE;
        }
        if (effect) *effect = selected;
        accepted_ = false;
        point_accepted_ = false;
        try { owner_.ClearPlaylistDropCue(); }
        catch (...) {}
        return S_OK;
    }

    [[nodiscard]] HWND Window() const noexcept { return window_; }

private:
    ~FileDropTarget() {
        if (helper_) helper_->Release();
    }

    void EnsureHelper() {
        if (!helper_)
            CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&helper_));
    }

    [[nodiscard]] bool AcceptsPoint(POINTL point) const noexcept {
        if (surface_ != FileDropSurface::player) return true;
        RECT bounds{};
        if (!window_ || !GetWindowRect(window_, &bounds)) return false;
        const POINT screen{point.x, point.y};
        return PtInRect(&bounds, screen) != FALSE;
    }

    std::atomic<ULONG> references_{1};
    PlayerWindow& owner_;
    HWND window_{};
    FileDropSurface surface_{};
    IDropTargetHelper* helper_{};
    bool accepted_{};
    bool point_accepted_{};
};

bool PlayerWindow::RegisterFileDropTarget(HWND window, FileDropSurface surface) {
    if (!window) return false;
    FileDropTarget** slot{};
    switch (surface) {
    case FileDropSurface::player: slot = &player_drop_target_; break;
    case FileDropSurface::playlist: slot = &playlist_drop_target_; break;
    case FileDropSurface::lyric: slot = &lyric_drop_target_; break;
    }
    if (!slot) return false;
    if (*slot) RevokeFileDropTarget((*slot)->Window());
    auto* target = new (std::nothrow) FileDropTarget(*this, window, surface);
    if (!target) return false;
    const HRESULT result = RegisterDragDrop(window, target);
    if (FAILED(result)) {
        target->Release();
        return false;
    }
    *slot = target;
    return true;
}

void PlayerWindow::RevokeFileDropTarget(HWND window) {
    const auto revoke = [window](FileDropTarget*& target) {
        if (!target || (window && target->Window() != window)) return;
        if (target->Window() && IsWindow(target->Window()))
            static_cast<void>(RevokeDragDrop(target->Window()));
        target->Release();
        target = nullptr;
    };
    revoke(player_drop_target_);
    revoke(playlist_drop_target_);
    revoke(lyric_drop_target_);
}

DWORD PlayerWindow::FileDropEffect(FileDropSurface surface, POINTL point,
                                   DWORD source_effect) {
    // Main 00468B09 leaves DragOver's source mask untouched.  Lyric's simple
    // target reports LINK on both Enter and Over.  CPlayListWnd alone runs
    // the region/hit-test path below and ORs LINK into the incoming mask.
    if (surface == FileDropSurface::player) return source_effect;
    if (surface == FileDropSurface::lyric) return DROPEFFECT_LINK;
    if (!playlist_window_ || !playlist_list_control_ ||
        !playlist_track_control_ || !skin_ || !skin_->Playlist().valid)
        return DROPEFFECT_NONE;
    const POINT screen{point.x, point.y};
    RECT lists{};
    RECT tracks{};
    GetWindowRect(playlist_list_control_, &lists);
    GetWindowRect(playlist_track_control_, &tracks);
    // 0048218C expands only the vertical hit band by two pixels.
    InflateRect(&lists, 0, 2);
    InflateRect(&tracks, 0, 2);
    if (!settings_.playlist.library_mode &&
        settings_.playlist.split_on_lists > 0 && PtInRect(&lists, screen)) {
        POINT client = screen;
        ScreenToClient(playlist_list_control_, &client);
        const LRESULT row = OriginalListDropHit(playlist_list_control_, client);
        playlist_list_hover_ = row >= 0
            ? std::optional<size_t>{static_cast<size_t>(row)} : std::nullopt;
        playlist_track_drop_row_.reset();
        playlist_external_dragging_ = false;
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        return source_effect | DROPEFFECT_LINK;
    }
    if (PtInRect(&tracks, screen)) {
        playlist_external_dragging_ = true;
        POINT client = screen;
        // The original 00482241..0048224B accidentally converts the Files
        // hover point through the PlayLists HWND, then gives those coordinates
        // to the Files wrapper.  Drop itself uses the correct Files HWND.
        ScreenToClient(playlist_list_control_, &client);
        const LRESULT row = OriginalListDropHit(playlist_track_control_, client);
        const size_t insertion = row >= 0
            ? static_cast<size_t>(row) : ActivePlaylist().Tracks().size();
        if (playlist_track_drop_row_ != insertion) {
            playlist_track_drop_row_ = insertion;
            if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        }
        playlist_list_hover_.reset();
        return source_effect | DROPEFFECT_LINK;
    }
    ClearPlaylistDropCue();
    return DROPEFFECT_NONE;
}

void PlayerWindow::ClearPlaylistDropCue() noexcept {
    const bool changed = playlist_track_drop_row_.has_value() ||
                         playlist_list_hover_.has_value() ||
                         playlist_external_dragging_;
    playlist_track_drop_row_.reset();
    playlist_list_hover_.reset();
    playlist_external_dragging_ = false;
    if (changed && playlist_window_)
        InvalidateRect(playlist_window_, nullptr, FALSE);
}

bool PlayerWindow::CollectImportedTracks(const std::filesystem::path& input,
                                         std::vector<playlist::Track>& tracks,
                                         bool directory_member,
                                         std::vector<std::wstring>* directory_ancestry) {
    if (input.empty()) return false;
    auto path = input;
    if (IsUrl(path.native())) {
        tracks.push_back({path, {}, {}, -2});
        return true;
    }
    if (LowerExtension(path) == L".lnk") {
        const auto resolved = ResolveShortcut(path);
        // 004739DB keeps the .lnk itself as an ordinary item when IShellLink
        // resolution fails.  If resolution succeeds, even a now-missing
        // target continues through the type-0 ordinary-item route.
        if (resolved) path = *resolved;
    }

    std::error_code error;
    if (std::filesystem::is_directory(path, error) && !error) {
        std::vector<std::wstring> root_ancestry;
        if (!directory_ancestry) directory_ancestry = &root_ancestry;
        const auto identity = DirectoryIdentity(path);
        if (std::ranges::find(*directory_ancestry, identity) !=
            directory_ancestry->end()) return false;
        directory_ancestry->push_back(identity);
        struct AncestryGuard {
            std::vector<std::wstring>& values;
            ~AncestryGuard() { values.pop_back(); }
        } ancestry_guard{*directory_ancestry};

        std::filesystem::path pattern = path / L"*";
        WIN32_FIND_DATAW data{};
        const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) return false;
        const ScopedFindHandle close_search(search);
        bool added{};
        do {
            if (std::wcscmp(data.cFileName, L".") == 0 ||
                std::wcscmp(data.cFileName, L"..") == 0) continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                !settings_.history.check_sub_folder)
                continue;
            added = CollectImportedTracks(path / data.cFileName, tracks, true,
                                          directory_ancestry) || added;
        } while (FindNextFileW(search, &data));
        return added;
    }
    error.clear();
    if (!std::filesystem::is_regular_file(path, error) || error) {
        if (directory_member) return false;
        tracks.push_back({path, {}, {}, -2});
        return true;
    }

    const auto extension = LowerExtension(path);
    if (extension == L".cue") {
        try {
            const auto sheet = audio::CueSheet::Load(path);
            const size_t before = tracks.size();
            for (const auto& cue_track : sheet.Tracks()) {
                const auto duration = cue_track.DurationMilliseconds();
                tracks.push_back({
                    path,
                    cue_track.title.empty() ? std::string{} :
                        core::WideToUtf8(cue_track.title),
                    cue_track.performer.empty() ? std::string{} :
                        core::WideToUtf8(cue_track.performer),
                    duration < 0 || duration > INT_MAX ? -2 :
                        static_cast<int>(duration),
                    cue_track.number,
                    sheet.Title().empty() ? std::string{} :
                        core::WideToUtf8(sheet.Title())});
            }
            return tracks.size() != before;
        } catch (const std::exception&) {
            return false;
        }
    }
    if (IsPlaylistFile(path)) {
        if (directory_member) return false;
        try {
            playlist::Playlist loaded;
            loaded.LoadFromFile(path,
                {settings_.playlist.ignore_bad_files});
            tracks.insert(tracks.end(), loaded.Tracks().begin(), loaded.Tracks().end());
            return !loaded.Tracks().empty();
        } catch (const std::exception&) {
            return false;
        }
    }
    if (extension == L".zip") {
        // FUN_00474051 preserves ordinary members as archive.zip|member and
        // extracts them only when the sound source is opened.  Do not reuse
        // the skin cache extraction path here: persisted playlists must not
        // depend on files under %TEMP%.
        try {
            const auto package = skin::SkinPackage::Open(path);
            const size_t before = tracks.size();
            for (const auto& entry : package.Entries()) {
                std::wstring member;
                try { member = audio::DecodeZipMemberName(entry.name); }
                catch (const std::exception&) { continue; }
                if (member.empty() || member.back() == L'\\') continue;
                const auto logical = audio::MakeZipMemberPath(path, member);
                const auto member_extension = LowerExtension(logical);
                if (member_extension == L".cue") {
                    // 00474051 calls the archive read vtable at +0x14 and
                    // passes both those bytes and `archive|member` to
                    // 00473EC5.  Playlist rows retain the logical CUE name;
                    // CueSegmentSource repeats the same memory parse when a
                    // sub-track is opened.
                    try {
                        const auto sheet = audio::CueSheet::LoadFromMemory(
                            package.ReadEntry(entry.name, ttpcomm_module_), logical);
                        for (const auto& cue_track : sheet.Tracks()) {
                            tracks.push_back({
                                logical,
                                ImportedTrackTitle(logical), {}, -2,
                                cue_track.number, {}});
                        }
                    } catch (const std::exception&) {
                        // Like 00474051, a malformed individual CUE member
                        // does not abort enumeration of the remaining archive.
                    }
                    continue;
                }
                // CDA and MIDI use device/file-name backends in the rebuild;
                // they cannot consume the archive IStream boundary.
                if (member_extension == L".cda" || member_extension == L".mid" ||
                    member_extension == L".midi" || member_extension == L".rmi")
                    continue;
                bool supported = IsBuiltInAudioPath(logical);
                if (!supported) {
                    supported = std::ranges::any_of(reader_formats_,
                        [&](const auto& format) {
                            return PatternMatchesPath(format.pattern, logical);
                        });
                }
                if (supported) {
                    tracks.push_back(
                        {logical, ImportedTrackTitle(logical), {}, -2});
                }
            }
            return tracks.size() != before;
        } catch (const std::exception&) {
            return false;
        }
    }
    if (extension == L".rar") {
        // 004738B5 selects a distinct RAR archive object, but 00474051 keeps
        // the same archive|member identity.  The rebuild uses Windows' RAR
        // namespace as the decompressor while ensuring cache paths never leak
        // into TTBL persistence.
        try {
            const size_t before = tracks.size();
            for (const auto& member : audio::ListRarArchiveMembers(path)) {
                const auto logical = audio::MakeArchiveMemberPath(path, member);
                const auto member_extension = LowerExtension(logical);
                if (member_extension == L".cue") {
                    try {
                        audio::ArchiveMemberPath archive_member;
                        if (!audio::ParseArchiveMemberPath(
                                logical.native(), archive_member)) continue;
                        const auto sheet = audio::CueSheet::LoadFromMemory(
                            audio::ReadArchiveMember(archive_member,
                                                     ttpcomm_module_),
                            logical);
                        for (const auto& cue_track : sheet.Tracks()) {
                            tracks.push_back({
                                logical,
                                ImportedTrackTitle(logical), {}, -2,
                                cue_track.number, {}});
                        }
                    } catch (const std::exception&) {
                    }
                    continue;
                }
                if (member_extension == L".cda" || member_extension == L".mid" ||
                    member_extension == L".midi" || member_extension == L".rmi")
                    continue;
                bool supported = IsBuiltInAudioPath(logical);
                if (!supported) {
                    supported = std::ranges::any_of(reader_formats_,
                        [&](const auto& format) {
                            return PatternMatchesPath(format.pattern, logical);
                        });
                }
                if (supported) {
                    tracks.push_back(
                        {logical, ImportedTrackTitle(logical), {}, -2});
                }
            }
            return tracks.size() != before;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool supported = IsBuiltInAudioPath(path);
    if (!supported) {
        supported = std::ranges::any_of(reader_formats_, [&](const auto& format) {
            return PatternMatchesPath(format.pattern, path);
        });
    }
    // FUN_004739DB type 1 accepts an explicitly supplied existing file even
    // if no reader recognizes it.  Recursive directory scans only admit the
    // type-2 audio/CUE/archive classes.
    if (directory_member && !supported) return false;
    tracks.push_back({path, {}, {}, -2});
    return true;
}

bool PlayerWindow::CommitImportedTracks(std::vector<playlist::Track> tracks,
                                        size_t playlist_index,
                                        size_t insertion,
                                        bool replace_current,
                                        ImportPlayback playback) {
    if (playlist_index >= playlists_.Size()) return false;
    // FUN_0048059D calls 00480386 only when the temporary batch is non-empty.
    // A successful replace operation has already requested 0047F67C and must
    // still clear; an empty append is a strict no-op, including selection and
    // delayed-save counters.
    if (tracks.empty() && !replace_current) return false;
    auto& destination = playlists_.At(playlist_index);
    const bool is_active = playlist_index == playlists_.ActiveIndex();
    // `current_` is an index into the list identified by
    // `playing_playlist_index_`. Structural insertion must keep that live
    // window/player reference attached to the same track in addition to the
    // Playlist object's own +0x1c adjustment.
    const bool has_current_item = playing_playlist_index_ && current_ &&
        *playing_playlist_index_ == playlist_index &&
        *current_ < destination.Tracks().size();
    // 00480386 auto-starts an appended batch only when the active native list
    // has no playing marker (+0x1c) and player message 0x7f3 reports no open
    // source. TTBL's fourth DWORD is this per-list +0x1c marker; the transient
    // ListCtrl caret is the distinct +0x20 field and must not enter the gate.
    // The marker survives explicit stop/natural completion even after 0x7f3
    // reports no open source.
    const bool destination_has_playing_marker =
        destination.PlayingRow().has_value();
    const bool was_idle = !destination_has_playing_marker &&
                          !playback_source_open_;

    const size_t inserted_count = tracks.size();
    bool shift_playing_item{};
    if (replace_current) {
        // Build the replacement before touching decoder/current state.  This
        // keeps a vector-allocation failure from leaving the live list half
        // cleared, while preserving 0047F67C's observable stop/clear ordering
        // once construction has succeeded.
        playlist::Playlist replacement = destination;
        replacement.Clear();
        replacement.InsertRange(0, std::move(tracks));
        // 0048059D:004807B7 and CPlayerWnd::Drop:0045A8BE call
        // 0047F67C before replacing the destination, irrespective of which
        // catalogue entry currently owns the decoder.  Conditioning this on
        // `is_playing` leaves an unrelated list audibly playing after the
        // active list has been replaced.
        Stop();
        ClearPersistedPlaybackIdentity();
        current_.reset();
        playing_playlist_index_.reset();
        opened_track_.reset();
        ClearLyrics();
        destination = std::move(replacement);
        insertion = 0;
    } else {
        insertion = std::min(insertion, destination.Tracks().size());
        shift_playing_item = has_current_item &&
                             insertion <= *current_ && inserted_count != 0;
        destination.InsertRange(insertion, std::move(tracks));
        // Commit dependent indices only after vector insertion succeeds.
        if (shift_playing_item) current_ = *current_ + inserted_count;
    }

    const size_t first = insertion;
    insertion += inserted_count;
    const size_t edit_count = std::max<size_t>(1, inserted_count);
    playlists_.MarkDirty(playlist_index, edit_count > UINT_MAX
        ? UINT_MAX : static_cast<unsigned int>(edit_count));
    if (settings_.playlist.read_info_mode == 1 && inserted_count != 0) {
        // 00480386 enters FUN_004801E7 only for the "read when added"
        // profile.  Queue the complete inserted span; the runtime drains it
        // through one private reader session at a time.
        QueuePlaylistInfoRange(playlist_index, first, inserted_count);
    }

    if (shift_playing_item && current_ &&
        *current_ < destination.Tracks().size()) {
        const auto& playing = destination.Tracks()[*current_];
        display_title_ = std::to_wstring(*current_ + 1) + L"." +
                         DisplayName(playing);
        if (title_) SetWindowTextW(title_, display_title_.c_str());
        RebuildSkinInfoItems(true);
        ResetSkinInfoScroll();
    }

    if (is_active) {
        playlist_selected_rows_.clear();
        for (size_t index = first; index < insertion; ++index)
            playlist_selected_rows_.insert(index);
        playlist_selection_ = first < insertion
            ? std::optional<size_t>{first} : std::nullopt;
        playlist_selection_anchor_ = playlist_selection_;
        RememberPlaylistRow(playlist_index, playlist_selection_);
        playlist_scroll_ = std::min(playlist_scroll_, destination.Tracks().size());
        EnsurePlaylistSelectionVisible();
        RefreshPlaylist();
    } else if (playlist_window_) {
        InvalidateRect(playlist_window_, nullptr, FALSE);
    }

    if (first == insertion) {
        if (replace_current) RefreshPlaybackUi();
        return false;
    }
    if (playback == ImportPlayback::force ||
        (playback == ImportPlayback::if_idle && was_idle && is_active)) {
        // 00480386 (file intake) and 0045A8BE (main-window OLE drop) dispatch
        // 0047FEA3 directly.  A failed auto-open clears the persisted source
        // identity but does not enter the interactive play-command error UI.
        SelectTrackFrom(playlist_index, first, false);
        PlayCurrent(false);
    }
    return true;
}

bool PlayerWindow::ImportFiles(
    const std::vector<std::filesystem::path>& paths, size_t playlist_index,
    std::optional<size_t> insertion, bool replace_current,
    ImportPlayback playback) {
    if (playlist_index >= playlists_.Size()) return false;
    std::vector<playlist::Track> tracks;
    for (const auto& path : paths)
        static_cast<void>(CollectImportedTracks(path, tracks));
    const size_t target = insertion.value_or(
        playlists_.At(playlist_index).Tracks().size());
    return CommitImportedTracks(std::move(tracks), playlist_index, target,
                                replace_current, playback);
}

void PlayerWindow::ChooseFiles(bool replace_and_play, HWND owner) {
    // Keep 00464A2E/0048059D's post-selection transaction, but use the Vista+
    // shell item dialog requested by the rebuilt application.  Main Open and
    // playlist Add Files share this path, so neither route can silently fall
    // back to the old OPENFILENAME hook.
    LeaveFullScreen();
    std::vector<std::filesystem::path> paths;
    {
        if (file_dialog_active_) return;
        struct DialogGuard {
            bool& active;
            explicit DialogGuard(bool& value) : active(value) { active = true; }
            ~DialogGuard() { active = false; }
        } guard(file_dialog_active_);
        auto filter = BuildAudioDialogFilter(ResourceModule(), reader_formats_);
        ModernOpenFileOptions dialog;
        dialog.owner = owner ? owner : window_;
        dialog.filters = ParseLegacyDialogFilter(
            std::span<const wchar_t>(filter.data(), filter.size()));
        dialog.initial_path = PreferredDialogHistory(
            settings_.history.sound_path, file_dialog_initial_directory_);
        dialog.allow_multiple = true;
        auto selected = ModernOpenFiles(dialog);
        if (!selected) return;
        paths = std::move(*selected);

        // 0048059D:0048074B publishes the complete single-selection path,
        // or the common multi-select directory with its trailing separator,
        // into Histroy/SoundPath.
        file_dialog_initial_directory_ = FileSelectionHistory(paths);
        settings_.history.sound_path = file_dialog_initial_directory_;
    }
    // DAT_00548FEC is cleared before 00480386 mutates/refreshes the list.
    // The nested guard above deliberately ends before this call.
    static_cast<void>(ImportFiles(paths, playlists_.ActiveIndex(),
        std::nullopt, replace_and_play,
        replace_and_play ? ImportPlayback::force : ImportPlayback::if_idle));
}

void PlayerWindow::ChooseFolder() {
    ModernFolderOptions dialog;
    dialog.owner = playlist_window_ ? playlist_window_ : window_;
    dialog.initial_path = PreferredDialogHistory(
        settings_.history.folder, file_dialog_initial_directory_);
    const auto selected = ModernPickFolder(dialog);
    if (!selected) return;
    file_dialog_initial_directory_ = selected->path;
    settings_.history.folder = selected->path;
    static_cast<void>(ImportFiles({selected->path},
        playlists_.ActiveIndex(), std::nullopt, false,
        ImportPlayback::if_idle));
}

bool PlayerWindow::CanPlayCompactDisc() const noexcept {
    return !OpticalDrives().empty();
}

void PlayerWindow::ShowPlayUrlDialog() {
    PlayUrlDialogState state;
    const INT_PTR result = DialogBoxParamW(
        ResourceModule(), MAKEINTRESOURCEW(204), window_, PlayUrlDialogProc,
        reinterpret_cast<LPARAM>(&state));
    if (result == kPlayUrlBrowse) {
        ChooseFiles(true, window_);
        return;
    }
    if (result != IDOK || state.value.empty()) return;
    if (OpenPath(std::filesystem::path(state.value), true) &&
        state.value.find(L"://") != std::wstring::npos) {
        // FUN_00483A64 records a successfully materialized item only when
        // FUN_0041B6EA finds the literal "://", using SHARD_PATHW (3), after
        // the item has entered the current list.
        SHAddToRecentDocs(SHARD_PATHW, state.value.c_str());
    }
}

void PlayerWindow::ShowPlayCdDialog() {
    PlayCdDialogState state;
    state.drives = OpticalDrives();
    state.add = [this](const std::vector<std::filesystem::path>& paths) {
        static_cast<void>(ImportFiles(
            paths, playlists_.ActiveIndex(), std::nullopt, false,
            ImportPlayback::none));
    };
    const INT_PTR result = DialogBoxParamW(
        ResourceModule(), MAKEINTRESOURCEW(229), window_, PlayCdDialogProc,
        reinterpret_cast<LPARAM>(&state));
    if (result != IDOK || state.selected.empty()) return;
    // FUN_0048398F replaces the current working list with all audio tracks (or
    // MPEGAV DAT files) returned by CPlayAudioCDDlg, then starts its first row.
    static_cast<void>(ImportFiles(
        state.selected, playlists_.ActiveIndex(), 0, true,
        ImportPlayback::force));
}

bool PlayerWindow::OpenPath(const std::filesystem::path& path,
                            bool start_playback) {
    // OpenPath is also the command-line/single-instance route.  It is not the
    // main window's 0xE101 Open command: starting a supplied media path must
    // not silently inherit that command's replace-list flag.  Preserve the
    // existing list for ordinary media/CUE input and put a started item at the
    // front; an explicitly supplied playlist remains a replace operation.
    const bool replace = IsPlaylistFile(path);
    const std::optional<size_t> insertion = replace
        ? std::optional<size_t>{0}
        : (start_playback ? std::optional<size_t>{0} : std::nullopt);
    return ImportFiles({path}, playlists_.ActiveIndex(), insertion,
        replace, start_playback ? ImportPlayback::force : ImportPlayback::none);
}

void PlayerWindow::OpenCommandLinePath(const std::filesystem::path& path,
                                       ULONG_PTR raw_mode) {
    auto mode = static_cast<CommandLineFileMode>(raw_mode);
    if (mode != CommandLineFileMode::open_and_play &&
        mode != CommandLineFileMode::append &&
        mode != CommandLineFileMode::control_packet) {
        mode = CommandLineFileMode::open_and_play;
    }

    // /e is not an enqueue spelling in 5.7.9. FUN_0045F2EF parses the inline
    // <ctrlparam> argument and FUN_00463BE5 handles message 0x784/wParam=3.
    if (mode == CommandLineFileMode::control_packet) {
        const auto packet = ParsePlayerControlPacketXml(path.native());
        if (!packet) return;
        const auto route = RoutePlayerControlPacket(*packet);

        size_t playlist_index = playlists_.ActiveIndex();
        if (route.select_playlist) {
            // The original opens ttpres.dll dialog 223 and permits choosing
            // an existing destination or its 0x7F01 "new list" row.
            ControlPacketPlaylistDialogState state{&playlists_};
            if (DialogBoxParamW(ResourceModule(), MAKEINTRESOURCEW(223),
                    window_, ControlPacketPlaylistDialogProc,
                    reinterpret_cast<LPARAM>(&state)) != IDOK) {
                return;
            }
            playlist_index = state.selected;
            if (playlist_index == playlists_.Size()) {
                playlist_index = playlists_.NewList(DefaultImportedListTitle(
                    ResourceModule(), playlists_.Size() + 1));
                settings_.player.playlist_scan_count =
                    static_cast<int>(playlists_.Size());
            }
            if (playlist_index >= playlists_.Size()) return;
            SwitchPlaylist(playlist_index);
        } else if (settings_.general.default_list_on_command) {
            std::wstring title = settings_.general.default_list;
            if (title.empty()) title = ResourceText(0x813a);
            auto found = playlists_.Size();
            for (size_t index = 0; index < playlists_.Size(); ++index) {
                if (_wcsicmp(playlists_.At(index).Title().c_str(),
                             title.c_str()) == 0) {
                    found = index;
                    break;
                }
            }
            if (found == playlists_.Size()) found = playlists_.NewList(title);
            SwitchPlaylist(found);
            playlist_index = found;
        }

        std::vector<playlist::Track> tracks;
        tracks.reserve(packet->songs.size());
        for (const auto& song : packet->songs) {
            // FUN_00463BE5 skips empty URL rows after parsing but retains the
            // packet-provided Artist and Title on the created CPlayItem.
            if (song.url.empty()) continue;
            try {
                playlist::Track track;
                track.path = std::filesystem::path(song.url);
                track.artist = core::WideToUtf8(song.artist);
                track.title = core::WideToUtf8(song.title);
                tracks.push_back(std::move(track));
            } catch (const std::exception&) {
                // One malformed conversion is the analogue of an individual
                // FUN_004742DD create failure; remaining rows still import.
            }
        }
        const bool replace = route.start_playback &&
                             settings_.general.clear_list_on_command;
        const size_t insertion = route.insert_at_head
            ? 0 : playlists_.At(playlist_index).Tracks().size();
        const bool imported = CommitImportedTracks(
            std::move(tracks), playlist_index, insertion, replace,
            route.start_playback ? ImportPlayback::force
                                 : ImportPlayback::none);
        if (imported && playlist_window_ && IsWindow(playlist_window_))
            BringWindowToTop(playlist_window_);
        return;
    }

    // FUN_00460EDF turns a second ordinary request received within one second
    // into the /a transaction so two shell launches cannot repeatedly clear
    // and restart the working list.
    const DWORD now = GetTickCount();
    if (mode == CommandLineFileMode::open_and_play && last_command_line_tick_ &&
        now - last_command_line_tick_ < 1000U) {
        mode = CommandLineFileMode::append;
    }
    last_command_line_tick_ = now;

    if (settings_.general.default_list_on_command) {
        std::wstring title = settings_.general.default_list;
        if (title.empty()) title = ResourceText(0x813a);
        auto found = playlists_.Size();
        for (size_t index = 0; index < playlists_.Size(); ++index) {
            if (_wcsicmp(playlists_.At(index).Title().c_str(), title.c_str()) == 0) {
                found = index;
                break;
            }
        }
        if (found == playlists_.Size()) found = playlists_.NewList(title);
        SwitchPlaylist(found);
    }

    const bool play = mode == CommandLineFileMode::open_and_play;
    const bool replace = play && settings_.general.clear_list_on_command;
    static_cast<void>(ImportFiles(
        {path}, playlists_.ActiveIndex(), replace ? std::optional<size_t>{0}
                                                  : std::nullopt,
        replace, play ? ImportPlayback::force : ImportPlayback::none));
}

void PlayerWindow::HandleDroppedFiles(FileDropSurface surface, IDataObject* data,
                                      DWORD, POINTL point, DWORD* effect) {
    const DWORD incoming_effect = effect ? *effect : DROPEFFECT_NONE;
    if (effect) *effect = DROPEFFECT_NONE;
    auto paths = ExtractDropPaths(data);
    if (paths.empty()) {
        // 0045A8BE clears the active list even when a data object which passed
        // DragEnter subsequently fails to materialize CF_HDROP.  Playlist and
        // lyric targets retain their own empty/failure behavior.
        if (surface == FileDropSurface::player) {
            static_cast<void>(ImportFiles({}, playlists_.ActiveIndex(), 0,
                true, ImportPlayback::force));
        }
        return;
    }

    if (surface == FileDropSurface::lyric) {
        LoadDroppedLyrics(paths.front());
        if (effect) *effect = DROPEFFECT_LINK;
        return;
    }
    if (surface == FileDropSurface::player) {
        if (LowerExtension(paths.front()) == L".skn") {
            const auto runtime = RuntimeDirectory();
            bool copied{};
            std::filesystem::path installed;
            if (!runtime.empty()) {
                const auto skin_directory = runtime / L"Skin";
                std::error_code error;
                std::filesystem::create_directories(skin_directory, error);
                if (!error) {
                    const auto source = std::filesystem::absolute(
                        paths.front(), error);
                    installed = skin_directory / paths.front().filename();
                    const auto destination = std::filesystem::absolute(
                        installed, error);
                    if (!error) {
                        copied = _wcsicmp(source.c_str(), destination.c_str()) == 0 ||
                                 CopyFileW(source.c_str(), destination.c_str(), FALSE);
                    }
                }
            }
            if (copied) {
                SaveCurrentSkinProfile();
                static_cast<void>(LoadSkinPackage(installed));
                // The catalogue scan is a preload optimization, not part of
                // the skin-switch transaction.  Waiting for it here blocks
                // the OLE STA (and therefore every player window) while a
                // large Skin directory is parsed.  Preserve the last complete
                // snapshot, discard an in-flight pre-copy result once ready,
                // and refresh it asynchronously.
                InvalidateSkinMenuCatalog();
                StartSkinMenuCatalogLoad();
                // 0045A8BE returns without assigning *pdwEffect after a
                // successful skin install, so the source-provided mask is
                // retained rather than being normalized to LINK.
                if (effect) *effect = incoming_effect;
                return;
            }
            // 0045A8BE consumes the drop only after CopyFile succeeds.  A
            // failed install falls through to the ordinary batch path.
        }
        auto classified = ClassifyExternalDropPaths(paths);
        const bool consumed_playlist = !classified.playlists.empty();
        auto ordered = OriginalExternalImportOrder(std::move(classified));
        const bool imported = ImportFiles(ordered, playlists_.ActiveIndex(), 0,
                                          true, ImportPlayback::force);
        if ((imported || consumed_playlist) && effect) {
            *effect = DROPEFFECT_LINK;
        }
        return;
    }

    if (!playlist_window_ || !skin_ || !skin_->Playlist().valid) return;
    const POINT screen{point.x, point.y};
    RECT list_bounds{};
    GetWindowRect(playlist_list_control_, &list_bounds);

    // 004822AD re-runs the native ListCtrl hit test at drop time.  Its
    // PlayLists rectangle is not inflated here; a point in the two-pixel
    // transition band therefore follows the Files branch selected by
    // 0048218C.
    const bool over_catalogue = !settings_.playlist.library_mode &&
                                settings_.playlist.split_on_lists > 0 &&
                                PtInRect(&list_bounds, screen);
    if (!over_catalogue) {
        POINT track_point = screen;
        ScreenToClient(playlist_track_control_, &track_point);
        const LRESULT hit_row = OriginalListDropHit(playlist_track_control_,
                                                    track_point);
        const size_t insertion = hit_row >= 0
            ? static_cast<size_t>(hit_row) : ActivePlaylist().Tracks().size();
        auto classified = ClassifyExternalDropPaths(paths);
        const bool consumed_playlist = !classified.playlists.empty();
        auto ordered = OriginalExternalImportOrder(std::move(classified));
        const bool imported = ImportFiles(ordered, playlists_.ActiveIndex(),
                                          insertion, false,
                                          ImportPlayback::none);
        if (imported || consumed_playlist) {
            if (playlist_track_control_) SetFocus(playlist_track_control_);
            if (effect) *effect = DROPEFFECT_LINK;
        }
        return;
    }
    POINT list_point = screen;
    ScreenToClient(playlist_list_control_, &list_point);
    const LRESULT list_row = OriginalListDropHit(playlist_list_control_,
                                                list_point);
    // FUN_0041CB83 returns itemCount for its synthetic row immediately below
    // the final item.  004822AD explicitly treats hit >= itemCount as blank
    // catalogue space, not as an addressable playlist index.
    const std::optional<size_t> hit =
        list_row >= 0 && static_cast<size_t>(list_row) < playlists_.Size()
        ? std::optional<size_t>{static_cast<size_t>(list_row)} : std::nullopt;
    auto classified = ClassifyExternalDropPaths(paths);
    auto& ordinary = classified.ordinary;
    auto& list_files = classified.playlists;
    const bool consumed_playlist = !list_files.empty();
    // 004824C4 passes the raw catalogue hit to FUN_00478E4E.  A true blank
    // hit is -1: the first playlist is appended, then 004824DE increments the
    // signed cursor to zero, so the second playlist is inserted at the front.
    // If ordinary files exist, 004823DD first normalizes blank to itemCount;
    // their newly-created list is consequently kept after the imported
    // playlist files.  Preserve the -1 sentinel instead of collapsing both
    // paths to itemCount.
    std::optional<size_t> list_insertion;
    if (hit) list_insertion = *hit;
    else if (!ordinary.empty()) list_insertion = playlists_.Size();
    bool imported{};
    if (hit && !ordinary.empty()) {
        std::vector<playlist::Track> tracks;
        for (const auto& path : ordinary)
            static_cast<void>(CollectImportedTracks(path, tracks));
        if (!tracks.empty()) {
            SwitchPlaylist(*hit);
            imported = CommitImportedTracks(std::move(tracks), *hit,
                playlists_.At(*hit).Tracks().size(), false,
                ImportPlayback::none);
        }
    } else if (!ordinary.empty()) {
        std::vector<playlist::Track> tracks;
        for (const auto& path : ordinary)
            static_cast<void>(CollectImportedTracks(path, tracks));
        if (!tracks.empty()) {
            std::wstring title;
            // 00481F2A overwrites the temporary list title for each directory
            // it encounters; consequently the last directory wins in a
            // mixed, multi-directory drop.
            for (const auto& path : ordinary) {
                std::error_code error;
                if (std::filesystem::is_directory(path, error) && !error)
                    title = path.filename().wstring();
            }
            if (title.empty())
                title = DefaultImportedListTitle(ResourceModule(), playlists_.Size() + 1);
            const size_t created = playlists_.NewList(std::move(title));
            imported = CommitImportedTracks(std::move(tracks), created, 0,
                false, ImportPlayback::none);
            if (imported) {
                // 004822AD blank-catalogue branch creates the list through
                // 0047928F/0047F294 but does not run 00480386, so the imported
                // rows are not selected. Existing-row and Files-surface drops
                // do run their selection path and retain CommitImportedTracks'
                // normal imported-range selection.
                playlist_selected_rows_.clear();
                playlist_selection_.reset();
                playlist_selection_anchor_.reset();
                RememberPlaylistRow(created, std::nullopt);
            }
        }
    }
    for (const auto& list_file : list_files) {
        try {
            const size_t requested_insertion =
                list_insertion.value_or(playlists_.Size());
            const size_t inserted_at = playlists_.InsertList(
                list_file, requested_insertion,
                {settings_.playlist.ignore_bad_files});
            // The native signed cursor advances even when loading a playlist
            // fails. In particular, -1 becomes zero after the first item.
            if (list_insertion) ++*list_insertion;
            else list_insertion = 0;
            if (inserted_at == playlist::PlaylistStore::npos) continue;
            // The store uses vector indices, whereas the native catalogue
            // keeps the playing CPlayList object alive while inserting a new
            // list before it. Preserve that identity in the rebuilt index.
            if (playing_playlist_index_ &&
                inserted_at <= *playing_playlist_index_) {
                ++*playing_playlist_index_;
            }
            // 004824FF..00482509 activates item-count - 1 after every
            // imported playlist.  Inserting at a catalogue row therefore
            // does not activate the newly inserted list; the pre-existing
            // final list (or the ordinary-input list just appended on a
            // blank drop) remains active.
            SwitchPlaylist(playlists_.Size() - 1);
            imported = true;
        } catch (const std::exception&) {}
    }
    if (imported) {
        settings_.player.playlist_scan_count = static_cast<int>(playlists_.Size());
        settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
        RefreshPlaylist();
    }
    if (imported || consumed_playlist) {
        // 004822AD focuses +0x548 (Files) after either drop branch.
        if (playlist_track_control_) SetFocus(playlist_track_control_);
        if (effect) *effect = DROPEFFECT_LINK;
    }
}

} // namespace ttplayer::ui
