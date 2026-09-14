#include "ttplayer/ui/player_window.h"
#include "ttplayer/core/text.h"
#include "ttplayer/ui/lyric_runtime_policy.h"
#include <fstream>
#include <iostream>
#include <functional>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << bytes; Check(file.good(), "write fixture");
}
std::string Read(const fs::path& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
template<class F> void Until(F ready) {
    const auto deadline = GetTickCount64() + 5000;
    while (!ready()) {
        Check(GetTickCount64() < deadline, "asynchronous lyric timeout");
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    }
}
struct Driver {
    std::function<void(HWND)> run;
    std::exception_ptr failure;
    ULONGLONG deadline{GetTickCount64() + 5000};
    UINT_PTR timer{};
    bool visited{};
    static inline Driver* current{};
    static void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
            auto& self = *current;
            if (!GetDlgItem(window, 2115) && !GetDlgItem(window, 0x854)) return TRUE;
            if (!IsWindowVisible(window)) return TRUE;
            try {
                Check(GetTickCount64() < self.deadline, "modal lyric dialog timeout");
                if (self.failure) { EndDialog(window, IDCANCEL); return TRUE; }
                self.visited = true; self.run(window);
            } catch (...) { self.failure = std::current_exception(); EndDialog(window, IDCANCEL); }
            return TRUE;
        }, 0);
    }
    explicit Driver(std::function<void(HWND)> callback) : run(std::move(callback)) {
        current = this; timer = SetTimer(nullptr, 0, 30, Tick); Check(timer != 0, "dialog driver timer");
    }
    ~Driver() { KillTimer(nullptr, timer); current = nullptr; }
    void Verify() { Check(visited, "association dialog was not created"); if (failure) std::rethrow_exception(failure); }
};

