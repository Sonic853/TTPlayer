#pragma once
#include "ttplayer/audio/disc_media.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/settings/settings.h"
#include <functional>
namespace ttplayer::audio {
struct DiscQuerySource {
    std::filesystem::path path;
    DiscLayout layout;
    std::optional<CueSheet> cue;
};
struct DiscCandidate {
    std::string release_id;
    std::wstring album, artist, date, country, label;
    unsigned medium{}, total_discs{}, tracks{};
    bool exact{};
};
struct DiscRelease { DiscCandidate candidate; DiscMetadata tracks; };
using DiscCancel = std::function<bool()>;
DiscQuerySource PrepareDiscQuery(const std::filesystem::path&, const plugins::PluginManager*, HMODULE,
                                const PlaybackOptions&, const DiscCancel&);
std::vector<DiscCandidate> ParseDiscCandidates(std::string_view json, const DiscLayout&);
DiscRelease ParseDiscRelease(std::string_view json, const DiscCandidate&, const DiscLayout&);
std::vector<DiscCandidate> QueryMusicBrainz(const DiscLayout&, const settings::NetworkSettings&, const DiscCancel&);
DiscRelease ReadMusicBrainzRelease(const DiscCandidate&, const DiscLayout&, const settings::NetworkSettings&, const DiscCancel&);
void SaveDiscRelease(const DiscQuerySource&, const DiscRelease&);
}
