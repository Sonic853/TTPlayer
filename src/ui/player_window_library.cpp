#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/media_library_playback.h"
#include "directory_change_monitor.h"
#include "player_window_internal.h"
#include "modern_file_dialog.h"

#include "ttplayer/core/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <propkey.h>
#include <propvarutil.h>
#include <propsys.h>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_map>
#include <utility>

namespace ttplayer::ui {

playlist::Playlist BuildMediaLibraryPlaybackSnapshot(
    std::span<const playlist::Track> tracks, size_t selected) {
    playlist::Playlist result;
    for (const auto& track : tracks) result.Add(track);
    if (selected < tracks.size()) result.SetCurrentRow(selected);
    return result;
}

std::wstring MediaLibraryTrackIdentity(const playlist::Track& track) {
    std::wstring value = track.path.wstring();
    if (value.find(L"://") == std::wstring::npos) {
        std::error_code error;
        auto absolute = std::filesystem::absolute(track.path, error);
        if (!error) value = absolute.lexically_normal().wstring();
    }
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
    value.push_back(L'\x1f');
    value += std::to_wstring(track.subtrack);
    return value;
}

size_t SetMediaLibraryPlaybackRating(
    playlist::Playlist& playback, std::wstring_view identity, int rating) {
    size_t changed{};
    for (size_t row = 0; row < playback.Tracks().size(); ++row) {
        if (MediaLibraryTrackIdentity(playback.Tracks()[row]) == identity &&
            playback.SetRating(row, rating)) ++changed;
    }
    return changed;
}

MediaLibraryPlaybackEraseResult EraseMediaLibraryPlaybackTracks(
    playlist::Playlist& playback,
    std::span<const std::wstring> identities,
    std::optional<size_t> current) {
    const std::set<std::wstring, std::less<>> targets(
        identities.begin(), identities.end());
    MediaLibraryPlaybackEraseResult result;
    for (size_t row = playback.Tracks().size(); row-- > 0;) {
        if (!targets.contains(MediaLibraryTrackIdentity(
                playback.Tracks()[row]))) continue;
        if (current) {
            if (row < *current) ++result.removed_before_current;
            else if (row == *current) result.removed_current = true;
        }
        if (playback.Remove(row)) ++result.removed;
    }
    return result;
}

namespace {
bool ApplyMediaLibraryOrder(
    std::vector<playlist::Track>& tracks, std::vector<size_t>& item_indices,
    playlist::Playlist&& ordered, const std::vector<size_t>& old_to_new) {
    if (tracks.size() != item_indices.size() ||
        old_to_new.size() != tracks.size() ||
        ordered.Tracks().size() != tracks.size()) return false;
    std::vector<size_t> reordered_items(item_indices.size());
    std::vector<bool> destinations(item_indices.size());
    for (size_t old_index = 0; old_index < old_to_new.size(); ++old_index) {
        const size_t new_index = old_to_new[old_index];
        if (new_index >= reordered_items.size() || destinations[new_index])
            return false;
        destinations[new_index] = true;
        reordered_items[new_index] = item_indices[old_index];
    }
    tracks = ordered.Tracks();
    item_indices = std::move(reordered_items);
    return true;
}
} // namespace

bool SortMediaLibraryResult(
    std::vector<playlist::Track>& tracks, std::vector<size_t>& item_indices,
    playlist::SortKey key, bool ascending,
    std::span<const std::wstring> display_titles) {
    if (tracks.size() != item_indices.size() || tracks.size() <= 1)
        return false;
    playlist::Playlist ordered;
    for (const auto& track : tracks) ordered.Add(track);
    const auto old_to_new = ordered.Sort(key, ascending, display_titles);
    return ApplyMediaLibraryOrder(tracks, item_indices, std::move(ordered),
                                  old_to_new);
}

bool ShuffleMediaLibraryResult(
    std::vector<playlist::Track>& tracks, std::vector<size_t>& item_indices) {
    if (tracks.size() != item_indices.size() || tracks.size() <= 1)
        return false;
    playlist::Playlist ordered;
    for (const auto& track : tracks) ordered.Add(track);
    const auto old_to_new = ordered.Shuffle();
    return ApplyMediaLibraryOrder(tracks, item_indices, std::move(ordered),
                                  old_to_new);
}

size_t ReplaceMediaLibraryTrackPath(
    std::vector<playlist::Track>& tracks, const playlist::Track& source,
    const std::filesystem::path& target) {
    size_t changed{};
    for (auto& track : tracks) {
        if (_wcsicmp(track.path.c_str(), source.path.c_str()) != 0) continue;
        track.path = target;
        ++changed;
    }
    return changed;
}

void ApplyMediaLibraryReaderInfo(playlist::Track& track,
                                 MediaLibraryReaderInfo info) {
    const auto equal_ascii = [](std::string_view left,
                                std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto lhs = static_cast<unsigned char>(left[index]);
            const auto rhs = static_cast<unsigned char>(right[index]);
            if (std::tolower(lhs) != std::tolower(rhs)) return false;
        }
        return true;
    };
    const auto value = [&info, &equal_ascii](
                           std::string_view key) -> std::string {
        const auto found = std::find_if(
            info.metadata.begin(), info.metadata.end(),
            [key, &equal_ascii](const auto& entry) {
                return equal_ascii(entry.first, key);
            });
        return found == info.metadata.end() ? std::string{} : found->second;
    };

    // 004ADA76 clears an ordinary physical CPlayItem's old metadata before
    // enumerating the reader interface.  A successful empty enumeration is
    // therefore authoritative too; it must not leave stale Shell/cache tags.
    track.title = value("Title");
    track.artist = value("Artist");
    if (track.artist.empty()) track.artist = value("Author");
    track.album = value("Album");
    track.duration_ms = info.duration_ms;
    track.media_type = std::move(info.media_type);
    track.bitrate_bps = info.bitrate_bps;
    track.sample_rate_hz = info.sample_rate_hz;
    track.metadata = std::move(info.metadata);
}
using namespace detail;

namespace {
constexpr size_t kNoSource = static_cast<size_t>(-1);
constexpr int kLibraryTargetListControl = 1064;

struct LibraryTargetDialogState {
    const playlist::PlaylistStore* store{};
    size_t selected{playlist::PlaylistStore::npos};
};

INT_PTR CALLBACK LibraryTargetDialogProc(HWND dialog, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LibraryTargetDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<LibraryTargetDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state || !state->store) return FALSE;
        const HWND list = GetDlgItem(dialog, kLibraryTargetListControl);
        for (const auto& entry : state->store->Entries()) {
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(entry.playlist.Title().c_str()));
        }
        if (state->store->Size() != 0)
            SendMessageW(list, LB_SETCURSEL,
                         state->store->ActiveIndex(), 0);
        return TRUE;
    }
    if (message != WM_COMMAND || !state) return FALSE;
    switch (LOWORD(wparam)) {
    case IDOK: {
        const LRESULT selected = SendDlgItemMessageW(
            dialog, kLibraryTargetListControl, LB_GETCURSEL, 0, 0);
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
    default:
        return FALSE;
    }
}

std::filesystem::path MediaLibraryStoragePath() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"Music.library";
}

std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    try { return core::Utf8ToWide(value); }
    catch (const std::exception&) { return {}; }
}

