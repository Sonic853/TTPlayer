#pragma once

#include "ttplayer/playlist/playlist.h"
#include "ttplayer/settings/settings.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <windows.h>

namespace ttplayer::plugins { class PluginManager; }

namespace ttplayer::ui {

[[nodiscard]] bool ReplayGainScanCommandAvailable(
    const plugins::PluginManager* library, HMODULE ttpcomm) noexcept;

// FUN_004CDB93 records the executable IEncoderCreator registry.  The command
// is available when at least one executable creator was registered.  Creator
// slot 6 controls the resource-220 Configure button; it does not gate Create.
[[nodiscard]] bool PlaylistConvertCommandAvailable(
    const plugins::PluginManager* library) noexcept;

struct PlaylistConversionResult {
    HRESULT result{E_FAIL};
    std::uint64_t pcm_bytes{};
    std::wstring diagnostic;
};

// The non-UI transaction at the heart of CConvertDlg::CWorkThread::Run
// (00412723): reader PCM -> encoder Open/Start/Write/Finalize.  The resource
// 221 dialog remains responsible for choosing the creator and destination.
[[nodiscard]] PlaylistConversionResult ConvertPlaylistTrack(
    plugins::PluginManager& library, const playlist::Track& track,
    size_t encoder_index, const std::filesystem::path& destination,
    const settings::ConvertSettings& settings, HMODULE ttpcomm,
    const settings::EqualizerSettings* equalizer = nullptr,
    std::stop_token stop = {});

// Resource-220 configuration followed by the executable resource-221 batch
// transaction used by command 0x7EF7. Targets use the selected folder (or
// each source folder when blank), the recovered %03d.%s numbering rule, and
// one status row per item while the worker converts sequentially.
[[nodiscard]] std::optional<std::vector<std::filesystem::path>>
ShowPlaylistConverter(
    HWND owner, HMODULE resources, HMODULE ttpcomm,
    const plugins::PluginManager* library,
    std::vector<playlist::Track> tracks,
    settings::ConvertSettings& settings,
    const settings::EqualizerSettings& equalizer);

// Implements the process-wide modeless CScanGainDlg singleton established by
// FUN_004A5584/FUN_004A55AF. If it already exists the current work is brought
// to the foreground and the new item vector is intentionally not appended.
bool ShowPlaylistReplayGainScanner(
    HWND owner, HMODULE resources, HMODULE ttpcomm,
    const plugins::PluginManager* library,
    std::vector<playlist::Track> tracks, bool skip_existing);

} // namespace ttplayer::ui
