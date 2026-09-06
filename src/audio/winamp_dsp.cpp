#include "ttplayer/audio/winamp_dsp.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace ttplayer::audio {
namespace {

constexpr int kMinimumHeaderVersion = 0x20;
constexpr int kMaximumModuleSlots = 10;
constexpr int kSplitFrameThreshold = 0x3fff;

// This is the original Winamp SDK layout used by TTPlayer 5.7.9.  It does not
// contain the later, frequently misremembered userData field.  The x86
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
    return error ? path : absolute;
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

} // namespace

class WinampDspChain::Impl {
public:
    struct LoadedModule {
        std::filesystem::path path;
        HMODULE library{};
        WinampDspModule* module{};

        LoadedModule() = default;
        LoadedModule(const LoadedModule&) = delete;
        LoadedModule& operator=(const LoadedModule&) = delete;
        LoadedModule(LoadedModule&& other) noexcept
            : path(std::move(other.path)), library(std::exchange(other.library, nullptr)),
              module(std::exchange(other.module, nullptr)) {}
        LoadedModule& operator=(LoadedModule&& other) noexcept {
            if (this == &other) return *this;
            Close(nullptr);
            path = std::move(other.path);
            library = std::exchange(other.library, nullptr);
            module = std::exchange(other.module, nullptr);
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
            if (old_library) FreeLibrary(old_library);
        }
    };

    void Update(const std::filesystem::path& folder,
                const std::vector<std::wstring>& configured,
                HWND parent_window) {
        std::vector<std::filesystem::path> requested;
        requested.reserve(configured.size());
        for (const auto& module : configured) {
            if (!module.empty())
                requested.push_back(ResolveModulePath(folder, module));
        }
        const bool same_parent = parent_window == parent_window_;
        const bool same_modules = requested.size() == requested_.size() &&
            std::equal(requested.begin(), requested.end(), requested_.begin(),
                       SamePath);
        if (same_parent && same_modules) return;

        CloseAll();
        requested_ = std::move(requested);
        parent_window_ = parent_window;
        for (const auto& path : requested_) Load(path);
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

        for (std::size_t index = 0; index < modules_.size();) {
            auto& loaded = modules_[index];
            const int frames = static_cast<int>(frame_count);
            bool good = true;
            // This apparently unusual one-time half split is literal
            // FUN_004280CD behaviour, rather than a loop with 0x3fff chunks.
            if (frames > kSplitFrameThreshold) {
                const int first = frames / 2;
                good = InvokeModify(loaded.module, samples.data(), first,
                                    channels, sample_rate);
                if (good) {
                    good = InvokeModify(
                        loaded.module,
                        reinterpret_cast<short*>(samples.data()) + first * channels,
                        frames - first, channels, sample_rate);
                }
            } else {
                good = InvokeModify(loaded.module, samples.data(), frames,
                                    channels, sample_rate);
            }
            if (good) {
                ++index;
                continue;
            }

            diagnostics_.push_back(
                L"Winamp DSP ModifySamples raised an exception; disabled: " +
                loaded.path.wstring());
            loaded.Close(&diagnostics_);
            modules_.erase(modules_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    void CloseAll() noexcept {
        // FUN_00428B16 destroys the wrappers in ascending vector order.
        for (auto& current : modules_) current.Close(&diagnostics_);
        modules_.clear();
    }

    ~Impl() { CloseAll(); }

    std::vector<std::wstring> TakeDiagnostics() {
        return std::exchange(diagnostics_, {});
    }

    std::vector<std::filesystem::path> requested_;
    std::vector<LoadedModule> modules_;
    std::vector<std::wstring> diagnostics_;
    HWND parent_window_{};

private:
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
        modules_.push_back(std::move(loaded));
    }
};

WinampDspChain::WinampDspChain() : impl_(std::make_unique<Impl>()) {}
WinampDspChain::~WinampDspChain() = default;
WinampDspChain::WinampDspChain(WinampDspChain&&) noexcept = default;
WinampDspChain& WinampDspChain::operator=(WinampDspChain&&) noexcept = default;

void WinampDspChain::Update(const std::filesystem::path& folder,
                            const std::vector<std::wstring>& modules,
                            HWND parent_window) {
    impl_->Update(folder, modules, parent_window);
}

void WinampDspChain::Process(std::span<std::int16_t> interleaved_samples,
                             int channels, int sample_rate) {
    impl_->Process(interleaved_samples, channels, sample_rate);
}

std::size_t WinampDspChain::ActiveCount() const noexcept {
    return impl_->modules_.size();
}

std::vector<std::wstring> WinampDspChain::TakeDiagnostics() {
    return impl_->TakeDiagnostics();
}

} // namespace ttplayer::audio