bool AsciiEqual(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const unsigned char lhs = static_cast<unsigned char>(left[index]);
        const unsigned char rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

std::wstring MetadataValue(const playlist::Track& track,
                           std::string_view key) {
    if (AsciiEqual(key, "Artist") && !track.artist.empty())
        return Wide(track.artist);
    if (AsciiEqual(key, "Album") && !track.album.empty())
        return Wide(track.album);
    if (AsciiEqual(key, "Title") && !track.title.empty())
        return Wide(track.title);
    const auto found = std::find_if(track.metadata.begin(),
        track.metadata.end(), [key](const auto& entry) {
            return AsciiEqual(entry.first, key);
        });
    return found == track.metadata.end() ? std::wstring{} : Wide(found->second);
}

std::wstring Fold(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring NormalizedLocalPath(const std::filesystem::path& path) {
    auto value = path;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(value, error);
    if (!error) value = absolute;
    value = value.lexically_normal();
    auto text = Fold(value.wstring());
    std::replace(text.begin(), text.end(), L'/', L'\\');
    return text;
}

bool IsSameOrBelowPath(const std::filesystem::path& path,
                       const std::filesystem::path& parent) {
    const auto candidate = NormalizedLocalPath(path);
    auto prefix = NormalizedLocalPath(parent);
    while (prefix.size() > 3 && prefix.back() == L'\\') prefix.pop_back();
    if (candidate == prefix) return true;
    return candidate.size() > prefix.size() &&
        candidate.compare(0, prefix.size(), prefix) == 0 &&
        candidate[prefix.size()] == L'\\';
}

std::vector<std::wstring> Split(std::wstring_view text, wchar_t delimiter);

std::set<std::wstring> LibraryReaderExtensions(
    std::span<const plugins::ReaderFormat> formats) {
    // FUN_004CB1DB registers these seven built-in reader groups before the
    // AddIn enumeration.  Directory indexing must not disappear for MP3/WAV
    // merely because those formats have no external ReaderFormat entry.
    static constexpr std::array<std::wstring_view, 16> built_in{
        L".cda", L".mp3", L".mp2", L".mp1", L".mpa", L".mp3pro",
        L".mid", L".midi", L".rmi", L".wav", L".wave", L".aif",
        L".aifc", L".aiff", L".au", L".snd"};
    std::set<std::wstring> result(built_in.begin(), built_in.end());
    for (const auto& format : formats) {
        for (auto token : Split(format.pattern, L';')) {
            const size_t dot = token.rfind(L'.');
            if (dot == std::wstring::npos) continue;
            auto extension = token.substr(dot);
            const size_t wildcard = extension.find_first_of(L"*?");
            if (wildcard != std::wstring::npos) extension.resize(wildcard);
            while (!extension.empty() && iswspace(extension.back()))
                extension.pop_back();
            if (extension.size() > 1) result.insert(Fold(extension));
        }
    }
    return result;
}

std::vector<std::wstring> Split(std::wstring_view text, wchar_t delimiter) {
    std::vector<std::wstring> result;
    size_t begin{};
    do {
        const size_t end = text.find(delimiter, begin);
        result.emplace_back(text.substr(begin,
            end == std::wstring_view::npos ? text.size() - begin : end - begin));
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    } while (begin <= text.size());
    return result;
}

int LogicalCompare(std::wstring_view left, std::wstring_view right) {
    const std::wstring lhs(left);
    const std::wstring rhs(right);
    return playlist::CompareLegacyLogicalText(lhs.c_str(), rhs.c_str());
}

bool LibraryTrackSourceValid(const playlist::Track& track) {
    if (track.path.empty() || track.duration_ms == -1) return false;
    const auto source = track.path.wstring();
    const auto scheme = source.find(L"://");
    if (scheme != std::wstring::npos && scheme != 0) return true;
    auto extension = track.path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   towlower);
    if (extension == L".cda" ||
        _wcsicmp(Wide(track.media_type).c_str(), L"CD|CD Audio") == 0)
        return true;
    auto lower = Fold(source);
    if (lower.find(L".zip|") != std::wstring::npos ||
        lower.find(L".rar|") != std::wstring::npos) return true;
    const DWORD attributes = GetFileAttributesW(track.path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

HGLOBAL MakeLibraryFileList(
    const std::vector<std::filesystem::path>& paths) {
    // URL-only IDataObjects advertise URLW, not a syntactically valid but
    // empty CF_HDROP.  The latter makes Shell targets believe that a local
    // file transfer is available and then fail after accepting the drag.
    if (paths.empty()) return nullptr;
    size_t characters = 1;
    for (const auto& path : paths) characters += path.wstring().size() + 1;
    const size_t bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return nullptr;
    auto* data = static_cast<unsigned char*>(GlobalLock(memory));
    if (!data) { GlobalFree(memory); return nullptr; }
    auto* header = reinterpret_cast<DROPFILES*>(data);
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    auto* output = reinterpret_cast<wchar_t*>(data + sizeof(DROPFILES));
    for (const auto& path : paths) {
        const auto value = path.wstring();
        memcpy(output, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
        output += value.size() + 1;
    }
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL MakeLibraryGlobal(const void* value, size_t bytes) {
    if (!value || bytes == 0) return nullptr;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return nullptr;
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return nullptr; }
    memcpy(target, value, bytes);
    GlobalUnlock(memory);
    return memory;
}

UINT LibraryPrivateClipboardFormat() {
    static const UINT value =
        RegisterClipboardFormatW(L"TTPlayer_DropItemsFormat");
    return value;
}

UINT LibraryUrlClipboardFormat() {
    static const UINT value = RegisterClipboardFormatW(L"UniformResourceLocatorW");
    return value;
}

UINT LibraryShellIdListArrayFormat() {
    static const UINT value = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    return value;
}

HGLOBAL MakeLibraryShellIdListArray(
    const std::vector<std::filesystem::path>& paths) {
    try {
        struct PidlDeleter {
            void operator()(ITEMIDLIST* item) const noexcept {
                CoTaskMemFree(item);
            }
        };
        using OwnedPidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;
        std::vector<OwnedPidl> items;
        items.reserve(paths.size());
        for (const auto& path : paths) {
            PIDLIST_ABSOLUTE item{};
            const HRESULT parsed = SHParseDisplayName(
                path.c_str(), nullptr, &item, 0, nullptr);
            OwnedPidl owned(item);
            if (SUCCEEDED(parsed) && owned) {
                items.push_back(std::move(owned));
            }
        }
        if (items.empty()) return nullptr;

        // 00481871 exposes CFSTR_SHELLIDLIST in addition to CF_HDROP.  The
        // desktop's empty PIDL is the common parent, so absolute child PIDLs
        // may represent files from different folders in one CIDA packet.
        if (items.size() >
            std::numeric_limits<UINT>::max() / sizeof(UINT) - 2)
            return nullptr;
        const size_t offsets_size = sizeof(UINT) * (items.size() + 2);
        size_t bytes = offsets_size + sizeof(USHORT);
        for (const auto& item : items) {
            const size_t item_size = ILGetSize(item.get());
            if (item_size > std::numeric_limits<size_t>::max() - bytes)
                return nullptr;
            bytes += item_size;
        }
        if (bytes > std::numeric_limits<UINT>::max()) return nullptr;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!memory) return nullptr;
        auto* data = static_cast<unsigned char*>(GlobalLock(memory));
        if (!data) {
            GlobalFree(memory);
            return nullptr;
        }
        auto* offsets = reinterpret_cast<UINT*>(data);
        offsets[0] = static_cast<UINT>(items.size());
        offsets[1] = static_cast<UINT>(offsets_size);
        size_t cursor = offsets_size + sizeof(USHORT);
        for (size_t index = 0; index < items.size(); ++index) {
            offsets[index + 2] = static_cast<UINT>(cursor);
            const size_t item_size = ILGetSize(items[index].get());
            memcpy(data + cursor, items[index].get(), item_size);
            cursor += item_size;
        }
        GlobalUnlock(memory);
        return memory;
    } catch (...) {
        // OwnedPidl releases every successfully parsed PIDL if vector/path
        // allocation fails while a mixed-folder CIDA is being assembled.
        return nullptr;
    }
}

class LibraryDataObject final : public IDataObject {
public:
    explicit LibraryDataObject(const std::vector<playlist::Track>& tracks) {
        const DWORD process = GetCurrentProcessId();
        if (auto memory = MakeLibraryGlobal(&process, sizeof(process)))
            Add(static_cast<CLIPFORMAT>(LibraryPrivateClipboardFormat()),
                memory);
        std::vector<std::filesystem::path> files;
        std::wstring url;
        for (const auto& track : tracks) {
            if (track.path.empty()) continue;
            const auto value = track.path.wstring();
            if (value.find(L"://") != std::wstring::npos) {
                if (url.empty()) url = value;
            } else {
                files.push_back(track.path);
            }
        }
        if (auto memory = MakeLibraryFileList(files))
            Add(CF_HDROP, memory);
        if (auto memory = MakeLibraryShellIdListArray(files))
            Add(static_cast<CLIPFORMAT>(LibraryShellIdListArrayFormat()),
                memory);
        if (!url.empty()) {
            if (auto memory = MakeLibraryGlobal(url.c_str(),
                    (url.size() + 1) * sizeof(wchar_t)))
                Add(static_cast<CLIPFORMAT>(LibraryUrlClipboardFormat()), memory);
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *result = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG result = --references_;
        if (!result) delete this;
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request,
                                      STGMEDIUM* medium) override {
        if (!request || !medium) return E_POINTER;
        const auto found = Find(*request);
        if (found == entries_.end()) return DV_E_FORMATETC;
        HGLOBAL copy = static_cast<HGLOBAL>(OleDuplicateData(
            found->memory, found->format.cfFormat, GMEM_MOVEABLE));
        if (!copy) return E_OUTOFMEMORY;
        *medium = {};
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
        return request && Find(*request) != entries_.end()
            ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                     FORMATETC* output) override {
        if (!output) return E_POINTER;
        output->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,
                                            IEnumFORMATETC** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> formats;
        for (const auto& entry : entries_) formats.push_back(entry.format);
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()),
                                    formats.data(), result);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                      DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
private:
    struct Entry { FORMATETC format{}; HGLOBAL memory{}; };
    ~LibraryDataObject() {
        for (const auto& entry : entries_) GlobalFree(entry.memory);
    }
    void Add(CLIPFORMAT format, HGLOBAL memory) noexcept {
        if (!memory) return;
        if (!format) {
            // RegisterClipboardFormatW returns zero on failure. Ownership of
            // an HGLOBAL transfers only after a valid FORMATETC is retained.
            GlobalFree(memory);
            return;
        }
        try {
            entries_.push_back({{format, nullptr, DVASPECT_CONTENT, -1,
                                 TYMED_HGLOBAL}, memory});
        } catch (...) {
            GlobalFree(memory);
        }
    }
    auto Find(const FORMATETC& request) const {
        return std::find_if(entries_.begin(), entries_.end(),
            [&request](const Entry& entry) {
                return request.cfFormat == entry.format.cfFormat &&
                    (request.tymed & TYMED_HGLOBAL) != 0 &&
                    request.dwAspect == DVASPECT_CONTENT;
            });
    }
    std::atomic<ULONG> references_{1};
    std::vector<Entry> entries_;
};

bool PublishLibraryClipboard(HWND owner,
                             const std::vector<playlist::Track>& tracks) {
    if (tracks.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool published{};
    const DWORD process = GetCurrentProcessId();
    if (auto memory = MakeLibraryGlobal(&process, sizeof(process))) {
        if (SetClipboardData(LibraryPrivateClipboardFormat(), memory))
            published = true;
        else GlobalFree(memory);
    }
    std::vector<std::filesystem::path> files;
    std::wstring url;
    for (const auto& track : tracks) {
        if (track.path.empty()) continue;
        const auto value = track.path.wstring();
        if (value.find(L"://") == std::wstring::npos) files.push_back(track.path);
        else if (url.empty()) url = value;
    }
    if (auto memory = MakeLibraryFileList(files)) {
        if (SetClipboardData(CF_HDROP, memory)) published = true;
        else GlobalFree(memory);
    }
    if (auto memory = MakeLibraryShellIdListArray(files)) {
        if (SetClipboardData(LibraryShellIdListArrayFormat(), memory))
            published = true;
        else
            GlobalFree(memory);
    }
    if (!url.empty()) {
        if (auto memory = MakeLibraryGlobal(url.c_str(),
                (url.size() + 1) * sizeof(wchar_t))) {
            if (SetClipboardData(LibraryUrlClipboardFormat(), memory))
                published = true;
            else GlobalFree(memory);
        }
    }
    CloseClipboard();
    return published;
}

} // namespace

struct PlayerWindow::MediaLibraryState {
    struct Source {
        size_t playlist{kNoSource};
        size_t row{kNoSource};
    };
    struct Seed {
        playlist::Track track;
        Source source;
    };
    struct Item {
        playlist::Track track;
        std::wstring identity;
        std::vector<Source> sources;
    };
    struct BuildResult {
        unsigned long long instance{};
        unsigned long long generation{};
        bool failed{};
        bool persistence_attempted{};
        std::vector<Item> items;
    };
    struct WorkerControl {
        std::stop_source stop;
        std::mutex receiver_mutex;
        HWND receiver{};
        unsigned long long instance{};
        unsigned long long generation{};
        // Both fields are guarded by receiver_mutex.  A result whose window
        // handoff fails stays owned here for the 250-ms UI poller/shutdown
        // path instead of leaving MediaLibraryState::indexing stuck forever.
        bool result_posted{};
        std::unique_ptr<BuildResult> unposted_result;
        std::atomic_bool complete{};
    };
    struct BuildRequest {
        unsigned long long instance{};
        unsigned long long generation{};
        std::vector<Seed> seeds;
        std::vector<Seed> priority_seeds;
        std::vector<std::filesystem::path> directories;
        std::set<std::wstring> extensions;
        // The detached scanner may still be inside a private reader when the
        // PlayerWindow/application begins teardown.  This independent sound
        // library owns AddIn references and loaded-module references until
        // every LegacyReaderSession in this request has been destroyed.
        std::shared_ptr<plugins::PluginManager> sound_library;
        // 004C03FD passes Library/MaxItemCount to 004AF271, whose only
        // consumer chooses the initial CPlayItem hash-table bucket count.
        // It is a capacity hint, never an admission/result limit.
        size_t expected_item_count{};
        std::filesystem::path persistence_path;
        bool load_persistence{};
        std::shared_ptr<WorkerControl> control;
    };
    enum class NodeKind { root, category, value, artist_album, rating };
    struct Node {
        NodeKind kind{NodeKind::root};
        std::string key;
        std::wstring text;
        std::wstring value;
        std::wstring artist;
        int rating{};
        HTREEITEM item{};
    };

    std::vector<Item> items;
    std::vector<playlist::Track> persisted_tracks;
    std::vector<playlist::Track> result_tracks;
    std::vector<size_t> result_items;
    std::set<std::wstring> excluded;
    std::vector<playlist::Track> pending_tracks;
    std::vector<std::filesystem::path> pending_files;
    std::vector<std::filesystem::path> pending_directories;
    std::set<std::wstring> monitored_directories;
    std::vector<std::unique_ptr<Node>> nodes;
    Node* root{};
    std::list<std::shared_ptr<WorkerControl>> workers;
    DirectoryChangeMonitor monitor;
    inline static std::atomic_ullong next_instance{1};
    const unsigned long long instance{
        next_instance.fetch_add(1, std::memory_order_relaxed)};
    unsigned long long generation{};
    bool rebuilding{};
    bool indexing{};
    bool refresh_pending{};
    bool persistence_loaded{};
    bool shutting_down{};
    DWORD context_menu_message_time{};
    HFONT tree_font{};

    ~MediaLibraryState() {
        for (const auto& worker : workers) worker->stop.request_stop();
        workers.clear();
        if (tree_font) DeleteObject(tree_font);
    }

    static std::wstring Identity(const playlist::Track& track) {
        return MediaLibraryTrackIdentity(track);
    }

    static void ResetShellMetadata(playlist::Track& track) {
        // FILE_ACTION_MODIFIED must not let ReadShellMetadata's "fill only"
        // policy preserve stale tag values. Keep decoder/runtime fields and
        // the independent TTPlayer rating, but clear every Shell-owned field
        // before re-reading it.
        track.title.clear();
        track.artist.clear();
        track.album.clear();
        std::erase_if(track.metadata, [](const auto& entry) {
            return AsciiEqual(entry.first, "Title") ||
                AsciiEqual(entry.first, "Artist") ||
                AsciiEqual(entry.first, "Album") ||
                AsciiEqual(entry.first, "Genre") ||
                AsciiEqual(entry.first, "Date");
        });
    }

    static void ReadShellMetadata(playlist::Track& track) {
        IPropertyStore* store{};
        if (FAILED(SHGetPropertyStoreFromParsingName(track.path.c_str(), nullptr,
                GPS_BESTEFFORT, IID_PPV_ARGS(&store))) || !store) return;
        const auto get = [store](const PROPERTYKEY& key) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            std::wstring result;
            if (SUCCEEDED(store->GetValue(key, &value))) {
                wchar_t text[1024]{};
                if (SUCCEEDED(PropVariantToString(value, text,
                        static_cast<UINT>(std::size(text))))) result = text;
            }
            PropVariantClear(&value);
            return result;
        };
        const auto assign = [](std::string& destination,
                               const std::wstring& value) {
            if (!destination.empty() || value.empty()) return;
            try { destination = core::WideToUtf8(value); }
            catch (const std::exception&) {}
        };
        assign(track.title, get(PKEY_Title));
        assign(track.artist, get(PKEY_Music_Artist));
        assign(track.album, get(PKEY_Music_AlbumTitle));
        const auto merge = [&track](std::string name, const std::wstring& value) {
            if (value.empty()) return;
            const auto found = std::find_if(track.metadata.begin(),
                track.metadata.end(), [&name](const auto& entry) {
                    return AsciiEqual(entry.first, name);
                });
            if (found != track.metadata.end() && !found->second.empty()) return;
            try {
                auto converted = core::WideToUtf8(value);
                if (found == track.metadata.end())
                    track.metadata.emplace_back(std::move(name),
                                                 std::move(converted));
                else
                    found->second = std::move(converted);
            }
            catch (const std::exception&) {}
        };
        merge("Genre", get(PKEY_Music_Genre));
        merge("Date", get(PKEY_Media_Year));
        PROPVARIANT rating{};
        PropVariantInit(&rating);
        ULONG rating_value{};
        if (SUCCEEDED(store->GetValue(PKEY_Rating, &rating)) &&
            SUCCEEDED(PropVariantToUInt32(rating, &rating_value)) &&
            rating_value != 0) {
            track.rating = std::clamp<int>(
                static_cast<int>((rating_value + 12) / 25), 1, 5);
        }
        PropVariantClear(&rating);
        store->Release();
    }

    // Returns true once an AddIn reader matched the physical source.  A
    // matched reader whose Open fails is authoritative failure, matching the
    // CPlayItem -2 -> -1 transition in 00481759/004ADA76; Shell metadata must
    // not turn a corrupt/private-reader source back into a successful item.
    static bool ReadSoundMetadata(
        playlist::Track& track,
        const std::shared_ptr<plugins::PluginManager>& library) {
        if (!library || track.path.empty() || track.subtrack != 0 ||
            !library->HasReaderForPath(track.path)) return false;

        // 004ADA76 clears ordinary physical-file fields before opening the
        // sound reader and enumerating ISoundMetadata.  Preserve only source
        // identity, CPlayItem rating and other non-reader state on failure.
        track.duration_ms = -1;
        track.title.clear();
        track.artist.clear();
        track.album.clear();
        track.media_type.clear();
        track.bitrate_bps = 0;
        track.sample_rate_hz = 0;
        track.metadata.clear();

        HRESULT opened{};
        auto reader = library->OpenReader(track.path, &opened);
        if (!reader) return true;

        MediaLibraryReaderInfo info;
        info.duration_ms = static_cast<int>(std::min<DWORD>(
            reader->DurationMilliseconds(), static_cast<DWORD>(
                std::numeric_limits<int>::max())));
        try {
            info.media_type = core::WideToUtf8(reader->CodecName());
        } catch (const std::exception&) {
            info.media_type.clear();
        }
        std::uint64_t encoded = reader->EncodedBitsPerSecond();
        if (encoded == 0)
            encoded = static_cast<std::uint64_t>(
                reader->Format().nAvgBytesPerSec) * 8U;
        info.bitrate_bps = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(encoded, 0x7fffffffU));
        info.sample_rate_hz = reader->Format().nSamplesPerSec;
        info.metadata.reserve(reader->Metadata().size());
        for (const auto& entry : reader->Metadata()) {
            if (entry.name.empty()) continue;
            try {
                info.metadata.emplace_back(
                    core::WideToUtf8(entry.name),
                    core::WideToUtf8(entry.value));
            } catch (const std::exception&) {
                // One malformed Unicode field does not invalidate the other
                // reader-owned fields or the successfully opened item.
            }
        }
        ApplyMediaLibraryReaderInfo(track, std::move(info));
        return true;
    }

    static void ReadTrackMetadata(
        playlist::Track& track,
        const std::shared_ptr<plugins::PluginManager>& library) {
        const auto source = track.path.wstring();
        if (source.find(L"://") != std::wstring::npos ||
            source.find(L'|') != std::wstring::npos) return;
        // A physical container reader cannot resolve one CUE subtrack's
        // duration/tags.  Those logical fields remain owned by the CUE
        // parser, just as they do in CPlayItem's segmented-source branch.
        if (track.subtrack != 0) return;
        if (!ReadSoundMetadata(track, library)) ReadShellMetadata(track);
    }

    static void DeliverBuildResult(
        const std::shared_ptr<WorkerControl>& control,
        std::unique_ptr<BuildResult> result) {
        if (!control || !result) return;
        std::scoped_lock receiver_lock(control->receiver_mutex);
        if (control->receiver &&
            PostMessageW(control->receiver, kMsgMediaLibraryReady, 0,
                         reinterpret_cast<LPARAM>(result.get()))) {
            control->result_posted = true;
            static_cast<void>(result.release());
            return;
        }
        control->result_posted = false;
        control->unposted_result = std::move(result);
    }

    static void RunBuild(BuildRequest request) {
        const auto control = request.control;
        const auto stop = control->stop.get_token();
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Apartment {
            HRESULT result;
            ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
        } apartment_guard{apartment};
        auto result = std::make_unique<BuildResult>();
        result->instance = request.instance;
        result->generation = request.generation;
        result->persistence_attempted = request.load_persistence;
        std::unordered_map<std::wstring, size_t> identities;
        const auto seed_capacity = request.seeds.size() * 2 + 64;
        identities.reserve(std::max(seed_capacity,
                                    request.expected_item_count));
        result->items.reserve(std::max(request.seeds.size(),
                                       request.expected_item_count));
        const auto add = [&](playlist::Track track, Source source) {
            const auto identity = Identity(track);
            const auto existing = identities.find(identity);
            if (existing != identities.end()) {
                if (source.playlist != kNoSource)
                    result->items[existing->second].sources.push_back(source);
                return;
            }
            Item item{std::move(track), identity, {}};
            if (source.playlist != kNoSource) item.sources.push_back(source);
            identities.emplace(item.identity, result->items.size());
            result->items.push_back(std::move(item));
        };
        // FILE_ACTION_ADDED/MODIFIED is processed before the persisted
        // catalogue, so the changed file replaces stale cached metadata with
        // the same path+subtrack identity rather than being discarded by the
        // interning map.
        for (auto& seed : request.priority_seeds) {
            if (stop.stop_requested()) return;
            ReadTrackMetadata(seed.track, request.sound_library);
            add(std::move(seed.track), seed.source);
        }
        if (request.load_persistence && !request.persistence_path.empty()) {
            // 004C038B -> 004AF7F4 calls CPlayList_LoadTtbl directly for
            // <exe>\\Music.library.  Keep disk parsing off the window STA,
            // but feed those CPlayItems into the same identity catalogue
            // before numbered lists and monitored folders are reconciled.
            try {
                std::error_code exists_error;
                if (std::filesystem::exists(request.persistence_path,
                                            exists_error) && !exists_error) {
                    playlist::Playlist persisted;
                    persisted.LoadTtbl(request.persistence_path);
                    for (const auto& track : persisted.Tracks()) {
                        if (stop.stop_requested()) return;
                        add(track, {});
                    }
                }
            } catch (const std::exception&) {
                // The original returns false for a missing/corrupt library
                // and continues startup with the live CPlayItem catalogue.
            }
        }
        for (auto& seed : request.seeds) {
            if (stop.stop_requested()) return;
            // Numbered TTBL/TTPL entries may have been persisted without
            // tags (ReadInfo=on-demand).  The original CPlayItem library is
            // populated after those objects have resolved their fields.  Do
            // the equivalent enrichment off the UI thread, once per unique
            // local identity, so Artist/Album/Genre/Date nodes do not depend
            // solely on a prior tooltip or playback request.
            const auto identity = Identity(seed.track);
            if (!identities.contains(identity))
                ReadTrackMetadata(seed.track, request.sound_library);
            add(std::move(seed.track), seed.source);
        }

        for (const auto& directory : request.directories) {
            if (stop.stop_requested()) break;
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(
                directory, std::filesystem::directory_options::skip_permission_denied,
                error);
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end && !stop.stop_requested()) {
                std::error_code status_error;
                if (iterator->is_regular_file(status_error)) {
                    auto extension = Fold(iterator->path().extension().wstring());
                    if (!extension.empty() && request.extensions.contains(extension)) {
                        playlist::Track track;
                        track.path = iterator->path();
                        if (!identities.contains(Identity(track)))
                            ReadTrackMetadata(track, request.sound_library);
                        add(std::move(track), {});
                    }
                }
                iterator.increment(error);
                if (error) error.clear();
            }
        }
        if (stop.stop_requested()) return;
        // Never hold a PlayerWindow pointer in the worker.  The receiver
        // handshake makes shutdown non-blocking without allowing a late
        // result to be posted after ShutdownMediaLibrary has drained the
        // queue.  A per-state token additionally rejects HWND reuse.
        DeliverBuildResult(control, std::move(result));
    }
};

