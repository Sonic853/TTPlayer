#pragma once

#include <array>
#include <windows.h>

namespace ttplayer::audio {

#if defined(_MSC_VER) && defined(_M_IX86)
inline bool InitializeLegacyEqualizer(void* object, DWORD rate,
                                      DWORD channels) noexcept {
    __try {
        auto table = *static_cast<void***>(object);
        // 60006380 consumes two DWORD stack arguments, including channels.
        return reinterpret_cast<unsigned char (__thiscall*)(
            void*, DWORD, DWORD)>(table[1])(object, rate, channels) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool SetLegacyEqualizer(void* object,
                               const std::array<int, 11>& values) noexcept {
    // XML and Settings are preamp-first. Original 0042906A rearranges that
    // text into ten bands followed by preamp; ordinal 103 / 60007710 adds
    // params[10] to each params[0..9]. Convert at the DLL boundary so playback
    // and offline conversion agree without migrating settings or presets.
    std::array<int, 11> native{};
    for (size_t band = 0; band < 10; ++band) native[band] = values[band + 1];
    native[10] = values[0];
    __try {
        auto table = *static_cast<void***>(object);
        reinterpret_cast<void (__thiscall*)(void*, const int*)>(
            table[2])(object, native.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#endif

} // namespace ttplayer::audio
