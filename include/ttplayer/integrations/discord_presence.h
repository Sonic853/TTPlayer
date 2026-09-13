#pragma once

#include "ttplayer/integrations/discord_presence_config.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::playlist { struct Track; }
namespace ttplayer::lyrics { struct Lyrics; }
namespace ttplayer::testing { struct DiscordPresenceAccess; }

namespace ttplayer::integrations {

enum class DiscordPlaybackState {
    stopped,
    playing,
    paused,
};

// A URL alone does not establish that the source is a live radio station.
enum class DiscordAudioKind { track, network_audio, radio };

struct DiscordTrackPresence {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::chrono::milliseconds position{};
    std::chrono::milliseconds duration{};
    DiscordPlaybackState playback{DiscordPlaybackState::stopped};
    DiscordAudioKind audio_kind{DiscordAudioKind::track};
    std::wstring station;
    // Local-only identity/clock fields; no path or revision is serialized.
    std::wstring track_identity;
    std::uint64_t timeline_revision{};
    bool seek_pending{};
    // Local sharing policy, not an RPC field. A user toggle is urgent, unlike
    // an ordinary timed lyric-line change which is coalesced by the worker.
    bool lyrics_enabled{true};
    std::wstring lyric;
    std::chrono::milliseconds lyric_start{};
    std::optional<std::chrono::milliseconds> lyric_end;

    friend bool operator==(const DiscordTrackPresence&,
                           const DiscordTrackPresence&) = default;
};

// Presentation-only adapter: consumes metadata already available to the player;
// never opens the media, uploads artwork or performs an online metadata lookup.
[[nodiscard]] DiscordTrackPresence BuildDiscordTrackPresence(
    const playlist::Track& track, std::chrono::milliseconds position,
    std::chrono::milliseconds duration, DiscordPlaybackState playback,
    bool network_source);
// Uses the same parsed timestamps/offset as the player's lyric window.
void ApplyDiscordLyric(DiscordTrackPresence& presence, const lyrics::Lyrics& lyrics);

// A non-blocking client for Discord's documented local RPC transport.  Calls
// from the player window only update the desired state; pipe discovery,
// handshake and SET_ACTIVITY run on a private worker thread.
class DiscordPresence final {
public:
    DiscordPresence();
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // application_id comes from TTPlayerRebuild.xml; an empty value falls back to
    // kDefaultDiscordApplicationId. Discord only needs this registered public
    // ID--no client secret or user authentication.
    void Configure(bool enabled, std::wstring application_id = {});
    void Update(DiscordTrackPresence presence,
                std::chrono::steady_clock::time_point observed_at =
                    std::chrono::steady_clock::now());
    void Clear();

private:
    friend struct ttplayer::testing::DiscordPresenceAccess;
    // Tests use a private pipe, never a real user's Discord IPC slot.
    explicit DiscordPresence(std::wstring test_pipe_name);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

namespace discord_presence_detail {

[[nodiscard]] bool IsValidApplicationId(std::wstring_view value) noexcept;
// A non-empty XML value overrides the built-in application identity.
[[nodiscard]] std::wstring SelectApplicationId(std::wstring_view configured);
[[nodiscard]] std::string BuildHandshakeJson(std::string_view application_id);
[[nodiscard]] std::chrono::milliseconds ProjectPresencePosition(
    const DiscordTrackPresence& presence, std::chrono::milliseconds elapsed);
[[nodiscard]] std::string BuildSetActivityJson(
    const DiscordTrackPresence* presence, std::uint32_t process_id,
    std::string_view nonce, std::int64_t unix_time_seconds);
[[nodiscard]] std::vector<std::uint8_t> BuildRpcFrame(
    std::uint32_t opcode, std::string_view json);

} // namespace discord_presence_detail
} // namespace ttplayer::integrations