namespace {
std::vector<std::wstring> LibraryLabels(std::wstring resource) {
    auto labels = Split(resource, L'|');
    static constexpr std::wstring_view fallback[] = {
        L"Music", L"Artist", L"Album", L"Genre", L"Date", L"Rating",
        L"%d stars"};
    if (labels.size() < std::size(fallback)) {
        labels.assign(std::begin(fallback), std::end(fallback));
    }
    return labels;
}
} // namespace

size_t PlayerWindow::VisiblePlaylistTrackCount() const noexcept {
    if (settings_.playlist.library_mode && media_library_)
        return media_library_->result_tracks.size();
    return ActivePlaylist().Tracks().size();
}

const playlist::Track* PlayerWindow::VisiblePlaylistTrack(
    size_t index) const noexcept {
    if (settings_.playlist.library_mode && media_library_) {
        return index < media_library_->result_tracks.size()
            ? &media_library_->result_tracks[index] : nullptr;
    }
    return index < ActivePlaylist().Tracks().size()
        ? &ActivePlaylist().Tracks()[index] : nullptr;
}

bool PlayerWindow::SetVisiblePlaylistRating(size_t index, int rating) {
    if (rating < 0 || rating > 5) return false;
    if (!settings_.playlist.library_mode || !media_library_) {
        if (!ActivePlaylist().SetRating(index, rating)) return false;
        playlists_.MarkDirty();
        return true;
    }
    auto& state = *media_library_;
    if (index >= state.result_items.size() ||
        index >= state.result_tracks.size()) return false;
    const size_t item_index = state.result_items[index];
    if (item_index >= state.items.size()) return false;
    auto& item = state.items[item_index];
    bool changed = item.track.rating != rating ||
                   state.result_tracks[index].rating != rating;
    item.track.rating = rating;
    state.result_tracks[index].rating = rating;

    for (auto& persisted : state.persisted_tracks) {
        if (MediaLibraryState::Identity(persisted) != item.identity) continue;
        changed |= persisted.rating != rating;
        persisted.rating = rating;
    }
    for (const auto source : item.sources) {
        if (source.playlist >= playlists_.Size()) continue;
        auto& list = playlists_.At(source.playlist);
        if (source.row >= list.Tracks().size() ||
            MediaLibraryState::Identity(list.Tracks()[source.row]) !=
                item.identity) continue;
        if (list.SetRating(source.row, rating)) {
            playlists_.MarkDirty(source.playlist);
            changed = true;
        }
    }
    if (media_library_playback_active_ &&
        SetMediaLibraryPlaybackRating(media_library_playback_,
                                      item.identity, rating) != 0)
        changed = true;
    return changed;
}

