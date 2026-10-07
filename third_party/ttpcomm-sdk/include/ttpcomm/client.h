#pragma once
#include <windows.h>
#include <array>
#include <cmath>
#include <utility>
#include <ttpcomm/extensions.h>

namespace ttpcomm::client {
inline bool HasCapability(HMODULE module, uint32_t flag) noexcept {
    const auto query = module ? reinterpret_cast<TtpCommQueryExtensionFn>(
        GetProcAddress(module, "ttpcomm_query_extension")) : nullptr;
    if (!query) return false;
    __try {
        TtpCommExtensionInfo info{sizeof(info), 0, 0, 0};
        return query(TTPCOMM_EXTENSION_ABI, &info) && info.size == sizeof(info) &&
            info.abi_version == TTPCOMM_EXTENSION_ABI && info.legacy_version == 0x50700 &&
            (info.capabilities & flag) == flag;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class ModuleReference {
public:
    explicit ModuleReference(HMODULE borrowed = nullptr) noexcept {
        if (borrowed) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(borrowed), &module_);
    }
    ~ModuleReference() { if (module_) FreeLibrary(module_); }
    ModuleReference(const ModuleReference&) = delete;
    ModuleReference& operator=(const ModuleReference&) = delete;
    ModuleReference(ModuleReference&& other) noexcept : module_(std::exchange(other.module_, nullptr)) {}
    ModuleReference& operator=(ModuleReference&& other) noexcept {
        if (this != &other) {
            if (module_) FreeLibrary(module_);
            module_ = std::exchange(other.module_, nullptr);
        }
        return *this;
    }
    HMODULE get() const noexcept { return module_; }
private:
    HMODULE module_{};
};

inline void* Create(HMODULE module, WORD ordinal) noexcept {
    const auto entry = module ? GetProcAddress(module, MAKEINTRESOURCEA(ordinal)) : nullptr;
    __try { return entry ? reinterpret_cast<void* (__cdecl*)()>(entry)() : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
inline void* CreateResampler(HMODULE module, int mode) noexcept {
    const auto entry = module ? GetProcAddress(module, MAKEINTRESOURCEA(102)) : nullptr;
    __try { return entry ? reinterpret_cast<void* (__cdecl*)(int)>(entry)(mode) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
inline void Destroy(void* object) noexcept {
    __try {
        if (object) reinterpret_cast<void (__thiscall*)(void*, int)>(
            (*static_cast<void***>(object))[0])(object, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
inline bool InitializeEqualizer(void* object, DWORD rate, DWORD channels) noexcept {
    __try { return object && reinterpret_cast<bool (__thiscall*)(void*, DWORD, DWORD)>(
        (*static_cast<void***>(object))[1])(object, rate, channels); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool SetEqualizer(void* object, const std::array<int, 11>& preampFirst) noexcept {
    int native[11];
    for (unsigned i = 0; i != 10; ++i) native[i] = preampFirst[i + 1];
    native[10] = preampFirst[0];
    __try {
        if (!object) return false;
        reinterpret_cast<void (__thiscall*)(void*, const int*)>(
            (*static_cast<void***>(object))[2])(object, native);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Equalize(void* object, double* samples, int* count) noexcept {
    __try {
        if (!object || !count || *count < 0 || (!samples && *count)) return false;
        if (!*count) return true;
        auto table = *static_cast<void***>(object);
        const int capacity = *count;
        if (!reinterpret_cast<bool (__thiscall*)(void*, const double*, int)>(table[4])(
                object, samples, capacity)) return false;
        return reinterpret_cast<bool (__thiscall*)(void*, double*, int*)>(table[5])(
            object, samples, count) && *count >= 0 && *count <= capacity;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void Reset(void* object, unsigned slot) noexcept {
    __try { if (object) reinterpret_cast<void (__thiscall*)(void*)>(
        (*static_cast<void***>(object))[slot])(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
inline bool InitializeSurround(void* object, DWORD rate, DWORD channels, int amount) noexcept {
    __try {
        if (!object) return false;
        reinterpret_cast<void (__thiscall*)(void*, DWORD, DWORD, int)>(
            (*static_cast<void***>(object))[1])(object, rate, channels, amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Surround(void* object, double* samples, int count) noexcept {
    __try {
        if (!object || count < 0 || (!samples && count)) return false;
        reinterpret_cast<void (__thiscall*)(void*, double*, int)>(
            (*static_cast<void***>(object))[2])(object, samples, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool InitializeResampler(void* object, DWORD input, DWORD output,
                                DWORD channels, bool quality) noexcept {
    __try { return object && reinterpret_cast<bool (__thiscall*)(void*, DWORD, DWORD, DWORD, bool)>(
        (*static_cast<void***>(object))[1])(object, input, output, channels, quality); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline int Resample(void* object, const double* input, int count, double* output, int capacity) noexcept {
    __try { return object ? reinterpret_cast<int (__thiscall*)(void*, const double*, int, double*, int)>(
        (*static_cast<void***>(object))[2])(object, input, count, output, capacity) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
inline bool Spectrum(void* object, int16_t* work, int channels) noexcept {
    __try {
        if (!object || !work || (channels != 1 && channels != 2)) return false;
        reinterpret_cast<void (__thiscall*)(void*, int16_t*, int)>(
            (*static_cast<void***>(object))[1])(object, work, channels);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool ReplayGainSupportsRate(HMODULE module, DWORD rate) noexcept {
    const auto entry = module ? GetProcAddress(module, MAKEINTRESOURCEA(100)) : nullptr;
    __try { return entry && reinterpret_cast<BOOL (__cdecl*)(DWORD)>(entry)(rate); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool InitializeReplayGain(void* object, DWORD rate) noexcept {
    __try { return object && reinterpret_cast<bool (__thiscall*)(void*, DWORD)>(
        (*static_cast<void***>(object))[1])(object, rate); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool AnalyzeReplayGain(void* object, const double* samples, DWORD channels, DWORD frames) noexcept {
    __try { return object && samples && channels && reinterpret_cast<bool (__thiscall*)(void*, const double*, DWORD, DWORD)>(
        (*static_cast<void***>(object))[2])(object, samples, channels, frames); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool FinishReplayGain(void* object, double* gain, double* peak) noexcept {
    __try {
        if (!object || !gain || !peak) return false;
        const auto table = *static_cast<void***>(object);
        *gain = reinterpret_cast<double (__thiscall*)(void*)>(table[3])(object);
        *peak = reinterpret_cast<double (__thiscall*)(void*)>(table[4])(object);
        return std::isfinite(*gain) && std::isfinite(*peak);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
