#pragma once
#include <ttpcomm/client.h>
namespace ttplayer::audio {
inline bool InitializeLegacyEqualizer(void* object, DWORD rate, DWORD channels) noexcept {
    return ttpcomm::client::InitializeEqualizer(object, rate, channels);
}
inline bool SetLegacyEqualizer(void* object, const std::array<int, 11>& values) noexcept {
    return ttpcomm::client::SetEqualizer(object, values);
}
}
