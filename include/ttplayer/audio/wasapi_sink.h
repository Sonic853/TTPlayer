#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ttplayer::audio {

struct WasapiEndpoint {
    std::wstring id;
    std::wstring name;
    std::wstring mix_format;
};

// COM must be initialized by the caller. XP returns an empty catalog without
// loading a newer DLL or adding a newer-system import to the executable.
[[nodiscard]] std::vector<WasapiEndpoint> EnumerateWasapiEndpoints();

// All COM interfaces and queued PCM belong to the audio worker. UI/fade
// threads communicate through AudioEngine state and never call these objects.
class WasapiSink {
public:
    WasapiSink();
    ~WasapiSink();
    WasapiSink(const WasapiSink&) = delete;
    WasapiSink& operator=(const WasapiSink&) = delete;

    bool OpenDevice(const std::wstring& endpoint_id, bool exclusive,
                    const WAVEFORMATEX& source_format, int buffer_ms);
    bool Submit(std::span<const std::byte> pcm);
    // Apply current gain only when copying into the short endpoint buffer;
    // decoded lookahead must not bake in a stale volume or fade value.
    bool Pump(float left_gain, float right_gain);
    bool SetPaused(bool paused);
    bool Reset();
    void Close() noexcept;
    [[nodiscard]] std::uint64_t PositionSourceBytes();
    [[nodiscard]] const WAVEFORMATEX& DeviceFormat() const noexcept;
    [[nodiscard]] const std::wstring& Error() const noexcept;
    [[nodiscard]] HRESULT ErrorResult() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttplayer::audio
