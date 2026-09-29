#include "ttplayer/audio/winamp_dsp.h"
#include "dsp_dispatcher.h"
#include "plugin_registry.h"
#include "ttplayer/core/text.h"
#include <atomic>
#include <exception>
#include <functional>
#include <iterator>
#include <optional>
#include <thread>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <mutex>
#include <utility>

namespace ttplayer::audio {
namespace {

constexpr int kMinimumHeaderVersion = 0x20;
constexpr int kMaximumModuleSlots = 10;
constexpr int kSplitFrameThreshold = 0x3fff;
constexpr wchar_t kDspOriginalProcedure[] = L"TTPlayer.DspOriginalProcedure";
constexpr wchar_t kDspHostWindow[] = L"TTPlayer.DspHostWindow";
constexpr wchar_t kDspDragState[] = L"TTPlayer.DspDragState";

struct DspDragState {
    RECT first_proposed{};
    POINT first_cursor{};
    bool anchored{};
};

void ClearDspDragState(HWND window) noexcept {
    delete reinterpret_cast<DspDragState*>(RemovePropW(window, kDspDragState));
}

LRESULT CALLBACK DspWindowProcedure(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    if (message == WM_ENTERSIZEMOVE) {
        ClearDspDragState(window);
        auto* drag = new (std::nothrow) DspDragState;
        if (drag && !SetPropW(window, kDspDragState, drag)) delete drag;
    }
    const auto previous = reinterpret_cast<WNDPROC>(GetPropW(window, kDspOriginalProcedure));
    const LRESULT result = previous
        ? CallWindowProcW(previous, window, message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_MOVING && lparam) {
        auto* proposed = reinterpret_cast<RECT*>(lparam);
        auto* drag = reinterpret_cast<DspDragState*>(GetPropW(window, kDspDragState));
        POINT cursor{};
        if (drag && GetCursorPos(&cursor)) {
            // USER32's next proposal can be the snapped HWND plus only the
            // latest mouse delta. Re-snapping that delta pins slow drags to
            // the edge forever. Preserve an unsnapped screen-space anchor,
            // like the fixed grab point in 0044F670/0046EB64. The first
            // WM_MOVING includes the native caption-drag threshold; keep it.
            if (!drag->anchored) {
                drag->first_proposed = *proposed;
                drag->first_cursor = cursor;
                drag->anchored = true;
            } else {
                OffsetRect(proposed,
                    drag->first_proposed.left + cursor.x - drag->first_cursor.x - proposed->left,
                    drag->first_proposed.top + cursor.y - drag->first_cursor.y - proposed->top);
            }
        }
        const HWND host = reinterpret_cast<HWND>(GetPropW(window, kDspHostWindow));
        if (IsWindow(host) && SendMessageW(host, kWinampDspMovingMessage,
                reinterpret_cast<WPARAM>(window), lparam)) return TRUE;
    } else if (message == WM_EXITSIZEMOVE || message == WM_CANCELMODE) {
        ClearDspDragState(window);
    } else if (message == WM_NCDESTROY) {
        ClearDspDragState(window);
        RemovePropW(window, kDspHostWindow);
        RemovePropW(window, kDspOriginalProcedure);
    }
    return result;
}

// This is the prefix of the original Winamp SDK module. A plug-in may append
// userData at +0x1C (Ozone does); it must never be inserted at the start. The x86
// offsets proven by the decompilation are Config=0x0C, Init=0x10,
// ModifySamples=0x14 and Quit=0x18.
struct WinampDspModule {
    const char* description;
    HWND parent;
    HINSTANCE instance;
    void (__cdecl* configure)(WinampDspModule*);
    int (__cdecl* initialize)(WinampDspModule*);
    int (__cdecl* modify_samples)(WinampDspModule*, short*, int, int, int, int);
    void (__cdecl* quit)(WinampDspModule*);
};

struct WinampDspHeader {
    int version;
    const char* description;
    WinampDspModule* (__cdecl* get_module)(int);
};

static_assert(offsetof(WinampDspModule, configure) == sizeof(void*) * 3);
static_assert(offsetof(WinampDspModule, initialize) == sizeof(void*) * 4);
static_assert(offsetof(WinampDspModule, modify_samples) == sizeof(void*) * 5);
static_assert(offsetof(WinampDspModule, quit) == sizeof(void*) * 6);

std::wstring WindowsError(std::wstring message, DWORD error) {
    message += L" (";
    message += std::to_wstring(error);
    message += L")";
    return message;
}

std::filesystem::path ResolveModulePath(
    const std::filesystem::path& folder, std::wstring_view configured) {
    std::filesystem::path path(configured);
    if (path.is_relative()) path = folder / path;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

bool SamePath(const std::filesystem::path& left,
              const std::filesystem::path& right) noexcept {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool ContainsPacemaker(WinampDspHeader* header) noexcept {
    if (!header) return false;
#if defined(_MSC_VER)
    __try {
#endif
        const char* value = header->description;
        if (!value) return false;
        constexpr char needle[] = "pacemaker";
        constexpr std::size_t needle_length = sizeof(needle) - 1;
        for (std::size_t index = 0; index != 32768 && value[index] != '\0'; ++index)
            if (_strnicmp(value + index, needle, needle_length) == 0) return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return false;
}

int ReadHeaderVersion(WinampDspHeader* header) noexcept {
    if (!header) return 0;
#if defined(_MSC_VER)
    __try { return header->version; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    return header->version;
#endif
}

bool PrepareModule(WinampDspModule* module, HWND parent,
                   HMODULE library) noexcept {
    if (!module) return false;
#if defined(_MSC_VER)
    __try {
        if (!module->initialize || !module->modify_samples || !module->quit)
            return false;
        module->parent = parent;
        module->instance = library;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!module->initialize || !module->modify_samples || !module->quit)
        return false;
    module->parent = parent;
    module->instance = library;
    return true;
#endif
}

WinampDspHeader* InvokeGetHeaderSeh(FARPROC entry) {
    if (!entry) return nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<WinampDspHeader* (__cdecl*)()>(entry)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    return reinterpret_cast<WinampDspHeader* (__cdecl*)()>(entry)();
#endif
}

WinampDspHeader* InvokeGetHeader(FARPROC entry) noexcept {
    try { return InvokeGetHeaderSeh(entry); }
    catch (...) { return nullptr; }
}

WinampDspModule* InvokeGetModuleSeh(WinampDspHeader* header, int index) {
    if (!header) return nullptr;
#if defined(_MSC_VER)
    __try {
        return header->get_module ? header->get_module(index) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#else
    return header->get_module ? header->get_module(index) : nullptr;
#endif
}

WinampDspModule* InvokeGetModule(WinampDspHeader* header, int index) noexcept {
    try { return InvokeGetModuleSeh(header, index); }
    catch (...) { return nullptr; }
}

bool InvokeInitializeSeh(WinampDspModule* module) {
    if (!module) return false;
#if defined(_MSC_VER)
    __try {
        if (!module->initialize) return false;
        // FUN_00427EDC ignores the SDK return value and retains the module.
        static_cast<void>(module->initialize(module));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!module->initialize) return false;
    static_cast<void>(module->initialize(module));
    return true;
#endif
}

bool InvokeInitialize(WinampDspModule* module) noexcept {
    try { return InvokeInitializeSeh(module); }
    catch (...) { return false; }
}

bool InvokeModifySeh(WinampDspModule* module, short* samples, int frames,
                     int channels, int sample_rate) {
    if (!module) return false;
#if defined(_MSC_VER)
    __try {
        if (!module->modify_samples) return false;
        static_cast<void>(module->modify_samples(
            module, samples, frames, 16, channels, sample_rate));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!module->modify_samples) return false;
    static_cast<void>(module->modify_samples(
        module, samples, frames, 16, channels, sample_rate));
    return true;
#endif
}

bool InvokeModify(WinampDspModule* module, short* samples, int frames,
                  int channels, int sample_rate) noexcept {
    try { return InvokeModifySeh(module, samples, frames, channels, sample_rate); }
    catch (...) { return false; }
}

bool InvokeQuitSeh(WinampDspModule* module) {
    if (!module) return true;
#if defined(_MSC_VER)
    __try {
        if (module->quit) module->quit(module);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    if (module->quit) module->quit(module);
    return true;
#endif
}

bool InvokeQuit(WinampDspModule* module) noexcept {
    try { return InvokeQuitSeh(module); }
    catch (...) { return false; }
}

bool HasConfiguration(WinampDspModule* module) noexcept {
#if defined(_MSC_VER)
    __try { return module && module->configure; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return module && module->configure;
#endif
}

bool InvokeConfigureSeh(WinampDspModule* module) {
#if defined(_MSC_VER)
    __try {
#endif
        if (!module || !module->configure) return false;
        module->configure(module);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

bool InvokeConfigure(WinampDspModule* module) noexcept {
    try { return InvokeConfigureSeh(module); }
    catch (...) { return false; }
}

} // namespace

class WinampDspChain::Impl {
public:
    detail::DspDispatcher dispatcher;
    std::atomic<std::size_t> active_count{};
    struct LoadedModule {
        std::filesystem::path path;
        HMODULE library{};
        WinampDspModule* module{};
        std::unique_ptr<detail::PluginRegistry> registry;
        std::atomic<bool> failed{};

        LoadedModule() = default;
        LoadedModule(const LoadedModule&) = delete;
        LoadedModule& operator=(const LoadedModule&) = delete;
        LoadedModule(LoadedModule&& other) noexcept
            : path(std::move(other.path)), library(std::exchange(other.library, nullptr)),
              module(std::exchange(other.module, nullptr)), registry(std::move(other.registry)),
              failed(other.failed.load()) {}
        LoadedModule& operator=(LoadedModule&& other) noexcept {
            if (this == &other) return *this;
            Close(nullptr);
            path = std::move(other.path);
            library = std::exchange(other.library, nullptr);
            module = std::exchange(other.module, nullptr);
            registry = std::move(other.registry);
            failed = other.failed.load();
            return *this;
        }
        ~LoadedModule() { Close(nullptr); }

        void Close(std::vector<std::wstring>* diagnostics) noexcept {
            WinampDspModule* old_module = std::exchange(module, nullptr);
            HMODULE old_library = std::exchange(library, nullptr);
            if (old_module && !InvokeQuit(old_module) && diagnostics) {
                try {
                    diagnostics->push_back(
                        L"Winamp DSP Quit raised an exception: " + path.wstring());
                } catch (...) {}
            }
            if (old_library) {
                // A legacy modeless Config may leave a window behind even
                // after Quit. Destroy apartment-owned windows before their
                // WNDPROC code disappears from the process.
                EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
                    if (GetClassLongPtrW(window, GCLP_HMODULE) == static_cast<ULONG_PTR>(value))
                        DestroyWindow(window);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(old_library));
                FreeLibrary(old_library);
            }
            if (registry) {
                if (!registry->Flush() && diagnostics) {
                    try { diagnostics->push_back(L"Unable to save plug-in file configuration: " + path.wstring()); }
                    catch (...) {}
                }
                registry.reset();
            }
        }
    };

    void Update(const std::filesystem::path& folder,
                const std::vector<std::wstring>& configured,
                HWND parent_window) {
        std::vector<std::filesystem::path> requested;
        requested.reserve(configured.size());
        for (const auto& module : configured) {
            if (module.empty()) continue;
            auto path = ResolveModulePath(folder, module);
            if (std::none_of(requested.begin(), requested.end(),
                    [&path](const auto& current) { return SamePath(current, path); }))
                requested.push_back(std::move(path));
        }
        // Config can run a modal message loop. Nested requests must never
        // unload its DLL (or a ModifySamples stack which queried the player).
        // Publish before trying the PCM lock: otherwise the final callback
        // could finish between try_lock failing and setting the flag, leaving
        // a removal pending forever when there is no next block at EOF.
        pending_ = Pending{folder, configured, parent_window};
        pending_update_ = true;
        std::unique_lock processing_lock(processing_mutex_, std::try_to_lock);
        if (callback_depth_ != 0 || !processing_lock.owns_lock() || processing_) return;
        // A newer accepted control request supersedes a deferred older one.
        pending_.reset();
        pending_update_ = false;
        const bool same_parent = parent_window == parent_window_;
        const bool same_modules = requested.size() == requested_.size() &&
            std::equal(requested.begin(), requested.end(), requested_.begin(),
                       SamePath);
        if (same_parent && same_modules && prepared_.empty()) return;

        // Move surviving wrappers, rather than initializing the entire chain.
        // This is the pointer-preserving 00428A7F ordering contract.
        std::vector<LoadedModule> next;
        std::vector<std::filesystem::path> missing;
        next.reserve(requested.size());
        missing.reserve(requested.size());
        for (const auto& path : requested) {
            LoadedModule* found{};
            for (auto* source : {&modules_, &prepared_}) {
                const auto it = std::find_if(source->begin(), source->end(),
                    [&path](const auto& item) { return item.module && SamePath(item.path, path); });
                if (it != source->end()) { found = &*it; break; }
            }
            if (found) {
                PrepareModule(found->module, parent_window, found->library);
                next.push_back(std::move(*found));
            } else if (!same_parent || !same_modules) missing.push_back(path);
        }
        ++callback_depth_;
        auto retired = std::move(modules_);
        auto stale_prepared = std::move(prepared_);
        modules_ = std::move(next);
        requested_ = std::move(requested);
        parent_window_ = parent_window;
        PublishActive();
        processing_lock.unlock();

        // 00428471 removes the instance under the chain lock; 00428061 calls
        // Quit/FreeLibrary only after releasing it. Neither plug-in teardown
        // nor registry persistence may block the remaining audio chain.
        CloseModules(retired);
        CloseModules(stale_prepared);
        for (const auto& path : missing) Load(path);
        // Init also runs outside the PCM lock. Publish prepared instances at
        // a callback boundary; never wait on PCM which can query this GUI.
        if (!prepared_.empty() && !pending_) {
            pending_ = Pending{folder, configured, parent_window};
            pending_update_ = true;
        }
        FinishCallback();
        Windows();
    }

    void Process(std::span<std::int16_t> samples, int channels,
                 int sample_rate, std::vector<std::wstring>* diagnostics) {
        // The recovered 0042898D/004280CD runs PCM on the audio worker, not
        // on the apartment that owns plug-in windows. Serialize overlapping
        // playback sessions, but never make UI Update wait for a callback
        // which may itself synchronously query that UI.
        std::unique_lock processing_lock(processing_mutex_);
        if (processing_) return; // A third-party message pump re-entered PCM.
        processing_ = true;
        struct Completion {
            Impl& self;
            std::unique_lock<std::recursive_mutex>& lock;
            ~Completion() {
                self.processing_ = false;
                lock.unlock();
                if (self.pending_update_.load())
                    self.dispatcher.Post([&self = self] { self.FinishPending(); });
            }
        } completion{*this, processing_lock};
        if (samples.empty() || channels <= 0 || sample_rate <= 0 ||
            samples.size() % static_cast<std::size_t>(channels) != 0)
            return;
        const std::size_t frame_count = samples.size() /
                                        static_cast<std::size_t>(channels);
        if (frame_count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return;

        // Winamp's ABI permits ModifySamples to write up to twice the input.
        // The recovered host ignores its returned length, but must still give
        // it that capacity. Stage each half separately so an expanding first
        // half cannot overwrite the second half's input.
        const auto largest_chunk = frame_count > kSplitFrameThreshold
            ? (frame_count + 1) / 2 : frame_count;
        if (largest_chunk > scratch_.max_size() /
                static_cast<std::size_t>(channels) / 2) return;
        scratch_.resize(largest_chunk * static_cast<std::size_t>(channels) * 2);
        const auto modify = [&](WinampDspModule* module, std::size_t offset,
                                int frames) {
            const auto count = static_cast<std::size_t>(frames) * channels;
            std::copy_n(samples.data() + offset, count, scratch_.data());
            const bool good = InvokeModify(module, scratch_.data(), frames,
                                            channels, sample_rate);
            if (good) std::copy_n(scratch_.data(), count, samples.data() + offset);
            return good;
        };
        for (auto& loaded : modules_) {
            if (loaded.failed.load()) continue;
            const int frames = static_cast<int>(frame_count);
            bool good = true;
            // This apparently unusual one-time half split is literal
            // FUN_004280CD behaviour, rather than a loop with 0x3fff chunks.
            if (frames > kSplitFrameThreshold) {
                const int first = frames / 2;
                good = modify(loaded.module, 0, first);
                if (good) {
                    good = modify(loaded.module,
                        static_cast<std::size_t>(first) * channels, frames - first);
                }
            } else {
                good = modify(loaded.module, 0, frames);
            }
            if (good) continue;
            loaded.failed = true;
            --active_count;
            {
                std::scoped_lock lock(status_mutex_);
                std::erase_if(active_paths_, [&](const auto& path) { return SamePath(path, loaded.path); });
            }
            {
                std::scoped_lock lock(diagnostics_mutex_);
                diagnostics_.push_back(
                    L"Winamp DSP ModifySamples raised an exception; disabled: " +
                    loaded.path.wstring());
            }
            // Original +0x24 marks a failed processor as bypassed. Keep its
            // instance mapped until UI-side removal; Config may still be on
            // its stack, and Quit must run on the initializing apartment.
        }
        if (diagnostics) {
            auto messages = TakeDiagnostics();
            diagnostics->insert(diagnostics->end(),
                std::make_move_iterator(messages.begin()),
                std::make_move_iterator(messages.end()));
        }
    }

    void CloseModules(std::vector<LoadedModule>& modules) noexcept {
        // FUN_00428B16 destroys the wrappers in ascending vector order.
        std::vector<std::wstring> messages;
        for (auto& current : modules) current.Close(&messages);
        modules.clear();
        try { for (auto& message : messages) AddDiagnostic(std::move(message)); }
        catch (...) {} // Teardown must still release the remaining host state.
    }

    ~Impl() {
        dispatcher.Stop([this] {
            std::unique_lock processing_lock(processing_mutex_);
            pending_.reset();
            pending_update_ = false;
            auto retired = std::move(modules_);
            auto prepared = std::move(prepared_);
            PublishActive();
            processing_lock.unlock();
            CloseModules(retired);
            CloseModules(prepared);
            for (const HWND window : hooked_windows_) {
                if (!IsWindow(window)) continue;
                const auto previous = reinterpret_cast<WNDPROC>(GetPropW(window, kDspOriginalProcedure));
                if (previous && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == DspWindowProcedure) {
                    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
                    ClearDspDragState(window);
                    RemovePropW(window, kDspOriginalProcedure);
                    RemovePropW(window, kDspHostWindow);
                }
            }
        });
    }

    LoadedModule* Find(const std::filesystem::path& path) {
        const auto normalized = ResolveModulePath({}, path.wstring());
        const auto found = std::find_if(modules_.begin(), modules_.end(),
            [&normalized](const auto& loaded) {
                return !loaded.failed.load() && SamePath(loaded.path, normalized);
            });
        return found == modules_.end() ? nullptr : &*found;
    }

    bool Configure(const std::filesystem::path& path) {
        const auto* loaded = Find(path);
        if (!loaded || !HasConfiguration(loaded->module)) return false;
        return dispatcher.Post([this, path] {
            auto* current = Find(path);
            if (!current || callback_depth_ != 0) return;
            if(current->registry) {
                try {if(current->registry->ConfigureDfx(parent_window_))return;}
                catch(const std::exception&) {AddDiagnostic(L"DFX file configuration helper could not start: "+path.wstring());return;}
            }
            // This bundle's DFX Config does nothing except read the obsolete
            // HKLM top_folder and launch Apps/dfxwsettings.exe (10001070).
            // Resolve that existing helper beside the DLL instead. This fixes
            // relocation without importing registration data or writing HKLM.
            const auto helper = current->path.parent_path() / L"DFX" /
                                L"Apps" / L"dfxwsettings.exe";
            std::error_code error;
            if (_wcsicmp(current->path.filename().c_str(), L"dsp_dfx.dll") == 0 &&
                std::filesystem::is_regular_file(helper, error)) {
                auto command = L"\"" + helper.wstring() + L"\"";
                STARTUPINFOW startup{sizeof(startup)};
                PROCESS_INFORMATION process{};
                const auto working = helper.parent_path().wstring();
                if (CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, working.c_str(), &startup, &process)) {
                    CloseHandle(process.hThread);
                    CloseHandle(process.hProcess);
                    return;
                }
                AddDiagnostic(WindowsError(L"Unable to open DFX settings", GetLastError()));
                return;
            }
            // Keep only the ABI pointer across Config: nested audio calls may
            // retire its vector entry, but cannot unload it until we return.
            auto* module = current->module;
            ++callback_depth_;
            if (!InvokeConfigure(module)) {
                AddDiagnostic(L"Winamp DSP Config failed: " + path.wstring());
            }
            FinishCallback();
            Windows();
        });
    }

    std::vector<HWND> Windows() {
        std::vector<HWND> result;
        if (!modules_.empty()) EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            DWORD process{};
            GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId()) return TRUE;
            wchar_t name[64]{};
            GetClassNameW(window, name, 64);
            if (_wcsicmp(name, L"DFX_WINDOW") == 0 || _wcsicmp(name, L"DFX_WINDOW_11") == 0 || _wcsicmp(name, L"Dee2") == 0)
                reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        if (IsWindow(parent_window_)) {
            for (const HWND window : result) {
                if (GetPropW(window, kDspOriginalProcedure)) continue;
                const auto previous = GetWindowLongPtrW(window, GWLP_WNDPROC);
                if (!previous || !SetPropW(window, kDspOriginalProcedure,
                                           reinterpret_cast<HANDLE>(previous))) continue;
                if (!SetPropW(window, kDspHostWindow, parent_window_) ||
                    !SetWindowLongPtrW(window, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(DspWindowProcedure))) {
                    RemovePropW(window, kDspOriginalProcedure);
                    RemovePropW(window, kDspHostWindow);
                    continue;
                }
                hooked_windows_.push_back(window);
            }
        }
        { std::scoped_lock lock(status_mutex_); windows_ = result; }
        return result;
    }

    bool IsActive(const std::filesystem::path& path) const {
        const auto normalized = ResolveModulePath({}, path.wstring());
        std::scoped_lock lock(status_mutex_);
        return std::any_of(active_paths_.begin(), active_paths_.end(),
            [&](const auto& item) { return SamePath(item, normalized); });
    }

    std::vector<HWND> WindowSnapshot() {
        if (!windows_refresh_pending_.exchange(true)) {
            if (!dispatcher.Post([this] { windows_refresh_pending_ = false; Windows(); }))
                windows_refresh_pending_ = false;
        }
        std::scoped_lock lock(status_mutex_);
        auto result = windows_;
        std::erase_if(result, [](HWND window) { return !IsWindow(window); });
        return result;
    }

    std::vector<std::wstring> TakeDiagnostics() {
        // A UI caller must not wait for PCM that may be querying its window.
        // PCM already owns this recursive lock; other callers can always
        // drain published messages and leave live registry polling to it.
        std::unique_lock processing_lock(processing_mutex_, std::try_to_lock);
        std::scoped_lock lock(diagnostics_mutex_);
        if (processing_lock.owns_lock()) for(auto& module:modules_)if(module.registry) {
            auto messages=module.registry->Diagnostics();
            diagnostics_.insert(diagnostics_.end(),std::make_move_iterator(messages.begin()),std::make_move_iterator(messages.end()));
        }
        return std::exchange(diagnostics_, {});
    }

    std::vector<std::filesystem::path> requested_;
    std::vector<LoadedModule> modules_;
    std::vector<std::wstring> diagnostics_;
    HWND parent_window_{};
    std::filesystem::path storage_directory;

private:
    void PublishActive() {
        std::scoped_lock lock(status_mutex_);
        active_paths_.clear();
        for (const auto& module : modules_)
            if (module.module && !module.failed.load()) active_paths_.push_back(module.path);
        active_count = active_paths_.size();
    }
    void AddDiagnostic(std::wstring message) {
        std::scoped_lock lock(diagnostics_mutex_);
        diagnostics_.push_back(std::move(message));
    }
    struct Pending {
        std::filesystem::path folder;
        std::vector<std::wstring> configured;
        HWND parent{};
    };
    std::optional<Pending> pending_;
    std::atomic<bool> pending_update_{};
    std::recursive_mutex processing_mutex_;
    std::mutex diagnostics_mutex_;
    mutable std::mutex status_mutex_;
    std::vector<std::filesystem::path> active_paths_;
    std::vector<HWND> windows_;
    std::atomic<bool> windows_refresh_pending_{};
    std::vector<LoadedModule> prepared_; // initialized, awaiting a PCM boundary
    bool processing_{}; // protected by processing_mutex_, including recursion
    std::vector<HWND> hooked_windows_;
    std::vector<std::int16_t> scratch_;
    unsigned callback_depth_{};

    void FinishCallback() {
        if (--callback_depth_ != 0) return;
        FinishPending();
    }

    void FinishPending() {
        if (callback_depth_ == 0 && pending_) {
            auto pending = std::exchange(pending_, std::nullopt);
            pending_update_ = false;
            Update(pending->folder, pending->configured, pending->parent);
        }
    }

    void Load(const std::filesystem::path& path) {
        // LOAD_WITH_ALTERED_SEARCH_PATH is the full-path counterpart to the
        // original LoadLibraryW call and lets old DSPs resolve dependencies
        // beside themselves without changing the process-wide DLL directory.
        HMODULE library = LoadLibraryExW(path.c_str(), nullptr,
                                         LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!library) {
            AddDiagnostic(WindowsError(
                L"Unable to load Winamp DSP: " + path.wstring(), GetLastError()));
            return;
        }
        std::unique_ptr<detail::PluginRegistry> registry;
        try { registry = detail::PluginRegistry::Attach(library, path, storage_directory); }
        catch (const std::exception& error) {
            const auto message=L"注册表配置接入失败："+core::Utf8ToWide(error.what());
            AddDiagnostic(message+L"；"+path.wstring());
            FreeLibrary(library);
            return;
        }
        const auto fail = [&](std::wstring reason) {
            AddDiagnostic(std::move(reason) + L": " + path.wstring());
            FreeLibrary(library);
        };

        const FARPROC entry = GetProcAddress(library, "winampDSPGetHeader2");
        WinampDspHeader* header = InvokeGetHeader(entry);
        if (!header) {
            fail(L"Winamp DSP header callback is missing or failed");
            return;
        }
        const int version = ReadHeaderVersion(header);
        if (version < kMinimumHeaderVersion) {
            fail(L"Winamp DSP header version is older than 0x20");
            return;
        }
        if (ContainsPacemaker(header)) {
            fail(L"Winamp DSP header is the incompatible pacemaker plug-in");
            return;
        }

        WinampDspModule* module{};
        for (int index = 0; index < kMaximumModuleSlots && !module; ++index)
            module = InvokeGetModule(header, index);
        if (!module) {
            fail(L"Winamp DSP exposes no module in slots 0..9");
            return;
        }

        if (!PrepareModule(module, parent_window_, library)) {
            fail(L"Winamp DSP module has an incomplete callback table");
            return;
        }
        if (!InvokeInitialize(module)) {
            fail(L"Winamp DSP Init raised an exception");
            return;
        }

        LoadedModule loaded;
        loaded.path = path;
        loaded.library = library;
        loaded.module = module;
        loaded.registry = std::move(registry);
        prepared_.push_back(std::move(loaded));
    }
};

WinampDspChain::WinampDspChain() : impl_(std::make_unique<Impl>()) {}
WinampDspChain::~WinampDspChain() = default;
WinampDspChain::WinampDspChain(WinampDspChain&&) noexcept = default;
WinampDspChain& WinampDspChain::operator=(WinampDspChain&&) noexcept = default;

void WinampDspChain::SetStorageDirectory(const std::filesystem::path& directory) {
    impl_->dispatcher.Invoke([&] {
        if (impl_->active_count != 0) throw std::logic_error("DSP storage must be set before loading plugins");
        impl_->storage_directory = directory;
    });
}

void WinampDspChain::Update(const std::filesystem::path& folder,
                            const std::vector<std::wstring>& modules,
                            HWND parent_window) {
    impl_->dispatcher.Invoke([&] { impl_->Update(folder, modules, parent_window); });
}

void WinampDspChain::RequestUpdate(const std::filesystem::path& folder,
                                  const std::vector<std::wstring>& modules,
                                  HWND parent_window) {
    impl_->dispatcher.Post([state = impl_.get(), folder, modules, parent_window] {
        state->Update(folder, modules, parent_window);
    });
}

bool WinampDspChain::Configure(const std::filesystem::path& module) {
    if (!impl_->IsActive(module)) return false;
    return impl_->dispatcher.Post([state = impl_.get(), module] { state->Configure(module); });
}

bool WinampDspChain::IsActive(const std::filesystem::path& module) const {
    return impl_->IsActive(module);
}

std::vector<HWND> WinampDspChain::Windows() const {
    return impl_->WindowSnapshot();
}

void WinampDspChain::Process(std::span<std::int16_t> interleaved_samples,
                             int channels, int sample_rate,
                             std::vector<std::wstring>* diagnostics) {
    if (impl_->active_count == 0) return;
    impl_->Process(interleaved_samples, channels, sample_rate, diagnostics);
}

std::size_t WinampDspChain::ActiveCount() const noexcept {
    return impl_->active_count;
}

std::vector<std::wstring> WinampDspChain::TakeDiagnostics() {
    return impl_->TakeDiagnostics();
}

} // namespace ttplayer::audio
