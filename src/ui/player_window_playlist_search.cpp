#include "ttplayer/ui/player_window.h"

#include "file_info_probe_client.h"
#include "modern_file_dialog.h"
#include "player_window_internal.h"
#include "ttplayer/ui/playlist_local_search.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

namespace ttplayer::ui {
namespace {

constexpr int kSearchRoot = 0x404;
constexpr int kSearchToggle = 0x416;
constexpr int kSearchTypes = 0x898;
constexpr int kSearchSelectAllTypes = 0x899;
constexpr int kSearchClearAllTypes = 0x89a;
constexpr int kSearchMinimumEnabled = 0x89c;
constexpr int kSearchMinimumSeconds = 0x3f5;
constexpr int kSearchResults = 0x428;
constexpr int kSearchStatus = 0x41c;
constexpr int kSearchAddSelected = 0x403;
constexpr UINT kSearchUpdateMessage = WM_APP + 0x316;
constexpr size_t kSearchBatchSize = 16;

std::wstring_view Trim(std::wstring_view value) noexcept {
    while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
    while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
    return value;
}

bool PatternTokenMatches(std::wstring_view token,
                         const std::filesystem::path& path) {
    token = Trim(token);
    if (token.empty()) return false;
    if (_wcsicmp(std::wstring(token).c_str(), L"*") == 0 ||
        _wcsicmp(std::wstring(token).c_str(), L"*.*") == 0) return true;
    const size_t dot = token.rfind(L'.');
    if (dot == std::wstring_view::npos) return false;
    const auto extension = path.extension().wstring();
    return !extension.empty() &&
        _wcsicmp(std::wstring(token.substr(dot)).c_str(),
                 extension.c_str()) == 0;
}

bool PatternListMatches(std::wstring_view patterns,
                        const std::filesystem::path& path) {
    size_t begin{};
    while (begin <= patterns.size()) {
        const size_t end = patterns.find(L';', begin);
        const auto token = patterns.substr(begin,
            end == std::wstring_view::npos ? patterns.size() - begin
                                           : end - begin);
        if (PatternTokenMatches(token, path)) return true;
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return false;
}

bool HasSearchPattern(std::wstring_view patterns) {
    return PatternListMatches(patterns, L"probe.any-extension") ||
        patterns.find(L'.') != std::wstring_view::npos;
}

const PlaylistLocalSearchFormat* MatchingFormat(
    const std::filesystem::path& path,
    std::span<const PlaylistLocalSearchFormat> formats) {
    const auto found = std::find_if(formats.begin(), formats.end(),
        [&path](const auto& format) {
            return PatternListMatches(format.pattern, path);
        });
    return found == formats.end() ? nullptr : &*found;
}

std::wstring WindowText(HWND window) {
    if (!window) return {};
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, value.data(), length + 1);
    value.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return value;
}

std::vector<std::wstring> Split(std::wstring_view value, wchar_t delimiter) {
    std::vector<std::wstring> result;
    size_t begin{};
    while (begin <= value.size()) {
        const size_t end = value.find(delimiter, begin);
        result.emplace_back(value.substr(begin,
            end == std::wstring_view::npos ? value.size() - begin
                                           : end - begin));
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return result;
}

std::filesystem::path RuntimeDirectory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::filesystem::path InitialSearchRoot(std::filesystem::path value) {
    // A remembered UNC path may be offline.  Merely constructing a path is
    // non-blocking; all existence/type checks belong to the search worker.
    if (!value.empty()) return value;
    return RuntimeDirectory();
}

std::wstring MetadataValue(const detail::FileInfoProbeReadResult& probe,
                           std::wstring_view name) {
    const auto found = std::find_if(probe.metadata.begin(),
        probe.metadata.end(), [name](const auto& item) {
            return _wcsicmp(item.name.c_str(),
                            std::wstring(name).c_str()) == 0;
        });
    return found == probe.metadata.end() ? std::wstring{} : found->value;
}

struct LocalSearchResult {
    std::filesystem::path path;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring format;
};

struct LocalSearchUpdate {
    std::uint64_t generation{};
    std::vector<LocalSearchResult> results;
    std::filesystem::path current_path;
    bool completed{};
    bool cancelled{};
};

struct LocalSearchReceiver {
    std::mutex mutex;
    std::stop_source stop;
    HWND target{};
    std::uint64_t generation{};
    bool stopped{};
};

struct LocalSearchRequest {
    std::vector<PlaylistLocalSearchRoot> roots;
    std::vector<PlaylistLocalSearchFormat> formats;
    bool minimum_enabled{true};
    std::uint32_t minimum_seconds{30};
    std::filesystem::path helper;
    std::filesystem::path addin_directory;
    std::filesystem::path ttpcomm_path;
    std::uint64_t generation{};
};

struct LocalSearchDialogState {
    std::filesystem::path root;
    std::vector<PlaylistLocalSearchRoot> locations;
    std::vector<PlaylistLocalSearchFormat> formats;
    std::vector<LocalSearchResult> results;
    std::vector<std::filesystem::path> accepted_paths;
    std::vector<std::wstring> columns;
    std::wstring custom_folder_text;
    std::wstring folder_picker_title;
    std::wstring start_text;
    std::wstring stop_text;
    std::wstring status_format;
    std::shared_ptr<LocalSearchReceiver> receiver;
    std::uint64_t generation{};
    int custom_folder_index{-1};
    size_t selected_location{};
    int sort_column{-1};
    bool sort_ascending{true};
    bool searching{};
    bool folder_picker_open{};
};

bool PostSearchUpdate(const std::shared_ptr<LocalSearchReceiver>& receiver,
                      std::unique_ptr<LocalSearchUpdate> update,
                      std::stop_token stop) {
    for (;;) {
        bool posted{};
        {
            std::scoped_lock lock(receiver->mutex);
            if (receiver->stopped || !receiver->target ||
                !IsWindow(receiver->target)) return false;
            posted = PostMessageW(receiver->target, kSearchUpdateMessage,
                static_cast<WPARAM>(receiver->generation),
                reinterpret_cast<LPARAM>(update.get())) != FALSE;
        }
        if (posted) {
            static_cast<void>(update.release());
            return true;
        }
        if (stop.stop_requested()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::optional<LocalSearchResult> InspectSearchCandidate(
    const LocalSearchRequest& request, const std::filesystem::path& path,
    const PlaylistLocalSearchFormat& format, std::stop_token stop) {
    LocalSearchResult result;
    result.path = path;
    result.title = path.stem().wstring();
    result.format = format.description;

    int duration_ms{-1};
    const auto probe = detail::RunPlaylistInfoReadProbe(
        stop, request.helper, request.addin_directory, path,
        request.ttpcomm_path, 0, 15000);
    if (probe && SUCCEEDED(probe->status)) {
        duration_ms = probe->duration_ms > static_cast<DWORD>(INT_MAX)
            ? INT_MAX : static_cast<int>(probe->duration_ms);
        auto title = MetadataValue(*probe, L"title");
        if (!title.empty()) result.title = std::move(title);
        result.artist = MetadataValue(*probe, L"artist");
        if (result.artist.empty())
            result.artist = MetadataValue(*probe, L"author");
        result.album = MetadataValue(*probe, L"album");
        if (!probe->codec.empty()) result.format = probe->codec;
    }
    if (!PlaylistLocalSearchPassesMinimum(duration_ms,
            request.minimum_enabled, request.minimum_seconds))
        return std::nullopt;
    return result;
}

void RunLocalSearch(LocalSearchRequest request,
                    std::shared_ptr<LocalSearchReceiver> receiver) {
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComGuard {
        HRESULT status;
        ~ComGuard() {
            if (SUCCEEDED(status)) CoUninitialize();
        }
    } com_guard{com_status};
    const auto stop = receiver->stop.get_token();
    std::vector<LocalSearchResult> pending;
    pending.reserve(kSearchBatchSize);
    std::filesystem::path current;
    size_t scanned{};

    const auto publish = [&](bool completed) {
        auto update = std::make_unique<LocalSearchUpdate>();
        update->generation = request.generation;
        update->results = std::move(pending);
        update->current_path = current;
        update->completed = completed;
        update->cancelled = stop.stop_requested();
        pending.clear();
        pending.reserve(kSearchBatchSize);
        return PostSearchUpdate(receiver, std::move(update), stop);
    };

    std::set<std::wstring> visited;
    for (const auto& root_spec : request.roots) {
        if (stop.stop_requested()) break;

        std::filesystem::path resolved;
        if (root_spec.kind == PlaylistLocalSearchRootKind::shell_csidl) {
            // CSIDL_DRIVES is expanded by BuildPlaylistLocalSearchRootPlan.
            if (root_spec.csidl == CSIDL_DRIVES) continue;
            std::array<wchar_t, MAX_PATH> folder{};
            PIDLIST_ABSOLUTE item{};
            if (SUCCEEDED(SHGetSpecialFolderLocation(
                    nullptr, root_spec.csidl, &item)) && item) {
                if (SHGetPathFromIDListW(item, folder.data()))
                    resolved = folder.data();
                CoTaskMemFree(item);
            }
            if (resolved.empty() && SUCCEEDED(SHGetFolderPathW(
                    nullptr, root_spec.csidl, nullptr,
                    SHGFP_TYPE_CURRENT, folder.data())))
                resolved = folder.data();
        } else {
            resolved = root_spec.path;
        }
        if (resolved.empty()) continue;

        // Volume discovery and availability checks intentionally occur only
        // in this worker. A disconnected network root or empty optical drive
        // must never stall resource 228's UI thread.
        if (root_spec.kind == PlaylistLocalSearchRootKind::drive) {
            const UINT type = GetDriveTypeW(resolved.c_str());
            if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
        }
        const DWORD attributes = GetFileAttributesW(resolved.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;

        auto key = resolved.lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(),
            [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
        if (!visited.insert(std::move(key)).second) continue;
        current = resolved;
        if (!publish(false)) return;

        try {
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(
                resolved,
                std::filesystem::directory_options::skip_permission_denied,
                error);
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end && !stop.stop_requested()) {
                current = iterator->path();
                std::error_code status_error;
                const bool regular = iterator->is_regular_file(status_error);
                if (regular) {
                    if (const auto* format = MatchingFormat(
                            current, request.formats)) {
                        auto result = InspectSearchCandidate(
                            request, current, *format, stop);
                        if (result) pending.push_back(std::move(*result));
                    }
                    ++scanned;
                }
                if (pending.size() >= kSearchBatchSize ||
                    (scanned != 0 && scanned % 128 == 0)) {
                    if (!publish(false)) return;
                }
                iterator.increment(error);
                if (error) error.clear();
            }
        } catch (const std::exception&) {
            // A broken root ends only that enumerator; the original Computer
            // selection continues with the remaining shell children.
        }
    }
    static_cast<void>(publish(true));
}

void AppendSearchResults(LocalSearchDialogState& state, HWND dialog,
                         std::vector<LocalSearchResult> results);
void SetSearchStatus(const LocalSearchDialogState& state, HWND dialog,
                     const std::filesystem::path& current);

void DrainSearchUpdates(HWND dialog, LocalSearchDialogState* state) {
    if (!dialog) return;
    MSG message{};
    while (PeekMessageW(&message, dialog, kSearchUpdateMessage,
                        kSearchUpdateMessage, PM_REMOVE)) {
        std::unique_ptr<LocalSearchUpdate> update(
            reinterpret_cast<LocalSearchUpdate*>(message.lParam));
        if (!state || !update ||
            update->generation != state->generation ||
            static_cast<std::uint64_t>(message.wParam) != state->generation)
            continue;
        AppendSearchResults(*state, dialog, std::move(update->results));
        SetSearchStatus(*state, dialog, update->current_path);
    }
}

void StopSearch(LocalSearchDialogState& state, HWND dialog,
                bool accept_queued_results = false) {
    auto receiver = std::move(state.receiver);
    if (receiver) {
        receiver->stop.request_stop();
        std::scoped_lock lock(receiver->mutex);
        receiver->stopped = true;
        receiver->target = nullptr;
    }
    DrainSearchUpdates(dialog, accept_queued_results ? &state : nullptr);
    state.searching = false;
}

struct SearchRootPresentation {
    std::wstring text;
    int image{-1};
    HIMAGELIST image_list{};
};

std::wstring FallbackShellName(int csidl) {
    switch (csidl) {
    case 17: return L"计算机";
    case 12: return L"CSIDL 12";
    case 13: return L"我的音乐";
    case 46: return L"公用文档";
    case 53: return L"公用音乐";
    default: return L"CSIDL " + std::to_wstring(csidl);
    }
}

SearchRootPresentation GenericFolderPresentation(std::wstring text) {
    SearchRootPresentation result;
    result.text = std::move(text);
    SHFILEINFOW info{};
    const DWORD_PTR image_list = SHGetFileInfoW(
        L"folder", FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (image_list) {
        result.image = info.iIcon;
        result.image_list = reinterpret_cast<HIMAGELIST>(image_list);
    }
    return result;
}

SearchRootPresentation PresentSearchRoot(
    HWND owner, const PlaylistLocalSearchRoot& root,
    std::wstring_view custom_text) {
    if (root.kind == PlaylistLocalSearchRootKind::shell_csidl) {
        SearchRootPresentation result =
            GenericFolderPresentation(FallbackShellName(root.csidl));
        PIDLIST_ABSOLUTE item{};
        if (SUCCEEDED(SHGetSpecialFolderLocation(owner, root.csidl, &item)) &&
            item) {
            SHFILEINFOW info{};
            const DWORD_PTR image_list = SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(item), 0, &info, sizeof(info),
                SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX |
                    SHGFI_SMALLICON);
            CoTaskMemFree(item);
            if (image_list) {
                if (info.szDisplayName[0]) result.text = info.szDisplayName;
                result.image = info.iIcon;
                result.image_list = reinterpret_cast<HIMAGELIST>(image_list);
            }
        }
        return result;
    }
    if (root.kind == PlaylistLocalSearchRootKind::drive) {
        SearchRootPresentation result;
        result.text = root.path.wstring();
        SHFILEINFOW info{};
        // USEFILEATTRIBUTES avoids probing an offline network/optical root on
        // the dialog thread while retaining the shared system icon index.
        const DWORD_PTR image_list = SHGetFileInfoW(
            root.path.c_str(), FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
            SHGFI_USEFILEATTRIBUTES | SHGFI_DISPLAYNAME |
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (image_list) {
            if (info.szDisplayName[0]) result.text = info.szDisplayName;
            result.image = info.iIcon;
            result.image_list = reinterpret_cast<HIMAGELIST>(image_list);
        }
        return result;
    }
    return GenericFolderPresentation(custom_text.empty()
        ? std::wstring(L"自定义...") : std::wstring(custom_text));
}

void InsertComboItem(HWND combo, int index, std::wstring_view text,
                     LPARAM data, int image) {
    std::wstring owned(text);
    COMBOBOXEXITEMW item{};
    item.mask = CBEIF_TEXT | CBEIF_LPARAM;
    if (image >= 0) {
        item.mask |= CBEIF_IMAGE | CBEIF_SELECTEDIMAGE;
        item.iImage = image;
        item.iSelectedImage = image;
    }
    item.iItem = index;
    item.pszText = owned.data();
    item.lParam = data;
    SendMessageW(combo, CBEM_INSERTITEMW, 0,
                 reinterpret_cast<LPARAM>(&item));
}

void PopulateRootCombo(LocalSearchDialogState& state, HWND dialog) {
    const HWND combo = GetDlgItem(dialog, kSearchRoot);
    if (!combo) return;
    while (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(combo, CBEM_DELETEITEM, 0, 0);
    state.custom_folder_index = -1;
    HIMAGELIST shared_images{};
    for (size_t index = 0; index < state.locations.size() &&
                           index <= static_cast<size_t>(INT_MAX); ++index) {
        const auto presentation = PresentSearchRoot(
            dialog, state.locations[index], state.custom_folder_text);
        if (!shared_images && presentation.image_list)
            shared_images = presentation.image_list;
        InsertComboItem(combo, static_cast<int>(index), presentation.text,
            static_cast<LPARAM>(index), presentation.image);
        if (state.locations[index].kind ==
            PlaylistLocalSearchRootKind::custom)
            state.custom_folder_index = static_cast<int>(index);
    }
    // SHGetFileInfo returns the process-wide shared system image list. The
    // ComboBoxEx borrows it; this dialog must never call ImageList_Destroy.
    if (shared_images)
        SendMessageW(combo, CBEM_SETIMAGELIST, 0,
                     reinterpret_cast<LPARAM>(shared_images));
    if (state.selected_location >= state.locations.size())
        state.selected_location = 0;
    SendMessageW(combo, CB_SETCURSEL,
        static_cast<WPARAM>(state.selected_location), 0);
}

void PopulateSearchTypes(LocalSearchDialogState& state, HWND dialog) {
    const HWND list = GetDlgItem(dialog, kSearchTypes);
    if (!list) return;
    ListView_SetExtendedListViewStyle(list,
        LVS_EX_LABELTIP | LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
    RECT bounds{};
    GetClientRect(list, &bounds);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = std::max(20L, bounds.right - bounds.left - 4);
    ListView_InsertColumn(list, 0, &column);
    for (size_t index = 0; index < state.formats.size() &&
                           index <= static_cast<size_t>(INT_MAX); ++index) {
        auto text = state.formats[index].description;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = text.data();
        item.lParam = static_cast<LPARAM>(index);
        const int row = ListView_InsertItem(list, &item);
        if (row >= 0) ListView_SetCheckState(list, row, TRUE);
    }
}

void PopulateSearchResults(LocalSearchDialogState& state, HWND dialog) {
    const HWND list = GetDlgItem(dialog, kSearchResults);
    if (!list) return;
    ListView_SetExtendedListViewStyle(list,
        LVS_EX_LABELTIP | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);
    static constexpr int widths[]{120, 100, 120, 50, 300};
    for (size_t column_index = 0; column_index < std::size(widths);
         ++column_index) {
        std::wstring text = column_index < state.columns.size()
            ? state.columns[column_index] : std::wstring{};
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.cx = widths[column_index];
        column.iSubItem = static_cast<int>(column_index);
        column.pszText = text.data();
        ListView_InsertColumn(list, static_cast<int>(column_index), &column);
    }
}

std::vector<PlaylistLocalSearchFormat> EnabledSearchFormats(
    const LocalSearchDialogState& state, HWND dialog) {
    std::vector<PlaylistLocalSearchFormat> formats;
    const HWND list = GetDlgItem(dialog, kSearchTypes);
    const int count = list ? ListView_GetItemCount(list) : 0;
    for (int row = 0; row < count; ++row) {
        if (!ListView_GetCheckState(list, row)) continue;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(list, &item) || item.lParam < 0) continue;
        const size_t index = static_cast<size_t>(item.lParam);
        if (index < state.formats.size()) formats.push_back(state.formats[index]);
    }
    return formats;
}

std::optional<size_t> SelectedSearchLocation(HWND dialog) {
    const HWND combo = GetDlgItem(dialog, kSearchRoot);
    if (!combo) return std::nullopt;
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected < 0 || selected > INT_MAX) return std::nullopt;
    COMBOBOXEXITEMW item{};
    item.mask = CBEIF_LPARAM;
    item.iItem = static_cast<int>(selected);
    if (!SendMessageW(combo, CBEM_GETITEMW, 0,
                      reinterpret_cast<LPARAM>(&item)) ||
        item.lParam < 0) return std::nullopt;
    return static_cast<size_t>(item.lParam);
}

void SetSearchStatus(const LocalSearchDialogState& state, HWND dialog,
                     const std::filesystem::path& current) {
    const size_t count = state.results.size();
    if (state.status_format.empty()) {
        const auto text = std::to_wstring(count) + L" 个";
        SetDlgItemTextW(dialog, kSearchStatus, text.c_str());
        return;
    }
    std::vector<wchar_t> text(32768);
    _snwprintf_s(text.data(), text.size(), _TRUNCATE,
        state.status_format.c_str(),
        count > static_cast<size_t>(INT_MAX) ? INT_MAX
                                             : static_cast<int>(count),
        current.c_str());
    SetDlgItemTextW(dialog, kSearchStatus, text.data());
}

void UpdateSearchControls(LocalSearchDialogState& state, HWND dialog) {
    SetDlgItemTextW(dialog, kSearchToggle,
        (state.searching ? state.stop_text : state.start_text).c_str());
    const bool idle = !state.searching;
    for (const int identifier : {kSearchRoot, kSearchTypes,
            kSearchSelectAllTypes, kSearchClearAllTypes,
            kSearchMinimumEnabled, IDOK}) {
        EnableWindow(GetDlgItem(dialog, identifier), idle);
    }
    const bool minimum = IsDlgButtonChecked(
        dialog, kSearchMinimumEnabled) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, kSearchMinimumSeconds),
                 idle && minimum);
    const HWND results = GetDlgItem(dialog, kSearchResults);
    EnableWindow(GetDlgItem(dialog, kSearchAddSelected),
        idle && results && ListView_GetSelectedCount(results) != 0);
}

void AppendSearchResults(LocalSearchDialogState& state, HWND dialog,
                         std::vector<LocalSearchResult> results) {
    const HWND list = GetDlgItem(dialog, kSearchResults);
    if (!list) return;
    for (auto& result : results) {
        if (state.results.size() > static_cast<size_t>(INT_MAX)) break;
        const size_t identity = state.results.size();
        state.results.push_back(std::move(result));
        const auto& stored = state.results.back();
        std::wstring title = stored.title.empty()
            ? stored.path.stem().wstring() : stored.title;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(list);
        item.pszText = title.data();
        item.lParam = static_cast<LPARAM>(identity);
        const int row = ListView_InsertItem(list, &item);
        if (row < 0) continue;
        const std::wstring values[]{stored.artist, stored.album,
                                    stored.format, stored.path.wstring()};
        for (int column = 1; column < 5; ++column) {
            ListView_SetItemText(list, row, column,
                const_cast<wchar_t*>(values[column - 1].c_str()));
        }
    }
}

std::wstring_view SearchColumnValue(const LocalSearchResult& result,
                                    int column) {
    switch (column) {
    case 0: return result.title;
    case 1: return result.artist;
    case 2: return result.album;
    case 3: return result.format;
    default: return result.path.native();
    }
}

int CALLBACK CompareSearchRows(LPARAM left, LPARAM right, LPARAM parameter) {
    const auto* state = reinterpret_cast<const LocalSearchDialogState*>(parameter);
    if (!state || left < 0 || right < 0 ||
        static_cast<size_t>(left) >= state->results.size() ||
        static_cast<size_t>(right) >= state->results.size()) return 0;
    const auto lhs = SearchColumnValue(state->results[static_cast<size_t>(left)],
                                       state->sort_column);
    const auto rhs = SearchColumnValue(state->results[static_cast<size_t>(right)],
                                       state->sort_column);
    int compared = playlist::CompareLegacyLogicalText(
        std::wstring(lhs).c_str(), std::wstring(rhs).c_str());
    if (!state->sort_ascending) compared = -compared;
    return compared;
}

std::vector<std::filesystem::path> CollectResultPaths(
    const LocalSearchDialogState& state, HWND dialog, bool selected_only) {
    std::vector<std::filesystem::path> paths;
    const HWND list = GetDlgItem(dialog, kSearchResults);
    if (!list) return paths;
    const int count = ListView_GetItemCount(list);
    paths.reserve(selected_only ? ListView_GetSelectedCount(list)
                                : static_cast<size_t>(std::max(0, count)));
    for (int row = 0; row < count; ++row) {
        if (selected_only &&
            (ListView_GetItemState(list, row, LVIS_SELECTED) &
             LVIS_SELECTED) == 0) continue;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(list, &item) || item.lParam < 0) continue;
        const size_t index = static_cast<size_t>(item.lParam);
        if (index < state.results.size())
            paths.push_back(state.results[index].path);
    }
    return paths;
}

void BeginSearch(LocalSearchDialogState& state, HWND dialog) {
    auto formats = EnabledSearchFormats(state, dialog);
    if (formats.empty()) {
        MessageBeep(MB_ICONEXCLAMATION);
        return;
    }
    auto roots = BuildPlaylistLocalSearchRootPlan(
        state.locations, state.selected_location);
    if (roots.empty()) {
        MessageBeep(MB_ICONEXCLAMATION);
        return;
    }

    ListView_DeleteAllItems(GetDlgItem(dialog, kSearchResults));
    state.results.clear();
    state.sort_column = -1;
    const bool minimum_enabled = IsDlgButtonChecked(
        dialog, kSearchMinimumEnabled) == BST_CHECKED;
    const UINT minimum_seconds = GetDlgItemInt(
        dialog, kSearchMinimumSeconds, nullptr, FALSE);
    const auto runtime = RuntimeDirectory();

    std::shared_ptr<LocalSearchReceiver> receiver;
    try {
        receiver = std::make_shared<LocalSearchReceiver>();
    } catch (const std::exception&) {
        MessageBeep(MB_ICONEXCLAMATION);
        return;
    }
    receiver->target = dialog;
    receiver->generation = ++state.generation;
    state.receiver = receiver;
    state.searching = true;
    UpdateSearchControls(state, dialog);
    SetSearchStatus(state, dialog,
        std::filesystem::path(WindowText(GetDlgItem(dialog, kSearchRoot))));

    LocalSearchRequest request;
    request.roots = std::move(roots);
    request.formats = std::move(formats);
    request.minimum_enabled = minimum_enabled;
    request.minimum_seconds = minimum_seconds;
    request.helper = runtime / L"ttplayer_file_info_probe.exe";
    request.addin_directory = runtime / L"AddIn";
    request.ttpcomm_path = runtime / L"ttpcomm.dll";
    request.generation = state.generation;
    try {
        std::thread([request = std::move(request), receiver]() mutable {
            const auto generation = request.generation;
            try {
                RunLocalSearch(std::move(request), receiver);
            } catch (...) {
                std::unique_ptr<LocalSearchUpdate> completed(
                    new (std::nothrow) LocalSearchUpdate());
                if (!completed) return;
                completed->generation = generation;
                completed->completed = true;
                completed->cancelled = receiver->stop.stop_requested();
                static_cast<void>(PostSearchUpdate(receiver,
                    std::move(completed), receiver->stop.get_token()));
            }
        }).detach();
    } catch (const std::system_error&) {
        StopSearch(state, dialog);
        UpdateSearchControls(state, dialog);
    }
}

bool BrowseSearchRoot(LocalSearchDialogState& state, HWND dialog) {
    if (state.folder_picker_open) return false;
    state.folder_picker_open = true;
    detail::ModernFolderOptions options;
    options.owner = dialog;
    options.initial_path = state.root;
    options.title = state.folder_picker_title;
    const auto selected = detail::ModernPickFolder(options);
    const bool changed = selected.has_value();
    if (selected) {
        state.root = selected->path;
        if (state.custom_folder_index >= 0 &&
            static_cast<size_t>(state.custom_folder_index) <
                state.locations.size()) {
            const size_t custom =
                static_cast<size_t>(state.custom_folder_index);
            state.locations[custom].path = state.root;
            state.selected_location = custom;
        }
        PopulateRootCombo(state, dialog);
    }
    state.folder_picker_open = false;
    return changed;
}

void FinishSearchDialog(LocalSearchDialogState& state, HWND dialog,
                        bool selected_only) {
    // 004A6E68/004A6F3F stop CSearchListCtrl::Run before walking its current
    // ListView. Consume updates already handed to this HWND so "完成" includes
    // the same completed rows; later worker output is cancelled and rejected.
    if (state.searching) StopSearch(state, dialog, true);
    state.accepted_paths = CollectResultPaths(state, dialog, selected_only);
    EndDialog(dialog, IDOK);
}

INT_PTR CALLBACK LocalSearchDialogProc(HWND dialog, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LocalSearchDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<LocalSearchDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state) return FALSE;
        auto labels = Split(WindowText(GetDlgItem(dialog, kSearchToggle)), L'|');
        state->start_text = labels.empty() ? L"开始搜索" : labels.front();
        state->stop_text = labels.size() < 2 ? L"停止搜索" : labels[1];
        state->status_format = WindowText(GetDlgItem(dialog, kSearchStatus));
        SetDlgItemTextW(dialog, kSearchStatus, L"");
        PopulateRootCombo(*state, dialog);
        PopulateSearchTypes(*state, dialog);
        PopulateSearchResults(*state, dialog);
        CheckDlgButton(dialog, kSearchMinimumEnabled, BST_CHECKED);
        SetDlgItemInt(dialog, kSearchMinimumSeconds, 30, FALSE);
        UpdateSearchControls(*state, dialog);
        return TRUE;
    }

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wparam)) {
        case kSearchToggle:
            if (HIWORD(wparam) == BN_CLICKED) {
                if (state->searching) {
                    StopSearch(*state, dialog, true);
                    UpdateSearchControls(*state, dialog);
                } else {
                    BeginSearch(*state, dialog);
                }
                return TRUE;
            }
            break;
        case kSearchSelectAllTypes:
        case kSearchClearAllTypes:
            if (HIWORD(wparam) == BN_CLICKED) {
                const HWND list = GetDlgItem(dialog, kSearchTypes);
                const int count = list ? ListView_GetItemCount(list) : 0;
                for (int row = 0; row < count; ++row)
                    ListView_SetCheckState(list, row,
                        LOWORD(wparam) == kSearchSelectAllTypes);
                return TRUE;
            }
            break;
        case kSearchMinimumEnabled:
            if (HIWORD(wparam) == BN_CLICKED) {
                UpdateSearchControls(*state, dialog);
                return TRUE;
            }
            break;
        case kSearchRoot:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const size_t previous = state->selected_location;
                const auto selected = SelectedSearchLocation(dialog);
                if (!selected || *selected >= state->locations.size())
                    return TRUE;
                if (state->locations[*selected].kind ==
                    PlaylistLocalSearchRootKind::custom) {
                    if (!BrowseSearchRoot(*state, dialog)) {
                        state->selected_location = previous;
                        SendDlgItemMessageW(dialog, kSearchRoot,
                            CB_SETCURSEL, static_cast<WPARAM>(previous), 0);
                    }
                } else {
                    state->selected_location = *selected;
                }
                return TRUE;
            }
            break;
        case kSearchAddSelected:
            if (HIWORD(wparam) == BN_CLICKED) {
                FinishSearchDialog(*state, dialog, true);
                return TRUE;
            }
            break;
        case IDOK:
            FinishSearchDialog(*state, dialog, false);
            return TRUE;
        case IDCANCEL:
            StopSearch(*state, dialog);
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_NOTIFY:
        if (state && lparam) {
            const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
            if (notification->idFrom == kSearchResults &&
                notification->code == LVN_ITEMCHANGED) {
                UpdateSearchControls(*state, dialog);
                return TRUE;
            }
            if (notification->idFrom == kSearchResults &&
                notification->code == LVN_COLUMNCLICK) {
                const auto* click = reinterpret_cast<const NMLISTVIEW*>(lparam);
                if (state->sort_column == click->iSubItem)
                    state->sort_ascending = !state->sort_ascending;
                else {
                    state->sort_column = click->iSubItem;
                    state->sort_ascending = true;
                }
                ListView_SortItems(GetDlgItem(dialog, kSearchResults),
                    CompareSearchRows, reinterpret_cast<LPARAM>(state));
                return TRUE;
            }
        }
        break;

