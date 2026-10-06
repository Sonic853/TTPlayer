#pragma once
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/disc_media.h"
namespace ttplayer::audio {
// Device operations are separate from the reader's buffering/decoder state.
// This also permits deterministic virtual-drive regression tests without a CD.
class CdAudioDevice {
public:
    virtual ~CdAudioDevice() = default;
    virtual DiscLayout Open(const std::filesystem::path&) = 0;
    // Returns a Win32 error code; bytes is meaningful only on ERROR_SUCCESS.
    virtual DWORD Read(unsigned lba, unsigned sectors, std::span<std::byte> output,
                       DWORD& bytes) = 0;
};
std::unique_ptr<DecodedAudioSource> CreateCdaSource(const plugins::PluginManager*, HMODULE,
    std::unique_ptr<CdAudioDevice> device = {});
}
