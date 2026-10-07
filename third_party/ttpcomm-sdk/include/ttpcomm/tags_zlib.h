#pragma once
#include <ttpcomm/tags.h>
#include <zlib.h>
#include <utility>
namespace ttpcomm::tags {
inline std::optional<View> DecodePayload(
    View data, unsigned char major,
    unsigned char flags, bool tag_unsynchronized, std::vector<unsigned char>& storage,
    std::uint64_t& inflate_budget) {
    if (major < 2 || major > 4) return std::nullopt;
    if (major == 2) return data;
    const bool compressed = (flags & (major == 3 ? 0x80U : 0x08U)) != 0;
    const bool encrypted = (flags & (major == 3 ? 0x40U : 0x04U)) != 0;
    const bool grouped = (flags & (major == 3 ? 0x20U : 0x40U)) != 0;
    if (encrypted || (flags & (major == 3 ? 0x1fU : 0xb0U)))
        return std::nullopt;
    std::uint32_t decoded_size{};
    if (major == 3 && compressed) {
        if (data.size() < 4) return std::nullopt;
        decoded_size = bytes::Be32(data.data());
        data = data.subspan(4);
    }
    if (grouped) {
        if (data.empty()) return std::nullopt;
        data = data.subspan(1);
    }
    if (major == 4) {
        if (compressed && !(flags & 0x01U)) return std::nullopt;
        if (flags & 0x01U) {
            if (data.size() < 4) return std::nullopt;
            decoded_size = bytes::Synchsafe32(data.data());
            if (decoded_size == UINT32_MAX) return std::nullopt;
            data = data.subspan(4);
        }
        if ((flags & 0x02U) && !tag_unsynchronized) {
            storage = RemoveUnsynchronization(data);
            data = View(storage.data(), storage.size());
        }
    }
    if (!compressed) return data;
    if (decoded_size > inflate_budget || decoded_size == UINT32_MAX || data.size() > UINT32_MAX)
        return std::nullopt;
    // One spare byte detects a stream whose actual size exceeds the prefix.
    std::vector<unsigned char> inflated(static_cast<size_t>(decoded_size) + 1);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = inflated.data();
    stream.avail_out = static_cast<uInt>(inflated.size());
    if (inflateInit(&stream) != Z_OK) return std::nullopt;
    const int status = inflate(&stream, Z_FINISH);
    const bool complete = status == Z_STREAM_END && stream.total_out == decoded_size;
    inflateEnd(&stream);
    if (!complete) return std::nullopt;
    inflated.resize(decoded_size);
    inflate_budget -= decoded_size;
    storage = std::move(inflated);
    return View(storage.data(), storage.size());
}
}
