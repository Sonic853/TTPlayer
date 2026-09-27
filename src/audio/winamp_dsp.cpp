#include "ttplayer/audio/winamp_dsp.h"
#include "dsp_dispatcher.h"
#include "plugin_registry.h"
#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <thread>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace ttplayer::audio {
namespace {

constexpr int kMinimumHeaderVersion = 0x20;
constexpr int kMaximumModuleSlots = 10;
constexpr int kSplitFrameThreshold = 0x3fff;
constexpr wchar_t kDspOriginalProcedure[] = L"TTPlayer.DspOriginalProcedure";
constexpr wchar_t kDspHostWindow[] = L"TTPlayer.DspHostWindow";

LRESULT CALLBACK DspWindowProcedure(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    const auto previous = reinterpret_cast<WNDPROC>(GetPropW(window, kDspOriginalProcedure));
    const LRESULT result = previous
        ? CallWindowProcW(previous, window, message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_MOVING) {
        const HWND host = reinterpret_cast<HWND>(GetPropW(window, kDspHostWindow));
        if (IsWindow(host) && SendMessageW(host, kWinampDspMovingMessage,
                reinterpret_cast<WPARAM>(window), lparam)) return TRUE;
    } else if (message == WM_NCDESTROY) {
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
    int processing_priority = THREAD_PRIORITY_NORMAL;
    struct LoadedModule {
        std::filesystem::path path;
        HMODULE library{};
        WinampDspModule* module{};
        std::unique_ptr<detail::PluginRegistry> registry;

        LoadedModule() = default;
        LoadedModule(const LoadedModule&) = delete;
        LoadedModule& operator=(const LoadedModule&) = delete;
        LoadedModule(LoadedModule&& other) noexcept
            : path(std::move(other.path)), library(std::exchange(other.library, nullptr)),
              module(std::exchange(other.module, nullptr)), registry(std::move(other.registry)) {}
        LoadedModule& operator=(LoadedModule&& other) noexcept {
            if (this == &other) return *this;
            Close(nullptr);
            path = std::move(other.path);
            library = std::exchange(other.library, nullptr);
            module = std::exchange(other.module, nullptr);
            registry = std::move(other.registry);
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
                    try { diagnostics->push_back(L"Unable to save Ozone file configuration: " + path.wstring()); }
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
        if (callback_depth_ != 0) {
            pending_ = Pending{folder, configured, parent_window};
            return;
        }
        const bool same_parent = parent_window == parent_window_;
        const bool same_modules = requested.size() == requested_.size() &&
            std::equal(requested.begin(), requested.end(), requested_.begin(),
                       SamePath);
        if (same_parent && same_modules) return;

        // Move surviving wrappers, rather than initializing the entire chain.
        // This is the pointer-preserving 00428A7F ordering contract.
        std::vector<LoadedModule> surviving;
        surviving.reserve(requested.size());
        for (auto& loaded : modules_) {
            if (std::any_of(requested.begin(), requested.end(),
                    [&loaded](const auto& path) { return SamePath(loaded.path, path); }))
                surviving.push_back(std::move(loaded));
        }
        ++callback_depth_;
        CloseAll(false);
        requested_ = std::move(requested);
        parent_window_ = parent_window;
        for (const auto& path : requested_) {
            const auto found = std::find_if(surviving.begin(), surviving.end(),
                [&path](const auto& loaded) { return loaded.module && SamePath(loaded.path, path); });
            if (found != surviving.end()) {
                PrepareModule(found->module, parent_window_, found->library);
                modules_.push_back(std::move(*found));
            } else {
                Load(path);
            }
        }
        active_count = modules_.size();
        FinishCallback();
        Windows();
    }

    void Process(std::span<std::int16_t> samples, int channels,
                 int sample_rate) {
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
        ++callback_depth_;
        for (std::size_t index = 0; index < modules_.size();) {
            auto& loaded = modules_[index];
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
            if (good) {
                ++index;
                continue;
            }

            diagnostics_.push_back(
                L"Winamp DSP ModifySamples raised an exception; disabled: " +
                loaded.path.wstring());
            // A Config frame may still own the failed module. Delay Quit
            // until the outermost third-party callback has returned.
            retired_.push_back(std::move(loaded));
            modules_.erase(modules_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        active_count = modules_.size();
        FinishCallback();
    }

    void CloseAll(bool publish_count = true) noexcept {
        // FUN_00428B16 destroys the wrappers in ascending vector order.
        for (auto& current : modules_) current.Close(&diagnostics_);
        modules_.clear();
        if (publish_count) active_count = 0;
    }

    ~Impl() {
        dispatcher.Stop([this] {
            pending_.reset();
            CloseAll();
            for (auto& retired : retired_) retired.Close(&diagnostics_);
            retired_.clear();
            for (const HWND window : hooked_windows_) {
                if (!IsWindow(window)) continue;
                const auto previous = reinterpret_cast<WNDPROC>(GetPropW(window, kDspOriginalProcedure));
                if (previous && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == DspWindowProcedure) {
                    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
                    RemovePropW(window, kDspOriginalProcedure);
                    RemovePropW(window, kDspHostWindow);
                }
            }
        });
    }

    LoadedModule* Find(const std::filesystem::path& path) {
        const auto normalized = ResolveModulePath({}, path.wstring());
        const auto found = std::find_if(modules_.begin(), modules_.end(),
            [&normalized](const auto& loaded) { return SamePath(loaded.path, normalized); });
        return found == modules_.end() ? nullptr : &*found;
    }

    bool Configure(const std::filesystem::path& path) {
        const auto* loaded = Find(path);
        if (!loaded || !HasConfiguration(loaded->module)) return false;
        return dispatcher.Post([this, path] {
            auto* current = Find(path);
            if (!current || callback_depth_ != 0) return;
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
                diagnostics_.push_back(WindowsError(L"Unable to open DFX settings", GetLastError()));
                return;
            }
            // Keep only the ABI pointer across Config: nested audio calls may
            // retire its vector entry, but cannot unload it until we return.
            auto* module = current->module;
            ++callback_depth_;
            if (!InvokeConfigure(module))
                diagnostics_.push_back(L"Winamp DSP Config failed: " + path.wstring());
            FinishCallback();
            Windows();
        });
    }

    std::vector<HWND> Windows() {
        std::vector<HWND> result;
        if (modules_.empty()) return result;
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            DWORD process{};
            GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId()) return TRUE;
            wchar_t name[64]{};
            GetClassNameW(window, name, 64);
            if (_wcsicmp(name, L"DFX_WINDOW") == 0 || _wcsicmp(name, L"Dee2") == 0)
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
        return result;
    }

    std::vector<std::wstring> TakeDiagnostics() {
        return std::exchange(diagnostics_, {});
    }

    std::vector<std::filesystem::path> requested_;
    std::vector<LoadedModule> modules_;
    std::vector<std::wstring> diagnostics_;
    HWND parent_window_{};
    std::filesystem::path storage_directory;

private:
    struct Pending {
        std::filesystem::path folder;
        std::vector<std::wstring> configured;
        HWND parent{};
    };
    std::optional<Pending> pending_;
    std::vector<LoadedModule> retired_;
    std::vector<HWND> hooked_windows_;
    std::vector<std::int16_t> scratch_;
    unsigned callback_depth_{};

    void FinishCallback() {
        if (--callback_depth_ != 0) return;
        ++callback_depth_;
        for (auto& retired : retired_) retired.Close(&diagnostics_);
        retired_.clear();
        --callback_depth_;
        if (pending_) {
            auto pending = std::exchange(pending_, std::nullopt);
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
            diagnostics_.push_back(WindowsError(
                L"Unable to load Winamp DSP: " + path.wstring(), GetLastError()));
            return;
        }
        std::unique_ptr<detail::PluginRegistry> registry;
        try { registry = detail::PluginRegistry::Attach(library, path, storage_directory); }
        catch (const std::exception&) {
            diagnostics_.push_back(L"Ozone file configuration is invalid, busy, or read-only: " + path.wstring());
            FreeLibrary(library);
            return;
        }
        if (!registry && _wcsicmp(path.filename().c_str(), L"dsp_izOzone.dll") == 0)
            diagnostics_.push_back(L"This Ozone version has no file-registry adapter; native registry behavior is unchanged: " + path.wstring());
        const auto fail = [&](std::wstring reason) {
            diagnostics_.push_back(std::move(reason) + L": " + path.wstring());
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
        modules_.push_back(std::move(loaded));
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

bool WinampDspChain::Configure(const std::filesystem::path& module) {
    bool result{};
    impl_->dispatcher.Invoke([&] { result = impl_->Configure(module); });
    return result;
}

bool WinampDspChain::IsActive(const std::filesystem::path& module) const {
    bool result{};
    impl_->dispatcher.Invoke([&] { result = impl_->Find(module) != nullptr; });
    return result;
}

std::vector<HWND> WinampDspChain::Windows() const {
    std::vector<HWND> result;
    impl_->dispatcher.Invoke([&] { result = impl_->Windows(); });
    return result;
}

void WinampDspChain::Process(std::span<std::int16_t> interleaved_samples,
                             int channels, int sample_rate) {
    if (impl_->active_count == 0) return;
    const int priority = GetThreadPriority(GetCurrentThread());
    impl_->dispatcher.InvokeQueued([&] {
        if (priority != THREAD_PRIORITY_ERROR_RETURN &&
            priority != impl_->processing_priority &&
            SetThreadPriority(GetCurrentThread(), priority))
            impl_->processing_priority = priority;
        impl_->Process(interleaved_samples, channels, sample_rate);
    });
}

std::size_t WinampDspChain::ActiveCount() const noexcept {
    return impl_->active_count;
}

std::vector<std::wstring> WinampDspChain::TakeDiagnostics() {
    std::vector<std::wstring> result;
    impl_->dispatcher.Invoke([&] { result = impl_->TakeDiagnostics(); });
    return result;
}

} // namespace ttplayer::audio
