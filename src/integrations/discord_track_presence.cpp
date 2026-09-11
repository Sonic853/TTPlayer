#include "ttplayer/integrations/discord_presence.h"

#include "ttplayer/core/text.h"
#include "ttplayer/playlist/playlist.h"
#include "ttplayer/lyrics/lrc_parser.h"

#include <algorithm>
#include <cwctype>
#include <initializer_list>

namespace ttplayer::integrations {
namespace {

std::wstring MusicText(std::string_view text) {
    std::wstring result;
    try { result = core::Utf8ToWide(text); }
    catch (const std::exception&) { return {}; }
    // Metadata is displayed as a single line, including ICY/program titles.
    for (auto& ch : result) if (ch < L' ') ch = L' ';
    const auto first = result.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return result.substr(first, result.find_last_not_of(L" \t\r\n") - first + 1);
}

std::string MetadataKey(std::string_view text) {
    std::string result;
    for (const unsigned char ch : text) {
        if (ch >= 'A' && ch <= 'Z') result += static_cast<char>(ch - 'A' + 'a');
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            result += static_cast<char>(ch);
    }
    return result;
}

std::wstring MetadataText(const playlist::Track& track,
                          std::initializer_list<std::string_view> keys) {
    // Alias priority is intentional; generic "title" must not mask StreamTitle.
    for (const auto key : keys) {
        for (const auto& [name, value] : track.metadata) {
            if (MetadataKey(name) == key) {
                auto text = MusicText(value);
                if (!text.empty()) return text;
            }
        }
    }
    return {};
}

bool IsLocation(std::wstring_view text) {
    // Do not publish a stream URL (possibly containing credentials/query
    // tokens) when a decoder/playlist uses it as the fallback title.
    return text.find(L"://") != std::wstring_view::npos ||
           text.starts_with(L"\\\\") ||
           (text.size() > 2 && text[1] == L':' &&
            (text[2] == L'\\' || text[2] == L'/'));
}

} // namespace

DiscordTrackPresence BuildDiscordTrackPresence(
    const playlist::Track& track, std::chrono::milliseconds position,
    std::chrono::milliseconds duration, DiscordPlaybackState playback,
    bool network_source) {
    DiscordTrackPresence result;
    result.position = std::max(position, std::chrono::milliseconds::zero());
    result.duration = std::max(duration, std::chrono::milliseconds::zero());
    result.playback = playback;
    result.track_identity = track.path.native() + L"|" + std::to_wstring(track.subtrack);
    result.title = MusicText(track.title);
    result.artist = MusicText(track.artist);
    result.album = MusicText(track.album);
    if (result.title.empty()) result.title = MetadataText(track, {"title"});
    if (result.artist.empty()) result.artist = MetadataText(track, {"artist", "author"});
    if (result.album.empty()) result.album = MetadataText(track, {"album", "wmalbumtitle"});

    if (network_source) {
        result.audio_kind = DiscordAudioKind::network_audio;
        result.station = MetadataText(track,
            {"icyname", "stationname", "radiostationname", "wmradiostationname", "station"});
        if (IsLocation(result.station)) result.station.clear();
        if (!result.station.empty()) result.audio_kind = DiscordAudioKind::radio;
        if (auto title = MetadataText(track, {"streamtitle", "icytitle", "nowplaying"});
            !title.empty()) result.title = std::move(title);
        if (auto artist = MetadataText(track, {"streamartist"}); !artist.empty())
            result.artist = std::move(artist);
        if (auto album = MetadataText(track, {"streamalbum"}); !album.empty())
            result.album = std::move(album);
        if (IsLocation(result.title)) result.title.clear();
        if (result.title.empty()) result.title = result.station.empty()
            ? L"网络音频" : result.station;
    } else if (result.title.empty()) {
        try { result.title = track.path.stem().wstring(); }
        catch (const std::exception&) {}
    }
    if (result.title.empty()) result.title = L"未知曲目";
    return result;
}

void ApplyDiscordLyric(DiscordTrackPresence& presence, const lyrics::Lyrics& lyrics) {
    presence.lyric.clear();
    presence.lyric_start = {};
    presence.lyric_end.reset();
    if (presence.playback == DiscordPlaybackState::stopped) return;
    const auto index = lyrics.LineAt(presence.position);
    if (!index || *index >= lyrics.lines.size()) return;
    const auto& line = lyrics.lines[*index];
    presence.lyric = MusicText(line.text);
    presence.lyric_start = line.time + lyrics.offset;
    if (*index + 1 < lyrics.lines.size())
        presence.lyric_end = lyrics.lines[*index + 1].time + lyrics.offset;
    else if (presence.duration.count() > 0) presence.lyric_end = presence.duration;
}

} // namespace ttplayer::integrations
