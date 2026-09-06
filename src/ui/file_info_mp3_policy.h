#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ttplayer::ui::detail {

// CFileInfoDlg::OnInitDialog (00431101..0043124F) stores these DWORDs as
// combo-box item data.  Keep the values separate from their presentation:
// MP3ReadTagPriority is a packed, most-significant-byte-first tag order and
// MP3WriteTagType is the original ID3v1/APEv2/ID3v2 bit mask.
struct Mp3TagPolicyChoice {
    std::uint32_t value;
    std::wstring_view label;
};

inline constexpr std::array<Mp3TagPolicyChoice, 4>
    kMp3ReadPriorityChoices{{
        {0x04080102U, L"APEv2 > ID3v2 > ID3v1"},
        {0x08040102U, L"ID3v2 > APEv2 > ID3v1"},
        {0x01080402U, L"ID3v1 > ID3v2 > APEv2"},
        {0x01040802U, L"ID3v1 > APEv2 > ID3v2"},
    }};

inline constexpr std::array<Mp3TagPolicyChoice, 6>
    kMp3WriteTypeChoices{{
        {0x01U, L"ID3v1"},
        {0x08U, L"ID3v2"},
        {0x04U, L"APEv2"},
        {0x09U, L"ID3v1 & ID3v2"},
        {0x05U, L"ID3v1 & APEv2"},
        {0x0cU, L"ID3v2 & APEv2"},
    }};

// The third visible entry is deliberately backed by value 3, not 2
// (0043122F..0043123A).  A legacy persisted value 2 is displayed as that
// third entry without being rewritten until the user makes a selection
// (0043123C..00431249).
inline constexpr std::array<Mp3TagPolicyChoice, 3>
    kMp3Id3v2EncodingChoices{{
        {0U, L"ISO-8859-1"},
        {1U, L"UTF-16"},
        {3U, L"UTF-8"},
    }};

template <std::size_t Count>
[[nodiscard]] constexpr int FindMp3TagPolicyChoice(
    const std::array<Mp3TagPolicyChoice, Count>& choices,
    std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < choices.size(); ++index) {
        if (choices[index].value == value) return static_cast<int>(index);
    }
    return -1;
}

[[nodiscard]] constexpr int Mp3Id3v2EncodingSelection(
    std::uint32_t persisted_value) noexcept {
    return persisted_value == 3U ? 2 :
        persisted_value < kMp3Id3v2EncodingChoices.size()
            ? static_cast<int>(persisted_value) : -1;
}

[[nodiscard]] constexpr bool Mp3WriteTypeIncludesId3v2(
    std::uint32_t write_type) noexcept {
    return (write_type & 0x08U) != 0;
}

} // namespace ttplayer::ui::detail
