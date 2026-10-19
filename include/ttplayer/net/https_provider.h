#pragma once
#include "ttplayer/net/ttp_https.h"
#include <filesystem>
#include <windows.h>

namespace ttplayer::net {

// The module must outlive every API response/stream it allocates. Consumers
// retain their own ABI requirements, redirects, cache and integrity policies.
class HttpsProvider {
public:
    ~HttpsProvider() { if (module_) FreeLibrary(module_); }
    HttpsProvider() = default;
    HttpsProvider(const HttpsProvider&) = delete;
    HttpsProvider& operator=(const HttpsProvider&) = delete;
    bool Load(std::filesystem::path runtime = {}) {
        if (module_) return true;
        if (runtime.empty()) {
            wchar_t executable[32768]{};
            const auto size = GetModuleFileNameW(nullptr, executable, 32768);
            if (!size || size >= 32768) { error_ = ERROR_BAD_PATHNAME; return false; }
            runtime = std::filesystem::path(executable).parent_path();
        }
        const auto path = std::filesystem::absolute(runtime / L"AddIn" / L"ttp_https.dll");
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module_) { error_ = GetLastError(); return false; }
        get_ = reinterpret_cast<ttp_https_get_api_fn>(GetProcAddress(module_, "ttp_https_get_api"));
        error_ = get_ ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND;
        return true;
    }
    const ttp_https_api* Api(unsigned version) const {
        return get_ ? get_(version) : nullptr;
    }
    DWORD Error() const noexcept { return error_; }
private:
    HMODULE module_{};
    ttp_https_get_api_fn get_{};
    DWORD error_{ERROR_MOD_NOT_FOUND};
};

} // namespace ttplayer::net