void Core(const fs::path& directory) {
    const auto file = directory / L"TTPlayerRebuild.rll";
    const lyrics::SongKey song{directory / L"曲目.flac", 2};
    const auto lyric = directory / L"手动关联.lrc";
    lyrics::AssociationStore store;
    Check(store.Load(file), "new rll load");
    Check(store.Set(song, lyric) && store.Dirty() && store.Save(), "three-field rll save");
    auto upper = song; auto path = upper.path.wstring(); CharUpperBuffW(path.data(), static_cast<DWORD>(path.size())); upper.path = path;
    Check(store.Find(upper) == lyric && !store.Find({song.path, 0}), "association identity path case / subtrack");
    Check(Read(file).starts_with("\xef\xbb\xbf") && Read(file).ends_with("\r\n"), "UTF-8 BOM and CRLF");
    lyrics::AssociationStore reload; Check(reload.Load(file) && reload.Find(song) == lyric && !reload.Dirty(), "association restart persistence");
    Check(reload.Set(song, lyrics::kNoLyric) && reload.Save(), "no-lyric sentinel save");
    Check(store.Load(file) && store.Find(song) == fs::path(lyrics::kNoLyric), "sentinel reload");
    Check(!store.Set({L"bad|song", 0}, lyric) && !store.Set(song, L"bad\nlyric"), "rll delimiter injection");
    Write(file, "C:\\old.mp3|C:\\old.lrc\r\nC:\\bad.mp3|bad|C:\\bad.lrc\r\n");
    Check(store.Load(file) && store.All().size() == 1 && store.Find({L"C:\\OLD.mp3", 0}) == fs::path(L"C:\\old.lrc"), "legacy two-field row");
    Check(store.Erase({L"C:\\old.mp3", 0}) && store.Save() && !fs::exists(file), "empty table removes only its own rll");

    const auto media = directory / L"sound/Raw.flac";
    Write(media, "fixture, not playable");
    const auto metadata_match = directory / L"second/Artist - Song.lrc";
    Write(media.parent_path() / L"Raw.lrc", "[00:00]filename fallback");
    Write(metadata_match, "[00:00]metadata preferred");
    lyrics::LocalSearchRequest request{media, L"Artist", L"Song"};
    request.roots = lyrics::LocalSearchRoots(media, directory, directory / L"second", {L"<Sound Folder>", L"<Lyrics Download Folder>"});
    Check(request.roots.size() == 2 && !request.roots.front().recursive, "unstarred roots must be searched");
    std::atomic_bool canceled{};
    auto found = lyrics::SearchLocalLyrics(request, canceled);
    Check(found.loaded_path == metadata_match && found.lyric.lines.front().text == "metadata preferred", "score 9 supersedes earlier filename score 4");
    const auto associated = directory / L"specific.lrc";
    Write(associated, "[00:00]explicit association"); request.associated = associated;
    Check(lyrics::SearchLocalLyrics(request, canceled).loaded_path == associated, "association precedes local search");
    request.associated = directory / L"missing.lrc";
    Check(lyrics::SearchLocalLyrics(request, canceled).loaded_path == metadata_match, "missing association must fall through");
    Write(associated, "[ti:no timed lyrics]"); request.associated = associated;
    Check(lyrics::SearchLocalLyrics(request, canceled).loaded_path == metadata_match, "invalid associated text must fall through");
    request.associated = lyrics::kNoLyric;
    Check(lyrics::SearchLocalLyrics(request, canceled).loaded_path.empty(), "sentinel must not search local files");
    request.associated.clear(); request.roots = {{directory / L"recursive", false}};
    const auto nested = directory / L"recursive/a/b/c/Artist - Song.lrc";
    Write(nested, "[00:00]depth four");
    Write(directory / L"recursive/a/b/c/d/Artist - Song.lrc", "[00:00]too deep");
    Check(lyrics::SearchLocalLyrics(request, canceled).loaded_path.empty(), "unchecked root recursed");
    request.roots.front().recursive = true;
    Check(lyrics::SearchLocalLyrics(request, canceled).loaded_path == nested, "four-level recursive search");
    request.all_matches = true;
    Check(lyrics::SearchLocalLyrics(request, canceled).matches.size() == 1, "manual search recursion depth");
    request.title = L"Son";
    Check(lyrics::SearchLocalLyrics(request, canceled).matches.empty(), "normal search ignored word boundaries");
    request.partial = true;
    Check(lyrics::SearchLocalLyrics(request, canceled).matches.size() == 1, "partial association search");
    canceled = true;
    Check(lyrics::SearchLocalLyrics(request, canceled).matches.empty(), "canceled search published rows");
    // Non-BOM GBK must decode independently of an English/UTF-8 host ACP.
    if (GetACP() != 950) {
        Write(directory / L"gbk.lrc", "[00:00]\xB8\xE8\xB4\xCA");
        Check(lyrics::LoadLrc(directory / L"gbk.lrc").lines.front().text == "歌词", "GBK fallback");
    }
}
}

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void Ui(HMODULE resources, const fs::path& directory) {
        settings::Settings settings;
        settings.general.send_title_to_msn = false; settings.general.tray_icon = false;
        settings.lyric.auto_download = false; settings.lyric.auto_visible = false;
        settings.lyric.folders = {(directory / L"second").wstring()};
        ui::PlayerWindow player(settings);
        Check(player.lyric_associations_.Load(directory / L"ui.rll"), "isolated UI rll");
        player.SetSkinResourceModule(resources);
        player.window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Lyric association test owner", WS_OVERLAPPEDWINDOW,
            10, 10, 350, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        player.lyric_window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"Lyric test", WS_OVERLAPPEDWINDOW,
            20, 20, 350, 200, player.window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(player.window_ && player.lyric_window_, "fixture native windows");
        playlist::Track track;
        track.path = directory / L"sound/Raw.flac"; track.artist = "Artist"; track.title = "Song"; track.subtrack = 2;
        player.opened_track_ = track;
        const lyrics::SongKey key{track.path, 2};
        const auto selected_path = directory / L"second/Artist - Song.lrc";
        auto open = [&](std::function<void(HWND)> action) {
            Driver driver(std::move(action));
            Check(player.HandleLyricCommand(0x802d), "lyric associate command missing"); driver.Verify();
            Check(IsWindowEnabled(player.lyric_window_) != FALSE, "modal owner was not restored");
        };
        open([&](HWND dialog) {
            Check(GetWindow(dialog, GW_OWNER) == player.lyric_window_ && !IsWindowEnabled(player.lyric_window_), "native disabled lyric owner");
            Check((GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "owned association lost topmost");
            for (int id : {1009, 2119, 1046, 1064, 1023, 2116, 2117, 2118, 2115, 2087, 2120}) Check(GetDlgItem(dialog, id) != nullptr, "original 206 control missing");
            for (int id : {2115, 2120}) {
                BUTTON_IMAGELIST images{};
                Check(SendDlgItemMessageW(dialog, id, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&images)) && images.himl,
                    "original association button bitmap missing");
            }
            if (!IsWindowEnabled(GetDlgItem(dialog, 1046))) return;
            const HWND list = GetDlgItem(dialog, 1064);
            if (!ListView_GetItemCount(list)) return;
            wchar_t text[128]{}; GetWindowTextW(dialog, text, 128);
            Check(std::wstring(text) == L"指定歌词文件", "resource 206 title");
            LVCOLUMNW column{}; column.mask = LVCF_TEXT; column.pszText = text; column.cchTextMax = 128;
            ListView_GetColumn(list, 0, &column); Check(std::wstring(text) == L"文件名", "resource 8158 columns");
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            NMHDR notification{list, 1064, NM_DBLCLK}; SendMessageW(dialog, WM_NOTIFY, 1064, reinterpret_cast<LPARAM>(&notification));
        });
        Check(player.lyric_associations_.Find(key) == selected_path && player.lyric_path_ == selected_path, "double-click association did not apply");
        Check(player.lyric_associations_.Save(), "save UI association");
        lyrics::AssociationStore restart; Check(restart.Load(directory / L"ui.rll") && restart.Find(key) == selected_path, "UI association survives restart");
        // Same ListView rename notifications as 004480B3/0044829A, on a
        // disposable fixture only; the source music and user lyrics are untouched.
        const auto renamed = selected_path.parent_path() / L"renamed.lrc";
        open([&](HWND dialog) {
            if (!IsWindowEnabled(GetDlgItem(dialog, 1046))) return;
            const HWND list = GetDlgItem(dialog, 1064);
            if (!ListView_GetItemCount(list)) return;
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            SendMessageW(dialog, WM_COMMAND, 2117, 0);
            Check(ListView_GetEditControl(list) != nullptr, "rename button did not begin label edit");
            ListView_CancelEditLabel(list);
            std::wstring name = renamed.filename().wstring();
            NMLVDISPINFOW info{}; info.hdr = {list, 1064, LVN_ENDLABELEDITW}; info.item.iItem = 0; info.item.pszText = name.data();
            Check(SendMessageW(dialog, WM_NOTIFY, 1064, reinterpret_cast<LPARAM>(&info)) != 0, "label rename rejected");
            Check(fs::exists(renamed) && !fs::exists(selected_path), "rename did not move fixture");
            SendMessageW(dialog, WM_COMMAND, 2115, 0);
        });
        Check(player.lyric_associations_.Find(key) == renamed && player.lyric_path_ == renamed, "renamed association target");
        fs::rename(renamed, selected_path);
        player.lyric_associations_.Set(key, selected_path);
        // Native nested 205 dialog; edits are immediately reflected in the map.
        int stage = 0;
        open([&](HWND dialog) {
            if (GetDlgItem(dialog, 0x854)) {
                if (stage != 1) return;
                stage = 3; // Native focus/dialog messages can reenter the timer callback.
                Check(!IsWindowEnabled(GetWindow(dialog, GW_OWNER)), "all-associations native modality");
                Check(ListView_GetItemCount(GetDlgItem(dialog, 1064)) == 1, "all-associations rows");
                SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); stage = 2;
            } else if (stage == 0) {
                stage = 1;
                const HWND link = GetDlgItem(dialog, 2087);
                Check((GetWindowLongPtrW(link, GWL_STYLE) & SS_NOTIFY) != 0, "view-all static does not accept mouse clicks");
                SendMessageW(link, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(3, 3));
                SendMessageW(link, WM_LBUTTONUP, 0, MAKELPARAM(3, 3));
            }
            else if (stage == 2) SendMessageW(dialog, WM_CLOSE, 0, 0);
        });
        Check(stage == 2 && player.lyric_associations_.Find(key) == selected_path, "cancel or inspect modified mapping");
        // Multi-select delete affects mappings only, never either media/lyric file.
        stage = 0;
        open([&](HWND dialog) {
            if (GetDlgItem(dialog, 0x854)) {
                if (stage != 1) return;
                stage = 3;
                const HWND list = GetDlgItem(dialog, 1064);
                ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                SendMessageW(dialog, WM_COMMAND, 0x802e, 0);
                Check(IsWindow(list) && ListView_GetItemCount(list) == 0, "all-associations rows not refreshed after remove");
                Check(fs::exists(selected_path) && fs::exists(track.path), "all-associations remove deleted files");
                SendMessageW(dialog, WM_CLOSE, 0, 0); stage = 2;
            } else if (stage == 0) {
                stage = 1;
                const HWND link = GetDlgItem(dialog, 2087);
                SetFocus(link);
                PostMessageW(link, WM_KEYDOWN, VK_RETURN, 0);
                PostMessageW(link, WM_KEYUP, VK_RETURN, 0);
            }
            else if (stage == 2) SendMessageW(dialog, WM_CLOSE, 0, 0);
        });
        Check(!player.lyric_associations_.Find(key), "all-associations change not committed");
        player.CancelLocalLyricSearch(); player.lyric_associations_.Set(key, selected_path);
        open([&](HWND dialog) { SendMessageW(dialog, WM_COMMAND, 2116, 0); });
        Check(!player.lyric_associations_.Find(key) && player.lyrics_.lines.empty(), "dialog remove must clear without searching");
        open([&](HWND dialog) { SendMessageW(dialog, WM_COMMAND, 2120, 0); });
        Check(player.lyric_associations_.Find(key) == fs::path(lyrics::kNoLyric) && player.lyrics_.lines.empty(), "no association sentinel/UI clear");
        player.LoadCurrentLyrics(true);
        Check(!player.local_lyric_search_ && player.lyrics_.lines.empty(), "blocked song searched externally");
        // Original embedded-first priority still applies with a no-file sentinel.
        player.opened_track_->metadata = {{"Lyrics", "[00:00]embedded"}};
        player.LoadCurrentLyrics(true);
        Check(player.lyrics_embedded_ && player.lyrics_.lines.front().text == "embedded", "embedded must precede no-file sentinel");
        player.settings_.lyric.dont_load_lyric_tag = true; player.LoadCurrentLyrics(true);
        Check(player.lyrics_.lines.empty(), "DontLoadLyricTag gate");
        open([&](HWND dialog) {
            Check(!IsWindowEnabled(GetDlgItem(dialog, 1009)) && !IsWindowEnabled(GetDlgItem(dialog, 1023)), "blocked association controls enabled");
            wchar_t caption[100]{}; GetDlgItemTextW(dialog, 2120, caption, 100);
            Check(std::wstring(caption) == L"撤销不关联", "blocked action resource 803A");
            SendMessageW(dialog, WM_COMMAND, 2120, 0);
        });
        Check(!player.lyric_associations_.Find(key) && player.lyrics_.lines.empty(), "unblock should clear, not immediately re-search");
        player.LoadCurrentLyrics(true);
        Until([&] { player.PollLocalLyricSearch(); return !player.local_lyric_search_; });
        Check(player.lyric_path_ == selected_path && !player.lyric_associations_.Find(key), "automatic local match must not create association");
        const auto dropped = directory / L"drop.lrc"; Write(dropped, "[00:00]drop only");
        player.LoadDroppedLyrics(dropped);
        Check(player.lyric_path_ == dropped && !player.lyric_associations_.Find(key), "drop persisted an unintended association");
        player.lyric_associations_.Set(key, selected_path);
        Check(player.HandleLyricCommand(0x802e) && !player.lyric_associations_.Find(key) && player.lyrics_.lines.empty(), "context remove must clear without reloading same local file");
        // Keep the captured CUE identity while the player advances inside the modal loop.
        open([&](HWND dialog) {
            if (!IsWindowEnabled(GetDlgItem(dialog, 1046)) || !ListView_GetItemCount(GetDlgItem(dialog, 1064))) return;
            player.opened_track_->subtrack = 3;
            SendMessageW(dialog, WM_COMMAND, 2115, 0);
        });
        Check(player.lyric_associations_.Find(key) == selected_path && !player.lyric_associations_.Find({track.path, 3}) && player.lyrics_.lines.empty(), "modal result associated wrong subtrack");
        player.LoadCurrentLyrics(true); player.ClearLyrics();
        Sleep(50); player.PollLocalLyricSearch();
        Check(player.lyrics_.lines.empty(), "canceled local worker overwrote clear");
        DestroyWindow(player.lyric_window_); player.lyric_window_ = nullptr;
        DestroyWindow(player.window_); player.window_ = nullptr;
    }
};
}

int wmain(int count, wchar_t** args) {
    try {
        Check(count == 2, "repository path required");
        const auto directory = fs::temp_directory_path() / (L"TTPlayer-lyric-association-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory);
        Check(SUCCEEDED(OleInitialize(nullptr)), "OLE initialization");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
        Core(directory);
        const auto module = LoadLibraryExW((fs::path(args[1]) / L"ttpres.dll").c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Check(module != nullptr, "original 5.7.9 resource DLL");
        testing::SkinRebindAccess::Ui(module, directory);
        FreeLibrary(module); OleUninitialize();
        std::wcout << L"PASS: rll identity/persistence/sentinel, scored recursive local search, original dialogs 206/205,\n"
                      L"native owner/topmost, associate/cancel/remove/block/unblock, embedded priority, drop, stale CUE results.\nFixtures: " << directory << L'\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "lyric association: " << error.what() << '\n'; return 1; }
}
