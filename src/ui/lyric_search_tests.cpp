// Real shipped x86 DLL against a loopback-only server. No historical endpoint
// is contacted: provider names from our same-name .ini are asserted FIRST.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ttplayer/ui/player_window.h"
#include "ttplayer/core/text.h"
#include "ttplayer/lyrics/lyric_http.h"
#include "../app/resource_ids.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void Until(F&& ready) {
    const auto end = GetTickCount64() + 10000;
    while (!ready()) {
        Require(GetTickCount64() < end, "asynchronous lyric operation timed out");
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        Sleep(10);
    }
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file << bytes;
    Require(file.good(), "cannot write isolated test fixture");
}
class LoopbackServer {
public:
    std::atomic_bool slow{};
    std::atomic_bool single{};
    std::atomic_uint searches{}, downloads{}, refreshes{};
    std::string base, xml;
    std::mutex mutex;
    std::vector<std::string> requests;
    LoopbackServer() {
        WSADATA data{}; Require(WSAStartup(MAKEWORD(2,2), &data) == 0, "WSAStartup");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "loopback bind");
        int size = sizeof(address); getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        Require(listen(listener_, 4) == 0, "loopback listen");
        base = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/lyrics";
        xml = "<ttp_lrcsvr><server name=\"Fixture one\" url=\"" + base +
            "\"/><server name=\"Fixture two\" url=\"" + base +
            "\"/><extra title=\"Fixture help\" url=\"" + base + "\"/></ttp_lrcsvr>";
        thread_ = std::thread([this] { Run(); });
    }
    ~LoopbackServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        closesocket(listener_); WSACleanup();
    }
