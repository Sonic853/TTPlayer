#pragma once
#include <windows.h>
#include <cstring>

// ABI 1 adds data only. Legacy resource IDs, languages and payloads are immutable.
namespace ttpres {
inline constexpr char kSignature[8] = {'T','T','P','R','E','S','1','\0'};
inline constexpr UINT kRequiredDialogs[] = {4090,4200,4210,4220,4230,5800,5810,5820,5901};
inline bool IsRequiredString(UINT id) noexcept {
    return id == 4091 || (id >= 4093 && id <= 4106) || (id >= 4110 && id <= 4158);
}
inline bool IsExtension(LPCWSTR type, UINT id) noexcept {
    if (type == RT_STRING) return id >= 4091 && id <= 4158;
    if (type == RT_DIALOG)
        for (const auto dialog : kRequiredDialogs) if (id == dialog) return true;
    return false;
}
inline bool HasExtensions(HMODULE module) noexcept {
    if (!module) return false;
    const auto resource = FindResourceW(module, L"TTPLAYER_RES_ABI", RT_RCDATA);
    if (!resource || SizeofResource(module, resource) != 16) return false;
    const auto bytes = static_cast<const unsigned char*>(LockResource(LoadResource(module, resource)));
    if (!bytes || std::memcmp(bytes, kSignature, 8) != 0) return false;
    DWORD abi{}, features{};
    std::memcpy(&abi, bytes + 8, 4);
    std::memcpy(&features, bytes + 12, 4);
    return abi == 1 && (features & 1) != 0;
}
inline bool HasRequiredExtensions(HMODULE module) noexcept {
    if (!HasExtensions(module)) return false;
    for (const auto id : kRequiredDialogs) {
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_DIALOG);
        if (!resource || !SizeofResource(module, resource) ||
            !LockResource(LoadResource(module, resource))) return false;
    }
    for (UINT id = 4091; id <= 4158; ++id) {
        if (!IsRequiredString(id)) continue;
        const wchar_t* text{};
        if (LoadStringW(module, id, reinterpret_cast<LPWSTR>(&text), 0) <= 0) return false;
    }
    return true;
}
inline HMODULE Resolve(HMODULE logical_module, LPCWSTR type, UINT id) noexcept {
    if (logical_module != GetModuleHandleW(nullptr)) return logical_module;
    // Keep original DLLs and third-party modules completely outside this route.
    if (!IsExtension(type, id)) return logical_module;
    const auto module = GetModuleHandleW(L"ttpres.dll");
    const auto name = MAKEINTRESOURCEW(type == RT_STRING ? id / 16 + 1 : id);
    // Extensions exist only in the required DLL. Callers must reject nullptr;
    // passing it to a Win32 resource API would implicitly select the EXE.
    return HasExtensions(module) && FindResourceW(module, name, type) ? module : nullptr;
}
}
