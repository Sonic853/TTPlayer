#pragma once

#include "ttplayer/audio/audio_engine.h"

namespace ttplayer::audio {

// FUN_004E6FB6 extends the ordinary MPEG reader with the optional Winamp
// input decoder. It is not a Sound AddIn or an output/DSP plug-in.
// The fallback must successfully open before enhancement is attempted.
[[nodiscard]] std::unique_ptr<DecodedAudioSource> WrapMp3ProSource(
    std::unique_ptr<DecodedAudioSource> fallback);

} // namespace ttplayer::audio