private:
    void Run() {
        while (!stop_) {
            fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable);
            timeval timeout{0,100000};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD timeout_ms = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout_ms), sizeof(timeout_ms));
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == request.npos && request.size() < 32000) {
                const int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, count);
            }
            { std::lock_guard lock(mutex); requests.push_back(request); }
            std::string body;
            if (request.find("?svrlst") != request.npos) { ++refreshes; body = xml; }
            else if (request.find("?sh?") != request.npos) {
                ++searches;
                if (slow) for (int i = 0; i < 150 && !stop_; ++i) Sleep(10);
                if (request.find("6500720072006F007200") != request.npos) body = "<result errmsg=\"Fixture error\" errcode=\"32010\"/>";
                else if (single) body = "<result><lrc id=\"101\" artist=\"陈慧娴\" title=\"千千阙歌\"/></result>";
                else body = "<result><lrc id=\"100\" artist=\"Someone\" title=\"Other\"/>"
                            "<lrc id=\"101\" artist=\"陈慧娴\" title=\"千千阙歌\"/></result>";
            } else if (request.find("?dl?") != request.npos) {
                ++downloads; body = "[ar:陈慧娴]\r\n[ti:千千阙歌]\r\n[00:00.00]fixture lyric\r\n[00:01.00]歌词测试\r\n";
            }
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            for (size_t sent = 0; sent < response.size();) {
                const int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
                if (count <= 0) break;
                sent += count;
            }
            closesocket(client);
        }
    }
    SOCKET listener_{INVALID_SOCKET};
    std::thread thread_;
    std::atomic_bool stop_{};
};
}

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void CheckServiceColumns(ui::PlayerWindow& player) {
        const HWND editor = player.lyric_service_editor_;
        const HWND list = GetDlgItem(editor, IDC_LYRIC_SERVICES_LIST);
        Require(Header_GetItemCount(ListView_GetHeader(list)) == 2, "server editor must have only name and URL columns");
        for (int i = 0; i < 2; ++i) {
            wchar_t text[128]{};
            LVCOLUMNW column{}; column.mask = LVCF_TEXT; column.pszText = text; column.cchTextMax = 128;
            Require(ListView_GetColumn(list, i, &column) && std::wstring(text) == (i == 0 ? L"名称" : L"地址"),
                "server editor column caption");
        }
        const HWND storage = GetDlgItem(editor, IDC_LYRIC_SERVICES_STORAGE);
        Require(storage && (GetWindowLongPtrW(storage, GWL_STYLE) & ES_READONLY), "storage field must remain read-only");
        const int previous = player.lyric_service_selection_;
        for (int index : {0, 2}) { // Real selection notifications: DLL row, then INI row.
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            wchar_t text[32768]{}; GetWindowTextW(storage, text, static_cast<int>(std::size(text)));
            Require(player.lyric_service_selection_ == index &&
                std::wstring(text) == player.lyric_service_draft_[index].storage.wstring(),
                "selected server storage not displayed below list");
        }
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (previous >= 0) ListView_SetItemState(list, previous, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    static void CheckDiscardPrompt(ui::PlayerWindow& player, int answer) {
        // Drive the REAL nested MessageBox on this UI thread, without sending
        // global mouse/keyboard input or touching another application's window.
        struct Probe {
            ui::PlayerWindow* player;
            HWND entry;
            int answer;
            bool seen{}, blocked{}, retained{};
        } probe{&player, GetWindow(player.lyric_service_editor_, GW_OWNER), answer};
        const HWND editor = player.lyric_service_editor_;
        constexpr auto key = L"TTPlayer.LyricServiceModalProbe";
        constexpr UINT_PTR timer = 0x4c534d;
        Require(SetPropW(editor, key, &probe), "cannot install modal test state");
        Require(SetTimer(editor, timer, 20, [](HWND owner, UINT, UINT_PTR id, DWORD) {
            auto* state = static_cast<Probe*>(GetPropW(owner, L"TTPlayer.LyricServiceModalProbe"));
            if (!state || IsWindowEnabled(owner)) return;
            const HWND prompt = GetWindow(owner, GW_ENABLEDPOPUP);
            if (prompt == owner || !IsWindowVisible(prompt) || !GetDlgItem(prompt, IDNO)) return;
            KillTimer(owner, id);
            auto& p = *state->player;
            state->seen = GetWindow(prompt, GW_OWNER) == owner;
            state->blocked = !IsWindowEnabled(state->entry) && !IsWindowEnabled(owner);
            if (state->entry == p.lyric_search_dialog_) p.ShowOnlineLyricSearch();
            else p.ShowOptions(0);
            SendMessageW(state->entry, WM_CLOSE, 0, 0);
            state->retained = GetActiveWindow() == prompt && p.lyric_service_editor_ == owner &&
                !IsWindowEnabled(owner) && !IsWindowEnabled(state->entry);
            PostMessageW(prompt, WM_COMMAND, state->answer, 0);
        }), "cannot install modal test timer");
        SendMessageW(editor, WM_CLOSE, 0, 0);
        KillTimer(editor, timer);
        if (IsWindow(editor)) RemovePropW(editor, key);
        Require(probe.seen && probe.blocked && probe.retained,
            "nested discard prompt lost native ownership, modality or activation");
        if (answer == IDNO) {
            Require(IsWindow(editor) && IsWindowEnabled(editor) && player.lyric_service_dirty_ &&
                !IsWindowEnabled(probe.entry), "cancel discard broke editor modality/draft");
        } else {
            Require(!IsWindow(editor) && IsWindowEnabled(probe.entry),
                "confirm discard failed to restore the entry window");
        }
    }
    static void CheckSearchEditor(ui::PlayerWindow& player, LoopbackServer& server, const fs::path& directory) {
        const HWND search = player.lyric_search_dialog_;
        player.ShowOptions(8); // Both entry windows exist: the caller, not existence, determines the owner.
        Until([&] { return !player.lyric_catalog_job_; });
        const HWND options = player.options_window_;
        const auto open = [&] {
            SendMessageW(search, WM_COMMAND, MAKEWPARAM(2185, BN_CLICKED), 0);
            const HWND editor = player.lyric_service_editor_;
            Require(editor && GetWindow(editor, GW_OWNER) == search &&
                GetWindow(search, GW_ENABLEDPOPUP) == editor,
                "search entry did not establish the native modal owner/popup relationship");
            Require(!IsWindowEnabled(search) && IsWindowEnabled(editor) && IsWindowEnabled(options),
                "search editor disabled the wrong entry window");
            return editor;
        };
        const HWND editor = open();
        CheckServiceColumns(player);
        const auto searches = server.searches.load(), downloads = server.downloads.load();
        for (const int command : {1046, IDOK, IDCANCEL}) SendMessageW(search, WM_COMMAND, command, 0);
        SendMessageW(search, WM_SYSCOMMAND, SC_CLOSE, 0);
        SendMessageW(search, WM_CLOSE, 0, 0);
        player.ShowOnlineLyricSearch();
        Require(player.lyric_search_dialog_ == search && player.lyric_service_editor_ == editor &&
            !IsWindowEnabled(search) && server.searches == searches && server.downloads == downloads,
            "queued search/download/close or repeated entry bypassed search modality");

        const auto saved = Read(directory / L"AddIn/ttp_lrcsh.ini");
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_ADD, 0);
        CheckDiscardPrompt(player, IDNO);
        CheckDiscardPrompt(player, IDYES);
        Require(Read(directory / L"AddIn/ttp_lrcsh.ini") == saved, "search editor discard wrote the INI");

        DestroyWindow(open());
        Require(IsWindowEnabled(search) && !player.lyric_service_disabled_owner_,
            "forced search editor destruction did not restore its owner");
        EnableWindow(search, FALSE);
        player.ShowLyricServiceEditor(search);
        SendMessageW(player.lyric_service_editor_, WM_CLOSE, 0, 0);
        Require(!IsWindowEnabled(search), "search editor re-enabled an owner disabled by another dialog");
        EnableWindow(search, TRUE);
        player.CloseOptions();
    }
    static void Ui(HMODULE resources, plugins::PluginManager& library, const fs::path& directory,
                   LoopbackServer& server) {
        settings::Settings settings;
        settings.source_path = directory / L"test-only.xml";
        settings.general.tray_icon = false; settings.general.send_title_to_msn = false;
        settings.general.fade_windows = false; settings.lyric.auto_download = false;
        settings.hotkey.global = false; settings.network.proxy_type = 0;
        settings.lyric.download_folder = directory / L"downloads";
        // Emulate a selection saved by the previous INI-first version.
        settings.lyric.server_key = lyrics::ServiceKey({L"Fixture one", core::Utf8ToWide(server.base), {},
            directory / L"AddIn/ttp_lrcsh.dll", directory / L"AddIn/ttp_lrcsh.ini", false, 0});
        ui::PlayerWindow player(settings);
        Require(player.lyric_associations_.Load(directory / L"test-only.rll"), "isolated association store");
        player.instance_ = GetModuleHandleW(nullptr);
        player.SetSkinResourceModule(resources);
        player.SetSoundLibrary(&library);
        player.ShowOptions(8);
        Require(player.lyric_catalog_job_ != nullptr, "options did not request asynchronous INI refresh");
        Until([&] { return player.lyric_services_ready_; }); // options timer, not a blocking load
        const HWND combo = GetDlgItem(player.options_pages_[8], 2090);
        Require(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 4, "INI and immutable DLL services not merged");
        Require(player.lyric_services_.entries[0].read_only && player.lyric_services_.entries[1].read_only &&
            !player.lyric_services_.entries[2].read_only && !player.lyric_services_.entries[3].read_only,
            "options services must show all DLL entries before INI entries");
        Require(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 2 &&
            player.settings_.lyric.server_key == settings.lyric.server_key, "DLL-first sorting changed selected server");
        wchar_t name[256]{}; SendMessageW(combo, CB_GETLBTEXT, 2, reinterpret_cast<LPARAM>(name));
        Require(std::wstring(name) == L"Fixture one", "options retained hard-coded servers");
        wchar_t klass[32]{};
        GetClassNameW(GetDlgItem(player.options_pages_[8], 2185), klass, 32);
        Require(std::wstring(klass) == L"Button", "edit-list control is not a button");
        GetDlgItemTextW(player.options_pages_[8], 2185, name, 256);
        Require(std::wstring(name) == L"编辑列表", "proxy link caption survived");
        SendMessageW(player.options_pages_[8], WM_COMMAND, MAKEWPARAM(2185, BN_CLICKED), 0);
        const HWND editor = player.lyric_service_editor_;
        Require(editor && IsWindow(editor), "edit-list button did not open editor");
        const HWND blocked_options = player.options_window_;
        Require(!IsWindowEnabled(blocked_options) && IsWindowEnabled(editor), "options not modal to editor");
        Require(GetWindow(editor, GW_OWNER) == blocked_options &&
            GetWindow(blocked_options, GW_ENABLEDPOPUP) == editor,
            "User32 cannot resolve the editor from disabled options");
        player.ShowOptions(0);
        SendMessageW(blocked_options, WM_COMMAND, IDOK, 0);
        Require(player.options_window_ == blocked_options && player.options_page_index_ == 8 &&
            IsWindow(editor) && !IsWindowEnabled(blocked_options), "options entry/queued close bypassed editor");
        RECT editor_bounds{}, options_bounds{};
        GetWindowRect(editor, &editor_bounds); GetWindowRect(player.options_window_, &options_bounds);
        std::cout << "service editor=" << editor_bounds.right-editor_bounds.left << 'x'
            << editor_bounds.bottom-editor_bounds.top << " options=" << options_bounds.right-options_bounds.left
            << 'x' << options_bounds.bottom-options_bounds.top << '\n';
        Require(editor_bounds.right-editor_bounds.left < options_bounds.right-options_bounds.left &&
            editor_bounds.bottom-editor_bounds.top < options_bounds.bottom-options_bounds.top,
            "service editor must be smaller than options");
        const HWND services = GetDlgItem(editor, IDC_LYRIC_SERVICES_LIST);
        CheckServiceColumns(player);
        RECT list_rect{}, prior_button{}; GetWindowRect(services, &list_rect);
        for (const auto [button, local] : {std::pair{IDC_LYRIC_SERVICES_ADD, 1027},
            {IDC_LYRIC_SERVICES_DELETE, 1039}, {IDC_LYRIC_SERVICES_UP, 1042}, {IDC_LYRIC_SERVICES_DOWN, 1045}}) {
            RECT rect{}; GetWindowRect(GetDlgItem(editor, button), &rect);
            Require(rect.left > list_rect.right && rect.top >= list_rect.top && rect.bottom <= list_rect.bottom &&
                (IsRectEmpty(&prior_button) || rect.top >= prior_button.bottom), "buttons must form a right-hand column");
            prior_button = rect;
            BUTTON_IMAGELIST image{}, local_image{};
            Require(SendDlgItemMessageW(editor, button, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&image)) &&
                SendDlgItemMessageW(player.options_pages_[8], local, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&local_image)),
                "server/local search button image missing");
            Require(image.himl && ImageList_GetImageCount(image.himl) == 1 &&
                image.uAlign == local_image.uAlign && EqualRect(&image.margin, &local_image.margin),
                "server button image style differs from local search");
            int image_width{}, image_height{};
            Require(image.uAlign == BUTTON_IMAGELIST_ALIGN_CENTER && IsRectEmpty(&image.margin) &&
                ImageList_GetIconSize(image.himl, &image_width, &image_height) && image_width == 16 && image_height == 15,
                "server icon must be centered without stretching the original 16x15 bitmap");
        }
        Require(ListView_GetItemCount(services) == 4, "editor list incomplete");
        ListView_SetItemState(services, 0, LVIS_SELECTED, LVIS_SELECTED);
        GetDlgItemTextW(editor, IDC_LYRIC_SERVICES_STORAGE, name, 256);
        Require(std::filesystem::path(name).extension() == L".dll", "editor first row not from DLL");
        ListView_SetItemState(services, 2, LVIS_SELECTED, LVIS_SELECTED);
        GetDlgItemTextW(editor, IDC_LYRIC_SERVICES_STORAGE, name, 256);
        Require(std::filesystem::path(name).extension() == L".ini", "editor INI rows not below DLL rows");
        ListView_SetItemState(services, 0, LVIS_SELECTED, LVIS_SELECTED);
        Require(GetWindowLongW(GetDlgItem(editor, IDC_LYRIC_SERVICES_NAME), GWL_STYLE) & ES_READONLY,
            "DLL name editable");
        Require(GetWindowLongW(GetDlgItem(editor, IDC_LYRIC_SERVICES_URL), GWL_STYLE) & ES_READONLY,
            "DLL URL editable");
        Require(!IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_DELETE)), "DLL delete enabled");
        Require(!IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_UP)) &&
            !IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_DOWN)), "DLL ordering buttons enabled");
        const auto original_draft = player.lyric_service_draft_;
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_UP, 0);
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_DOWN, 0);
        Require(player.lyric_service_draft_ == original_draft && !player.lyric_service_dirty_, "DLL move command changed draft");
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_DELETE, 0);
        Require(player.lyric_service_draft_.size() == 4, "DLL removed through command route");
        ListView_SetItemState(services, 2, LVIS_SELECTED, LVIS_SELECTED);
        Require(!IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_UP)) &&
            IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_DOWN)), "first INI boundary buttons incorrect");
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_UP, 0);
        Require(player.lyric_service_draft_ == original_draft, "INI moved across DLL boundary");
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_DOWN, 0);
        Require(player.lyric_service_selection_ == 3 && player.lyric_service_draft_[3].name == L"Fixture one" &&
            !IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_DOWN)), "INI move/last-row selection incorrect");
        const auto moved_draft = player.lyric_service_draft_;
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_DOWN, 0);
        Require(player.lyric_service_draft_ == moved_draft, "last INI moved out of bounds");
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        Require(!IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_UP)), "reorder allowed during save");
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty() && player.settings_.lyric.add_in_index == 3 &&
            lyrics::ReadServiceCatalog(library.LyricSearchProviders()).entries[3].name == L"Fixture one",
            "INI order or selected service not preserved on disk");
        // Return to the original INI order for the remaining import tests.
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_UP, 0);
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty() && player.settings_.lyric.add_in_index == 2, "INI move-up save failed");
        ListView_SetItemState(services, -1, 0, LVIS_SELECTED);
        Require(player.lyric_service_selection_ == -1 && !IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_UP)) &&
            !IsWindowEnabled(GetDlgItem(editor, IDC_LYRIC_SERVICES_DOWN)), "cleared selection left stale reorder target");
        ListView_SetItemState(services, 0, LVIS_SELECTED, LVIS_SELECTED);
        SendMessageW(editor, WM_COMMAND, IDC_LYRIC_SERVICES_ADD, 0);
        Require(player.lyric_service_selection_ == 4, "new row not selected");
        Require(GetWindowLongW(GetDlgItem(editor, IDC_LYRIC_SERVICES_STORAGE), GWL_STYLE) & ES_READONLY,
            "storage location editable");
        Require(player.lyric_service_draft_.back().storage == directory / L"AddIn/ttp_lrcsh.ini", "new row storage mismatch");
        SetDlgItemTextW(editor, IDC_LYRIC_SERVICES_NAME, L"Third edited service");
        SetDlgItemTextW(editor, IDC_LYRIC_SERVICES_URL, core::Utf8ToWide(server.base).c_str());
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        Require(player.lyric_catalog_save_job_ && !IsWindowEnabled(GetDlgItem(editor, IDOK)), "save not asynchronous");
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty() && !player.lyric_service_dirty_, "editor save failed");
        Require(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 5, "save did not refresh options combo");
        player.SelectLyricService(4);
        // Repeat atomic replacement and backup creation, including an HTTPS URL.
        SetDlgItemTextW(editor, IDC_LYRIC_SERVICES_URL, L"https://example.test/lyrics");
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty(), "second save/HTTPS validation failed");
        Require(player.settings_.lyric.add_in_index == 4 &&
            player.settings_.lyric.server_key == player.lyric_services_.entries[4].key,
            "editing selected URL changed selected server");
        SetDlgItemTextW(editor, IDC_LYRIC_SERVICES_URL, core::Utf8ToWide(server.base).c_str());
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty(), "third save/backup replacement failed");
        SetDlgItemTextW(editor, IDC_LYRIC_SERVICES_NAME, L"Third renamed service");
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty() && player.settings_.lyric.add_in_index == 4 &&
            player.settings_.lyric.server_key == player.lyric_services_.entries[4].key,
            "renaming selected service lost selection");
        player.SelectLyricService(4);
        const auto selected_key = player.settings_.lyric.server_key;
        Require(!IsWindowEnabled(blocked_options), "saving re-enabled options before editor closed");
        bool owner_restored_before_destroy = false;
        Require(SetWindowSubclass(editor, [](HWND window, UINT message, WPARAM wp, LPARAM lp,
                                             UINT_PTR, DWORD_PTR data) -> LRESULT {
            if (message == WM_DESTROY)
                *reinterpret_cast<bool*>(data) = IsWindowEnabled(GetWindow(window, GW_OWNER)) != FALSE;
            return DefSubclassProc(window, message, wp, lp);
        }, 0x4c5352, reinterpret_cast<DWORD_PTR>(&owner_restored_before_destroy)),
            "cannot observe editor destruction order");
        SendMessageW(editor, WM_CLOSE, 0, 0);
        Require(!IsWindow(editor) && IsWindowEnabled(blocked_options), "editor close did not restore options");
        Require(owner_restored_before_destroy, "editor destroyed before restoring its modal owner");
        const auto saved_ini = Read(directory / L"AddIn/ttp_lrcsh.ini");
        player.ShowLyricServiceEditor();
        SendMessageW(player.lyric_service_editor_, WM_COMMAND, IDC_LYRIC_SERVICES_ADD, 0);
        CheckDiscardPrompt(player, IDNO);
        CheckDiscardPrompt(player, IDYES);
        Require(Read(directory / L"AddIn/ttp_lrcsh.ini") == saved_ini,
            "discarding a draft changed the saved INI");
        player.CloseOptions();
        // Reopening preserves both the DLL prefix and the appended INI row.
        player.ShowOptions(8);
        Until([&] { return !player.lyric_catalog_job_; });
        Require(player.settings_.lyric.server_key == selected_key && player.settings_.lyric.add_in_index == 4,
            "INI ordering changed selected service identity");
        auto externally_edited = Read(directory / L"AddIn/ttp_lrcsh.ini");
        const auto at = externally_edited.find("Fixture one");
        externally_edited.replace(at, 11, "Changed externally");
        Write(directory / L"AddIn/ttp_lrcsh.ini", externally_edited);
        player.SelectOptionsPage(0); player.SelectOptionsPage(8);
        Require(!player.lyric_catalog_job_ && player.lyric_services_.entries[2].name == L"Fixture one",
            "page activation unexpectedly reread INI");
        player.CloseOptions(); player.ShowOptions(8);
        Until([&] { return !player.lyric_catalog_job_; });
        Require(player.lyric_services_.entries[2].name == L"Changed externally", "options reopening did not reread INI");
        player.CloseOptions();
        player.ShowOnlineLyricSearch();
        HWND dialog = player.lyric_search_dialog_;
        Require(dialog && GetDlgItem(dialog, 1046), "original dialog 209 missing");
        CheckSearchEditor(player, server, directory);
        SendDlgItemMessageW(dialog, 2090, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(name));
        Require(std::wstring(name) == player.lyric_services_.entries[0].name &&
            player.lyric_services_.entries[0].read_only, "manual search combo lost DLL-first ordering");
        SetWindowPos(dialog, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetDlgItemTextW(dialog, 1021, L"陈慧娴"); SetDlgItemTextW(dialog, 1009, L"千千阙歌");
        SendMessageW(dialog, WM_COMMAND, 1046, 0);
        Until([&] { player.PollOnlineLyricSearch(); return player.lyric_search_results_shown_; });
        HWND list = GetDlgItem(dialog, 1064);
        Require(ListView_GetItemCount(list) == 2, "results not populated");
        Require(ListView_GetNextItem(list, -1, LVNI_SELECTED) == 1, "best match selection");
        // A track change/network timer must not destroy an active service draft.
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(2185, BN_CLICKED), 0);
        const HWND live_editor = player.lyric_service_editor_;
        SendMessageW(live_editor, WM_COMMAND, IDC_LYRIC_SERVICES_ADD, 0);
        const auto search_track = player.lyric_search_track_;
        player.lyric_search_track_.path = directory / L"previous-track.flac";
        player.LoadCurrentLyrics(false);
        player.PollOnlineLyricSearch();
        Require(player.lyric_search_ && player.lyric_search_dialog_ == dialog &&
            player.lyric_service_editor_ == live_editor && player.lyric_service_dirty_,
            "track change or background result destroyed the modal service draft");
        player.lyric_search_track_ = search_track;
        CheckDiscardPrompt(player, IDYES);
        wchar_t heading[256]{}; LVCOLUMNW column{}; column.mask = LVCF_TEXT;
        column.pszText = heading; column.cchTextMax = 256;
        Require(ListView_GetColumn(list, 0, &column) && *heading, "resource column caption missing");
        SetDlgItemTextW(dialog, 2001, L"fixture-ui.lrc");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        const auto saved = directory / L"downloads/fixture-ui.lrc";
        Until([&] { player.PollOnlineLyricSearch(); return fs::exists(saved); });
        Require(!player.lyrics_.lines.empty() && player.lyric_path_ == saved, "download not applied to lyrics");
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SetDlgItemTextW(dialog, 2001, L"fixture-second.lrc");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        Until([&] { player.PollOnlineLyricSearch(); return fs::exists(directory / L"downloads/fixture-second.lrc"); });
        Require(IsWindowEnabled(GetDlgItem(dialog, IDOK)), "cannot download another candidate after success");
        SendMessageW(dialog, WM_CLOSE, 0, 0);
        Require(!IsWindow(dialog) && !player.lyric_search_, "close leaked UI/session");
        // Remove the custom third row through the editor and retain DLL rows.
        player.ShowLyricServiceEditor();
        player.SelectLyricServiceEditorRow(4);
        SendMessageW(player.lyric_service_editor_, WM_COMMAND, IDC_LYRIC_SERVICES_DELETE, 0);
        SendMessageW(player.lyric_service_editor_, WM_COMMAND, IDOK, 0);
        Until([&] { return !player.lyric_catalog_save_job_; });
        Require(player.lyric_services_.error.empty() && player.lyric_services_.entries.size() == 4, "custom delete failed");
        SendMessageW(player.lyric_service_editor_, WM_CLOSE, 0, 0);
        player.SelectLyricService(2); // loopback INI only; never contact a historical DLL endpoint

        // Auto-search policy, silent best match, and stale-track cancellation.
        player.settings_.lyric.auto_download = true;
        player.settings_.lyric.auto_select_download = true;
        player.settings_.lyric.auto_associate = true;
        playlist::Track track; track.path = directory / L"media.flac";
        track.title = "千千阙歌"; track.artist = "陈慧娴";
        player.opened_track_ = track;
        player.ClearLyrics();
        player.StartOnlineLyricSearch(true);
        Until([&] { player.PollOnlineLyricSearch(); return !player.lyric_path_.empty(); });
        Require(!player.lyric_search_dialog_, "auto-select unexpectedly displayed modal results");
        Require(player.lyric_associations_.Find({track.path, track.subtrack}) == player.lyric_path_,
            "automatic download did not persist captured song association");
        Require(player.lyric_associations_.Save(), "cannot save downloaded association");
        lyrics::AssociationStore restarted;
        Require(restarted.Load(directory / L"test-only.rll") &&
            restarted.Find({track.path, track.subtrack}) == player.lyric_path_, "download association lost after restart");
        const auto searches = server.searches.load();
        player.StartOnlineLyricSearch(true);
        Require(server.searches == searches && !player.lyric_search_, "automatic retry storm");

        player.settings_.lyric.auto_select_download = false;
        player.ClearLyrics(); player.lyric_auto_search_key_.clear();
        player.StartOnlineLyricSearch(true);
        Until([&] { player.PollOnlineLyricSearch(); return player.lyric_search_dialog_ != nullptr; });
        dialog = player.lyric_search_dialog_;
        Require(GetDlgItem(dialog, 2067) && !GetDlgItem(dialog, 1046), "automatic dialog 208 missing");
        wchar_t countdown[256]{}; GetDlgItemTextW(dialog, 1052, countdown, 256);
        Require(std::wstring(countdown).find(L"15") != std::wstring::npos, "countdown did not reuse original resource text");
        SendMessageW(dialog, WM_LBUTTONDOWN, 0, 0);
        Require(player.lyric_download_deadline_ == 0, "user input failed to cancel countdown");
        player.opened_track_->path = directory / L"next.flac";
        player.PollOnlineLyricSearch();
        Require(!player.lyric_search_ && !IsWindow(dialog), "old-track response/dialog survived song change");

        // Single result follows 0044BEED's automatic branch even with the
        // AutoSelectDownload checkbox off. Existing downloads are not replaced.
        server.single = true;
        player.ClearLyrics(); player.lyric_auto_search_key_.clear();
        player.StartOnlineLyricSearch(true);
        Until([&] { player.PollOnlineLyricSearch(); return !player.lyric_path_.empty(); });
        Require(!player.lyric_search_dialog_ && !player.lyric_search_, "single-result choice was not automatic");
        server.single = false;
        player.ClearLyrics(); player.lyric_auto_search_key_.clear();
        player.settings_.lyric.download_when_full_info = true;
        player.opened_track_->artist.clear();
        player.StartOnlineLyricSearch(true);
        Require(!player.lyric_search_, "full-info gate ignored missing artist");

        // No DLLs -> no pretend server names and no enabled search button.
        player.SetSoundLibrary(nullptr); player.ShowOptions(8);
        Until([&] { return !player.lyric_catalog_job_; });
        Require(SendDlgItemMessageW(player.options_pages_[8], 2090, CB_GETCOUNT, 0, 0) == 0, "empty registry fabricated servers");
        player.ShowLyricServiceEditor();
        Require(!IsWindowEnabled(player.options_window_), "empty editor failed to disable options");
        DestroyWindow(player.lyric_service_editor_);
        Require(IsWindowEnabled(player.options_window_) && !player.lyric_service_disabled_owner_,
            "forced editor destruction did not restore options");
        EnableWindow(player.options_window_, FALSE);
        player.ShowLyricServiceEditor();
        SendMessageW(player.lyric_service_editor_, WM_COMMAND, IDCANCEL, 0);
        Require(!IsWindowEnabled(player.options_window_), "editor re-enabled options disabled by another owner");
        EnableWindow(player.options_window_, TRUE);
        player.CloseOptions(); player.ShowOnlineLyricSearch();
        Require(!IsWindowEnabled(GetDlgItem(player.lyric_search_dialog_, 1046)), "search enabled without plugins");
        SendMessageW(player.lyric_search_dialog_, WM_COMMAND, MAKEWPARAM(2185, BN_CLICKED), 0);
        const HWND closing_editor = player.lyric_service_editor_;
        Require(GetWindow(closing_editor, GW_OWNER) == player.lyric_search_dialog_,
            "search entry without options fell back to the main player");
        player.CloseOnlineLyricSearch();
        Require(!IsWindow(closing_editor) && !player.lyric_service_editor_ && !player.lyric_service_disabled_owner_,
            "forced search teardown leaked its modal editor or disabled-owner state");
        Require(!fs::exists(settings.source_path), "test saved user settings");
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "expected repository root");
        Require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        LoopbackServer server;
        const auto directory = fs::temp_directory_path() /
            (L"TTPlayer-lyric-search-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory / L"AddIn");
        fs::copy_file(fs::path(argv[1]) / L"AddIn/ttp_lrcsh.dll", directory / L"AddIn/ttp_lrcsh.dll");
        Write(directory / L"AddIn/ttp_lrcsh.ini", server.xml);
        plugins::PluginManager library;
        Require(SUCCEEDED(library.Load(directory / L"AddIn")), "load fixture DLL");
        const auto& providers = library.LyricSearchProviders();
        Require(providers.size() == 2 && providers[0].name == L"Fixture one" &&
            providers[1].name == L"Fixture two", "STOP: DLL did not accept loopback-only external .ini");
        settings::NetworkSettings network; network.proxy_type = 0;
        {
            lyrics::OnlineSearch search(library, 1, network, L"陈慧娴", L"千千阙歌");
            Until([&] { const auto s = search.Snapshot();
                if (s.phase == lyrics::SearchPhase::failed) std::wcerr << L"DLL error: " << s.error << L'\n';
                Require(s.phase != lyrics::SearchPhase::failed, "search callback failed");
                return s.phase == lyrics::SearchPhase::results; });
            auto s = search.Snapshot();
            Require(s.provider == 1 && s.results.size() == 2 && s.results[1].artist == L"陈慧娴", "callback ABI data corruption");
            Require(!search.Download(2) && !search.Download(-1), "invalid result index accepted");
            Require(search.Download(1), "cannot submit download");
            Until([&] { return search.Snapshot().phase == lyrics::SearchPhase::downloaded; });
            s = search.Snapshot();
            Require(s.text.find(L"歌词测试") != s.text.npos, "UTF-8 download conversion");
            const auto path = directory / L"downloaded.lrc";
            Require(lyrics::SaveDownloadedLyric(path, s.text, false), "download save failed");
            const auto before = Read(path);
            Require(!lyrics::SaveDownloadedLyric(path, L"overwrite", false) && Read(path) == before, "overwrite protection");
            Require(lyrics::LoadLrc(path).lines.size() == 2, "saved lyric cannot be loaded");
        }
        Require(server.refreshes == 1 && server.searches == 1 && server.downloads == 1, "native server refresh/search/download chain");
        const auto cache = Read(directory / L"AddIn/ttp_lrcsh.ini");
        Require(cache.find("Fixture one") != cache.npos && cache.find("<extra") == cache.npos, "DLL did not persist sanitized server cache");
        {
            std::lock_guard lock(server.mutex);
            Require(server.requests[1].find("Artist=") != std::string::npos &&
                server.requests[1].find("Title=") != std::string::npos &&
                server.requests[2].find("Id=101&Code=") != std::string::npos, "native protocol/index mapping");
            const auto search_url = core::WideToUtf8(lyrics::LyricSearchUrl(L"/lyrics", L"陈慧娴", L"千千阙歌"));
            const auto download_url = core::WideToUtf8(lyrics::LyricDownloadUrl(L"/lyrics", {101,L"陈慧娴",L"千千阙歌"}));
            Require(server.requests[1].find(search_url) != std::string::npos, "host search encoding differs from actual DLL");
            Require(server.requests[2].find(download_url) != std::string::npos, "host download Code differs from actual DLL");
        }
        const auto resources = LoadLibraryExW((fs::path(argv[1]) / L"ttpres.dll").c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(resources != nullptr, "5.7.9 resources");
        testing::SkinRebindAccess::Ui(resources, library, directory, server);
        FreeLibrary(resources);
        // Exercise the native normalizer against punctuation, number prefixes,
        // bracketed version text and traditional Chinese, not just plain ASCII.
        for (const auto& [artist, title] : std::vector<std::pair<std::wstring,std::wstring>>{
            {L"01. ARTIST (Live)", L"02. Song [Demo]"}, {L"陳慧嫻", L"千千闕歌"},
            {L"A.B & C", L"1234 Track《现场》"}}) {
            lyrics::OnlineSearch native(library, 0, network, artist, title);
            Until([&] { return native.Snapshot().phase == lyrics::SearchPhase::results; });
            const auto expected = core::WideToUtf8(lyrics::LyricSearchUrl(L"/lyrics", artist, title));
            std::lock_guard lock(server.mutex);
            Require(server.requests.back().find(expected) != std::string::npos, "normalized host request differs from DLL");
        }
        lyrics::LyricService http{L"Host fixture", core::Utf8ToWide(server.base)};
        {
            lyrics::OnlineSearch search(http, 0, network, L"", L"error");
            Until([&] { return search.Snapshot().phase == lyrics::SearchPhase::failed; });
            Require(search.Snapshot().error == L"Fixture error", "host protocol error not surfaced");
        }
        {
            lyrics::OnlineSearch search(library, 99, network, L"", L"error");
            Until([&] { return search.Snapshot().phase == lyrics::SearchPhase::failed; });
            Require(search.Snapshot().provider == 0, "invalid provider did not fall back to zero");
        }
        server.slow = true;
        {
            const auto requests = server.searches.load();
            auto slow_http = std::make_unique<lyrics::OnlineSearch>(http, 0, network, L"", L"slow");
            Until([&] { return server.searches > requests; });
            const auto canceled_at = GetTickCount64(); slow_http.reset();
            Require(GetTickCount64() - canceled_at < 100, "host cancel waited for HTTP response");
        }
        const auto before = server.searches.load();
        auto slow = std::make_unique<lyrics::OnlineSearch>(library, 0, network, L"", L"slow");
        Until([&] { return server.searches > before; });
        const auto start = GetTickCount64();
        slow.reset();
        Require(GetTickCount64() - start < 100, "cancel waited for network/DLL release");
        library.Shutdown(); // worker must retain its own DLL and creator references
        // Let the private DLL's destructor finish off-thread before test teardown.
        Sleep(1800);
        Require(server.refreshes == 1, "server list refreshed repeatedly in same module lifetime");
        Require(lyrics::LyricFileName(L"../CON:x") == L".._CON_x.lrc", "unsafe filename handling");
        Require(lyrics::LyricFileName(L"CON") == L"_CON.lrc", "DOS device handling");
        Require(lyrics::BestSearchResult({{L"陳慧嫻",L"千千闕歌"}, {L"",L"other"}},
            L"陈慧娴",L"千千阙歌") == 0, "simplified/traditional matching");
        Require(lyrics::BestSearchResult({{L"A",L"小歌谣"},{L"A",L"歌 (现场)"}},
            L"A",L"歌") == 1, "matching ignored substring word boundaries");
        Require(lyrics::BestSearchResult({{L"A",L"Other"},{L"A",L"01. Song【Live】"}},
            L"A",L"Song") == 1, "bracket/track-number normalization");
        OleUninitialize();
        std::wcout << L"PASS: actual DLL .ini enumeration/cache, search/download ABI, UI 208/209, settings registry,\n"
                      L"auto selection, stale-track cancel, error/fallback, nonblocking close, safe save.\nFixtures: " << directory << L'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lyric search test: " << error.what() << '\n'; return 1;
    }
}
