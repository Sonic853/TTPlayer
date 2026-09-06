#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ttplayer::audio {

struct CueTrack {
    int number{}; // The private CUE reader accepts one-based TRACK numbers.
    std::filesystem::path audio_path;
    std::wstring title;
    std::wstring performer;
    std::uint64_t start_frame{}; // 75 frames per second.
    std::optional<std::uint64_t> end_frame;

    [[nodiscard]] std::int64_t DurationMilliseconds() const noexcept;
};

class CueSheet {
public:
    static CueSheet Load(const std::filesystem::path& path);
    // FUN_00474051 reads an embedded CUE through the archive object's memory
    // entry point, then gives the CUE parser the logical `archive|member`
    // name.  004E323B joins a relative FILE directive to that member's
    // directory textually; the later 0047E177 lookup deliberately sees any
    // uncollapsed `.`/`..` components.
    static CueSheet LoadFromMemory(std::span<const unsigned char> bytes,
                                   const std::filesystem::path& source_path);

    [[nodiscard]] const std::vector<CueTrack>& Tracks() const noexcept {
        return tracks_;
    }
    [[nodiscard]] const CueTrack* FindTrack(int one_based_number) const noexcept;
    [[nodiscard]] const std::wstring& Title() const noexcept { return title_; }
    [[nodiscard]] const std::wstring& Performer() const noexcept {
        return performer_;
    }

private:
    static CueSheet Parse(std::span<const unsigned char> bytes,
                          const std::filesystem::path& source_path);
    std::vector<CueTrack> tracks_;
    std::wstring title_;
    std::wstring performer_;
};

} // namespace ttplayer::audio
