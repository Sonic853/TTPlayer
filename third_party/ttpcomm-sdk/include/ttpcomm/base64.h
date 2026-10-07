#pragma once
#include <array>
#include <cwctype>
#include <string_view>
#include <vector>
namespace ttpcomm::text {
inline std::vector<unsigned char> DecodeLegacyBase64(std::wstring_view source) {
    std::array<int, 256> values{};
    values.fill(-1);
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int index = 0; index < 64; ++index)
        values[static_cast<unsigned char>(alphabet[index])] = index;
    std::vector<unsigned char> output;
    output.reserve(source.size() / 4 * 3);
    unsigned int accumulator{};
    int bits{};
    for (const wchar_t wide : source) {
        if (wide == L'=') break;
        if (iswspace(wide)) continue;
        if (wide < 0 || wide > 0xff || values[static_cast<unsigned char>(wide)] < 0)
            return {};
        accumulator = (accumulator << 6) |
                      static_cast<unsigned int>(values[static_cast<unsigned char>(wide)]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<unsigned char>(accumulator >> bits));
            accumulator &= (1U << bits) - 1U;
        }
    }
    return output;
}
}