    case kSearchUpdateMessage: {
        std::unique_ptr<LocalSearchUpdate> update(
            reinterpret_cast<LocalSearchUpdate*>(lparam));
        if (!state || !update ||
            update->generation != state->generation ||
            static_cast<std::uint64_t>(wparam) != state->generation)
            return TRUE;
        AppendSearchResults(*state, dialog, std::move(update->results));
        SetSearchStatus(*state, dialog, update->current_path);
        if (update->completed) {
            if (state->receiver) {
                std::scoped_lock lock(state->receiver->mutex);
                state->receiver->stopped = true;
                state->receiver->target = nullptr;
            }
            state->receiver.reset();
            state->searching = false;
            UpdateSearchControls(*state, dialog);
        }
        return TRUE;
    }

    case WM_CLOSE:
        if (state) StopSearch(*state, dialog);
        EndDialog(dialog, IDCANCEL);
        return TRUE;

    case WM_DESTROY:
        if (state) StopSearch(*state, dialog);
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

} // namespace

std::vector<PlaylistLocalSearchRoot> BuildPlaylistLocalSearchLocations(
    std::uint32_t logical_drive_mask,
    const std::filesystem::path& custom_path) {
    // 004A6423 stores these as raw decimal values (including the historical
    // gap at 12), so preserve their order rather than replacing the array
    // with a modern Known Folder catalogue.
    static constexpr std::array<int, 5> shell_roots{17, 12, 13, 46, 53};
    std::vector<PlaylistLocalSearchRoot> result;
    result.reserve(shell_roots.size() + 27);
    for (const int csidl : shell_roots) {
        result.push_back({PlaylistLocalSearchRootKind::shell_csidl,
                          csidl, {}});
    }
    for (unsigned drive = 0; drive < 26; ++drive) {
        if ((logical_drive_mask & (std::uint32_t{1} << drive)) == 0)
            continue;
        const wchar_t path[]{static_cast<wchar_t>(L'A' + drive),
                             L':', L'\\', L'\0'};
        result.push_back({PlaylistLocalSearchRootKind::drive, 0, path});
    }
    result.push_back({PlaylistLocalSearchRootKind::custom, 0, custom_path});
    return result;
}

std::vector<PlaylistLocalSearchRoot> BuildPlaylistLocalSearchRootPlan(
    std::span<const PlaylistLocalSearchRoot> locations,
    std::size_t selected_index) {
    if (selected_index >= locations.size()) return {};
    const auto& selected = locations[selected_index];
    if (selected.kind == PlaylistLocalSearchRootKind::shell_csidl &&
        selected.csidl == 17) {
        std::vector<PlaylistLocalSearchRoot> drives;
        for (const auto& location : locations) {
            if (location.kind == PlaylistLocalSearchRootKind::drive &&
                !location.path.empty()) drives.push_back(location);
        }
        return drives;
    }
    if ((selected.kind == PlaylistLocalSearchRootKind::drive ||
         selected.kind == PlaylistLocalSearchRootKind::custom) &&
        selected.path.empty()) return {};
    return {selected};
}

std::vector<PlaylistLocalSearchFormat> BuildPlaylistLocalSearchFormats(
    std::span<const plugins::ReaderFormat> reader_formats) {
    std::vector<PlaylistLocalSearchFormat> result;
    result.reserve(reader_formats.size());
    for (const auto& reader : reader_formats) {
        if (reader.description.empty() || reader.pattern.empty() ||
            !HasSearchPattern(reader.pattern)) continue;
        result.push_back({reader.description, reader.pattern});
    }
    return result;
}

bool PlaylistLocalSearchMatches(
    const std::filesystem::path& path,
    std::span<const PlaylistLocalSearchFormat> enabled_formats) {
    return MatchingFormat(path, enabled_formats) != nullptr;
}

bool PlaylistLocalSearchPassesMinimum(
    int duration_ms, bool enabled, std::uint32_t seconds) noexcept {
    if (!enabled) return true;
    if (duration_ms < 0) return false;
    return static_cast<std::uint64_t>(duration_ms) >=
        static_cast<std::uint64_t>(seconds) * 1000U;
}

void PlayerWindow::ShowPlaylistLocalSearch() {
    if (file_dialog_active_) return;
    struct DialogGuard {
        bool& active;
        explicit DialogGuard(bool& value) : active(value) { active = true; }
        ~DialogGuard() { active = false; }
    } guard(file_dialog_active_);
    LeaveFullScreen();
    LocalSearchDialogState state;
    state.root = InitialSearchRoot(file_dialog_initial_directory_.empty()
        ? settings_.history.sound_path : file_dialog_initial_directory_);
    state.formats = BuildPlaylistLocalSearchFormats(reader_formats_);
    state.columns = Split(ResourceText(0x815c), L'|');
    state.custom_folder_text = ResourceText(0x814b);
    state.folder_picker_title = ResourceText(0x8152);
    state.locations = BuildPlaylistLocalSearchLocations(
        GetLogicalDrives(), state.root);
    state.selected_location = 0;

    // 004A6423's raw special-folder rows and Computer children are retained.
    // The final custom row still routes through the requested modern
    // IFileDialog folder picker.
    const HWND owner = playlist_window_ ? playlist_window_ : window_;
    const INT_PTR result = DialogBoxParamW(
        ResourceModule(), MAKEINTRESOURCEW(228), owner,
        LocalSearchDialogProc, reinterpret_cast<LPARAM>(&state));
    StopSearch(state, nullptr);
    if (result != IDOK || state.accepted_paths.empty()) return;

    file_dialog_initial_directory_ = state.root;
    settings_.history.sound_path = state.root;
    static_cast<void>(ImportFiles(state.accepted_paths,
        playlists_.ActiveIndex(), std::nullopt, false,
        ImportPlayback::if_idle));
}

} // namespace ttplayer::ui
