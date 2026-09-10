#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <windows.h>

namespace ttplayer::audio {

// In-process adapter for the Winamp DSP ABI recovered at
// FUN_00427EDC/FUN_00428061/FUN_004280CD.  Instances are deliberately owned
// by one playback worker: UI-side discovery/configuration remains isolated in
// a private worker mode of the player EXE, while ModifySamples runs in
// the process which owns the PCM stream.
class WinampDspChain {
public:
    WinampDspChain();
    ~WinampDspChain();
    WinampDspChain(const WinampDspChain&) = delete;
    WinampDspChain& operator=(const WinampDspChain&) = delete;
    WinampDspChain(WinampDspChain&&) noexcept;
    WinampDspChain& operator=(WinampDspChain&&) noexcept;

    // Relative module names are resolved against folder, matching the
    // /Plugin Folder + Modules_N representation in TTPlayer.xml.  Repeating
    // an identical configuration is a no-op; changing it performs Quit and
    // re-loads the ordered chain before the next decoded block.
    void Update(const std::filesystem::path& folder,
                const std::vector<std::wstring>& modules,
                HWND parent_window = nullptr);

    // Winamp's ABI is signed 16-bit interleaved PCM.  The original player
    // converts other decoded sample formats to 16-bit around this call.  The
    // return value from ModifySamples is intentionally ignored, as it is by
    // FUN_004280CD; TTPlayer treats the processor as an in-place transform.
    void Process(std::span<std::int16_t> interleaved_samples,
                 int channels, int sample_rate);

    [[nodiscard]] std::size_t ActiveCount() const noexcept;
    [[nodiscard]] std::vector<std::wstring> TakeDiagnostics();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttplayer::audio
