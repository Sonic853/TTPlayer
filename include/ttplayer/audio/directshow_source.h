#pragma once

#include "ttplayer/audio/audio_engine.h"

namespace ttplayer::audio {
// 004E86B3 / 004E8BD6: generic system reader, with PCM delivered to the
// player's existing DSP/output chain. No system audio renderer is installed.
[[nodiscard]] std::unique_ptr<DecodedAudioSource> CreateDirectShowSource();
} // namespace ttplayer::audio
