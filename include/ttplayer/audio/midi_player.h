#pragma once

#include <chrono>
#include <filesystem>
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

namespace ttplayer::testing { struct MidiPlaybackAccess; }

namespace ttplayer::audio {

[[nodiscard]] bool IsMidiPath(const std::filesystem::path& path);
// 004E9CA5 describes the reader facade, not captured synthesizer PCM.
[[nodiscard]] WAVEFORMATEX MidiReaderFormat() noexcept;
[[nodiscard]] long MidiVolumeLevel(float volume) noexcept;
[[nodiscard]] long MidiBalanceLevel(int balance) noexcept;

// Recovered 004E28C9 / 004E2A40. The caller initializes COM and keeps every
// call, including destruction, on that same thread. No PCM/DSP output exists.
class MidiPlayer {
public:
    MidiPlayer() = default;
    MidiPlayer(const MidiPlayer&) = delete;
    MidiPlayer& operator=(const MidiPlayer&) = delete;
    ~MidiPlayer();
    HRESULT Open(const std::filesystem::path& path);
    HRESULT Run();
    HRESULT Pause();
    HRESULT Stop();
    HRESULT Seek(std::chrono::milliseconds position);
    HRESULT Position(std::chrono::milliseconds& position) const;
    HRESULT SetVolume(float volume);
    HRESULT SetBalance(int balance);
    // Drain/free graph events; surface device failures to the engine. The
    // original 50 ms position/duration check remains the normal EOF path.
    HRESULT PollEvents(bool& complete);
    [[nodiscard]] std::chrono::milliseconds Duration() const { return duration_; }

private:
    friend struct ttplayer::testing::MidiPlaybackAccess;
    Microsoft::WRL::ComPtr<IGraphBuilder> graph_;
    Microsoft::WRL::ComPtr<IMediaControl> control_;
    Microsoft::WRL::ComPtr<IMediaSeeking> seeking_;
    Microsoft::WRL::ComPtr<IBasicAudio> audio_;
    Microsoft::WRL::ComPtr<IMediaEvent> events_;
    std::chrono::milliseconds duration_{};
};

} // namespace ttplayer::audio
