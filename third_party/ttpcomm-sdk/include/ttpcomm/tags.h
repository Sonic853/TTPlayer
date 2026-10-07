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
// Input has already had tag-level unsynchronization removed. v2.2 tag
// compression has no supported framing and must not be read as an extension.
inline std::optional<size_t> FrameStart(View tag, unsigned major, unsigned flags) noexcept {
    if (major < 2 || major > 4) return {};
    if (!(flags & 0x40)) return 0;
    if (major == 2 || tag.size() < 4) return {};
    const uint32_t size = major == 4 ? bytes::Synchsafe32(tag.data()) : bytes::Be32(tag.data());
    const uint64_t skip = uint64_t(size) + (major == 3 ? 4 : 0);
    if (size == UINT32_MAX || skip > tag.size() || (major == 4 && size < 4)) return {};
    return size_t(skip);
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
