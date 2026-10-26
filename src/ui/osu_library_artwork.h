#pragma once

#include "track_metadata.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/core/text.h"

namespace ttplayer::ui::detail {

// Resolved by the library DLL, not by guessing adjacent files in the hash store.
inline std::filesystem::path OsuLibraryBackgroundPath(const playlist::Track& track) {
    if (IsOsuLibraryTrack(track))
        for (const auto& [name, value] : track.metadata)
            if (name == "TTPlayer.Osu.Background") return core::Utf8ToWide(value);
    return {};
}

inline std::filesystem::path OsuLibraryBackgroundPath(const audio::AudioMetadata& metadata) {
    const bool game_source = std::any_of(metadata.entries.begin(), metadata.entries.end(),
        [](const auto& field) { return field.first == L"TTPlayer.Osu.Source" &&
            (field.second == L"1" || field.second == L"2"); });
    if (game_source)
        for (const auto& [name, value] : metadata.entries)
            if (name == L"TTPlayer.Osu.Background") return value;
    return {};
}

// Only a UI snapshot is enriched. Never publish these fields into decoder tags
// or write the fallback image into the audio/game files.
inline void MergeOsuLibraryArtwork(const playlist::Track& track, audio::AudioMetadata& metadata) {
    std::erase_if(metadata.entries, [](const auto& field) {
        return field.first.starts_with(L"TTPlayer.Osu.");
    });
    if (IsOsuLibraryTrack(track))
        for (const auto& [name, value] : track.metadata)
            if (name.starts_with("TTPlayer.Osu."))
                metadata.entries.emplace_back(core::Utf8ToWide(name), core::Utf8ToWide(value));
}

} // namespace ttplayer::ui::detail
