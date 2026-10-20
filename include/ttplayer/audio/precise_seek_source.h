#pragma once

#include "ttplayer/audio/audio_engine.h"

namespace ttplayer::audio {

// Apply CSoundFileReader's 004B16FE / 004B1375 decoded-prefix discard to
// ordinary sources. CUE already performs its own exact segment positioning.
[[nodiscard]] std::unique_ptr<DecodedAudioSource> MakePreciseSeekSource(
    std::unique_ptr<DecodedAudioSource> source);

} // namespace ttplayer::audio
