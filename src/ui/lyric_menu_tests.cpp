#include "player_window_internal.h"
#include "lyric_menu_contract.h"
#include "lyric_upload_window.h"
#include <mshtml.h>
#include <oleacc.h>
#include <richedit.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace ttplayer;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void Write(const fs::path& path, std::string_view text) { std::ofstream file(path, std::ios::binary); file << text; Check(file.good(), "fixture write"); }
template<class F> void Until(F done, DWORD timeout = 15000) {
    const auto deadline = GetTickCount64() + timeout;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (!ui::TranslateLyricUploadMessage(msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (done()) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("lyric menu native UI timeout");
}
void Core() {
    using namespace std::chrono_literals;
    const auto original = lyrics::ParseLrc("[ti:音乐][ar:歌手]\n[00:01]音乐一\n[00:02]音乐二\n[00:03]音乐三\n");
    auto lrc = original;
    Check(!ui::AdjustLyricDocument(lrc, 0x8025, 0ms) && lrc.lines[0].time == 1s, "current before first row must be a no-op");
    Check(ui::AdjustLyricDocument(lrc, 0x8027, 1500ms) && lrc.lines[0].time == 1s &&
        lrc.lines[1].time == 1500ms && lrc.lines[2].time == 2500ms, "following must exclude the current row");
    lrc = original;
    Check(!ui::AdjustLyricDocument(lrc, 0x8028, 3500ms), "no row follows the last row");
    Check(ui::AdjustLyricDocument(lrc, 0x8028, 0ms) && lrc.lines[0].time == 1500ms, "following before first includes all rows");
    lrc = original;
    ui::AdjustLyricDocument(lrc, 0x8026, 1500ms);
    Check(lrc.lines[0].time == 1500ms && lrc.lines[1].time == 2s, "current-only adjustment");
    ui::AdjustLyricDocument(lrc, 0x8029, 0ms);
    Check(lrc.lines[0].time == 1s && lrc.lines[1].time == 1500ms, "all-row adjustment");
    Check(ui::ConvertLyricDocument(lrc, LCMAP_TRADITIONAL_CHINESE) && lrc.lines[0].text == "音樂一" && lrc.title == "音乐", "display conversion touches rows only");
    Check(ui::ConvertLyricDocument(lrc, LCMAP_SIMPLIFIED_CHINESE) && lrc.lines[0].text == "音乐一", "simplified conversion");
    lrc = lyrics::ParseLrc("[offset:250]\n[00:01]repeat\n[00:02]different\n[00:03]repeat");
    const auto text = ui::SerializeLyricDocument(lrc, true);
    Check(text.find(L"[00:03.25][00:01.25]repeat") != text.npos, "compact prepends later tags");
    auto roundtrip = lyrics::ParseLrc(core::WideToUtf8(text));
    Check(roundtrip.offset == 0ms && roundtrip.lines[0].time == 1250ms && roundtrip.lines[2].time == 3250ms, "serialized effective clock");
    Check(ui::SerializeLyricDocument({}, true).empty(), "empty lyrics must not create a metadata-only upload");
    Check(ui::LyricUploadUrl(L"https://lyrics.example:8443/api/search/?q=1#x") == L"https://lyrics.example:8443/dll/lrcup.php", "HTTPS origin/port/path replacement");
    Check(ui::LyricUploadUrl(L"http://[::1]:1234/search") == L"http://[::1]:1234/dll/lrcup.php", "IPv6 origin");
    Check(ui::LyricUploadUrl(L"https://lyrics.example") == L"https://lyrics.example/dll/lrcup.php", "bare origin");
    for (const auto* url : {L"", L"file:///C:/lyrics", L"ftp://host/search", L"https://user:password@host/search", L"javascript:alert(1)", L"https://host\r\nInjected:1/path"})
        Check(ui::LyricUploadUrl(url).empty(), "invalid upload origin accepted");
    const std::array<BYTE, 15> expected{0,134,136,130,128,178,186,238,161,129,177,204,222,162,163};
    Check(ui::kLyricCharsets == expected, "original 15-entry charset table");
}

ComPtr<IHTMLDocument2> BrowserDocument(HWND window) {
    HWND server{};
    EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
        wchar_t name[80]{}; GetClassNameW(child, name, 80);
        if (wcscmp(name, L"Internet Explorer_Server")) return TRUE;
        *reinterpret_cast<HWND*>(data) = child; return FALSE;
    }, reinterpret_cast<LPARAM>(&server));
    ComPtr<IHTMLDocument2> document;
    DWORD_PTR object{};
    if (server && SendMessageTimeoutW(server, RegisterWindowMessageW(L"WM_HTML_GETOBJECT"), 0, 0,
            SMTO_ABORTIFHUNG, 1000, &object) && object)
        ObjectFromLresult(static_cast<LRESULT>(object), IID_IHTMLDocument2, 0, reinterpret_cast<void**>(document.GetAddressOf()));
    return document;
}
std::wstring Value(IHTMLDocument2* document, const wchar_t* name, bool area = false) {
    ComPtr<IHTMLElementCollection> all; document->get_all(&all);
    VARIANT key{}, index{}; key.vt = VT_BSTR; key.bstrVal = SysAllocString(name); index.vt = VT_I4;
    ComPtr<IDispatch> item; if (all) all->item(key, index, &item); VariantClear(&key);
    if (!item) return {};
    BSTR value{};
    if (area) { ComPtr<IHTMLTextAreaElement> field; if (SUCCEEDED(item.As(&field))) field->get_value(&value); }
    else { ComPtr<IHTMLInputElement> field; if (SUCCEEDED(item.As(&field))) field->get_value(&value); }
    std::wstring result = value ? value : L""; SysFreeString(value); return result;
}
void Browser(HWND owner, const fs::path& directory) {
    const auto page = directory / L"upload-form.html";
    // No script, no network, no auto-submission. Only local DOM properties.
    Write(page, "<!doctype html><html><head><meta charset=utf-8></head><body><form>"
        "<input name=artist><input name=title><input name=album><textarea name=lyrics></textarea>"
        "</form></body></html>");
    wchar_t uri[2048]{}; DWORD size = static_cast<DWORD>(std::size(uri));
    Check(SUCCEEDED(UrlCreateFromPathW(page.c_str(), uri, &size, 0)), "local fixture URL");
    const ui::LyricUploadData data{L"歌手 & <test>", L"测试歌曲", L"测试专辑", L"[00:01.00]音乐 & <not html>\r\n[00:02.00]第二行"};
    const auto run = [&] {
        const HWND window = ui::ShowLyricUploadWindow(owner, L"上传歌词 - 本地测试", data, uri);
        Check(window && IsWindow(window), "upload window did not open");
        try {
            ComPtr<IHTMLDocument2> document;
            Until([&] { document = BrowserDocument(window); return document && Value(document.Get(), L"artist") == data.artist; });
            Check(Value(document.Get(), L"title") == data.title && Value(document.Get(), L"album") == data.album &&
                Value(document.Get(), L"lyrics", true) == data.lyrics, "upload four-field DOM values");
            RECT before{}; GetClientRect(window, &before);
            Check(before.right == 500 && before.bottom == 480, "original 500 x 480 client size");
            SetWindowPos(window, nullptr, 0, 0, 620, 580, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Check(GetDlgItem(window, 1) != nullptr && GetWindow(window, GW_OWNER) == owner, "upload status bar or owner missing");
            // Missing fields must not silently pass as a successful upload.
            ComPtr<IHTMLElement> body; document->get_body(&body);
            BSTR error = SysAllocString(L"<input name=artist value=untouched>error page"); body->put_innerHTML(error); SysFreeString(error);
            ComPtr<IDispatch> dispatch; document.As(&dispatch);
            Check(!ui::PopulateLyricUploadDocument(dispatch.Get(), data) && Value(document.Get(), L"artist") == L"untouched", "incompatible page partially populated");
            document.Reset(); dispatch.Reset(); body.Reset();
        } catch (...) { DestroyWindow(window); throw; }
        SendMessageW(window, WM_CLOSE, 0, 0);
        Check(!IsWindow(window) && IsWindowEnabled(owner), "upload close/lifetime");
    };
    run(); run(); // Browser teardown, reopen, event connection lifecycle.
}
}

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void Ui(HMODULE resources, const fs::path& root, const fs::path& directory, const fs::path& audio_fixture) {
        settings::Settings settings;
        settings.general.send_title_to_msn = settings.general.tray_icon = settings.general.fade_windows = false;
        settings.lyric.auto_download = settings.lyric.auto_visible = false;
        ui::PlayerWindow p(settings); p.instance_ = GetModuleHandleW(nullptr); p.SetSkinResourceModule(resources);
        Check(p.LoadSkinResource(resources), "5.7.9 lyric skin");
        p.window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Lyric commands test", WS_OVERLAPPEDWINDOW,
            10, 10, 350, 200, nullptr, nullptr, p.instance_, nullptr);
        p.lyric_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Lyric owner", WS_OVERLAPPEDWINDOW,
            20, 20, 500, 300, p.window_, nullptr, p.instance_, nullptr);
        p.lyric_control_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 480, 240, p.lyric_window_, nullptr, p.instance_, nullptr);
        Check(p.window_ && p.lyric_window_ && p.lyric_control_, "lyric fixture HWNDs");
        const auto menu_state = [&](UINT command) {
            HMENU root = LoadMenuW(resources, MAKEINTRESOURCEW(143)); Check(root != nullptr, "lyric menu resource 143");
            HMENU menu = GetSubMenu(root, 0); p.PrepareLyricMenu(menu);
            const auto parent = ui::detail::FindCommandMenu(menu, command);
            Check(parent != nullptr, "lyric command missing from resource");
            UINT result = GetMenuState(parent, command, MF_BYCOMMAND); DestroyMenu(root); return result;
        };
        for (const UINT cmd : {0x8035U,0x8030U,0x8031U,0x8032U,0x86dU,0x86eU,0x8025U})
            Check(menu_state(cmd) & MF_GRAYED, "empty-document command should be disabled");
        playlist::Track track; track.path = directory / L"fixture.flac"; track.title = "Song"; track.artist = "Artist";
        p.opened_track_ = track;
        const auto lyric = directory / L"fixture.lrc"; Write(lyric, "[00:00.00]音乐\n[00:02.00]第二行\n");
        p.LoadLyricsFrom(lyric, false);
        Check(!(menu_state(0x8035) & MF_GRAYED) && !(menu_state(0x8031) & MF_GRAYED), "populated-document commands disabled");
        Check(p.HandleLyricCommand(0x86d) && p.lyrics_.lines[0].text == "音樂" && p.lyric_document_modified_, "ordinary UI conversion route");
        const auto before = p.audio_.Position();
        p.HandleLyricCommand(0x8028);
        Check(p.lyrics_.lines[0].time.count() == 0 && p.lyrics_.lines[1].time.count() == 2500 && p.audio_.Position() == before, "UI following command seeks or includes current");
        for (UINT index = 0; index < ui::kLyricCharsets.size(); ++index) {
            Check(p.HandleLyricCommand(0x805c + index) && p.settings_.lyric.charset == ui::kLyricCharsets[index], "charset routing");
            Check(menu_state(0x805c + index) & MF_CHECKED, "charset check mark");
        }
        p.HandleLyricCommand(0x806f); Check(p.settings_.lyric.charset == 163, "out-of-table charset accepted");
        Check(p.lyrics_.lines[0].text == "音樂" && p.lyric_document_modified_, "font charset erased unsaved changes");
        Check(p.EnterLyricEditor(), "lyric editor entry");
        Check(p.LyricEditorText().find(L"音樂") != std::wstring::npos &&
            p.LyricEditorText().find(L"[00:02.50]") != std::wstring::npos &&
            SendMessageW(p.lyric_editor_, EM_GETMODIFY, 0, 0), "editor reloaded disk and lost adjusted/converted document");
        p.SetLyricEditorText(L"音乐 中国", false);
        SendMessageW(p.lyric_editor_, EM_SETSEL, 0, 0);
        p.HandleLyricCommand(0x86d);
        Check(p.LyricEditorText() == L"音乐 中国", "no-selection editor conversion changed the whole text");
        SendMessageW(p.lyric_editor_, EM_SETSEL, 0, 2);
        p.HandleLyricCommand(0x86d);
        Check(p.LyricEditorText() == L"音樂 中国", "editor conversion must only replace selection");
        SendMessageW(p.lyric_editor_, EM_UNDO, 0, 0);
        Check(p.LyricEditorText() == L"音乐 中国", "editor conversion undo");
        p.HandleLyricCommand(0x8029); p.HandleLyricCommand(0x805c);
        Check(p.lyrics_.lines[0].time.count() == 0 && p.settings_.lyric.charset == 163, "display-only commands changed model while editing");
        p.DestroyLyricEditor();
        p.lyric_services_ready_ = false;
        Check(p.HandleLyricCommand(0x8035) && p.lyric_upload_pending_ &&
            p.lyric_upload_pending_->title == L"Song", "upload command must capture before asynchronous catalog read");
        p.opened_track_->title = "Another song";
        Check(p.lyric_upload_pending_->title == L"Song", "pending upload changed with current track");
        p.lyric_upload_pending_.reset(); p.lyric_catalog_job_.reset();
        p.ClearLyrics(); Check(!p.lyric_document_modified_, "clear left document dirty");

        Browser(p.lyric_window_, directory);
        // Optional manual integration run uses a COPY of the given generated
        // FLAC, never the caller's original or any library file.
        if (!audio_fixture.empty()) {
            fs::copy_file(audio_fixture, track.path, fs::copy_options::overwrite_existing);
            plugins::PluginManager manager;
            Check(SUCCEEDED(manager.Load(root / L"AddIn")), "metadata fixture AddIns");
            p.SetSoundLibrary(&manager);
            const std::wstring embedded = L"[00:01.00]内嵌歌词\r\n[00:02.00]第二行\r\n";
            Check(p.WriteEmbeddedLyrics(embedded, false), "embedded FLAC write");
            Check(p.ReadEmbeddedLyrics() == embedded, "embedded read-back after Release");
            {
                auto reader = manager.OpenReader(track.path);
                Check(reader != nullptr, "metadata verification reader");
                const auto singular = reader->MetadataValue("Lyric");
                Check(!singular || singular->empty(), "legacy singular tag remains after write");
            }
            Check(p.opened_track_->metadata.size() == 1, "embedded playback cache update");
            Check(p.WriteEmbeddedLyrics({}, true) && !p.ReadEmbeddedLyrics(), "embedded removal");
            Check(p.opened_track_->metadata.empty(), "deleted embedded lyrics resurrect from cache");
            p.audio_.SetVolume(0.0f); // The generated test tone must not be audible.
            Check(p.audio_.Play(track.path), "play metadata-lock fixture");
            Until([&] { return p.audio_.State() == audio::PlaybackState::playing && p.audio_.Duration().count() > 0; });
            const auto clock = p.audio_.Position();
            // A playback reader may deny another write handle. This path is
            // deliberately silent like 0044C88F, but must NEVER issue Stop.
            const bool removed_live = p.WriteEmbeddedLyrics({}, true);
            Check(p.audio_.State() == audio::PlaybackState::playing && p.audio_.Position() >= clock, "embedded operation stopped/restarted playback");
            std::cout << "embedded FLAC: write/read/delete passed; live delete=" << removed_live << ", playback preserved\n";
            p.audio_.Stop();
            p.SetSoundLibrary(nullptr);
        }
        p.lyric_control_ = nullptr;
        DestroyWindow(p.lyric_window_); p.lyric_window_ = nullptr;
        DestroyWindow(p.window_); p.window_ = nullptr;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc >= 2, "repository path required");
        Check(SUCCEEDED(OleInitialize(nullptr)), "OLE initialization");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        const auto directory = fs::temp_directory_path() / (L"TTPlayer-lyric-menu-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory);
        const HMODULE resources = LoadLibraryExW((fs::path(argv[1]) / L"ttpres.dll").c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Check(resources != nullptr, "5.7.9 ttpres.dll required");
        Core(); testing::SkinRebindAccess::Ui(resources, argv[1], directory, argc > 2 ? fs::path(argv[2]) : fs::path{});
        FreeLibrary(resources); OleUninitialize();
        std::wcout << L"PASS: adjustment, conversion, charset, menu states, editor dirty state, local upload DOM, resize/close/reopen.\nFixtures: " << directory << L'\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "lyric menu: " << e.what() << '\n'; return 1; }
}
