#pragma once

#include "ttplayer/audio/audio_engine.h"

namespace ttplayer::audio {
// Windows Media Format synchronous reader, used when MF is absent (XP).
std::unique_ptr<DecodedAudioSource> CreateLegacyWindowsSource(HMODULE ttpcomm);
}
