#pragma once
#include <ttpcomm/bytes.h>
#include <optional>
#include <string_view>

namespace ttpcomm::zip {
// Policies belong to callers: legacy skins accept an EOCD before trailing
// bytes; downloaded updates require the comment to end exactly at EOF.
enum class EndPolicy { legacy_skin, exact_eof };
struct CentralRecord {
    uint16_t flags{}, method{}, disk{};
    uint32_t crc{}, packed{}, size{}, local{};
    std::string_view name;
    size_t record_size{};
};
inline std::optional<CentralRecord> ReadCentral(bytes::View data, size_t offset) noexcept {
    if (!bytes::Contains(data.size(), offset, 46)) return {};
    const auto p = data.data() + offset;
    if (bytes::Le32(p) != 0x02014b50) return {};
    const size_t name = bytes::Le16(p + 28);
    const size_t size = 46 + name + bytes::Le16(p + 30) + bytes::Le16(p + 32);
    if (!bytes::Contains(data.size(), offset, size)) return {};
    return CentralRecord{bytes::Le16(p + 8), bytes::Le16(p + 10), bytes::Le16(p + 34),
        bytes::Le32(p + 16), bytes::Le32(p + 20), bytes::Le32(p + 24), bytes::Le32(p + 42),
        {reinterpret_cast<const char*>(p + 46), name}, size};
}
inline std::optional<size_t> FindDirectoryEnd(bytes::View data, EndPolicy policy) noexcept {
    if (data.size() < 22) return {};
    const size_t minimum = policy == EndPolicy::exact_eof && data.size() > 65557 ? data.size() - 65557 : 0;
    size_t offset = data.size() - 22;
    for (;;) {
        if (bytes::Le32(data.data() + offset) == 0x06054b50 &&
            (policy == EndPolicy::legacy_skin || bytes::Le16(data.data() + offset + 20) == data.size() - offset - 22)) return offset;
        if (offset == minimum) return {};
        --offset;
    }
}
}
