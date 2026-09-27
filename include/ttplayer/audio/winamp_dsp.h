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

inline constexpr UINT kWinampDspMovingMessage = WM_APP + 0x393;

// In-process adapter for the Winamp DSP ABI recovered at
// FUN_00427EDC/FUN_00428061/FUN_004280CD.  Instances are deliberately owned
// by the playback session family. Discovery is isolated; Init/Config/Modify/Quit
// share a persistent in-process thread with a window message loop. This keeps
// modeless plug-in windows alive and preserves one instance across track fades.
class WinampDspChain {
public:
    WinampDspChain();
    ~WinampDspChain();
    WinampDspChain(const WinampDspChain&) = delete;
    WinampDspChain& operator=(const WinampDspChain&) = delete;
    WinampDspChain(WinampDspChain&&) noexcept;
    WinampDspChain& operator=(WinampDspChain&&) noexcept;
    void SetStorageDirectory(const std::filesystem::path& directory);

    // Relative module names are resolved against folder, matching the
    // /Plugin Folder + Modules_N representation in TTPlayer.xml.  Repeating
    // an identical configuration is a no-op. Reordering preserves instances;
    // only added/removed paths receive Init/Quit. This also works while stopped.
    void Update(const std::filesystem::path& folder,
                const std::vector<std::wstring>& modules,
                HWND parent_window = nullptr);

    // Schedule Config on the active instance without blocking the player's UI
    // for a modal third-party dialog. False means no configurable active module.
    [[nodiscard]] bool Configure(const std::filesystem::path& module);
    [[nodiscard]] bool IsActive(const std::filesystem::path& module) const;
    [[nodiscard]] std::vector<HWND> Windows() const;

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
