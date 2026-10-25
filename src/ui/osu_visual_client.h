#pragma once
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/ui/cover_image.h"
#include <memory>
#include <span>
#include <windows.h>
namespace ttplayer::settings { struct VisualSettings; }
namespace ttplayer::ui::detail {
// Bounded, Unicode-safe image import. Caller owns the resulting PBGRA bitmap.
HRESULT LoadOsuLogoImage(const std::filesystem::path&,CoverImage&) noexcept;
// Host adapter only: archive I/O, decoded images, audio snapshots and HWND.
// Scene/FFT/timing grammar/CPU and GPU renderers belong to ttp_osu.dll.
class OsuVisualClient {
public:
    OsuVisualClient();
    ~OsuVisualClient();
    OsuVisualClient(const OsuVisualClient&)=delete;
    OsuVisualClient& operator=(const OsuVisualClient&)=delete;
    void Configure(const settings::VisualSettings&);
    void FallbackImage(const std::filesystem::path&); // shared fullscreen backup image
    void Source(const std::filesystem::path&, std::span<const unsigned char>, HMODULE);
    void Viewport(HWND, const RECT&); // actual provider canvas, in client coordinates
    void Update(const audio::ExtendedVisualizationSamples&, const audio::PlaybackClockSnapshot&, HWND, float volume);
    void Paint(HDC, const RECT&, bool gpu);
    void Detach();
    bool BeginExit();
    bool ExitComplete();
    bool TransitionActive();
    void CancelTransition();
    // End a visual session on the UI/device thread. Caller serializes adapter
    // access; joins asset I/O before destroying CPU/GPU state and DLL references.
    void Release();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
