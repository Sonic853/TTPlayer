#pragma once

#include "ttplayer/audio/audio_engine.h"
#include <ttpbrowser.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace ttplayer::ui {
// Optional module boundary. Only open_stream runs on a worker; it never reads
// PlayerWindow or its mutable settings. The DLL joins its workers in destroy.
class BrowserClient final {
public:
    BrowserClient();
    ~BrowserClient();
    BrowserClient(const BrowserClient&) = delete;
    BrowserClient& operator=(const BrowserClient&) = delete;

    [[nodiscard]] bool Available() const noexcept { return module_ && api_.create; }
    [[nodiscard]] bool Created() const noexcept { return instance_ != nullptr; }
    HRESULT Create(const ttp_browser_host&, const ttp_browser_config&,
                   const audio::PlaybackOptions&);
    HRESULT Configure(const ttp_browser_config&, const audio::PlaybackOptions&);
    HRESULT SetSkin(const ttp_browser_skin&);
    HRESULT Show(uint32_t view);
    [[nodiscard]] HWND Window() const noexcept;
    [[nodiscard]] bool Translate(MSG&) const noexcept;
    [[nodiscard]] bool State(ttp_browser_state&) const noexcept;
    HRESULT Enqueue(const ttp_browser_track&, uint64_t* task_id = nullptr);
    HRESULT Prefetch(const wchar_t* url);
    HRESULT CreateSettingsPage(HWND parent, HWND* page);
    HRESULT CommitSettingsPage();
    void DisableUiCallbacks() noexcept;
    void Shutdown() noexcept;

private:
    struct ResolverState;
    std::shared_ptr<ResolverState> resolver_;
    static HRESULT WINAPI Submit(void*, const ttp_browser_track*, uint32_t, int, int, int);
    static HRESULT WINAPI Query(void*, uint32_t, uint32_t, BSTR*);
    static HRESULT WINAPI OpenStream(void*, const wchar_t*, HANDLE, IStream**, BOOL*);
    static void WINAPI Changed(void*, const ttp_browser_state*);
    static HRESULT WINAPI OpenMediaSettings(void*);
    static HRESULT WINAPI SetWindowVisible(void*, HWND, BOOL);
    static HRESULT WINAPI TrackPopupMenu(void*, HWND, HMENU, POINT, UINT*);
    static HRESULT WINAPI OpenOptions(void*);
    static HRESULT WINAPI GetDownloadPage(void*, BSTR*, BSTR*);
    static HRESULT WINAPI RecognizePageDownload(void*, const wchar_t*, const wchar_t*, BSTR*);
    static HRESULT WINAPI GetPageScript(void*, BSTR*);
    static HRESULT WINAPI RecognizePageAudio(void*, const wchar_t*, const wchar_t*);
    static HRESULT WINAPI Setup(void*, const wchar_t*, const wchar_t*, const wchar_t*);
    static HRESULT WINAPI AdjustWindowRect(void*, HWND, uint32_t, RECT*);
    static HRESULT WINAPI ResolveAssociatedLyric(void*, const wchar_t*, BSTR*);
    bool UiCallbacksAllowed() const noexcept;
    void Snapshot(const audio::PlaybackOptions&);

    HMODULE module_{};
    ttp_browser_api api_{};
    void* instance_{};
    ttp_browser_host callbacks_{};
    DWORD ui_thread_{};
    bool ui_enabled_{};
    std::mutex options_mutex_;
    audio::PlaybackOptions options_;
    std::shared_ptr<std::atomic_bool> stopping_{std::make_shared<std::atomic_bool>(false)};
};
}
