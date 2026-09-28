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

// One audio producer calls this facade. A separate render worker owns every
// COM interface and drains the bounded PCM queue even while decoding or a
// legacy DSP callback blocks the producer. Close joins that worker before
// returning, including on failed opens and exclusive-mode track changes.
class WasapiSink {
public:
    WasapiSink();
    ~WasapiSink();
    WasapiSink(const WasapiSink&) = delete;
    WasapiSink& operator=(const WasapiSink&) = delete;

    bool OpenDevice(const std::wstring& endpoint_id, bool exclusive,
                    const WAVEFORMATEX& source_format, int buffer_ms);
    bool Submit(std::span<const std::byte> pcm);
    // Update gain and service the endpoint immediately. The render worker
    // also services it independently between calls. Gain is applied only at
    // that short endpoint buffer, never baked into decoded lookahead.
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
