#include "ttplayer/ui/player_window.h"
#include "ttplayer/core/text.h"
#include "ttplayer/ui/window_drag.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "player_window_internal.h"
#include <algorithm>
#include <cstdlib>

namespace ttplayer::ui {
using namespace detail;
namespace {
bool HasBounds(const RECT& value) {
    return value.right > value.left && value.bottom > value.top;
}
}

bool PlayerWindow::BrowserAvailable() const noexcept {
    return browser_client_ && browser_client_->Available();
}
HWND PlayerWindow::MusicBrowserWindow() const noexcept {
    return browser_client_ ? browser_client_->Window() : nullptr;
}
HRESULT WINAPI PlayerWindow::BrowserSubmitCallback(void* context, const ttp_browser_track* tracks,
                                                   uint32_t count, int a, int b, int c) {
    return static_cast<PlayerWindow*>(context)->SubmitMusicBrowserTracks(tracks, count, a, b, c);
}
HRESULT WINAPI PlayerWindow::BrowserQueryCallback(void* context, uint32_t kind,
                                                  uint32_t selector, BSTR* text) {
    return static_cast<PlayerWindow*>(context)->QueryMusicBrowserInfo(kind, selector, text);
}
void WINAPI PlayerWindow::BrowserStateCallback(void* context, const ttp_browser_state* state) {
    if (state && state->size >= sizeof(*state))
        static_cast<PlayerWindow*>(context)->MusicBrowserStateChanged(*state);
}
HRESULT WINAPI PlayerWindow::BrowserSetupCallback(void* context, const wchar_t*, const wchar_t*, const wchar_t*) {
    // The page cannot choose executable URLs, command lines or a path.
    // Reuse the configured release-check/download/update workflow.
    static_cast<PlayerWindow*>(context)->CheckForUpdates(true);
    return S_OK;
}
HRESULT WINAPI PlayerWindow::BrowserAdjustWindowRectCallback(
    void* context, HWND window, uint32_t hit, RECT* bounds) {
    if (!context || !bounds) return E_POINTER;
    auto& self = *static_cast<PlayerWindow*>(context);
    if (self.browser_shutting_down_ || !window || window != self.MusicBrowserWindow())
        return RPC_E_DISCONNECTED;
    if (hit != HTCAPTION && (hit < HTLEFT || hit > HTBOTTOMRIGHT)) return E_INVALIDARG;
    const auto option = DecodePackedRuntimeOption(self.settings_.general.snap_windows, 1, 100);
    if (!option.enabled) return S_FALSE;
    std::vector<RECT> stationary;
    for (const HWND candidate : self.RegisteredDragWindows()) {
        if (candidate == window || !IsWindowVisible(candidate)) continue;
        RECT target{};
        if (GetWindowRect(candidate, &target)) stationary.push_back(target);
    }
    const RECT work = DragWorkAreaForRect(*bounds);
    if (hit == HTCAPTION) {
        // Auxiliary-window drags do not build the main window's attached group.
        const POINT correction = ComputeDragSnapCorrection({bounds, 1}, stationary, work, option.value);
        OffsetRect(bounds, correction.x, correction.y);
    } else {
        // FUN_004709A2 compares only the active edges. Unlike a move, a resize
        // does not require overlap on the other axis. The DLL clamps size last.
        const auto snap = [&](LONG coordinate, bool horizontal) {
            LONG correction{};
            int closest = option.value + 1;
            const auto consider = [&](LONG edge) {
                const LONG delta = edge - coordinate;
                if (std::abs(delta) < closest) { correction = delta; closest = std::abs(delta); }
            };
            consider(horizontal ? work.left : work.top);
            consider(horizontal ? work.right : work.bottom);
            for (const RECT& target : stationary) {
                consider(horizontal ? target.left : target.top);
                consider(horizontal ? target.right : target.bottom);
            }
            return coordinate + correction;
        };
        if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT) bounds->left = snap(bounds->left, true);
        if (hit == HTRIGHT || hit == HTTOPRIGHT || hit == HTBOTTOMRIGHT) bounds->right = snap(bounds->right, true);
        if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT) bounds->top = snap(bounds->top, false);
        if (hit == HTBOTTOM || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT) bounds->bottom = snap(bounds->bottom, false);
    }
    return S_OK;
}
HRESULT WINAPI PlayerWindow::BrowserResolveAssociatedLyricCallback(
    void* context, const wchar_t* media_path, BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!context || !media_path || !*media_path) return E_INVALIDARG;
    auto& self = *static_cast<PlayerWindow*>(context);
    if (self.browser_shutting_down_) return RPC_E_DISCONNECTED;
    if (!self.window_ || GetWindowThreadProcessId(self.window_, nullptr) != GetCurrentThreadId())
        return RPC_E_WRONG_THREAD;
    try {
        const DWORD media_attributes = GetFileAttributesW(media_path);
        if (media_attributes == INVALID_FILE_ATTRIBUTES ||
            (media_attributes & FILE_ATTRIBUTE_DIRECTORY)) return S_FALSE;
        self.EnsureLyricAssociationsLoaded();
        // Downloaded files identify the whole source (subtrack zero). Reuse
        // the exact persisted association used by LoadCurrentLyrics, including
        // the explicit NoLyric sentinel. Do not search by stem/title here.
        const auto associated = self.lyric_associations_.Find({media_path, 0});
        if (associated == std::filesystem::path(lyrics::kNoLyric)) return S_FALSE;
        std::filesystem::path lyric = associated.value_or(std::filesystem::path{});
        if (!associated) {
            const auto* playing = self.PlaybackTrackForUi();
            if (playing && !playing->subtrack && !_wcsicmp(playing->path.c_str(), media_path))
                lyric = self.associated_lyric_path_;
        }
        if (lyric.empty()) return S_FALSE;
        const DWORD attributes = GetFileAttributesW(lyric.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return S_FALSE;
        std::error_code error;
        lyric = std::filesystem::absolute(lyric, error).lexically_normal();
        if (error || !_wcsicmp(lyric.c_str(), media_path)) return S_FALSE;
        *output = SysAllocString(lyric.c_str());
        return *output ? S_OK : E_OUTOFMEMORY;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
bool PlayerWindow::ConfigureMusicBrowser() {
    if (browser_shutting_down_ || close_after_skin_window_fade_ ||
        !BrowserAvailable() || !window_ || !IsWindow(window_)) return false;
    const auto runtime = PlayerRuntimeDirectory().wstring();
    const auto root = settings_.source_path.empty() ? PlayerRuntimeDirectory()
                                                   : settings_.source_path.parent_path();
    const auto state_file = (root / L"HttpDownloads" / L"cachelist.xml").wstring();
    const auto downloads = settings_.network.download_folder.wstring();
    const auto cache = settings_.network.cache_folder.wstring();
    ttp_browser_config config{};
    config.size = sizeof(config);
    config.runtime_directory = runtime.c_str();
    config.state_file = state_file.c_str();
    config.download_directory = downloads.c_str();
    config.home_url = settings_.player.browser_home_url.c_str();
    config.maximum_downloads = settings_.network.max_download_tasks;
    config.artist_folders = settings_.network.create_folder_by_artist;
    config.replace_files = settings_.network.replace_file;
    config.topmost = mini_mode_ ? settings_.player.mini_top_most : settings_.player.top_most;
    config.opacity = EffectiveSkinWindowAlpha(window_);
    config.cache_directory = cache.c_str();
    config.engine = static_cast<uint32_t>(std::clamp(settings_.player.browser_engine, 0, 2));
    config.accept_recommendations = settings_.network.accept_recommendation_list;
    config.initial_state.size = sizeof(config.initial_state);
    config.initial_state.bounds = settings_.player.browser_window;
    config.initial_state.alternate_bounds = settings_.player.browser_window2;
    config.initial_state.flags = (settings_.player.browser_auto_startup ? TTP_BROWSER_STATE_AUTOSTART : 0);
    const auto options = PlaybackOptionsFromSettings(settings_, PlayerRuntimeDirectory());
    if (browser_client_->Created()) return SUCCEEDED(browser_client_->Configure(config, options));
    ttp_browser_host host{};
    host.size = sizeof(host); host.version = TTP_BROWSER_ABI_VERSION;
    host.context = this; host.owner = window_; host.resources = ResourceModule();
    host.submit_tracks = BrowserSubmitCallback;
    host.query_info = BrowserQueryCallback;
    host.state_changed = BrowserStateCallback;
    host.setup_pack = BrowserSetupCallback;
    host.adjust_window_rect = BrowserAdjustWindowRectCallback;
    host.resolve_associated_lyric = BrowserResolveAssociatedLyricCallback;
    browser_state_applying_ = true;
    const HRESULT result = browser_client_->Create(host, config, options);
    browser_state_applying_ = false;
    if (FAILED(result)) return false;
    UpdateMusicBrowserSkin();
    return true;
}

void PlayerWindow::UpdateMusicBrowserSkin(bool restore_bounds) {
    if (!BrowserAvailable() || !browser_client_->Created()) return;
    ttp_browser_skin snapshot{}; snapshot.size = sizeof(snapshot);
    if (skin_) {
        const auto& source = skin_->Browser();
        snapshot.valid = source.valid;
        snapshot.position = source.position;
        snapshot.content = source.content;
        snapshot.background = source.background.image;
        snapshot.background_size = source.background.size;
        snapshot.transparent_color = source.transparent_color;
        snapshot.resize_rect = source.resize_rect;
        snapshot.resize_tile = source.resize_tile;
        const skin::SkinElement* fields[] = {&source.close, &source.music, &source.downloads,
            &source.back, &source.forward, &source.refresh, &source.startup, &source.link};
        for (size_t index = 0; index < std::size(fields); ++index) {
            const auto& element = *fields[index];
            auto& control = snapshot.controls[index];
            control.bounds = element.bounds; control.image = element.image;
            control.image_size = element.image_size; control.frames = element.frames;
            control.color = element.color;
            if (!element.font.empty()) {
                control.font.lfHeight = element.font_size;
                control.font.lfWeight = FW_NORMAL;
                control.font.lfCharSet = DEFAULT_CHARSET;
                wcsncpy_s(control.font.lfFaceName, element.font.c_str(), _TRUNCATE);
            }
            if (index == TTP_BROWSER_STARTUP) control.checkbox_interval = source.checkbox_interval;
        }
    }
    browser_state_applying_ = true;
    browser_client_->SetSkin(snapshot);
    browser_state_applying_ = false;
    if (restore_bounds) ApplyMusicBrowserState();
}
void PlayerWindow::ApplyMusicBrowserState() {
    if (!BrowserAvailable() || !browser_client_->Created()) return;
    browser_state_applying_ = true;
    ttp_browser_state current{};
    static_cast<void>(browser_client_->State(current));
    browser_client_->Show(!mini_mode_ && settings_.player.browser_visible
        ? ((current.flags & TTP_BROWSER_STATE_DOWNLOADS) ? TTP_BROWSER_VIEW_DOWNLOADS : TTP_BROWSER_VIEW_MUSIC)
        : TTP_BROWSER_VIEW_HIDE);
    if (const HWND window = MusicBrowserWindow(); window && HasBounds(settings_.player.browser_window)) {
        const auto& bounds = settings_.player.browser_window;
        SetWindowPos(window, nullptr, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    browser_state_applying_ = false;
}
void PlayerWindow::ShowMusicBrowser(uint32_t view) {
    if (!ConfigureMusicBrowser()) return;
    if (mini_mode_ && view != TTP_BROWSER_VIEW_HIDE) return;
    const bool first_window = !MusicBrowserWindow();
    const RECT saved = settings_.player.browser_window;
    browser_client_->Show(view);
    if (first_window && MusicBrowserWindow() && HasBounds(saved)) {
        SetWindowPos(MusicBrowserWindow(), nullptr, saved.left, saved.top,
            saved.right - saved.left, saved.bottom - saved.top, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    CaptureMusicBrowserState();
    ApplySkinWindowTopMost();
    ApplySkinWindowAlpha(MusicBrowserWindow(), EffectiveSkinWindowAlpha(window_));
    ApplyWindowShadow();
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void PlayerWindow::MusicBrowserStateChanged(const ttp_browser_state& state) {
    if (browser_state_applying_) return;
    if (HasBounds(state.bounds)) settings_.player.browser_window = state.bounds;
    if (HasBounds(state.alternate_bounds)) settings_.player.browser_window2 = state.alternate_bounds;
    // Entering mini mode temporarily hides this surface; its normal-mode
    // preference must survive that transient owner/window visibility change.
    if (!mini_mode_) settings_.player.browser_visible = (state.flags & TTP_BROWSER_STATE_VISIBLE) != 0;
    settings_.player.browser_auto_startup = (state.flags & TTP_BROWSER_STATE_AUTOSTART) != 0;
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void PlayerWindow::CaptureMusicBrowserState() {
    if (!browser_client_ || !browser_client_->Created()) return;
    ttp_browser_state state{};
    if (browser_client_->State(state)) MusicBrowserStateChanged(state);
}
bool PlayerWindow::TranslateMusicBrowserMessage(const MSG& message) const {
    if (!BrowserAvailable() || !browser_client_->Created()) return false;
    const HWND window = MusicBrowserWindow();
    if (!window || (message.hwnd != window && !IsChild(window, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
        int modifiers{};
        if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= HOTKEYF_SHIFT;
        if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= HOTKEYF_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000) modifiers |= HOTKEYF_ALT;
        for (const auto& binding : settings_.hotkey.key_map)
            if (binding.command == kCmdShowBrowser && binding.application.virtual_key == static_cast<int>(message.wParam) &&
                (binding.application.modifiers & (HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT)) == modifiers) {
                PostMessageW(window_, WM_COMMAND, kCmdShowBrowser, 0); return true;
            }
    }
    MSG copy = message;
    return browser_client_->Translate(copy);
}
HRESULT PlayerWindow::QueryMusicBrowserInfo(uint32_t kind, uint32_t selector, BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    std::wstring value;
    if (kind == 1 && selector == 1) value = L"5.7.9";
    else if (kind == 1 && selector == 2) value = L"0";
    else if (kind == 2 && (selector == 1 || selector == 2)) {
        if (skin_) value = selector == 1 ? skin_->Metadata().name : skin_->Metadata().author;
    } else return E_INVALIDARG;
    *output = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    return *output ? S_OK : E_OUTOFMEMORY;
}
HRESULT PlayerWindow::SubmitMusicBrowserTracks(const ttp_browser_track* items, uint32_t count,
                                              int mode0, int mode1, int mode2) {
    if ((!items && count) || count > 65535) return E_INVALIDARG;
    if (!count) return S_FALSE;
    std::vector<playlist::Track> tracks; tracks.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const auto& item = items[index];
        if (!item.source || !*item.source) return E_INVALIDARG;
        playlist::Track track; track.path = item.source;
        if (item.artist) track.artist = core::WideToUtf8(item.artist);
        if (item.title) track.title = core::WideToUtf8(item.title);
        if (item.tid && *item.tid) track.metadata.emplace_back("TTPlayer.Tid", core::WideToUtf8(item.tid));
        tracks.push_back(std::move(track));
    }
    size_t target = playlists_.ActiveIndex();
    if (mode0) {
        const auto selected = ChooseMusicBrowserPlaylist();
        if (!selected) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        target = *selected;
    }
    if (target >= playlists_.Size()) return E_UNEXPECTED;
    // mode1 zero supplies the current insertion context; nonzero supplies
    // the native null context, which appends to the destination list.
    size_t insertion = playlists_.At(target).Tracks().size();
    if (!mode1 && target == playlists_.ActiveIndex() && playlist_selection_)
        insertion = std::min(insertion, *playlist_selection_ + 1);
    return CommitImportedTracks(std::move(tracks), target, insertion, false,
        mode2 ? ImportPlayback::force : ImportPlayback::none) ? S_OK : E_FAIL;
}
}
