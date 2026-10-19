#pragma once

#include "ttplayer/audio/audio_engine.h"

struct IUnknown;
struct IGraphBuilder;
namespace ttplayer::audio {
namespace detail {
// Separate COM boundaries for isolated provider/abort regression tests.
HRESULT ConfigureLegacyNetworkSource(IUnknown* source, const PlaybackOptions& options);
HRESULT RunLegacyNetworkOperation(IGraphBuilder* graph, IUnknown* source,
    const PlaybackOptions& options, const std::function<HRESULT()>& operation);
}

// 004E86B3 / 004E8BD6: generic system reader, with PCM delivered to the
// player's existing DSP/output chain. No system audio renderer is installed.
[[nodiscard]] std::unique_ptr<DecodedAudioSource> CreateDirectShowSource();
} // namespace ttplayer::audio
