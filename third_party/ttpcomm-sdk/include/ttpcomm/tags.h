#pragma once
#include <ttpcomm/bytes.h>
#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

namespace ttpcomm::tags {
using bytes::View;
inline size_t TerminatorSize(unsigned encoding) noexcept { return encoding == 1 || encoding == 2 ? 2 : 1; }
inline size_t FindTerminator(View value, unsigned encoding) noexcept {
    const size_t step = TerminatorSize(encoding);
    for (size_t i = 0; bytes::Contains(value.size(), i, step); i += step)
        if (!value[i] && (step == 1 || !value[i + 1])) return i;
    return value.size();
}
inline std::vector<unsigned char> RemoveUnsynchronization(View value) {
    std::vector<unsigned char> output;
    output.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        output.push_back(value[i]);
        if (value[i] == 0xff && i + 1 < value.size() && !value[i + 1]) ++i;
    }
    return output;
}
namespace detail {
// Keep this header usable without a zlib link dependency. This is the IEEE
// CRC-32 used by ID3 extended headers; the small table avoids per-bit work.
inline uint32_t TagCrc32(View data) noexcept {
    static constexpr uint32_t table[]{
        0x00000000,0x1db71064,0x3b6e20c8,0x26d930ac,
        0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
        0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,
        0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
    uint32_t crc = UINT32_MAX;
    for (const auto byte : data) {
        crc ^= byte;
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return ~crc;
}
}
// v2.2/v2.3 input has already had tag-level unsynchronization removed.
// v2.4 input must keep its stored frame lengths and bytes; unsynchronization
// belongs to each frame. The supplied view excludes the tag header/footer.
// v2.2 tag compression has no supported framing.
inline std::optional<size_t> FrameStart(View tag, unsigned major, unsigned flags) noexcept {
    if (major < 2 || major > 4 || (flags & ~(major == 2 ? 0xc0u : major == 3 ? 0xe0u : 0xf0u))) return {};
    if (!(flags & 0x40)) return 0;
    if (major == 2 || tag.size() < 4) return {};
    const uint32_t size = major == 4 ? bytes::Synchsafe32(tag.data()) : bytes::Be32(tag.data());
    const uint64_t skip = uint64_t(size) + (major == 3 ? 4 : 0);
    if (size == UINT32_MAX || size < 6 || skip > tag.size()) return {};
    const auto start = static_cast<size_t>(skip);
    if (major == 3) {
        const unsigned extended_flags = unsigned(tag[4]) << 8 | tag[5];
        const bool crc_present = (extended_flags & 0x8000u) != 0;
        if ((extended_flags & ~0x8000u) || size != (crc_present ? 10u : 6u)) return {};
        const uint32_t padding = bytes::Be32(tag.data() + 6);
        if (padding > tag.size() - start) return {};
        const size_t end = tag.size() - padding;
        if (!std::all_of(tag.data() + end, tag.end(), [](unsigned char byte){ return byte == 0; })) return {};
        // v2.3: CRC precedes unsynchronization and excludes padding.
        if (crc_present && bytes::Be32(tag.data() + 10) !=
                detail::TagCrc32({tag.data() + start, end - start})) return {};
    } else {
        // v2.4 has one flag byte. Every set flag owns a length byte;
        // unknown/reserved flags cannot be safely treated as this version.
        if (tag[4] != 1 || (tag[5] & ~0x70u)) return {};
        size_t cursor = 6;
        for (unsigned flag = 0x40; flag; flag >>= 1) {
            if (!(tag[5] & flag)) continue;
            const size_t length = flag == 0x40 ? 0 : flag == 0x20 ? 5 : 1;
            if (cursor >= start || tag[cursor++] != length || length > start - cursor) return {};
            if (flag == 0x20) {
                // The 35-bit synchsafe encoding carries only 32 CRC bits.
                if (tag[cursor] > 0x0f) return {};
                uint32_t expected{};
                for (size_t i = 0; i < 5; ++i) {
                    if (tag[cursor + i] & 0x80) return {};
                    expected = (expected << 7) | tag[cursor + i];
                }
                // v2.4: CRC covers stored frames and padding, not footer.
                if (expected != detail::TagCrc32(tag.subspan(start))) return {};
            }
            cursor += length;
        }
        if (cursor != start) return {};
    }
    return start;
}
struct Frame {
    std::string_view identifier;
    View raw, payload;
    unsigned flags{};
};
enum class FrameStatus { frame, end, invalid };
inline FrameStatus NextFrame(View tag, unsigned major, size_t& offset, Frame& frame) noexcept {
    if (major < 2 || major > 4 || offset > tag.size()) return FrameStatus::invalid;
    const size_t header = major == 2 ? 6 : 10, idSize = major == 2 ? 3 : 4;
    if (!bytes::Contains(tag.size(), offset, header)) {
        const auto tail = tag.subspan(offset);
        return std::all_of(tail.begin(), tail.end(), [](unsigned char b){return !b;}) ? FrameStatus::end : FrameStatus::invalid;
    }
    const auto p = tag.data() + offset;
    if (std::all_of(p, p + idSize, [](unsigned char b){return !b;})) return FrameStatus::end;
    if (!std::all_of(p, p + idSize, [](unsigned char b){return (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9');}))
        return FrameStatus::invalid;
    const uint32_t size = major == 2 ? uint32_t(p[3]) << 16 | uint32_t(p[4]) << 8 | p[5]
        : major == 4 ? bytes::Synchsafe32(p + 4) : bytes::Be32(p + 4);
    if (size == UINT32_MAX || size > tag.size() - offset - header) return FrameStatus::invalid;
    frame = {{reinterpret_cast<const char*>(p), idSize}, {p, header + size}, {p + header, size}, major == 2 ? 0u : p[9]};
    offset += header + size;
    return FrameStatus::frame;
}
struct Picture {
    uint32_t type{}, width{}, height{}, depth{};
    std::string_view mime;
    View data;
};
inline std::optional<Picture> Id3Picture(View frame, unsigned major) noexcept {
    if (frame.size() < 4 || major < 2 || major > 4) return {};
    size_t cursor = 1;
    Picture picture;
    if (major == 2) {
        picture.mime = {reinterpret_cast<const char*>(frame.data() + 1), 3};
        if (picture.mime == "JPG") picture.mime = "image/jpeg";
        else if (picture.mime == "PNG") picture.mime = "image/png";
        else if (picture.mime == "BMP") picture.mime = "image/bmp";
        else if (picture.mime == "GIF") picture.mime = "image/gif";
        cursor = 4;
    } else {
        const size_t length = FindTerminator(frame.subspan(cursor), 0);
        if (length == frame.size() - cursor) return {};
        picture.mime = {reinterpret_cast<const char*>(frame.data() + cursor), length};
        cursor += length + 1;
    }
    if (cursor >= frame.size()) return {};
    picture.type = frame[cursor++];
    const size_t description = FindTerminator(frame.subspan(cursor), frame[0]);
    if (description == frame.size() - cursor) return {};
    cursor += description + TerminatorSize(frame[0]);
    picture.data = frame.subspan(cursor);
    return picture;
}
inline std::optional<Picture> FlacPicture(View block) noexcept {
    size_t cursor{};
    auto take = [&](uint32_t& out) {
        if (!bytes::Contains(block.size(), cursor, 4)) return false;
        out = bytes::Be32(block.data() + cursor); cursor += 4; return true;
    };
    Picture picture;
    uint32_t length{}, colors{};
    if (!take(picture.type) || !take(length) || !bytes::Contains(block.size(), cursor, length)) return {};
    picture.mime = {reinterpret_cast<const char*>(block.data() + cursor), length}; cursor += length;
    if (!take(length) || !bytes::Contains(block.size(), cursor, length)) return {};
    cursor += length;
    if (!take(picture.width) || !take(picture.height) || !take(picture.depth) || !take(colors) ||
        !take(length) || !bytes::Contains(block.size(), cursor, length)) return {};
    picture.data = {block.data() + cursor, length};
    return picture;
}
}
