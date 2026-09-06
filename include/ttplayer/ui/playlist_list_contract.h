#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

namespace ttplayer::ui {

// FUN_004C1D36 converts the displayed Unicode title through CP_ACP, walks the
// resulting DBCS string and replaces GB characters in these half-open ranges
// with their A-Z initial.  Keep the byte mapping separate so its historical
// boundary behaviour can be tested without depending on the host ANSI page.
[[nodiscard]] constexpr int LegacyGbOrdinal(
    std::uint8_t lead, std::uint8_t trail) noexcept {
    const int row = lead < 0xa1 ? static_cast<int>(lead) - 0x1f
                                : static_cast<int>(lead) - 0xa0;
    const int column = trail < 0xa1 ? static_cast<int>(trail) - 0x1f
                                    : static_cast<int>(trail) - 0xa0;
    return row * 100 + column;
}

[[nodiscard]] constexpr wchar_t LegacyGbPinyinInitialFromOrdinal(
    int ordinal) noexcept {
    constexpr std::array<std::pair<int, int>, 26> ranges{{
        {0x0641, 0x0664}, {0x0665, 0x0728}, {0x0729, 0x081e},
        {0x081f, 0x08e1}, {0x08e2, 0x08fd}, {0x08fe, 0x0980},
        {0x0981, 0x0a21}, {0x0a22, 0x0ae2}, {0, 0},
        {0x0ae3, 0x0c21}, {0x0c22, 0x0c8b}, {0x0c8c, 0x0d8f},
        {0x0d90, 0x0e32}, {0x0e33, 0x0e89}, {0x0e8a, 0x0e91},
        {0x0e92, 0x0f11}, {0x0f12, 0x0fba}, {0x0fbb, 0x0ff5},
        {0x0ff6, 0x1125}, {0x1126, 0x11cd}, {0, 0}, {0, 0},
        {0x11ce, 0x124b}, {0x1256, 0x133c}, {0x133d, 0x1480},
        {0x1481, 0x15d5},
    }};
    for (size_t index = 0; index < ranges.size(); ++index) {
        if (ranges[index].first <= ordinal &&
            ordinal < ranges[index].second)
            return static_cast<wchar_t>(L'A' + index);
    }
    return L'\0';
}

[[nodiscard]] constexpr wchar_t LegacyGbPinyinInitial(
    std::uint8_t lead, std::uint8_t trail) noexcept {
    return LegacyGbPinyinInitialFromOrdinal(
        LegacyGbOrdinal(lead, trail));
}

// Production callers retain code page zero, exactly as FUN_004C1D36 does.
// Tests may supply 936 explicitly so their result is independent of the host
// locale while still exercising the original WideCharToMultiByte/DBCS path.
[[nodiscard]] inline std::wstring LegacyPinyinInitials(
    std::wstring_view value, UINT code_page = CP_ACP) {
    if (value.empty()) return {};
    const std::wstring stable(value);
    const int byte_count = WideCharToMultiByte(
        code_page, 0, stable.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (byte_count <= 1) return {};
    std::string bytes(static_cast<size_t>(byte_count), '\0');
    if (WideCharToMultiByte(code_page, 0, stable.c_str(), -1, bytes.data(),
                            byte_count, nullptr, nullptr) == 0)
        return {};

    const UINT effective_code_page = code_page == CP_ACP ? GetACP() : code_page;
    const char* cursor = bytes.data();
    const char* const end = cursor + byte_count - 1;
    std::wstring result;
    result.reserve(value.size());
    while (cursor < end && *cursor != '\0') {
        const auto lead = static_cast<std::uint8_t>(*cursor);
        // The native MOVSX before appending the UTF-16 code unit is visible
        // in the 004C1D36 machine code.  Unmapped DBCS input consequently
        // becomes U+FFxx rather than retaining either the Han character or a
        // zero-extended U+00xx byte.
        const int signed_lead = lead < 0x80 ? static_cast<int>(lead)
                                            : static_cast<int>(lead) - 0x100;
        wchar_t output = static_cast<wchar_t>(signed_lead);
        if (lead >= 0x80 && cursor + 1 < end) {
            const auto initial = LegacyGbPinyinInitial(
                lead, static_cast<std::uint8_t>(cursor[1]));
            if (initial != L'\0') output = initial;
        }
        result.push_back(output);
        const char* next = CharNextExA(
            static_cast<WORD>(effective_code_page), cursor, 0);
        cursor = next > cursor ? std::min(next, end) : cursor + 1;
    }
    return result;
}

[[nodiscard]] inline bool LegacyPinyinPrefixMatches(
    std::wstring_view value, std::wstring_view prefix,
    UINT code_page = CP_ACP) {
    const auto starts_with = [prefix](std::wstring_view candidate) {
        return prefix.size() <= candidate.size() &&
            _wcsnicmp(candidate.data(), prefix.data(), prefix.size()) == 0;
    };
    if (starts_with(value)) return true;
    return starts_with(LegacyPinyinInitials(value, code_page));
}

// Owner-data ListView search contract recovered from the two
// LVN_ODFINDITEMW handlers at 00488A1F (Files) and 00489B9B (PlayLists).
// The notification supplies the first candidate, and LVFI_WRAP alone decides
// whether reaching the final row continues at zero.
template <class Matches>
[[nodiscard]] std::optional<size_t> FindOwnerDataListPrefix(
    size_t count, int start, bool wrap, Matches&& matches) {
    if (count == 0) return std::nullopt;
    size_t candidate = start < 0 || static_cast<size_t>(start) >= count
        ? 0 : static_cast<size_t>(start);
    for (size_t inspected = 0; inspected < count; ++inspected) {
        if (matches(candidate)) return candidate;
        ++candidate;
        if (candidate == count) {
            if (!wrap) return std::nullopt;
            candidate = 0;
        }
    }
    return std::nullopt;
}

// LVM_ENSUREVISIBLE changes the viewport only.  It never moves the native
// ListView's focused or selected item.  Keep the result clamped as item-count
// changes race the queued UI message in the original owner-data control.
[[nodiscard]] constexpr size_t OwnerDataListEnsureVisibleTop(
    size_t top, size_t visible_rows, size_t count, size_t item) noexcept {
    visible_rows = std::max<size_t>(1, visible_rows);
    const size_t maximum = count > visible_rows ? count - visible_rows : 0;
    top = std::min(top, maximum);
    if (item >= count) return top;
    if (item < top) return item;
    if (item - top >= visible_rows)
        return std::min(item - visible_rows + 1, maximum);
    return top;
}

} // namespace ttplayer::ui
