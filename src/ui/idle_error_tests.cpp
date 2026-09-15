#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "player_window_internal.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
std::wstring Text(HWND window) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}
void Pump(DWORD duration) {
    const auto end = GetTickCount64() + duration;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    } while (GetTickCount64() < end);
}
struct Canvas {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap{}; HGDIOBJ old{}; uint32_t* pixels{};
    Canvas() {
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 220; info.bmiHeader.biHeight = -110;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0);
        Require(dc && bitmap && pixels, "DIB allocation failed");
        old = SelectObject(dc, bitmap);
    }
    ~Canvas() { SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
};
void Wave(const fs::path& path) {
    // A silent, valid 20-second PCM file keeps the success case independent
    // of codecs, external media and audible output.
    std::ofstream f(path, std::ios::binary);
    const auto u16 = [&](uint16_t n) { f.write(reinterpret_cast<const char*>(&n), 2); };
    const auto u32 = [&](uint32_t n) { f.write(reinterpret_cast<const char*>(&n), 4); };
    constexpr uint32_t bytes = 8000 * 2 * 20;
    f.write("RIFF", 4); u32(bytes + 36); f.write("WAVEfmt ", 8); u32(16);
    u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    f.write("data", 4); u32(bytes);
    const std::string samples(bytes, '\0'); f.write(samples.data(), samples.size());
}
}
namespace ttplayer::testing {
struct ProgressSeekAccess {
    static void CheckPaint(ui::PlayerWindow& p, const std::wstring& text) {
        SetWindowPos(p.lyric_control_, nullptr, 0, 0, 220, 110, SWP_NOZORDER | SWP_NOACTIVATE);
        p.settings_.lyric.transparent = false;
        p.settings_.lyric.text_color = RGB(160, 180, 200);
        p.settings_.lyric.highlight_color = RGB(0, 255, 0);
        p.settings_.lyric.background_color = RGB(10, 20, 30);
        p.settings_.lyric.fade_index = 2;
        p.RebuildLyricFont(false);
        for (int scroll : {0, 1}) for (int align : {0, 1, 2}) {
            p.ActiveLyricScrollMode() = scroll; p.settings_.lyric.text_align = align;
            Canvas actual, expected;
            p.PaintLyricControl(p.lyric_control_, actual.dc, false);
            RECT rect{0, 0, 220, 110};
            auto brush = CreateSolidBrush(p.ActiveLyricBackgroundColor());
            FillRect(expected.dc, &rect, brush); DeleteObject(brush);
            const auto font = SelectObject(expected.dc, p.lyric_font_);
            SetBkMode(expected.dc, TRANSPARENT); SetTextColor(expected.dc, p.ActiveLyricTextColor());
            const UINT flags = DT_SINGLELINE | DT_NOPREFIX |
                (scroll ? DT_CENTER | (align == 0 ? DT_TOP : align == 1 ? DT_VCENTER : DT_BOTTOM)
                        : DT_VCENTER | (align == 0 ? DT_LEFT : align == 1 ? DT_CENTER : DT_RIGHT));
            DrawTextW(expected.dc, text.c_str(), -1, &rect, flags);
            SelectObject(expected.dc, font); GdiFlush();
            size_t foreground{};
            for (size_t i = 0; i < 220 * 110; ++i) {
                Require((actual.pixels[i] & 0xffffff) == (expected.pixels[i] & 0xffffff),
                        "idle lyric color/alignment/clipping/fade differs from original plain-text painter");
                foreground += (actual.pixels[i] & 0xffffff) != 0x0a141e;
            }
            Require(foreground > 40, "idle lyrics rendered blank");
        }
    }
    static void Run(HMODULE resources, const fs::path& runtime) {
        settings::Settings s;
        s.source_path = runtime / settings::kSettingsFileName;
        s.general.fade_windows = s.general.tray_icon = s.general.send_title_to_msn = false;
        s.lyric.auto_download = false; s.player.mute = true; s.player.volume = 0;
        s.player.play_mode = 2; s.player.auto_switch_list = false;
        s.playback.stop_when_fail = true; s.playback.sound_fade_mode = 0;
        ui::PlayerWindow p(s); p.SetSkinResourceModule(resources);
        Require(p.LoadSkinResource(resources) && p.Create(GetModuleHandleW(nullptr), SW_HIDE),
                "fixture window creation failed");
        struct Cleanup { ui::PlayerWindow& p; ~Cleanup() { if (IsWindow(p.window_)) DestroyWindow(p.window_); } } cleanup{p};
        Require(p.ActivePlaylist().Tracks().empty(), "startup fixture playlist not empty");
        Require(Text(p.lyric_control_) == L"千千静听 5.7 正式版", "initial lyric HWND lacks original default text");
        p.RestoreStartupPlayback(); p.RefreshPlaybackUi();
        Require(Text(p.lyric_control_) == p.DefaultPlayerTitle(), "startup refresh erased default text");
        Require(p.LyricFallbackText(true) == L"千千静听 尽听精彩", "desktop slogan replaced by version");
        CheckPaint(p, p.DefaultPlayerTitle());
        p.mini_mode_ = true; CheckPaint(p, p.DefaultPlayerTitle()); p.mini_mode_ = false;

        const auto bad = runtime / L"broken.wav";
        std::ofstream(bad, std::ios::binary) << "invalid audio";
        const auto good = runtime / L"valid.wav"; Wave(good);
        p.ActivePlaylist().Add({bad, "Broken title"});
        p.ActivePlaylist().Add({good, "Valid title"});
        p.SelectTrack(0, false); p.ClearLyrics();
        const auto fallback = p.LyricFallbackText();
        Require(fallback.find(L"Broken title") != std::wstring::npos, "track fallback lost title");
        Require(Text(p.lyric_control_) == fallback, "ClearLyrics erased track fallback");
        CheckPaint(p, fallback);
        p.SelectTrack(0, true);
        Require(p.audio_.State() == audio::PlaybackState::failed, "corrupt WAV unexpectedly playable");
        Require(p.info_items_ == std::vector<std::wstring>{L"无法打开 - 文件无效或不存在"},
                "failure did not publish main info reason");
        Require(p.PlaybackStatusText() == L"状态: 无效", "failure status missing");
        Require(!p.pending_failed_advance_, "StopWhenFail ignored");
        p.RebuildSkinInfoItems(true); p.ResetSkinInfoScroll();
        Pump(1100);
        Require(p.info_items_[0] == p.playback_error_text_ && p.info_scroll_offset_ == 0 &&
                p.info_vertical_offset_ == 0 && p.info_scroll_direction_ == 0,
                "skin rebind or scrolling replaced the error");
        Pump(4300);
        Require(p.playback_error_text_.empty() && p.PlaybackStatusText().empty() &&
                p.info_items_[0] == p.DefaultPlayerTitle(), "five-second error recovery failed");
        Pump(350);
        Require(p.playback_error_text_.empty() && p.current_ == size_t{0}, "failed UI tick rearmed error or advanced");

        p.settings_.playback.stop_when_fail = false;
        p.SelectTrack(0, true);
        SendMessageW(p.window_, WM_TIMER, ui::detail::kPlaybackErrorTimer, 0);
        SendMessageW(p.window_, WM_TIMER, ui::detail::kFailedAdvanceTimer, 0);
        Require(!p.playback_error_text_.empty() && p.pending_failed_advance_ && p.current_ == size_t{0},
                "old queued timer expired or advanced a newer failure");
        Pump(2200);
        Require(p.current_ == size_t{0} && !p.playback_error_text_.empty(), "auto-skip occurred before three seconds");
        Pump(1100);
        Require(p.current_ == size_t{1} && p.audio_.State() == audio::PlaybackState::playing &&
                p.playback_error_text_.empty(), "delayed failure advance did not start valid WAV");
        const auto playing_title = p.display_title_;
        Pump(2200);
        Require(p.display_title_ == playing_title && p.PlaybackStatusText() == L"状态: 播放",
                "stale error timer overwrote a successful track");

        // Reproduce an asynchronous read failure without unreliable network I/O.
        p.settings_.playback.stop_when_fail = true;
        p.audio_.Stop(); p.playback_was_active_ = true;
        p.audio_.SetError(L"synthetic reader diagnostic", static_cast<HRESULT>(0x80040070U));
        SendMessageW(p.window_, WM_TIMER, ui::detail::kUiTimer, 0);
        Require(p.playback_error_text_ == L"无法连接到网络媒体", "reader HRESULT lost on async failure");
        p.Stop(); Pump(350);
        Require(p.playback_error_text_.empty() && !p.pending_failed_advance_, "stop retained failure notice");
        SendMessageW(p.window_, WM_TIMER, ui::detail::kPlaybackErrorTimer, 0);
        SendMessageW(p.window_, WM_TIMER, ui::detail::kFailedAdvanceTimer, 0);
        Require(p.current_ == size_t{1} && p.audio_.State() == audio::PlaybackState::stopped,
                "cancelled timer restarted playback");

        p.ActivePlaylist().SetPath(0, runtime / L"missing.wav"); p.SelectTrack(0, true);
        Require(p.playback_error_text_ == L"无法打开 - 文件无效或不存在", "missing-file reason changed");
        p.SelectTrack(1, true);
        Require(p.playback_error_text_.empty() && p.audio_.State() == audio::PlaybackState::playing,
                "manual retry did not clear old error");
        p.Stop(); p.current_.reset(); p.opened_track_.reset(); p.ActivePlaylist().Clear(); p.ClearLyrics();
        p.RefreshPlaybackUi();
        Require(Text(p.lyric_control_) == p.DefaultPlayerTitle(), "clearing playlist lost idle text");
        Require(ui::PlaybackErrorResource(static_cast<HRESULT>(0x8004006fU), false) == 0x8290 &&
                ui::PlaybackErrorResource(static_cast<HRESULT>(0x80040071U), false) == 0x8291 &&
                ui::PlaybackErrorResource(E_FAIL, true) == 0x8291, "original network error mapping changed");
        std::cout << "idle text/pixels, missing/corrupt files, 3s advance, 5s recovery, async error and cancellation passed\n";
    }
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc >= 2, "expected resource directory");
        if (std::wstring_view(argv[1]) != L"--fixture") {
            // Run beside isolated data so Create/Destroy cannot touch user playlists or profiles.
            const auto runtime = fs::temp_directory_path() / (L"TTPlayerIdleError-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(runtime);
            wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
            const auto child = runtime / L"idle_error_tests.exe"; fs::copy_file(exe, child);
            for (const auto name : {L"ttpres.dll", L"ttpcomm.dll"})
                fs::copy_file(fs::path(argv[1]) / name, runtime / name);
            auto command = L"\"" + child.wstring() + L"\" --fixture";
            STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE; PROCESS_INFORMATION process{};
            Require(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                nullptr, runtime.c_str(), &startup, &process), "cannot launch isolated test");
            const auto waited = WaitForSingleObject(process.hProcess, 35000);
            if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 2);
            DWORD result{}; GetExitCodeProcess(process.hProcess, &result);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            Require(waited == WAIT_OBJECT_0 && result == 0, "isolated idle/error tests failed");
            // Keep the uniquely named fixture for failure reproduction; no recursive deletion.
            return 0;
        }
        Require(SUCCEEDED(OleInitialize(nullptr)), "OLE init failed");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        const auto resources = LoadLibraryExW(L"ttpres.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
        Require(resources != nullptr, "original resources missing");
        testing::ProgressSeekAccess::Run(resources, fs::current_path());
        FreeLibrary(resources); OleUninitialize(); return 0;
    } catch (const std::exception& e) { std::cerr << "idle/error: " << e.what() << '\n'; return 1; }
}
