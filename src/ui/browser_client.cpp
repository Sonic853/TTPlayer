#include "ttplayer/ui/browser_client.h"
#include "ttplayer/audio/http_stream.h"
#include <filesystem>
#include <cstring>
#include <new>
#include <string>

namespace ttplayer::ui {
namespace {
constexpr HRESULT kUnavailable = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
std::filesystem::path BrowserModulePath() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count);
    return std::filesystem::path(path).parent_path() / L"ttpbrowser.dll";
}
}

BrowserClient::BrowserClient() {
    try {
        const auto path = BrowserModulePath();
        if (path.empty() || !path.is_absolute()) return;
        // An absolute filename prevents loading an unrelated DLL from CWD/PATH.
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module_) return;
        const auto get = reinterpret_cast<ttp_browser_get_api_fn>(
            GetProcAddress(module_, "ttp_browser_get_api"));
        const auto* supplied = get ? get(TTP_BROWSER_ABI_VERSION) : nullptr;
        if (!supplied || supplied->size < sizeof(ttp_browser_api) ||
            supplied->version != TTP_BROWSER_ABI_VERSION || !supplied->create ||
            !supplied->destroy || !supplied->configure || !supplied->set_skin ||
            !supplied->show || !supplied->window || !supplied->translate_message ||
            !supplied->get_state || !supplied->enqueue || !supplied->prefetch ||
            !supplied->external_dispatch) {
            FreeLibrary(module_); module_ = nullptr; return;
        }
        api_ = *supplied;
    } catch (...) {
        if (module_) FreeLibrary(module_);
        module_ = nullptr; api_ = {};
    }
}
BrowserClient::~BrowserClient() {
    Shutdown();
    if (module_) FreeLibrary(module_);
}
void BrowserClient::Snapshot(const audio::PlaybackOptions& value) {
    auto snapshot = value;
    snapshot.network_canceled = {};
    snapshot.archive_opened = {};
    snapshot.archive_session.reset();
    std::lock_guard lock(options_mutex_);
    options_ = std::move(snapshot);
}
HRESULT BrowserClient::Create(const ttp_browser_host& host, const ttp_browser_config& config,
                              const audio::PlaybackOptions& options) {
    if (!Available()) return kUnavailable;
    if (instance_) return Configure(config, options);
    if (host.size < TTP_BROWSER_HOST_V1_SIZE || host.version != TTP_BROWSER_ABI_VERSION)
        return E_INVALIDARG;
    Snapshot(options);
    callbacks_ = {};
    std::memcpy(&callbacks_, &host, TTP_BROWSER_HOST_V1_SIZE);
    if (host.size >= offsetof(ttp_browser_host, adjust_window_rect) + sizeof(host.adjust_window_rect))
        callbacks_.adjust_window_rect = host.adjust_window_rect;
    if (host.size >= offsetof(ttp_browser_host, resolve_associated_lyric) + sizeof(host.resolve_associated_lyric))
        callbacks_.resolve_associated_lyric = host.resolve_associated_lyric;
    callbacks_.size = sizeof(callbacks_);
    ui_thread_ = GetCurrentThreadId(); ui_enabled_ = true;
    stopping_->store(false, std::memory_order_release);
    auto bridge = callbacks_;
    bridge.context = this;
    bridge.submit_tracks = Submit; bridge.query_info = Query;
    bridge.open_stream = OpenStream; bridge.state_changed = Changed; bridge.setup_pack = Setup;
    bridge.adjust_window_rect = callbacks_.adjust_window_rect ? AdjustWindowRect : nullptr;
    bridge.resolve_associated_lyric = callbacks_.resolve_associated_lyric ? ResolveAssociatedLyric : nullptr;
    const HRESULT result = api_.create(&bridge, &config, &instance_);
    if (FAILED(result)) {
        ui_enabled_ = false;
        if (instance_) api_.destroy(instance_);
        instance_ = nullptr; callbacks_ = {};
    }
    return result;
}
HRESULT BrowserClient::Configure(const ttp_browser_config& config,
                                 const audio::PlaybackOptions& options) {
    Snapshot(options);
    return instance_ ? api_.configure(instance_, &config) : kUnavailable;
}
HRESULT BrowserClient::SetSkin(const ttp_browser_skin& skin) {
    return instance_ ? api_.set_skin(instance_, &skin) : kUnavailable;
}
HRESULT BrowserClient::Show(uint32_t view) { return instance_ ? api_.show(instance_, view) : kUnavailable; }
HWND BrowserClient::Window() const noexcept { return instance_ ? api_.window(instance_) : nullptr; }
bool BrowserClient::Translate(MSG& message) const noexcept {
    return instance_ && api_.translate_message(instance_, &message) != FALSE;
}
bool BrowserClient::State(ttp_browser_state& state) const noexcept {
    state.size = sizeof(state);
    return instance_ && SUCCEEDED(api_.get_state(instance_, &state));
}
HRESULT BrowserClient::Enqueue(const ttp_browser_track& track, uint64_t* task_id) {
    uint64_t ignored{};
    return instance_ ? api_.enqueue(instance_, &track, task_id ? task_id : &ignored) : kUnavailable;
}
HRESULT BrowserClient::Prefetch(const wchar_t* url) {
    return instance_ ? api_.prefetch(instance_, url ? url : L"") : kUnavailable;
}
void BrowserClient::Shutdown() noexcept {
    // UI closures must stop before destroy can pump messages or join workers.
    DisableUiCallbacks();
    stopping_->store(true, std::memory_order_release);
    if (instance_) api_.destroy(instance_);
    instance_ = nullptr;
}
void BrowserClient::DisableUiCallbacks() noexcept {
    ui_enabled_ = false; callbacks_ = {};
}
bool BrowserClient::UiCallbacksAllowed() const noexcept {
    return ui_enabled_ && GetCurrentThreadId() == ui_thread_ && callbacks_.context;
}
HRESULT WINAPI BrowserClient::Submit(void* context, const ttp_browser_track* tracks,
                                    uint32_t count, int a, int b, int c) {
    auto& self = *static_cast<BrowserClient*>(context);
    if (!self.UiCallbacksAllowed() || !self.callbacks_.submit_tracks) return RPC_E_DISCONNECTED;
    try { return self.callbacks_.submit_tracks(self.callbacks_.context, tracks, count, a, b, c); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI BrowserClient::Query(void* context, uint32_t kind, uint32_t selector, BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    auto& self = *static_cast<BrowserClient*>(context);
    if (!self.UiCallbacksAllowed() || !self.callbacks_.query_info) return RPC_E_DISCONNECTED;
    try { return self.callbacks_.query_info(self.callbacks_.context, kind, selector, output); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
void WINAPI BrowserClient::Changed(void* context, const ttp_browser_state* state) {
    auto& self = *static_cast<BrowserClient*>(context);
    if (!self.UiCallbacksAllowed() || !self.callbacks_.state_changed) return;
    try { self.callbacks_.state_changed(self.callbacks_.context, state); } catch (...) {}
}
HRESULT WINAPI BrowserClient::Setup(void* context, const wchar_t* a, const wchar_t* b, const wchar_t* c) {
    auto& self = *static_cast<BrowserClient*>(context);
    if (!self.UiCallbacksAllowed() || !self.callbacks_.setup_pack) return RPC_E_DISCONNECTED;
    try { return self.callbacks_.setup_pack(self.callbacks_.context, a, b, c); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI BrowserClient::AdjustWindowRect(void* context, HWND window, uint32_t hit, RECT* bounds) {
    if (!bounds) return E_POINTER;
    auto& self = *static_cast<BrowserClient*>(context);
    if (!self.UiCallbacksAllowed() || !self.callbacks_.adjust_window_rect) return RPC_E_DISCONNECTED;
    try { return self.callbacks_.adjust_window_rect(self.callbacks_.context, window, hit, bounds); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI BrowserClient::ResolveAssociatedLyric(void* context, const wchar_t* path, BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!context) return E_INVALIDARG;
    auto& self = *static_cast<BrowserClient*>(context);
    if (!self.UiCallbacksAllowed() || !self.callbacks_.resolve_associated_lyric) return RPC_E_DISCONNECTED;
    try { return self.callbacks_.resolve_associated_lyric(self.callbacks_.context, path, output); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI BrowserClient::OpenStream(void* context, const wchar_t* url, HANDLE cancel,
                                        IStream** output, BOOL* can_persist) {
    if (!output || !can_persist) return E_POINTER;
    *output = nullptr; *can_persist = FALSE;
    if (!context || !url || !*url) return E_INVALIDARG;
    auto& self = *static_cast<BrowserClient*>(context);
    try {
        audio::PlaybackOptions options;
        { std::lock_guard lock(self.options_mutex_); options = self.options_; }
        std::shared_ptr<void> cancellation;
        if (cancel) {
            HANDLE duplicate{};
            if (!DuplicateHandle(GetCurrentProcess(), cancel, GetCurrentProcess(), &duplicate,
                                 SYNCHRONIZE, FALSE, 0)) return HRESULT_FROM_WIN32(GetLastError());
            cancellation.reset(duplicate, [](void* value) { CloseHandle(static_cast<HANDLE>(value)); });
        }
        const auto stopping = self.stopping_;
        options.network_canceled = [stopping, cancellation] {
            return stopping->load(std::memory_order_acquire) ||
                (cancellation && WaitForSingleObject(static_cast<HANDLE>(cancellation.get()), 0) != WAIT_TIMEOUT);
        };
        if (options.network_canceled()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        audio::HttpAudioStream* stream{};
        const HRESULT result = audio::OpenHttpAudioStream(url, options, &stream, audio::HttpStreamUse::background);
        if (FAILED(result)) return result;
        if (!stream) return E_UNEXPECTED;
        *can_persist = stream->CanPersist() ? TRUE : FALSE;
        *output = stream; // Transfers the initial COM reference to the DLL.
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}
}
