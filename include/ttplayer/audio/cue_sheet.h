#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::audio {

using CueMetadata = std::vector<std::pair<std::wstring, std::wstring>>;

struct CueTrack {
    int number{}; // One-based position in the parsed array (004DD69E/004E323B).
    std::filesystem::path audio_path;
    std::wstring title;
    std::wstring performer;
    std::uint64_t start_frame{}; // 75 frames per second.
    std::optional<std::uint64_t> end_frame;
    int source_number{};
    std::wstring file_reference;
    std::optional<std::uint64_t> index00;
    bool has_index01{};
    CueMetadata metadata;
    size_t first_line{};
    size_t last_line{};

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
    // Only use for migration of an unambiguously identified old playlist.
    [[nodiscard]] const CueTrack* FindSourceTrack(int source_number) const noexcept;
    [[nodiscard]] std::vector<std::filesystem::path> AudioCandidates(
        const CueTrack& track) const;
    // Merge only requested fields into this snapshot. Refuse stale snapshots,
    // archive members and read-only files; replace a flushed sibling file.
    void WriteTrackMetadata(int subtrack, const CueMetadata& fields) const;
    [[nodiscard]] static bool IsWritableField(std::wstring_view name) noexcept;
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
    struct Line { std::wstring text, ending; };
    std::vector<Line> lines_;
    std::vector<unsigned char> original_bytes_;
    std::filesystem::path source_path_;
    // 0 UTF-8, 1 UTF-8 BOM, 2 UTF-16 LE, 3 UTF-16 BE, 4 system ANSI.
    int encoding_{};
    bool from_memory_{};
};

} // namespace ttplayer::audio