std::vector<std::wstring>
PlayerWindow::CaptureSelectedMediaLibraryIdentities() const {
    std::vector<std::wstring> identities;
    if (!settings_.playlist.library_mode || !media_library_) return identities;
    std::set<std::wstring, std::less<>> unique;
    for (const size_t row : playlist_selected_rows_) {
        if (row >= media_library_->result_items.size()) continue;
        const size_t item = media_library_->result_items[row];
        if (item < media_library_->items.size())
            unique.insert(media_library_->items[item].identity);
    }
    identities.assign(unique.begin(), unique.end());
    return identities;
}

void PlayerWindow::RemoveMediaLibraryTracksByIdentity(
    const std::vector<std::wstring>& identities) {
    if (!media_library_ || identities.empty()) return;
    auto& state = *media_library_;
    const std::set<std::wstring, std::less<>> targets(
        identities.begin(), identities.end());
    state.excluded.insert(targets.begin(), targets.end());
    std::erase_if(state.persisted_tracks, [&targets](const auto& track) {
        return targets.contains(MediaLibraryTrackIdentity(track));
    });

    std::optional<size_t> first_removed;
    for (size_t row = state.result_items.size(); row-- > 0;) {
        const size_t item = state.result_items[row];
        if (item >= state.items.size() ||
            !targets.contains(state.items[item].identity)) continue;
        first_removed = row;
        state.result_items.erase(state.result_items.begin() +
                                 static_cast<ptrdiff_t>(row));
        state.result_tracks.erase(state.result_tracks.begin() +
                                  static_cast<ptrdiff_t>(row));
    }

    if (media_library_playback_active_) {
        const auto erased = EraseMediaLibraryPlaybackTracks(
            media_library_playback_, identities, current_);
        if (erased.removed_current) current_.reset();
        else if (current_) *current_ -= erased.removed_before_current;
    }
    if (!first_removed) return;

    playlist_selected_rows_.clear();
    if (state.result_tracks.empty()) {
        playlist_selection_.reset();
        playlist_selection_anchor_.reset();
        playlist_scroll_ = 0;
    } else {
        playlist_selection_ = std::min(*first_removed,
                                       state.result_tracks.size() - 1);
        playlist_selection_anchor_ = playlist_selection_;
        playlist_selected_rows_.insert(*playlist_selection_);
        EnsurePlaylistSelectionVisible();
    }
    if (playlist_track_control_)
        SendMessageW(playlist_track_control_, LVM_SETITEMCOUNT,
                     state.result_tracks.size(), 0);
    UpdatePlaylistItemTipRects();
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
}

bool PlayerWindow::CommitMediaLibraryTracks(
    std::vector<playlist::Track> tracks, size_t insertion,
    bool start_if_idle) {
    if (!settings_.playlist.library_mode || !media_library_ || tracks.empty())
        return false;
    auto& state = *media_library_;
    const bool was_idle = !VisiblePlaylistPlayingRow() &&
                          !playback_source_open_;
    insertion = std::min(insertion, state.result_tracks.size());

    std::set<std::wstring, std::less<>> incoming;
    std::vector<std::pair<size_t, playlist::Track>> additions;
    additions.reserve(tracks.size());
    for (auto& track : tracks) {
        const auto identity = MediaLibraryState::Identity(track);
        if (!incoming.insert(identity).second) continue;
        state.excluded.erase(identity);

        auto item = std::find_if(state.items.begin(), state.items.end(),
            [&identity](const auto& candidate) {
                return candidate.identity == identity;
            });
        size_t item_index{};
        if (item == state.items.end()) {
            item_index = state.items.size();
            state.items.push_back({track, identity, {}});
        } else {
            item_index = static_cast<size_t>(item - state.items.begin());
            track = item->track;
        }

        const bool visible = std::any_of(
            state.result_items.begin(), state.result_items.end(),
            [item_index](size_t current) { return current == item_index; });
        if (!visible) additions.emplace_back(item_index, track);

        const bool persisted = std::any_of(
            state.persisted_tracks.begin(), state.persisted_tracks.end(),
            [&identity](const auto& candidate) {
                return MediaLibraryState::Identity(candidate) == identity;
            });
        if (!persisted) state.persisted_tracks.push_back(track);
        state.pending_tracks.push_back(track);
    }
    if (additions.empty()) {
        state.refresh_pending = true;
        if (!state.indexing) StartMediaLibraryRefresh();
        return false;
    }

    const size_t first = insertion;
    for (auto& [item_index, track] : additions) {
        state.result_items.insert(state.result_items.begin() +
            static_cast<ptrdiff_t>(insertion), item_index);
        state.result_tracks.insert(state.result_tracks.begin() +
            static_cast<ptrdiff_t>(insertion), std::move(track));
        ++insertion;
    }
    playlist_selected_rows_.clear();
    for (size_t row = first; row < insertion; ++row)
        playlist_selected_rows_.insert(row);
    playlist_selection_ = first;
    playlist_selection_anchor_ = first;
    EnsurePlaylistSelectionVisible();
    if (playlist_track_control_)
        SendMessageW(playlist_track_control_, LVM_SETITEMCOUNT,
                     state.result_tracks.size(), 0);
    UpdatePlaylistItemTipRects();
    settings_.library.valid = true;
    state.refresh_pending = true;
    if (!state.indexing) StartMediaLibraryRefresh();
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
    if (start_if_idle && was_idle) ActivateMediaLibraryResult(first, true);
    return true;
}

bool PlayerWindow::UpdateMediaLibraryTrackPath(
    const playlist::Track& source, const std::filesystem::path& target) {
    if (!media_library_ || target.empty()) return false;
    auto& state = *media_library_;
    auto updated = source;
    updated.path = target;
    bool changed{};

    for (auto& item : state.items) {
        if (_wcsicmp(item.track.path.c_str(), source.path.c_str()) != 0)
            continue;
        const auto old_identity = item.identity;
        item.track.path = target;
        item.identity = MediaLibraryState::Identity(item.track);
        if (state.excluded.erase(old_identity) != 0)
            state.excluded.insert(item.identity);
        changed = true;
    }
    changed |= ReplaceMediaLibraryTrackPath(
                   state.result_tracks, source, target) != 0;
    changed |= ReplaceMediaLibraryTrackPath(
                   state.persisted_tracks, source, target) != 0;
    changed |= ReplaceMediaLibraryTrackPath(
                   state.pending_tracks, source, target) != 0;
    for (size_t row = 0; row < media_library_playback_.Tracks().size(); ++row) {
        if (_wcsicmp(media_library_playback_.Tracks()[row].path.c_str(),
                     source.path.c_str()) == 0)
            changed |= media_library_playback_.SetPath(row, target);
    }
    if (!changed) return false;

    state.pending_tracks.push_back(std::move(updated));
    settings_.library.valid = true;
    state.refresh_pending = true;
    if (!state.indexing) StartMediaLibraryRefresh();
    return true;
}

bool PlayerWindow::UpdateMediaLibraryTrackFromProperties(
    size_t index, const playlist::Track& updated) {
    if (!settings_.playlist.library_mode || !media_library_ ||
        index >= media_library_->result_items.size() ||
        index >= media_library_->result_tracks.size())
        return false;
    auto& state = *media_library_;
    const size_t item_index = state.result_items[index];
    if (item_index >= state.items.size()) return false;
    return UpdateMediaLibraryTrackByIdentity(
        state.items[item_index].track, updated);
}

bool PlayerWindow::UpdateMediaLibraryTrackByIdentity(
    const playlist::Track& source, const playlist::Track& updated) {
    // The main-window 00464A94 route can edit a playback item after the user
    // has selected another Tree query, so this update cannot depend on a
    // currently visible result ordinal.
    if (!media_library_) return false;
    auto& state = *media_library_;
    const auto identity = MediaLibraryState::Identity(source);
    if (MediaLibraryState::Identity(updated) != identity) return false;
    auto found = std::find_if(state.items.begin(), state.items.end(),
        [&identity](const auto& item) { return item.identity == identity; });
    if (found == state.items.end()) return false;
    auto& item = *found;

    // A library row is an index result, not an ActivePlaylist row.  Update
    // every materialized copy first and then each validated source identity;
    // this is the observable 00483C59 library branch without aliasing the
    // result ordinal onto an unrelated active-list item.
    item.track = updated;
    bool changed = true;
    for (auto& result : state.result_tracks) {
        if (MediaLibraryState::Identity(result) == identity) result = updated;
    }
    for (auto& persisted : state.persisted_tracks) {
        if (MediaLibraryState::Identity(persisted) == identity)
            persisted = updated;
    }
    for (const auto& origin : item.sources) {
        if (origin.playlist >= playlists_.Size()) continue;
        auto& list = playlists_.At(origin.playlist);
        if (origin.row >= list.Tracks().size() ||
            MediaLibraryState::Identity(list.Tracks()[origin.row]) !=
                identity)
            continue;
        changed |= list.SetDuration(origin.row, updated.duration_ms);
        changed |= list.SetMetadata(origin.row, updated.title, updated.artist,
                                    updated.album);
        changed |= list.SetExtendedMetadata(
            origin.row, updated.metadata, updated.media_type,
            updated.bitrate_bps, updated.sample_rate_hz);
        changed |= list.SetRating(origin.row, updated.rating);
        playlists_.MarkDirty(origin.playlist);
    }
    if (media_library_playback_active_) {
        for (size_t row = 0;
             row < media_library_playback_.Tracks().size(); ++row) {
            if (MediaLibraryState::Identity(
                    media_library_playback_.Tracks()[row]) != identity)
                continue;
            changed |= media_library_playback_.SetDuration(
                row, updated.duration_ms);
            changed |= media_library_playback_.SetMetadata(
                row, updated.title, updated.artist, updated.album);
            changed |= media_library_playback_.SetExtendedMetadata(
                row, updated.metadata, updated.media_type,
                updated.bitrate_bps, updated.sample_rate_hz);
            changed |= media_library_playback_.SetRating(row, updated.rating);
        }
    }
    return changed;
}

std::optional<size_t> PlayerWindow::VisiblePlaylistPlayingRow() const {
    if (!settings_.playlist.library_mode || !media_library_)
        return ActivePlaylist().PlayingRow();
    if (!HasPlaybackTrack()) return std::nullopt;
    const auto identity = MediaLibraryState::Identity(
        PlaybackPlaylist().Tracks()[*current_]);
    for (size_t row = 0; row < media_library_->result_items.size(); ++row) {
        const size_t item = media_library_->result_items[row];
        if (item < media_library_->items.size() &&
            media_library_->items[item].identity == identity) return row;
    }
    return std::nullopt;
}

