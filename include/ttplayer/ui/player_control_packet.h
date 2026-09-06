#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::ui {

// Payload accepted by the original /e command-line route.  FUN_0045F2EF
// reads these three integers with atoi and leaves missing attributes at zero.
struct PlayerControlPacketSong {
    std::wstring url;
    std::wstring artist;
    std::wstring title;
};

struct PlayerControlPacket {
    int select{};
    int head{};
    int play{};
    std::vector<PlayerControlPacketSong> songs;
};

struct PlayerControlPacketRoute {
    bool select_playlist{};
    bool insert_at_head{};
    bool start_playback{};
};

// FUN_00463BE5 treats every non-zero integer as enabled.  Keeping this
// routing rule separate from the DOM parser makes command behavior testable
// without a window or a playlist store.
[[nodiscard]] constexpr PlayerControlPacketRoute RoutePlayerControlPacket(
    const PlayerControlPacket& packet) noexcept {
    return {packet.select != 0, packet.head != 0, packet.play != 0};
}

// Parses the inline XML carried in WM_COPYDATA for /e.  The native program
// does not dereference the argument as a filename: the argument itself must
// have a <ctrlparam> document root.
[[nodiscard]] std::optional<PlayerControlPacket> ParsePlayerControlPacketXml(
    std::wstring_view xml) noexcept;

} // namespace ttplayer::ui
