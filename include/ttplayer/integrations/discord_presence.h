#pragma once

#include "ttplayer/integrations/discord_presence_config.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::integrations {

enum class DiscordPlaybackState {
    stopped,
    playing,
    paused,
};

struct DiscordTrackPresence {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::chrono::milliseconds position{};
    std::chrono::milliseconds duration{};
    DiscordPlaybackState playback{DiscordPlaybackState::stopped};

    friend bool operator==(const DiscordTrackPresence&,
                           const DiscordTrackPresence&) = default;
};

// A non-blocking client for Discord's documented local RPC transport.  Calls
// from the player window only update the desired state; pipe discovery,
// handshake and SET_ACTIVITY run on a private worker thread.
class DiscordPresence final {
public:
    DiscordPresence();
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // application_id comes from TTPlayer.xml; an empty value falls back to
    // kDefaultDiscordApplicationId. Discord only needs this registered public
    // ID--no client secret or user authentication.
    void Configure(bool enabled, std::wstring application_id = {});
    void Update(DiscordTrackPresence presence);
    void Clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

namespace discord_presence_detail {

[[nodiscard]] bool IsValidApplicationId(std::wstring_view value) noexcept;
// A non-empty XML value overrides the built-in application identity.
[[nodiscard]] std::wstring SelectApplicationId(std::wstring_view configured);
[[nodiscard]] std::string BuildHandshakeJson(std::string_view application_id);
[[nodiscard]] std::string BuildSetActivityJson(
    const DiscordTrackPresence* presence, std::uint32_t process_id,
    std::string_view nonce, std::int64_t unix_time_seconds);
[[nodiscard]] std::vector<std::uint8_t> BuildRpcFrame(
    std::uint32_t opcode, std::string_view json);

} // namespace discord_presence_detail
} // namespace ttplayer::integrations