void PlayerWindow::InitializeMediaLibraryTree() {
    if (!playlist_tree_control_) return;
    if (!media_library_) media_library_ = std::make_shared<MediaLibraryState>();
    auto& state = *media_library_;

    SendMessageW(playlist_tree_control_, TVM_SETBKCOLOR, 0,
                 settings_.playlist.background_color);
    SendMessageW(playlist_tree_control_, TVM_SETTEXTCOLOR, 0,
                 settings_.playlist.text_color);
    SendMessageW(playlist_tree_control_, TVM_SETLINECOLOR, 0,
                 settings_.playlist.text_color);
    if (!state.tree_font) {
        LOGFONTW font{};
        if (settings_.playlist.font_descriptor_valid) {
            font = settings_.playlist.font_descriptor;
        } else {
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0))
                font = metrics.lfMessageFont;
            font.lfHeight = settings_.playlist.font_height;
            wcsncpy_s(font.lfFaceName, settings_.playlist.font.c_str(), _TRUNCATE);
        }
        state.tree_font = CreateFontIndirectW(&font);
    }
    if (state.tree_font)
        SendMessageW(playlist_tree_control_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state.tree_font), TRUE);

    const auto rebuild = [this, &state]() {
        state.rebuilding = true;
        TreeView_DeleteAllItems(playlist_tree_control_);
        state.nodes.clear();
        auto node = std::make_unique<MediaLibraryState::Node>();
        node->kind = MediaLibraryState::NodeKind::root;
        node->text = LibraryLabels(ResourceText(0x81cc))[0];
        TVINSERTSTRUCTW insert{};
        insert.hParent = TVI_ROOT;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        insert.item.pszText = node->text.data();
        insert.item.lParam = reinterpret_cast<LPARAM>(node.get());
        insert.item.cChildren = 1;
        node->item = TreeView_InsertItem(playlist_tree_control_, &insert);
        state.root = node.get();
        state.nodes.push_back(std::move(node));
        state.rebuilding = false;
        TreeView_Expand(playlist_tree_control_, state.root->item, TVE_EXPAND);
    };
    rebuild();
    HTREEITEM select = state.root ? state.root->item : nullptr;
    auto path = Split(settings_.library.playing_catalog, L'\t');
    if (!path.empty() && !path[0].empty() && state.root) {
        HTREEITEM parent = state.root->item;
        for (size_t level = 0; level < path.size(); ++level) {
            TreeView_Expand(playlist_tree_control_, parent, TVE_EXPAND);
            HTREEITEM found{};
            for (HTREEITEM child = TreeView_GetChild(playlist_tree_control_, parent);
                 child; child = TreeView_GetNextSibling(playlist_tree_control_, child)) {
                TVITEMW item{};
                item.mask = TVIF_PARAM;
                item.hItem = child;
                TreeView_GetItem(playlist_tree_control_, &item);
                auto* data = reinterpret_cast<MediaLibraryState::Node*>(item.lParam);
                if (!data) continue;
                const bool match = level == 0
                    ? _wcsicmp(Wide(data->key).c_str(), path[level].c_str()) == 0
                    : _wcsicmp(data->text.c_str(), path[level].c_str()) == 0;
                if (match) { found = child; break; }
            }
            if (!found) break;
            select = found;
            parent = found;
        }
    }
    TreeView_SelectItem(playlist_tree_control_, select);
}

void PlayerWindow::SetMediaLibraryMode(bool enabled) {
    for (size_t index = 0; index < playlists_.Size(); ++index)
        playlists_.At(index).SetCurrentRow(std::nullopt);
    playlist_selection_.reset();
    playlist_selection_anchor_.reset();
    playlist_selected_rows_.clear();
    playlist_scroll_ = 0;
    settings_.playlist.library_mode = enabled;
    if (enabled) {
        StartMediaLibraryRefresh();
        StartMediaLibraryMonitoring();
        InitializeMediaLibraryTree();
    } else {
        RestorePlaylistRowSelection();
    }
    LayoutPlaylistListControls();
    UpdatePlaylistItemTipRects();
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::PollMediaLibraryWorkers() {
    const auto state_owner = media_library_;
    if (!state_owner) return;
    auto& state = *state_owner;
    std::optional<unsigned long long> completed_without_result;
    for (auto worker = state.workers.begin(); worker != state.workers.end();) {
        const auto control = *worker;
        if (!control->complete.load(std::memory_order_acquire)) {
            ++worker;
            continue;
        }
        bool posted{};
        std::unique_ptr<MediaLibraryState::BuildResult> recovered;
        {
            std::scoped_lock receiver_lock(control->receiver_mutex);
            posted = control->result_posted;
            recovered = std::move(control->unposted_result);
        }
        if (!posted && !recovered && control->instance == state.instance &&
            control->generation == state.generation)
            completed_without_result = control->generation;
        worker = state.workers.erase(worker);
        if (recovered) {
            ApplyMediaLibraryIndex(
                reinterpret_cast<LPARAM>(recovered.release()));
            if (!media_library_ || media_library_.get() != state_owner.get())
                return;
        }
    }
    if (!media_library_ || media_library_.get() != state_owner.get()) return;
    if (completed_without_result &&
        *completed_without_result == state.generation && state.indexing)
        state.indexing = false;
    if (!state.shutting_down && !state.indexing && state.refresh_pending)
        StartMediaLibraryRefresh();
}

void PlayerWindow::StartMediaLibraryRefresh() {
    if (!window_) return;
    if (!settings_.library.enabled) {
        if (media_library_) {
            media_library_->refresh_pending = false;
            for (const auto& worker : media_library_->workers)
                worker->stop.request_stop();
        }
        return;
    }
    if (!media_library_) media_library_ = std::make_shared<MediaLibraryState>();
    auto& state = *media_library_;
    if (state.shutting_down) return;
    // Do not stack scanners when a network folder or third-party shell
    // property handler is stalled.  Refresh is idempotent while the current
    // snapshot is being built; a later click starts a new generation after
    // the completed control has been reaped.
    if (state.indexing || !state.workers.empty()) {
        state.refresh_pending = true;
        return;
    }
    state.refresh_pending = false;

    MediaLibraryState::BuildRequest request;
    request.instance = state.instance;
    request.generation = ++state.generation;
    request.control = std::make_shared<MediaLibraryState::WorkerControl>();
    request.control->receiver = window_;
    request.control->instance = request.instance;
    request.control->generation = request.generation;
    if (sound_library_)
        request.sound_library = sound_library_->RetainForBackground();
    // Native startup 004C03FD passes DAT_00547D60 (Library/MaxItemCount) to
    // 004AF271.  004AF271 calls 004AF1C5 to choose a prime bucket count (at
    // least 257); no insertion path compares against the setting.  Preserve
    // that capacity-only meaning instead of silently truncating the library.
    if (settings_.library.max_item_count > 0) {
        request.expected_item_count = static_cast<size_t>(
            settings_.library.max_item_count);
    }
    if (state.persistence_loaded) {
        for (const auto& track : state.persisted_tracks)
            request.seeds.push_back({track, {}});
    } else if (settings_.library.enabled) {
        request.persistence_path = MediaLibraryStoragePath();
        request.load_persistence = !request.persistence_path.empty();
    }
    for (size_t list = 0; list < playlists_.Size(); ++list) {
        const auto& tracks = playlists_.At(list).Tracks();
        for (size_t row = 0; row < tracks.size(); ++row)
            request.seeds.push_back({tracks[row], {list, row}});
    }
    for (auto& track : state.pending_tracks)
        request.priority_seeds.push_back({std::move(track), {}});
    state.pending_tracks.clear();
    for (const auto& path : state.pending_files) {
        bool copied_known{};
        for (const auto& item : state.items) {
            if (!IsSameOrBelowPath(item.track.path, path) ||
                !IsSameOrBelowPath(path, item.track.path)) continue;
            auto refreshed = item.track;
            // A Shell property store describes the physical file, not one
            // CUE subtrack. Replacing subtrack tags with container metadata
            // would corrupt its stable result identity/title; those entries
            // retain their CUE fields until the segmented source is reparsed.
            if (refreshed.subtrack == 0)
                MediaLibraryState::ResetShellMetadata(refreshed);
            request.priority_seeds.push_back(
                {std::move(refreshed), {}});
            copied_known = true;
        }
        if (!copied_known)
            request.priority_seeds.push_back({playlist::Track{path}, {}});
    }
    request.directories = std::move(state.pending_directories);
    state.pending_files.clear();
    state.pending_directories.clear();
    request.extensions = LibraryReaderExtensions(reader_formats_);
    state.indexing = true;
    auto control = request.control;
    const auto request_instance = request.instance;
    const auto request_generation = request.generation;
    state.workers.push_back(control);
    // Sound readers and property handlers supplied by codecs/shell extensions
    // are outside our control and can block indefinitely.  A detached worker
    // plus the receiver handshake above keeps window destruction bounded;
    // BuildRequest also retains each AddIn/DLL until its last reader session
    // has left the worker.
    try {
        std::thread([request = std::move(request), control,
                     request_instance, request_generation]() mutable {
            const auto post_failure = [&]() noexcept {
                try {
                    auto failed = std::make_unique<
                        MediaLibraryState::BuildResult>();
                    failed->instance = request_instance;
                    failed->generation = request_generation;
                    failed->failed = true;
                    MediaLibraryState::DeliverBuildResult(
                        control, std::move(failed));
                } catch (...) {
                    // PollMediaLibraryWorkers also recognizes a completed
                    // control with no posted/stored result as a failed run.
                }
            };
            try {
                MediaLibraryState::RunBuild(std::move(request));
            } catch (const std::exception&) {
                // A malformed shell property handler must not terminate the
                // process.  Completion is published only after the result
                // handoff attempt, so the UI never reaps a control mid-post.
                post_failure();
            } catch (...) {
                post_failure();
            }
            control->complete.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error&) {
        control->complete.store(true, std::memory_order_release);
        state.workers.remove(control);
        state.indexing = false;
    }
}

void PlayerWindow::StartMediaLibraryMonitoring(bool scan_new_directories) {
    if (!media_library_) {
        if (!settings_.library.enabled ||
            !settings_.library.monitor_directories) return;
        media_library_ = std::make_shared<MediaLibraryState>();
    }
    auto& state = *media_library_;
    state.monitor.Stop();
    if (!settings_.library.enabled ||
        !settings_.library.monitor_directories || !window_) {
        state.monitored_directories.clear();
        return;
    }

    std::vector<std::filesystem::path> directories;
    directories.reserve(std::min<size_t>(
        settings_.library.directories.size(), 63));
    for (const auto& directory : settings_.library.directories) {
        if (!directory.enabled || directory.path.empty()) continue;
        directories.push_back(directory.path);
        if (directories.size() == 63) break;
    }
    std::set<std::wstring> current;
    for (const auto& directory : directories) {
        const auto identity = NormalizedLocalPath(directory);
        current.insert(identity);
        if (scan_new_directories &&
            !state.monitored_directories.contains(identity))
            state.pending_directories.push_back(directory);
    }
    state.monitored_directories = std::move(current);
    static_cast<void>(state.monitor.Start(
        window_, kMsgMediaLibraryChanged, directories));
}

void PlayerWindow::ApplyMediaLibraryConfiguration() {
    if (!settings_.library.enabled) {
        if (media_library_) {
            auto& state = *media_library_;
            state.monitor.Stop();
            state.refresh_pending = false;
            state.pending_tracks.clear();
            state.pending_files.clear();
            state.pending_directories.clear();
            for (const auto& worker : state.workers)
                worker->stop.request_stop();
        }
        const auto persistence_path = MediaLibraryStoragePath();
        if (!persistence_path.empty()) DeleteFileW(persistence_path.c_str());
        settings_.library.valid = false;
        if (settings_.playlist.library_mode) SetMediaLibraryMode(false);
        return;
    }
    StartMediaLibraryMonitoring(true);
    StartMediaLibraryRefresh();
}

LRESULT PlayerWindow::HandleMediaLibraryDirectoryChange(
    WPARAM action, LPARAM value) {
    std::unique_ptr<DirectoryChangeNotification> notification(
        reinterpret_cast<DirectoryChangeNotification*>(value));
    if (!notification || !media_library_ ||
        !media_library_->monitor.Accept(notification->generation)) return 0;
    auto& state = *media_library_;
    const DWORD file_action = notification->action != 0
        ? notification->action : static_cast<DWORD>(action);
    const auto& path = notification->path;

    if (file_action == FILE_ACTION_REMOVED ||
        file_action == FILE_ACTION_RENAMED_OLD_NAME) {
        std::vector<std::wstring> identities;
        for (const auto& item : state.items) {
            if (IsSameOrBelowPath(item.track.path, path))
                identities.push_back(item.identity);
        }
        RemoveMediaLibraryTracksByIdentity(identities);
        return 0;
    }

    if (file_action != FILE_ACTION_ADDED &&
        file_action != FILE_ACTION_RENAMED_NEW_NAME &&
        file_action != FILE_ACTION_MODIFIED) return 0;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;

    // A native add/rename notification revives a previously tombstoned
    // identity.  Clear only identities at/below this exact path; unrelated
    // user removals in the same monitored directory remain absent.
    for (const auto& item : state.items) {
        if (IsSameOrBelowPath(item.track.path, path))
            state.excluded.erase(item.identity);
    }
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (directory) {
        if (file_action != FILE_ACTION_MODIFIED)
            state.pending_directories.push_back(path);
    } else {
        const auto extensions = LibraryReaderExtensions(reader_formats_);
        if (!extensions.contains(Fold(path.extension().wstring()))) return 0;
        const bool known = std::any_of(
            state.items.begin(), state.items.end(), [&path](const auto& item) {
                return IsSameOrBelowPath(item.track.path, path) &&
                       IsSameOrBelowPath(path, item.track.path);
            });
        // 004833B6 updates an existing item in place for action 3.  A modify
        // notification must therefore outrank the persisted/numbered seeds
        // even when this identity is already known; otherwise the interning
        // map retains stale Title/Artist/Album/Genre/Date indefinitely.
        if (!known || file_action == FILE_ACTION_MODIFIED) {
            const auto pending_identity = NormalizedLocalPath(path);
            const bool already_pending = std::any_of(
                state.pending_files.begin(), state.pending_files.end(),
                [&pending_identity](const auto& pending) {
                    return NormalizedLocalPath(pending) == pending_identity;
                });
            if (!already_pending) state.pending_files.push_back(path);
        }
    }
    if (state.pending_files.empty() && state.pending_directories.empty()) {
        if (settings_.playlist.library_mode)
            InitializeMediaLibraryTree();
        return 0;
    }
    state.refresh_pending = true;
    if (!state.indexing) StartMediaLibraryRefresh();
    return 0;
}

void PlayerWindow::ShutdownMediaLibrary() {
    const auto persistence_path = MediaLibraryStoragePath();
    if (media_library_) {
        auto& state = *media_library_;
        state.shutting_down = true;
        state.refresh_pending = false;
        state.monitor.Stop();

        const auto drain_ready = [this] {
            MSG message{};
            while (PeekMessageW(&message, window_, kMsgMediaLibraryReady,
                                kMsgMediaLibraryReady, PM_REMOVE))
                ApplyMediaLibraryIndex(message.lParam);
        };
        // Give a result which is already being finalized a short opportunity
        // to reach the UI so Music.library does not persist the preceding
        // generation. Never wait on an untrusted property handler or an
        // unreachable share for more than this bounded close-path allowance.
        constexpr ULONGLONG kShutdownWorkerWaitMs = 100;
        const ULONGLONG started = GetTickCount64();
        for (;;) {
            drain_ready();
            PollMediaLibraryWorkers();
            if (!media_library_) break;
            const bool complete = std::all_of(
                state.workers.begin(), state.workers.end(),
                [](const auto& worker) {
                    return worker->complete.load(std::memory_order_acquire);
                });
            if (complete ||
                GetTickCount64() - started >= kShutdownWorkerWaitMs) break;
            Sleep(1);
        }
        for (const auto& worker : state.workers)
            worker->stop.request_stop();
        drain_ready();
        PollMediaLibraryWorkers();

        // Serialize with every possible final PostMessage. Results which
        // lost that race stay in WorkerControl and are applied below before
        // the persisted snapshot is synthesized.
        for (const auto& worker : state.workers) {
            std::unique_ptr<MediaLibraryState::BuildResult> recovered;
            {
                std::scoped_lock receiver_lock(worker->receiver_mutex);
                worker->receiver = nullptr;
                recovered = std::move(worker->unposted_result);
            }
            if (recovered)
                ApplyMediaLibraryIndex(
                    reinterpret_cast<LPARAM>(recovered.release()));
        }
        drain_ready();
        PollMediaLibraryWorkers();

        // Native shutdown 00461950 copies the live catalogue count
        // (DAT_00546F7C) back to DAT_00547D60 before settings are saved.  The
        // value is used solely to size the next run's hash table.  Tombstoned
        // CPlayItems are not emitted by 004AF838 and therefore are not part
        // of the reusable live-count hint.
        const auto live_count = std::count_if(
            state.items.begin(), state.items.end(), [&state](const auto& item) {
                return !state.excluded.contains(item.identity);
            });
        settings_.library.max_item_count = static_cast<int>(std::min<size_t>(
            live_count, static_cast<size_t>(std::numeric_limits<int>::max())));
    }

    if (!settings_.library.enabled) {
        // 004616BD deletes Music.library when DAT_00547D40 (Library/Enabled)
        // is false, even if the media-library UI was never entered.
        if (!persistence_path.empty())
            DeleteFileW(persistence_path.c_str());
    } else if (media_library_ && media_library_->persistence_loaded &&
               !persistence_path.empty()) {
        // 004AF838 first synthesizes a playlist from every live CPlayItem
        // whose tombstone (+0x90) is zero, then saves it through the ordinary
        // TTBL writer.  `excluded` is the rebuilt tombstone set.
        playlist::Playlist persisted;
        persisted.SetTitle(ResourceText(0x81ce));
        for (const auto& item : media_library_->items) {
            if (!media_library_->excluded.contains(item.identity))
                persisted.Add(item.track);
        }
        try {
            persisted.SaveTtbl(persistence_path);
            settings_.library.valid = true;
        } catch (const std::exception&) {
            // Match the non-fatal native shutdown path: playlist/settings
            // persistence and window destruction must still complete.
        }
    }
    if (!media_library_) return;
    if (playlist_tree_control_ && IsWindow(playlist_tree_control_)) {
        media_library_->rebuilding = true;
        TreeView_DeleteAllItems(playlist_tree_control_);
        SendMessageW(playlist_tree_control_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     FALSE);
    }
    media_library_.reset();
    MSG message{};
    while (PeekMessageW(&message, window_, kMsgMediaLibraryReady,
                        kMsgMediaLibraryReady, PM_REMOVE)) {
        delete reinterpret_cast<MediaLibraryState::BuildResult*>(message.lParam);
    }
    while (PeekMessageW(&message, window_, kMsgMediaLibraryChanged,
                        kMsgMediaLibraryChanged, PM_REMOVE)) {
        delete reinterpret_cast<DirectoryChangeNotification*>(message.lParam);
    }
}

LRESULT PlayerWindow::ApplyMediaLibraryIndex(LPARAM value) {
    std::unique_ptr<MediaLibraryState::BuildResult> result(
        reinterpret_cast<MediaLibraryState::BuildResult*>(value));
    if (!result || !media_library_ ||
        result->instance != media_library_->instance ||
        result->generation != media_library_->generation) return 0;
    media_library_->indexing = false;
    if (!settings_.library.enabled) {
        media_library_->refresh_pending = false;
        return 0;
    }
    if (result->failed) {
        if (!media_library_->shutting_down &&
            media_library_->refresh_pending) StartMediaLibraryRefresh();
        return 0;
    }
    media_library_->items = std::move(result->items);
    if (result->persistence_attempted)
        media_library_->persistence_loaded = true;
    if (media_library_->persistence_loaded) {
        media_library_->persisted_tracks.clear();
        media_library_->persisted_tracks.reserve(media_library_->items.size());
        for (const auto& item : media_library_->items) {
            if (!media_library_->excluded.contains(item.identity))
                media_library_->persisted_tracks.push_back(item.track);
        }
    }
    settings_.library.valid = true;
    if (!media_library_->shutting_down && settings_.playlist.library_mode)
        InitializeMediaLibraryTree();
    if (media_library_ && !media_library_->shutting_down &&
        media_library_->refresh_pending)
        StartMediaLibraryRefresh();
    return 0;
}

bool PlayerWindow::HandleMediaLibraryTreeNotification(
    const NMHDR* notification) {
    if (!notification || notification->hwndFrom != playlist_tree_control_ ||
        !media_library_) return false;
    auto& state = *media_library_;
    const auto node_from_item = [](const TVITEMW& item) {
        return reinterpret_cast<MediaLibraryState::Node*>(item.lParam);
    };
    const auto insert_node = [this, &state](HTREEITEM parent,
            MediaLibraryState::NodeKind kind, std::string key,
            std::wstring text, std::wstring value, std::wstring artist,
            int rating, int children) {
        auto node = std::make_unique<MediaLibraryState::Node>();
        node->kind = kind;
        node->key = std::move(key);
        node->text = std::move(text);
        node->value = std::move(value);
        node->artist = std::move(artist);
        node->rating = rating;
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        insert.item.pszText = node->text.data();
        insert.item.lParam = reinterpret_cast<LPARAM>(node.get());
        insert.item.cChildren = children;
        node->item = TreeView_InsertItem(playlist_tree_control_, &insert);
        auto* result = node.get();
        state.nodes.push_back(std::move(node));
        return result;
    };
    const auto unique_values = [&state](std::string_view key,
                                        std::wstring_view artist) {
        std::map<std::wstring, std::wstring> values;
        for (const auto& item : state.items) {
            if (state.excluded.contains(item.identity)) continue;
            if (!artist.empty() &&
                _wcsicmp(MetadataValue(item.track, "Artist").c_str(),
                         std::wstring(artist).c_str()) != 0) continue;
            auto value = MetadataValue(item.track, key);
            if (!value.empty()) values.try_emplace(Fold(value), std::move(value));
        }
        std::vector<std::wstring> result;
        result.reserve(values.size());
        for (auto& entry : values) result.push_back(std::move(entry.second));
        std::sort(result.begin(), result.end(), [](const auto& left,
                                                   const auto& right) {
            return LogicalCompare(left, right) < 0;
        });
        return result;
    };
    const auto populate = [&](MediaLibraryState::Node* node) {
        if (!node || TreeView_GetChild(playlist_tree_control_, node->item)) return;
        const auto labels = LibraryLabels(ResourceText(0x81cc));
        if (node->kind == MediaLibraryState::NodeKind::root) {
            static constexpr std::string_view keys[] = {
                "Artist", "Album", "Genre", "Date", "Rating"};
            for (size_t index = 0; index < std::size(keys); ++index) {
                std::wstring text = labels[index + 1];
                if (keys[index] != "Rating") {
                    const auto count = unique_values(keys[index], {}).size();
                    if (count != 0) text += L" (" + std::to_wstring(count) + L")";
                }
                insert_node(node->item, MediaLibraryState::NodeKind::category,
                    std::string(keys[index]), std::move(text), {}, {}, 0, 1);
            }
            return;
        }
        if (node->kind == MediaLibraryState::NodeKind::category) {
            if (node->key == "Rating") {
                for (int rating = 5; rating >= 1; --rating) {
                    wchar_t text[80]{};
                    const auto format = labels[6];
                    if (!format.empty()) swprintf_s(text, format.c_str(), rating);
                    insert_node(node->item, MediaLibraryState::NodeKind::rating,
                        "Rating", text[0] ? text : std::to_wstring(rating),
                        {}, {}, rating, 0);
                }
                return;
            }
            for (auto value : unique_values(node->key, {})) {
                const bool artist = node->key == "Artist";
                insert_node(node->item, MediaLibraryState::NodeKind::value,
                    node->key, value, value, {}, 0, artist ? 1 : 0);
            }
            return;
        }
        if (node->kind == MediaLibraryState::NodeKind::value &&
            node->key == "Artist") {
            for (auto album : unique_values("Album", node->value)) {
                insert_node(node->item,
                    MediaLibraryState::NodeKind::artist_album, "Album",
                    album, album, node->value, 0, 0);
            }
        }
    };
    const auto query = [this, &state](MediaLibraryState::Node* node) {
        state.result_tracks.clear();
        state.result_items.clear();
        if (!node) return;
        for (size_t index = 0; index < state.items.size(); ++index) {
            const auto& item = state.items[index];
            if (state.excluded.contains(item.identity)) continue;
            bool include{};
            switch (node->kind) {
            case MediaLibraryState::NodeKind::root:
                include = true;
                break;
            case MediaLibraryState::NodeKind::category:
                // A category selection is the aggregate of its populated
                // children; blank fields are not members of Artist/Album/
                // Genre/Date, and the Rating root contains only rated items.
                // Value/rating leaves below refine this same set.
                include = node->key == "Rating"
                    ? item.track.rating > 0
                    : !MetadataValue(item.track, node->key).empty();
                break;
            case MediaLibraryState::NodeKind::value:
                include = _wcsicmp(MetadataValue(item.track, node->key).c_str(),
                                   node->value.c_str()) == 0;
                break;
            case MediaLibraryState::NodeKind::artist_album:
                include = _wcsicmp(MetadataValue(item.track, "Artist").c_str(),
                                   node->artist.c_str()) == 0 &&
                          _wcsicmp(MetadataValue(item.track, "Album").c_str(),
                                   node->value.c_str()) == 0;
                break;
            case MediaLibraryState::NodeKind::rating:
                include = item.track.rating == node->rating;
                break;
            }
            if (include) state.result_items.push_back(index);
        }
        std::stable_sort(state.result_items.begin(), state.result_items.end(),
            [this, &state](size_t left, size_t right) {
                return LogicalCompare(PlaylistDisplayText(state.items[left].track),
                    PlaylistDisplayText(state.items[right].track)) < 0;
            });
        state.result_tracks.reserve(state.result_items.size());
        for (const auto index : state.result_items)
            state.result_tracks.push_back(state.items[index].track);

        std::vector<std::wstring> path;
        if (node->kind != MediaLibraryState::NodeKind::root) {
            if (node->kind == MediaLibraryState::NodeKind::artist_album) {
                path = {L"Artist", node->artist, node->text};
            } else {
                path.push_back(Wide(node->key));
                if (node->kind == MediaLibraryState::NodeKind::value ||
                    node->kind == MediaLibraryState::NodeKind::rating)
                    path.push_back(node->text);
            }
        }
        if (!state.indexing) {
            settings_.library.playing_catalog.clear();
            for (const auto& component : path) {
                if (!settings_.library.playing_catalog.empty())
                    settings_.library.playing_catalog.push_back(L'\t');
                settings_.library.playing_catalog += component;
            }
        }
        playlist_scroll_ = 0;
        playlist_selection_.reset();
        playlist_selection_anchor_.reset();
        playlist_selected_rows_.clear();
        if (playlist_track_control_)
            SendMessageW(playlist_track_control_, LVM_SETITEMCOUNT,
                         state.result_tracks.size(), 0);
        UpdatePlaylistItemTipRects();
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
    };

    if (notification->code == TVN_ITEMEXPANDINGW) {
        const auto* change = reinterpret_cast<const NMTREEVIEWW*>(notification);
        if ((change->action & TVE_EXPAND) != 0) populate(node_from_item(change->itemNew));
        return true;
    }
    if (notification->code == TVN_SELCHANGEDW) {
        if (!state.rebuilding) {
            const auto* change = reinterpret_cast<const NMTREEVIEWW*>(notification);
            query(node_from_item(change->itemNew));
        }
        return true;
    }
    if (notification->code == NM_DBLCLK) {
        TVITEMW item{};
        item.mask = TVIF_PARAM;
        item.hItem = TreeView_GetSelection(playlist_tree_control_);
        if (item.hItem && TreeView_GetItem(playlist_tree_control_, &item)) {
            auto* node = node_from_item(item);
            if (node && node->kind != MediaLibraryState::NodeKind::root &&
                !state.result_tracks.empty()) ActivateMediaLibraryResult(0, true);
        }
        return true;
    }
    if (notification->code == NM_RCLICK) {
        POINT point{};
        GetCursorPos(&point);
        return ShowMediaLibraryTreeContextMenu(point);
    }
    return false;
}

void PlayerWindow::ActivateMediaLibraryResult(size_t index,
                                               bool start_playback) {
    if (!media_library_ || index >= media_library_->result_tracks.size())
        return;
    if (!playlist_selection_) {
        playlist_selection_ = index;
        playlist_selection_anchor_ = index;
        playlist_selected_rows_.insert(index);
    }
    // FUN_0048A38E materializes the selected Tree query as its own CPlayList.
    // Even when an item is shared with a numbered list, Next/Previous follows
    // this query's complete order rather than the source list's order.  Keep
    // the whole result as a transient playback owner outside PlaylistStore.
    media_library_playback_ = BuildMediaLibraryPlaybackSnapshot(
        media_library_->result_tracks, index);
    media_library_playback_active_ = true;
    playing_playlist_index_.reset();
    SelectMediaLibraryPlaybackTrack(index, start_playback);
}

void PlayerWindow::SelectMediaLibraryPlaybackTrack(size_t index,
                                                    bool start_playback) {
    if (!media_library_playback_active_ ||
        index >= media_library_playback_.Tracks().size()) return;
    playing_playlist_index_.reset();
    current_ = index;
    media_library_playback_.SetCurrentRow(index);

    // If the same query is still visible, follow the playing identity without
    // assuming that a later Tree selection retained the snapshot's row order.
    if (settings_.playlist.library_mode && media_library_) {
        const auto identity = MediaLibraryState::Identity(
            media_library_playback_.Tracks()[index]);
        for (size_t row = 0; row < media_library_->result_items.size(); ++row) {
            const size_t item = media_library_->result_items[row];
            if (item >= media_library_->items.size() ||
                media_library_->items[item].identity != identity) continue;
            playlist_selection_ = row;
            playlist_selection_anchor_ = row;
            playlist_selected_rows_.clear();
            playlist_selected_rows_.insert(row);
            EnsurePlaylistSelectionVisible();
            break;
        }
    }

    const auto& track = media_library_playback_.Tracks()[index];
    display_title_ = std::to_wstring(index + 1) + L"." + DisplayName(track);
    display_artist_ = ArtistName(track, ResourceText(0x8ca5));
    RebuildSkinInfoItems(false);
    ResetSkinInfoScroll();
    associated_lyric_path_.clear();
    LoadCurrentLyrics();
    if (title_) SetWindowTextW(title_, display_title_.c_str());
    if (artist_) SetWindowTextW(artist_, display_artist_.c_str());
    if (skin_) InvalidateRect(window_, nullptr, FALSE);
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
    UpdateVisualFrame();
    if (start_playback) PlayCurrent();
}

bool PlayerWindow::ShowMediaLibraryTreeContextMenu(POINT screen_point) {
    if (!settings_.playlist.library_mode || !playlist_tree_control_ ||
        !media_library_) return false;
    // SysTreeView32 reports NM_RCLICK (the original 0048A818 route) and some
    // comctl32 builds additionally forward WM_CONTEXTMENU for the same mouse
    // release.  Suppress only that same queued input message; keyboard
    // context menus and a later right click remain independent.
    const DWORD message_time = static_cast<DWORD>(GetMessageTime());
    if (screen_point.x != -1 && screen_point.y != -1 &&
        media_library_->context_menu_message_time == message_time)
        return true;
    media_library_->context_menu_message_time = message_time;
    POINT client = screen_point;
    ScreenToClient(playlist_tree_control_, &client);
    TVHITTESTINFO hit{};
    hit.pt = client;
    HTREEITEM item = TreeView_HitTest(playlist_tree_control_, &hit);
    if (!item) item = TreeView_GetSelection(playlist_tree_control_);
    if (!item) return true;
    TreeView_SelectItem(playlist_tree_control_, item);
    SetFocus(playlist_tree_control_);

    TVITEMW tree_item{};
    tree_item.mask = TVIF_PARAM;
    tree_item.hItem = item;
    TreeView_GetItem(playlist_tree_control_, &tree_item);
    auto* node = reinterpret_cast<MediaLibraryState::Node*>(tree_item.lParam);
    HMENU owner = LoadMenuW(ResourceModule(), MAKEINTRESOURCEW(kMenuLibrary));
    if (!owner) return true;
    HMENU menu = GetSubMenu(owner, 0);
    if (menu) RemoveMenu(owner, 0, MF_BYPOSITION);
    DestroyMenu(owner);
    if (!menu) return true;
    if (!node || node->kind == MediaLibraryState::NodeKind::root ||
        node->kind == MediaLibraryState::NodeKind::category)
        DeleteMenu(menu, kPlaylistClear, MF_BYCOMMAND);
    if (screen_point.x == -1 && screen_point.y == -1) {
        RECT bounds{};
        TreeView_GetItemRect(playlist_tree_control_, item, &bounds, TRUE);
        screen_point = {bounds.left, bounds.bottom};
        ClientToScreen(playlist_tree_control_, &screen_point);
    }
    BeginPopupMenuStyle(menu);
    const UINT command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_point.x, screen_point.y,
        0, playlist_window_, nullptr);
    EndPopupMenuStyle();
    DestroyMenu(menu);
    if (command != 0 && !HandleMediaLibraryCommand(command))
        HandleContextCommand(command);
    return true;
}

bool PlayerWindow::HandleMediaLibraryCommand(UINT command) {
    if (!media_library_) return false;
    auto& state = *media_library_;
    const auto selected_tracks = [&]() {
        std::vector<playlist::Track> tracks;
        tracks.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (row < state.result_tracks.size())
                tracks.push_back(state.result_tracks[row]);
        }
        return tracks;
    };
    // These handlers already consume VisiblePlaylistTrack(), or operate on
    // window-level find state.  Let the shared command path run instead of
    // swallowing the command or translating a query row into ActivePlaylist
    // coordinates.  The three network actions retain their explicit retired-
    // backend notice and never claim a successful request.
    if (command == kPlaylistReplayGainScan ||
        command == kPlaylistReplayGainRemove ||
        command == kPlaylistConvert || command == kPlaylistDeleteFiles ||
        command == kPlaylistPaste || command == kPlaylistRenameTitle ||
        command == kPlaylistRenameArtistTitle ||
        command == kPlaylistRenameTitleArtist ||
        command == kPlaylistRenameCustom ||
        command == kPlaylistFreeDb || command == kPlaylistDownload ||
        command == kPlaylistReportOnline ||
        command == kPlaylistSendToFolder ||
        command == kPlaylistFind || command == kPlaylistFindNext ||
        command == kPlaylistQuickFind || command == kPlaylistSortLists)
        return false;
    if (command >= kPlaylistSendToFirst && command <= kPlaylistSendToLast) {
        if (!playlist_send_to_catalog_.Contains(command)) return true;
        const auto tracks = selected_tracks();
        auto* data = new (std::nothrow) LibraryDataObject(tracks);
        if (!data) return true;
        const auto outcome = playlist_send_to_catalog_.Drop(command, data);
        data->Release();
        if (outcome && outcome->remove_source)
            static_cast<void>(HandleMediaLibraryCommand(kPlaylistDeleteSelected));
        return true;
    }
    if (command == kPlaylistPlay) {
        if (playlist_selection_) ActivateMediaLibraryResult(*playlist_selection_, true);
        return true;
    }
    if (command == kPlaylistProperties) {
        // The recovered file-info sheet consumes VisiblePlaylistTrack(), so
        // its navigation, multi-item merge, reader metadata and tag writes
        // remain in library-result coordinates.  On close it calls
        // UpdateMediaLibraryTrackFromProperties rather than touching the
        // ActivePlaylist row with the same ordinal.
        if (GetFocus() == playlist_tree_control_) {
            // FUN_00483C59 distinguishes the focused control: LibraryTree
            // clones the complete query CPlayList, whereas Files contributes
            // only LVIS_SELECTED rows.  ShowPlaylistProperties snapshots the
            // visible model synchronously, so temporary selection state is
            // sufficient and need not repaint the Files list.
            const auto saved_rows = playlist_selected_rows_;
            const auto saved_selection = playlist_selection_;
            const auto saved_anchor = playlist_selection_anchor_;
            playlist_selected_rows_.clear();
            for (size_t row = 0; row < state.result_tracks.size(); ++row)
                playlist_selected_rows_.insert(row);
            if (!state.result_tracks.empty()) playlist_selection_ = 0;
            ShowPlaylistProperties();
            playlist_selected_rows_ = saved_rows;
            playlist_selection_ = saved_selection;
            playlist_selection_anchor_ = saved_anchor;
        } else {
            ShowPlaylistProperties();
        }
        return true;
    }
    if (command == kPlaylistBrowseFile) {
        const auto tracks = selected_tracks();
        if (tracks.empty()) return true;
        auto path = tracks.front().path.wstring();
        // FUN_004843CC first calls the original URL predicate 0041B6EA and
        // returns immediately for network items.  "Browse file" is an
        // Explorer-selection command, not a second way to launch a stream.
        if (path.find(L"://") != std::wstring::npos) return true;
        auto lower = Fold(path);
        size_t archive = lower.find(L".zip|");
        if (archive == std::wstring::npos) archive = lower.find(L".rar|");
        if (archive != std::wstring::npos) path.resize(archive + 4);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const std::wstring arguments = L"/e,/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(),
                          nullptr, SW_SHOWNORMAL);
        }
        return true;
    }
    if (command == kPlaylistCopy || command == kPlaylistCut) {
        auto tracks = selected_tracks();
        playlist_clipboard_tracks_ = tracks;
        static_cast<void>(PublishLibraryClipboard(playlist_window_, tracks));
        playlist_clipboard_sequence_ = GetClipboardSequenceNumber();
        if (command == kPlaylistCut)
            static_cast<void>(HandleMediaLibraryCommand(kPlaylistDeleteSelected));
        return true;
    }
    if (command == kPlaylistMoveToList || command == kPlaylistCopyToList) {
        auto tracks = selected_tracks();
        if (tracks.empty()) return true;
        LibraryTargetDialogState dialog{&playlists_};
        if (DialogBoxParamW(ResourceModule(), MAKEINTRESOURCEW(223),
                playlist_window_, LibraryTargetDialogProc,
                reinterpret_cast<LPARAM>(&dialog)) != IDOK)
            return true;

        const size_t original_active = playlists_.ActiveIndex();
        size_t target = dialog.selected;
        if (target == playlists_.Size()) {
            auto format = ResourceText(0x8192);
            wchar_t title[128]{};
            if (!format.empty())
                swprintf_s(title, format.c_str(), playlists_.Size() + 1);
            target = playlists_.NewList(title[0]
                ? std::wstring(title) : ResourceText(0x813a));
            playlists_.SetActive(original_active);
            settings_.player.playlist_scan_count =
                static_cast<int>(playlists_.Size());
        }
        if (target >= playlists_.Size()) return true;

        // CommitImportedTracks also updates delayed metadata and dirty-save
        // accounting.  When the chosen target is the hidden active numbered
        // list, preserve the Files query's selection/scroll state around that
        // shared transaction; 004864B2 copies CPlayItems without replacing
        // the currently materialized library query.
        const auto saved_rows = playlist_selected_rows_;
        const auto saved_selection = playlist_selection_;
        const auto saved_anchor = playlist_selection_anchor_;
        const size_t saved_scroll = playlist_scroll_;
        const auto saved_numbered_row = playlists_.At(target).CurrentRow();
        const bool imported = CommitImportedTracks(
            std::move(tracks), target, playlists_.At(target).Tracks().size(),
            false, ImportPlayback::none);
        playlists_.At(target).SetCurrentRow(saved_numbered_row);
        playlist_selected_rows_ = saved_rows;
        playlist_selection_ = saved_selection;
        playlist_selection_anchor_ = saved_anchor;
        playlist_scroll_ = saved_scroll;
        settings_.player.active_playlist =
            static_cast<int>(playlists_.ActiveSlot());
        UpdatePlaylistItemTipRects();
        if (imported && command == kPlaylistMoveToList) {
            const auto identities = CaptureSelectedMediaLibraryIdentities();
            RemoveMediaLibraryTracksByIdentity(identities);
        } else if (playlist_window_) {
            InvalidateRect(playlist_window_, nullptr, FALSE);
        }
        return true;
    }
    if (command == kPlaylistSaveList) {
        ModernSaveFileOptions dialog;
        dialog.owner = playlist_window_;
        dialog.filters = {
            {ResourceText(0x8125), L"*.ttpl;*.ttbl;*.m3u;*.m3u8"},
            {ResourceText(0x8124), L"*.*"}};
        dialog.default_extension = L"ttpl";
        if (const auto path = ModernSaveFile(dialog)) {
            playlist::Playlist result;
            result.SetTitle(ResourceText(0x81ce));
            for (const auto& track : state.result_tracks) result.Add(track);
            try {
                result.SaveToFile(*path, settings_.playlist.tag_title_format,
                    settings_.playlist.default_title_format,
                    settings_.playlist.save_relative_path,
                    settings_.playlist.save_tags);
            } catch (const std::exception&) {
                MessageBoxW(playlist_window_, ResourceText(0x828e).c_str(),
                    ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
            }
        }
        return true;
    }
    if (command == kPlaylistDeleteDuplicates) {
        // RunBuild interns normalized path+subtrack identities, so a healthy
        // query already contains no duplicate CPlayItem.  Still repair a
        // stale/malformed materialization without tombstoning the first live
        // catalogue item.
        std::set<std::wstring, std::less<>> seen;
        std::set<size_t> duplicate_rows;
        for (size_t row = 0; row < state.result_tracks.size(); ++row) {
            if (!seen.insert(MediaLibraryTrackIdentity(
                    state.result_tracks[row])).second)
                duplicate_rows.insert(row);
        }
        if (duplicate_rows.empty()) return true;
        const size_t first_removed = *duplicate_rows.begin();
        for (auto row = duplicate_rows.rbegin();
             row != duplicate_rows.rend(); ++row) {
            state.result_tracks.erase(state.result_tracks.begin() +
                static_cast<ptrdiff_t>(*row));
            state.result_items.erase(state.result_items.begin() +
                static_cast<ptrdiff_t>(*row));
        }
        playlist_selected_rows_.clear();
        if (state.result_tracks.empty()) {
            playlist_selection_.reset();
            playlist_selection_anchor_.reset();
            playlist_scroll_ = 0;
        } else {
            playlist_selection_ = std::min(first_removed,
                state.result_tracks.size() - 1);
            playlist_selection_anchor_ = playlist_selection_;
            playlist_selected_rows_.insert(*playlist_selection_);
            EnsurePlaylistSelectionVisible();
        }
        if (playlist_track_control_)
            SendMessageW(playlist_track_control_, LVM_SETITEMCOUNT,
                         state.result_tracks.size(), 0);
        UpdatePlaylistItemTipRects();
        if (playlist_window_)
            InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistDeleteInvalid) {
        std::vector<std::wstring> invalid;
        for (size_t row = 0; row < state.result_items.size() &&
                             row < state.result_tracks.size(); ++row) {
            if (LibraryTrackSourceValid(state.result_tracks[row])) continue;
            const size_t item = state.result_items[row];
            if (item < state.items.size())
                invalid.push_back(state.items[item].identity);
        }
        RemoveMediaLibraryTracksByIdentity(invalid);
        return true;
    }
    if ((command >= kPlaylistSortTitle &&
         command <= kPlaylistSortDuration) ||
        command == kPlaylistShuffle) {
        if (state.result_tracks.size() <= 1) return true;

        bool playback_is_query = media_library_playback_active_ &&
            media_library_playback_.Tracks().size() ==
                state.result_tracks.size();
        std::wstring playing_identity;
        if (playback_is_query) {
            std::set<std::wstring, std::less<>> visible;
            for (const auto& track : state.result_tracks)
                visible.insert(MediaLibraryTrackIdentity(track));
            for (const auto& track : media_library_playback_.Tracks()) {
                if (!visible.contains(MediaLibraryTrackIdentity(track))) {
                    playback_is_query = false;
                    break;
                }
            }
            if (playback_is_query && current_ &&
                *current_ < media_library_playback_.Tracks().size())
                playing_identity = MediaLibraryTrackIdentity(
                    media_library_playback_.Tracks()[*current_]);
        }

        bool reordered{};
        if (command == kPlaylistShuffle) {
            playlist_last_sort_command_ = 0;
            playlist_sort_ascending_ = false;
            reordered = ShuffleMediaLibraryResult(
                state.result_tracks, state.result_items);
        } else {
            const bool ascending = command == playlist_last_sort_command_
                ? !playlist_sort_ascending_ : true;
            playlist_last_sort_command_ = command;
            playlist_sort_ascending_ = ascending;
            const auto key = static_cast<playlist::SortKey>(
                command - kPlaylistSortTitle);
            if (key == playlist::SortKey::display_title) {
                std::vector<std::wstring> titles;
                titles.reserve(state.result_tracks.size());
                for (const auto& track : state.result_tracks)
                    titles.push_back(PlaylistDisplayText(track));
                reordered = SortMediaLibraryResult(state.result_tracks,
                    state.result_items, key, ascending, titles);
            } else {
                reordered = SortMediaLibraryResult(state.result_tracks,
                    state.result_items, key, ascending);
            }
        }
        if (!reordered) return true;

        if (playback_is_query) {
            media_library_playback_ = BuildMediaLibraryPlaybackSnapshot(
                state.result_tracks, state.result_tracks.size());
            current_.reset();
            for (size_t row = 0; row < state.result_tracks.size(); ++row) {
                if (MediaLibraryTrackIdentity(state.result_tracks[row]) !=
                    playing_identity) continue;
                current_ = row;
                media_library_playback_.SetCurrentRow(row);
                display_title_ = std::to_wstring(row + 1) + L"." +
                    DisplayName(media_library_playback_.Tracks()[row]);
                if (title_)
                    SetWindowTextW(title_, display_title_.c_str());
                RebuildSkinInfoItems(true);
                ResetSkinInfoScroll();
                break;
            }
        }
        if (skin_) InvalidateRect(window_, nullptr, FALSE);
        if (playlist_window_)
            InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistSelectAll) {
        playlist_selected_rows_.clear();
        for (size_t row = 0; row < state.result_tracks.size(); ++row)
            playlist_selected_rows_.insert(row);
        if (!state.result_tracks.empty()) playlist_selection_ = 0;
        if (playlist_track_control_) SetFocus(playlist_track_control_);
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistSelectNone) {
        playlist_selected_rows_.clear();
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistSelectInvert) {
        std::set<size_t> inverted;
        for (size_t row = 0; row < state.result_tracks.size(); ++row) {
            if (!playlist_selected_rows_.contains(row)) inverted.insert(row);
        }
        playlist_selected_rows_ = std::move(inverted);
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistDeleteSelected) {
        RemoveMediaLibraryTracksByIdentity(
            CaptureSelectedMediaLibraryIdentities());
        return true;
    }
    if (command == kPlaylistClear) {
        std::vector<std::wstring> identities;
        identities.reserve(state.result_items.size());
        for (const auto item : state.result_items) {
            if (item < state.items.size())
                identities.push_back(state.items[item].identity);
        }
        // FUN_00485BF7 tombstones the current result CPlayItems and clears
        // Files.  Its subsequent TVGN_PARENT probes do not delete or select a
        // TreeCtrl node, so keep both the catalogue path and its selection;
        // the stale value node disappears only on command 0x7FEE/rebuild.
        RemoveMediaLibraryTracksByIdentity(identities);
        return true;
    }
    if (command >= kPlaylistRatingFirst && command <= kPlaylistRatingLast) {
        const int rating = static_cast<int>(command - kPlaylistRatingFirst + 1);
        bool changed{};
        for (const size_t row : playlist_selected_rows_)
            changed |= SetVisiblePlaylistRating(row, rating);
        if (changed && playlist_window_)
            InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == 0x7fee) {
        StartMediaLibraryRefresh();
        return true;
    }
    if (command == kCmdLibraryOptions) {
        ShowOptions(6);
        return true;
    }
    return false;
}

} // namespace ttplayer::ui
