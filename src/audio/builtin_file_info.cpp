#include "ttplayer/audio/builtin_file_info.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <optional>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <span>
#include <shlobj.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

namespace ttplayer::audio {
namespace {

using Bytes = std::vector<unsigned char>;
constexpr std::uint64_t kMaximumTagBytes = 64ULL * 1024ULL * 1024ULL;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE Release() noexcept {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void Reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{};
};

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    [[nodiscard]] T* Get() const noexcept { return value_; }
    [[nodiscard]] T** Put() noexcept {
        Reset();
        return &value_;
    }
    void Reset() noexcept {
        if (value_) value_->Release();
        value_ = nullptr;
    }

private:
    T* value_{};
};

std::uint32_t ReadLe32(const unsigned char* value) noexcept {
    return static_cast<std::uint32_t>(value[0]) |
           static_cast<std::uint32_t>(value[1]) << 8U |
           static_cast<std::uint32_t>(value[2]) << 16U |
           static_cast<std::uint32_t>(value[3]) << 24U;
}

std::uint32_t ReadBe32(const unsigned char* value) noexcept {
    return static_cast<std::uint32_t>(value[0]) << 24U |
           static_cast<std::uint32_t>(value[1]) << 16U |
           static_cast<std::uint32_t>(value[2]) << 8U |
           static_cast<std::uint32_t>(value[3]);
}

std::uint32_t ReadSynchsafe(const unsigned char* value) noexcept {
    if ((value[0] | value[1] | value[2] | value[3]) & 0x80U)
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(value[0]) << 21U |
           static_cast<std::uint32_t>(value[1]) << 14U |
           static_cast<std::uint32_t>(value[2]) << 7U |
           static_cast<std::uint32_t>(value[3]);
}

void AppendLe32(Bytes& output, std::uint32_t value) {
    output.push_back(static_cast<unsigned char>(value));
    output.push_back(static_cast<unsigned char>(value >> 8U));
    output.push_back(static_cast<unsigned char>(value >> 16U));
    output.push_back(static_cast<unsigned char>(value >> 24U));
}

void AppendBe32(Bytes& output, std::uint32_t value) {
    output.push_back(static_cast<unsigned char>(value >> 24U));
    output.push_back(static_cast<unsigned char>(value >> 16U));
    output.push_back(static_cast<unsigned char>(value >> 8U));
    output.push_back(static_cast<unsigned char>(value));
}

void AppendSynchsafe(Bytes& output, std::uint32_t value) {
    output.push_back(static_cast<unsigned char>((value >> 21U) & 0x7fU));
    output.push_back(static_cast<unsigned char>((value >> 14U) & 0x7fU));
    output.push_back(static_cast<unsigned char>((value >> 7U) & 0x7fU));
    output.push_back(static_cast<unsigned char>(value & 0x7fU));
}

bool ReadAt(HANDLE file, std::uint64_t offset,
            std::span<unsigned char> output) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    size_t done{};
    while (done < output.size()) {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            output.size() - done, static_cast<size_t>(MAXDWORD)));
        DWORD read{};
        if (!ReadFile(file, output.data() + done, request, &read, nullptr) ||
            read == 0)
            return false;
        done += read;
    }
    return true;
}

bool ReadRange(HANDLE file, std::uint64_t offset, std::uint64_t size,
               Bytes& output) {
    if (size > kMaximumTagBytes ||
        size > static_cast<std::uint64_t>(
            std::numeric_limits<size_t>::max()))
        return false;
    try { output.resize(static_cast<size_t>(size)); }
    catch (...) { return false; }
    return ReadAt(file, offset, output);
}

bool FileSize(HANDLE file, std::uint64_t& size) noexcept {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(file, &value) || value.QuadPart < 0) return false;
    size = static_cast<std::uint64_t>(value.QuadPart);
    return true;
}

bool AsciiEqual(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

bool WideAsciiEqual(std::wstring_view left,
                    std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        wchar_t lhs = left[index];
        wchar_t rhs = right[index];
        if (lhs >= L'A' && lhs <= L'Z') lhs += L'a' - L'A';
        if (rhs >= L'A' && rhs <= L'Z') rhs += L'a' - L'A';
        if (lhs != rhs) return false;
    }
    return true;
}

std::wstring DecodeLatin1(std::span<const unsigned char> value) {
    std::wstring result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<wchar_t>(character));
    return result;
}

std::wstring DecodeUtf8(std::span<const unsigned char> value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(value.data()),
        static_cast<int>(std::min<size_t>(value.size(), INT_MAX)), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(value.data()),
            static_cast<int>(value.size()), result.data(), count) != count)
        return {};
    return result;
}

std::wstring DecodeUtf16(std::span<const unsigned char> value,
                         bool big_endian) {
    if (value.size() < 2) return {};
    size_t offset{};
    if (value.size() >= 2 && value[0] == 0xffU && value[1] == 0xfeU) {
        big_endian = false;
        offset = 2;
    } else if (value.size() >= 2 && value[0] == 0xfeU && value[1] == 0xffU) {
        big_endian = true;
        offset = 2;
    }
    std::wstring result;
    result.reserve((value.size() - offset) / 2);
    for (; offset + 1 < value.size(); offset += 2) {
        const std::uint16_t character = big_endian
            ? static_cast<std::uint16_t>(value[offset]) << 8U |
                  static_cast<std::uint16_t>(value[offset + 1])
            : static_cast<std::uint16_t>(value[offset]) |
                  static_cast<std::uint16_t>(value[offset + 1]) << 8U;
        if (character == 0) break;
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

std::wstring DecodeText(unsigned char encoding,
                        std::span<const unsigned char> value) {
    while (!value.empty() && value.back() == 0) value = value.first(value.size() - 1);
    switch (encoding) {
    case 0: return DecodeLatin1(value);
    case 1: return DecodeUtf16(value, false);
    case 2: return DecodeUtf16(value, true);
    case 3: return DecodeUtf8(value);
    default: return {};
    }
}

Bytes EncodeUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(std::min<size_t>(value.size(), INT_MAX)),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    Bytes result(static_cast<size_t>(count));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()),
            reinterpret_cast<char*>(result.data()), count, nullptr,
            nullptr) != count)
        return {};
    return result;
}

Bytes EncodeText(std::wstring_view value, unsigned char encoding) {
    Bytes result;
    if (encoding == 0) {
        result.reserve(value.size());
        for (const wchar_t character : value)
            result.push_back(character <= 0xff
                ? static_cast<unsigned char>(character)
                : static_cast<unsigned char>('?'));
    } else if (encoding == 3) {
        result = EncodeUtf8(value);
    } else {
        result.reserve(value.size() * 2 + 2);
        result.push_back(0xffU);
        result.push_back(0xfeU);
        for (const wchar_t character : value) {
            result.push_back(static_cast<unsigned char>(character));
            result.push_back(static_cast<unsigned char>(character >> 8U));
        }
    }
    return result;
}

struct TagData {
    std::vector<BuiltinMetadataEntry> fields;
    Bytes cover;
};

std::wstring GetField(const TagData& data, std::wstring_view name) {
    const auto found = std::find_if(data.fields.begin(), data.fields.end(),
        [name](const auto& field) { return WideAsciiEqual(field.name, name); });
    return found == data.fields.end() ? std::wstring{} : found->value;
}

void SetField(TagData& data, std::wstring name, std::wstring value,
              bool replace = true) {
    const auto found = std::find_if(data.fields.begin(), data.fields.end(),
        [&name](const auto& field) {
            return WideAsciiEqual(field.name, name);
        });
    if (found == data.fields.end()) {
        if (!value.empty())
            data.fields.push_back({std::move(name), std::move(value)});
    } else if (replace) {
        if (value.empty()) data.fields.erase(found);
        else found->value = std::move(value);
    }
}

void MergeMissing(TagData& destination, const TagData& source) {
    for (const auto& field : source.fields) {
        if (GetField(destination, field.name).empty() && !field.value.empty())
            SetField(destination, field.name, field.value, false);
    }
    if (destination.cover.empty() && !source.cover.empty())
        destination.cover = source.cover;
}

std::wstring CanonicalField(std::string_view name) {
    if (AsciiEqual(name, "title")) return L"Title";
    if (AsciiEqual(name, "artist") || AsciiEqual(name, "author"))
        return L"Artist";
    if (AsciiEqual(name, "album")) return L"Album";
    if (AsciiEqual(name, "track") || AsciiEqual(name, "tracknumber"))
        return L"Tracknumber";
    if (AsciiEqual(name, "genre")) return L"Genre";
    if (AsciiEqual(name, "date") || AsciiEqual(name, "year"))
        return L"Date";
    if (AsciiEqual(name, "comment")) return L"Comment";
    return {};
}

std::wstring ApeDisplayField(std::string_view name) {
    const auto canonical = CanonicalField(name);
    if (!canonical.empty()) return canonical;
    try {
        const Bytes bytes(name.begin(), name.end());
        return DecodeUtf8(bytes);
    } catch (...) {
        return {};
    }
}

std::string FrameSemantic(std::string_view identifier) {
    if (identifier == "TIT2" || identifier == "TT2") return "Title";
    if (identifier == "TPE1" || identifier == "TP1") return "Artist";
    if (identifier == "TALB" || identifier == "TAL") return "Album";
    if (identifier == "TRCK" || identifier == "TRK") return "Tracknumber";
    if (identifier == "TCON" || identifier == "TCO") return "Genre";
    if (identifier == "TDRC" || identifier == "TYER" ||
        identifier == "TYE") return "Date";
    if (identifier == "COMM" || identifier == "COM") return "Comment";
    return {};
}

struct Id3Frame {
    std::string identifier;
    std::string semantic;
    std::wstring metadata_name;
    Bytes raw;
};

struct Id3Tag {
    bool present{};
    bool rewrite_safe{true};
    unsigned char major{3};
    std::uint64_t total_size{};
    TagData data;
    std::vector<Id3Frame> frames;
};

size_t EncodedTerminatorSize(unsigned char encoding) noexcept {
    return encoding == 1 || encoding == 2 ? 2U : 1U;
}

size_t FindEncodedTerminator(std::span<const unsigned char> value,
                             unsigned char encoding) noexcept {
    if (encoding == 1 || encoding == 2) {
        for (size_t index = 0; index + 1 < value.size(); index += 2) {
            if (value[index] == 0 && value[index + 1] == 0) return index;
        }
        return value.size();
    }
    const auto found = std::find(value.begin(), value.end(), 0);
    return static_cast<size_t>(found - value.begin());
}

void DecodeId3Frame(std::string_view identifier,
                    std::span<const unsigned char> payload, TagData& data) {
    if (payload.empty()) return;
    const unsigned char encoding = payload[0];
    const auto semantic = FrameSemantic(identifier);
    if (!semantic.empty() && identifier != "COMM" && identifier != "COM") {
        SetField(data, std::wstring(semantic.begin(), semantic.end()),
                 DecodeText(encoding, payload.subspan(1)), false);
        return;
    }
    if (identifier == "COMM" || identifier == "COM") {
        if (payload.size() < 4) return;
        auto text = payload.subspan(4);
        const size_t separator = FindEncodedTerminator(text, encoding);
        const size_t skip = separator < text.size()
            ? separator + EncodedTerminatorSize(encoding) : text.size();
        SetField(data, L"Comment", DecodeText(encoding, text.subspan(skip)),
                 false);
        return;
    }
    if (identifier == "TXXX" || identifier == "TXX") {
        auto text = payload.subspan(1);
        const size_t separator = FindEncodedTerminator(text, encoding);
        if (separator >= text.size()) return;
        const auto description = DecodeText(encoding, text.first(separator));
        const size_t skip = separator + EncodedTerminatorSize(encoding);
        if (!description.empty() && skip <= text.size())
            SetField(data, description, DecodeText(encoding,
                     text.subspan(skip)), false);
        return;
    }
    if (identifier == "APIC") {
        auto rest = payload.subspan(1);
        const auto mime_end = std::find(rest.begin(), rest.end(), 0);
        if (mime_end == rest.end()) return;
        size_t offset = static_cast<size_t>(mime_end - rest.begin()) + 1;
        if (offset >= rest.size()) return;
        ++offset; // picture type
        const auto description = rest.subspan(offset);
        const size_t separator = FindEncodedTerminator(description, encoding);
        offset += separator < description.size()
            ? separator + EncodedTerminatorSize(encoding)
            : description.size();
        if (offset < rest.size() && data.cover.empty())
            data.cover.assign(rest.begin() + static_cast<ptrdiff_t>(offset),
                              rest.end());
    } else if (identifier == "PIC") {
        if (payload.size() < 5) return;
        auto description = payload.subspan(5);
        const size_t separator = FindEncodedTerminator(description, encoding);
        const size_t offset = 5 + (separator < description.size()
            ? separator + EncodedTerminatorSize(encoding)
            : description.size());
        if (offset < payload.size() && data.cover.empty())
            data.cover.assign(payload.begin() + static_cast<ptrdiff_t>(offset),
                              payload.end());
    }
}

std::wstring Id3UserTextName(std::string_view identifier,
                             std::span<const unsigned char> payload) {
    if ((identifier != "TXXX" && identifier != "TXX") || payload.empty())
        return {};
    const unsigned char encoding = payload[0];
    const auto text = payload.subspan(1);
    const size_t separator = FindEncodedTerminator(text, encoding);
    return DecodeText(encoding, text.first(separator));
}

Bytes RemoveUnsynchronization(std::span<const unsigned char> value) {
    Bytes output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        output.push_back(value[index]);
        if (value[index] == 0xffU && index + 1 < value.size() &&
            value[index + 1] == 0U)
            ++index;
    }
    return output;
}

Id3Tag ReadId3v2(HANDLE file, std::uint64_t file_size) {
    Id3Tag tag;
    if (file_size < 10) return tag;
    std::array<unsigned char, 10> header{};
    if (!ReadAt(file, 0, header) ||
        std::memcmp(header.data(), "ID3", 3) != 0)
        return tag;
    const auto major = header[3];
    const auto payload_size = ReadSynchsafe(header.data() + 6);
    if ((major < 2 || major > 4) ||
        payload_size == std::numeric_limits<std::uint32_t>::max())
        return tag;
    const std::uint64_t footer = major == 4 && (header[5] & 0x10U) ? 10U : 0U;
    const std::uint64_t total = 10ULL + payload_size + footer;
    if (total > file_size || payload_size > kMaximumTagBytes) return tag;
    Bytes payload;
    if (!ReadRange(file, 10, payload_size, payload)) return tag;
    tag.present = true;
    tag.major = major;
    tag.total_size = total;
    if (header[5] & 0x80U) {
        tag.rewrite_safe = false;
        payload = RemoveUnsynchronization(payload);
    }
    size_t offset{};
    if (header[5] & 0x40U) {
        if (payload.size() < 4) return tag;
        const auto extended = major == 4
            ? ReadSynchsafe(payload.data()) : ReadBe32(payload.data());
        const std::uint64_t skip = major == 3
            ? 4ULL + extended : static_cast<std::uint64_t>(extended);
        if (extended == std::numeric_limits<std::uint32_t>::max() ||
            skip > payload.size())
            return tag;
        offset = static_cast<size_t>(skip);
    }
    const size_t header_size = major == 2 ? 6U : 10U;
    while (offset + header_size <= payload.size()) {
        const size_t identifier_size = major == 2 ? 3U : 4U;
        if (std::all_of(payload.begin() + static_cast<ptrdiff_t>(offset),
                payload.begin() + static_cast<ptrdiff_t>(offset + identifier_size),
                [](unsigned char value) { return value == 0; }))
            break;
        std::string identifier(
            reinterpret_cast<const char*>(payload.data() + offset),
            identifier_size);
        if (!std::all_of(identifier.begin(), identifier.end(), [](char value) {
                return (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9');
            }))
            break;
        const std::uint32_t frame_size = major == 2
            ? static_cast<std::uint32_t>(payload[offset + 3]) << 16U |
                  static_cast<std::uint32_t>(payload[offset + 4]) << 8U |
                  static_cast<std::uint32_t>(payload[offset + 5])
            : major == 4 ? ReadSynchsafe(payload.data() + offset + 4)
                         : ReadBe32(payload.data() + offset + 4);
        if (frame_size == std::numeric_limits<std::uint32_t>::max() ||
            frame_size > payload.size() - offset - header_size)
            break;
        const size_t end = offset + header_size + frame_size;
        Id3Frame frame;
        frame.identifier = identifier;
        frame.semantic = FrameSemantic(identifier);
        frame.raw.assign(payload.begin() + static_cast<ptrdiff_t>(offset),
                         payload.begin() + static_cast<ptrdiff_t>(end));
        // Compressed/encrypted frames are retained byte-for-byte but cannot
        // be decoded as ordinary text.
        const bool encoded_frame = major != 2 &&
            ((major == 3 && (payload[offset + 9] & 0xc0U) != 0) ||
             (major == 4 && (payload[offset + 9] & 0x0eU) != 0));
        if (major == 4 && (payload[offset + 9] & 0x0eU) != 0)
            tag.rewrite_safe = false;
        const auto frame_payload =
            std::span<const unsigned char>(payload).subspan(
                offset + header_size, frame_size);
        if (!encoded_frame) {
            DecodeId3Frame(identifier, frame_payload, tag.data);
            frame.metadata_name = Id3UserTextName(identifier, frame_payload);
        }
        tag.frames.push_back(std::move(frame));
        offset = end;
    }
    return tag;
}

struct ApeItem {
    std::string key;
    std::wstring semantic;
    Bytes raw;
    bool cover{};
};

struct ApeTag {
    bool present{};
    std::uint64_t physical_start{};
    std::uint64_t footer_end{};
    TagData data;
    std::vector<ApeItem> items;
};

std::optional<ApeTag> ReadApeAt(HANDLE file, std::uint64_t footer_end) {
    if (footer_end < 32) return std::nullopt;
    std::array<unsigned char, 32> footer{};
    if (!ReadAt(file, footer_end - 32, footer) ||
        std::memcmp(footer.data(), "APETAGEX", 8) != 0)
        return std::nullopt;
    const std::uint32_t size = ReadLe32(footer.data() + 12);
    const std::uint32_t count = ReadLe32(footer.data() + 16);
    if (size < 32 || size > kMaximumTagBytes || size > footer_end ||
        count > 65536U)
        return std::nullopt;
    const std::uint64_t item_start = footer_end - size;
    std::uint64_t physical_start = item_start;
    if (item_start >= 32) {
        std::array<unsigned char, 8> marker{};
        if (ReadAt(file, item_start - 32, marker) &&
            std::memcmp(marker.data(), "APETAGEX", 8) == 0)
            physical_start -= 32;
    }
    Bytes items;
    if (!ReadRange(file, item_start, size - 32U, items)) return std::nullopt;
    ApeTag tag;
    tag.present = true;
    tag.physical_start = physical_start;
    tag.footer_end = footer_end;
    size_t offset{};
    for (std::uint32_t index = 0; index < count; ++index) {
        if (offset + 8 > items.size()) return std::nullopt;
        const std::uint32_t value_size = ReadLe32(items.data() + offset);
        const std::uint32_t flags = ReadLe32(items.data() + offset + 4);
        const size_t key_begin = offset + 8;
        const auto key_end = std::find(items.begin() +
                static_cast<ptrdiff_t>(key_begin), items.end(), 0);
        if (key_end == items.end()) return std::nullopt;
        const size_t key_finish = static_cast<size_t>(key_end - items.begin());
        const size_t value_begin = key_finish + 1;
        if (value_size > items.size() - value_begin) return std::nullopt;
        const size_t end = value_begin + value_size;
        ApeItem item;
        item.key.assign(reinterpret_cast<const char*>(items.data() + key_begin),
                        key_finish - key_begin);
        item.semantic = CanonicalField(item.key);
        item.cover = AsciiEqual(item.key, "Cover Art (Front)");
        item.raw.assign(items.begin() + static_cast<ptrdiff_t>(offset),
                        items.begin() + static_cast<ptrdiff_t>(end));
        const std::span<const unsigned char> value(
            items.data() + value_begin, value_size);
        const std::uint32_t kind = flags >> 1U & 3U;
        if (kind == 0) {
            const auto decoded = DecodeUtf8(value);
            if (!decoded.empty())
                SetField(tag.data,
                    item.semantic.empty()
                        ? ApeDisplayField(item.key) : item.semantic,
                    decoded, false);
        } else if (kind == 1 && AsciiEqual(item.key, "Cover Art (Front)")) {
            const auto separator = std::find(value.begin(), value.end(), 0);
            if (separator != value.end()) {
                const size_t image = static_cast<size_t>(
                    separator - value.begin()) + 1;
                tag.data.cover.assign(
                    value.begin() + static_cast<ptrdiff_t>(image), value.end());
            }
        }
        tag.items.push_back(std::move(item));
        offset = end;
    }
    return tag;
}

std::wstring TrimFixed(std::span<const unsigned char> value) {
    while (!value.empty() && (value.back() == 0 || value.back() == ' '))
        value = value.first(value.size() - 1);
    if (value.empty()) return {};
    // ID3v1 files produced by the Chinese-era player commonly use the active
    // ANSI code page.  Prefer valid UTF-8, otherwise preserve that behavior.
    auto result = DecodeUtf8(value);
    if (!result.empty()) return result;
    const int count = MultiByteToWideChar(CP_ACP, 0,
        reinterpret_cast<const char*>(value.data()),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return DecodeLatin1(value);
    result.resize(static_cast<size_t>(count));
    MultiByteToWideChar(CP_ACP, 0,
        reinterpret_cast<const char*>(value.data()),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

TagData ReadId3v1(std::span<const unsigned char, 128> tag) {
    TagData data;
    if (std::memcmp(tag.data(), "TAG", 3) != 0) return data;
    SetField(data, L"Title", TrimFixed(tag.subspan(3, 30)), false);
    SetField(data, L"Artist", TrimFixed(tag.subspan(33, 30)), false);
    SetField(data, L"Album", TrimFixed(tag.subspan(63, 30)), false);
    SetField(data, L"Date", TrimFixed(tag.subspan(93, 4)), false);
    const bool version_11 = tag[125] == 0 && tag[126] != 0;
    SetField(data, L"Comment", TrimFixed(tag.subspan(
        97, version_11 ? 28U : 30U)), false);
    if (version_11)
        SetField(data, L"Tracknumber", std::to_wstring(tag[126]), false);
    if (tag[127] != 0xffU)
        SetField(data, L"Genre", std::to_wstring(tag[127]), false);
    return data;
}

struct FileLayout {
    std::uint64_t file_size{};
    std::uint64_t body_begin{};
    std::uint64_t body_end{};
    Id3Tag id3v2;
    ApeTag ape;
    TagData id3v1;
    bool has_id3v1{};
};

bool HasId3v1At(HANDLE file, std::uint64_t offset,
                std::array<unsigned char, 128>& bytes) {
    return ReadAt(file, offset, bytes) &&
           std::memcmp(bytes.data(), "TAG", 3) == 0;
}

bool ReadMp3Layout(HANDLE file, FileLayout& layout) {
    if (!FileSize(file, layout.file_size)) return false;
    layout.id3v2 = ReadId3v2(file, layout.file_size);
    layout.body_begin = layout.id3v2.present ? layout.id3v2.total_size : 0;
    layout.body_end = layout.file_size;

    std::array<unsigned char, 128> id3v1_bytes{};
    std::uint64_t tail_without_v1 = layout.file_size;
    if (layout.file_size >= id3v1_bytes.size() &&
        HasId3v1At(file, layout.file_size - id3v1_bytes.size(), id3v1_bytes)) {
        layout.has_id3v1 = true;
        layout.id3v1 = ReadId3v1(id3v1_bytes);
        tail_without_v1 -= id3v1_bytes.size();
        layout.body_end = tail_without_v1;
    }
    if (const auto ape = ReadApeAt(file, tail_without_v1)) {
        layout.ape = *ape;
        layout.body_end = std::min(layout.body_end, ape->physical_start);
    } else if (tail_without_v1 != layout.file_size) {
        // no second layout to inspect
    } else if (const auto trailing_ape = ReadApeAt(file, layout.file_size)) {
        layout.ape = *trailing_ape;
        layout.body_end = trailing_ape->physical_start;
        if (trailing_ape->physical_start >= id3v1_bytes.size() &&
            HasId3v1At(file,
                       trailing_ape->physical_start - id3v1_bytes.size(),
                       id3v1_bytes)) {
            layout.has_id3v1 = true;
            layout.id3v1 = ReadId3v1(id3v1_bytes);
            layout.body_end =
                trailing_ape->physical_start - id3v1_bytes.size();
        }
    }
    return layout.body_begin <= layout.body_end;
}

TagData MergeMp3Tags(const FileLayout& layout,
                     std::uint32_t priority) {
    // 004DC174 supplies the historical ID3v1/Lyrics3 tail when an old XML
    // value only contains the two high-priority bytes.
    if ((priority & 0xffffU) == 0) priority |= 0x0102U;
    TagData merged;
    for (int shift = 24; shift >= 0; shift -= 8) {
        switch (priority >> static_cast<unsigned>(shift) & 0xffU) {
        case 1: MergeMissing(merged, layout.id3v1); break;
        case 4: MergeMissing(merged, layout.ape.data); break;
        case 8: MergeMissing(merged, layout.id3v2.data); break;
        default: break; // Lyrics3 (2) is retained in the body, not rewritten.
        }
    }
    return merged;
}

struct MpegHeader {
    unsigned version{};
    unsigned layer{};
    unsigned bitrate_kbps{};
    unsigned sample_rate{};
    unsigned channels{};
};

std::optional<MpegHeader> DecodeMpegHeader(std::uint32_t value) {
    if ((value & 0xffe00000U) != 0xffe00000U) return std::nullopt;
    const unsigned version_bits = value >> 19U & 3U;
    const unsigned layer_bits = value >> 17U & 3U;
    const unsigned bitrate_index = value >> 12U & 15U;
    const unsigned sample_index = value >> 10U & 3U;
    if (version_bits == 1 || layer_bits == 0 || bitrate_index == 0 ||
        bitrate_index == 15 || sample_index == 3)
        return std::nullopt;
    const unsigned version = version_bits == 3 ? 1U
        : version_bits == 2 ? 2U : 25U;
    const unsigned layer = 4U - layer_bits;
    static constexpr unsigned rates[3]{44100, 48000, 32000};
    unsigned sample_rate = rates[sample_index];
    if (version == 2) sample_rate /= 2;
    else if (version == 25) sample_rate /= 4;
    static constexpr unsigned mpeg1_l1[16]{
        0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0};
    static constexpr unsigned mpeg1_l2[16]{
        0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
    static constexpr unsigned mpeg1_l3[16]{
        0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static constexpr unsigned mpeg2_l1[16]{
        0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0};
    static constexpr unsigned mpeg2_l23[16]{
        0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    unsigned bitrate{};
    if (version == 1)
        bitrate = layer == 1 ? mpeg1_l1[bitrate_index]
            : layer == 2 ? mpeg1_l2[bitrate_index]
                         : mpeg1_l3[bitrate_index];
    else
        bitrate = layer == 1 ? mpeg2_l1[bitrate_index]
                             : mpeg2_l23[bitrate_index];
    if (bitrate == 0) return std::nullopt;
    return MpegHeader{version, layer, bitrate, sample_rate,
        (value >> 6U & 3U) == 3U ? 1U : 2U};
}

std::optional<MpegHeader> FindMpegHeader(HANDLE file,
                                        const FileLayout& layout) {
    if (layout.body_end <= layout.body_begin) return std::nullopt;
    const std::uint64_t length = std::min<std::uint64_t>(
        layout.body_end - layout.body_begin, 2ULL * 1024ULL * 1024ULL);
    Bytes bytes;
    if (!ReadRange(file, layout.body_begin, length, bytes)) return std::nullopt;
    for (size_t index = 0; index + 4 <= bytes.size(); ++index) {
        const std::uint32_t header = ReadBe32(bytes.data() + index);
        if (const auto decoded = DecodeMpegHeader(header)) return decoded;
    }
    return std::nullopt;
}

HRESULT ReadMp3(const std::filesystem::path& path,
                const Mp3TagPolicy& policy, BuiltinFileInfo& result) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return HRESULT_FROM_WIN32(GetLastError());
    FileLayout layout;
    if (!ReadMp3Layout(file.Get(), layout))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const auto tags = MergeMp3Tags(layout, policy.read_priority);
    result.capabilities = 4U;
    result.codec = L"MPEG Audio";
    result.metadata = tags.fields;
    result.cover = tags.cover;
    if (const auto header = FindMpegHeader(file.Get(), layout)) {
        result.format.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
        result.format.nChannels = static_cast<WORD>(header->channels);
        result.format.nSamplesPerSec = header->sample_rate;
        result.format.wBitsPerSample = 16;
        result.format.nAvgBytesPerSec = header->bitrate_kbps * 125U;
        result.format.nBlockAlign = 1;
        result.encoded_bits_per_second = header->bitrate_kbps * 1000U;
        result.codec = L"MPEG " + std::to_wstring(header->version) +
            L" Layer " + std::to_wstring(header->layer);
        if (header->bitrate_kbps != 0 && layout.body_end > layout.body_begin) {
            const std::uint64_t milliseconds =
                (layout.body_end - layout.body_begin) * 8ULL /
                header->bitrate_kbps;
            result.duration_ms = static_cast<DWORD>(
                std::min<std::uint64_t>(milliseconds, MAXDWORD));
        }
    }
    return S_OK;
}

std::wstring RiffInfoName(std::string_view identifier) {
    if (identifier == "INAM") return L"Title";
    if (identifier == "IART") return L"Artist";
    if (identifier == "IPRD") return L"Album";
    if (identifier == "ITRK") return L"Tracknumber";
    if (identifier == "IGNR") return L"Genre";
    if (identifier == "ICRD") return L"Date";
    if (identifier == "ICMT") return L"Comment";
    return {};
}

HRESULT ReadWave(const std::filesystem::path& path, BuiltinFileInfo& result) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return HRESULT_FROM_WIN32(GetLastError());
    std::uint64_t file_size{};
    std::array<unsigned char, 12> riff{};
    if (!FileSize(file.Get(), file_size) || file_size < riff.size() ||
        !ReadAt(file.Get(), 0, riff) ||
        std::memcmp(riff.data(), "RIFF", 4) != 0 ||
        std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::uint64_t offset = 12;
    std::uint64_t data_bytes{};
    while (offset + 8 <= file_size) {
        std::array<unsigned char, 8> header{};
        if (!ReadAt(file.Get(), offset, header)) break;
        const std::uint32_t size = ReadLe32(header.data() + 4);
        const std::uint64_t begin = offset + 8;
        if (size > file_size - begin) break;
        const std::string identifier(
            reinterpret_cast<const char*>(header.data()), 4);
        if (identifier == "fmt " && size >= 16) {
            Bytes format;
            if (ReadRange(file.Get(), begin, std::min<std::uint32_t>(
                    size, static_cast<std::uint32_t>(sizeof(WAVEFORMATEX))),
                    format)) {
                std::memcpy(&result.format, format.data(),
                            std::min(format.size(), sizeof(result.format)));
            }
        } else if (identifier == "data") {
            data_bytes += size;
        } else if (identifier == "LIST" && size >= 4 &&
                   size <= kMaximumTagBytes) {
            Bytes list;
            if (ReadRange(file.Get(), begin, size, list) &&
                std::memcmp(list.data(), "INFO", 4) == 0) {
                size_t item = 4;
                while (item + 8 <= list.size()) {
                    const std::string key(
                        reinterpret_cast<const char*>(list.data() + item), 4);
                    const auto item_size = ReadLe32(list.data() + item + 4);
                    item += 8;
                    if (item_size > list.size() - item) break;
                    const auto name = RiffInfoName(key);
                    if (!name.empty())
                        result.metadata.push_back({name, TrimFixed(
                            std::span<const unsigned char>(list).subspan(
                                item, item_size))});
                    item += item_size + (item_size & 1U);
                }
            }
        }
        offset = begin + size + (size & 1U);
    }
    if (result.format.nAvgBytesPerSec != 0) {
        result.duration_ms = static_cast<DWORD>(std::min<std::uint64_t>(
            data_bytes * 1000ULL / result.format.nAvgBytesPerSec, MAXDWORD));
        result.encoded_bits_per_second = result.format.nAvgBytesPerSec * 8U;
    }
    result.codec = result.format.wFormatTag == WAVE_FORMAT_PCM
        ? L"PCM Audio" : L"WAVE Audio";
    return S_OK;
}

std::wstring PropertyText(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        PWSTR text{};
        if (SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text) {
            result = text;
            CoTaskMemFree(text);
        }
    }
    PropVariantClear(&value);
    return result;
}

std::uint64_t PropertyUInt64(IPropertyStore* store,
                             const PROPERTYKEY& key) {
    PROPVARIANT value{};
    PropVariantInit(&value);
    ULONGLONG result{};
    if (FAILED(store->GetValue(key, &value)) ||
        FAILED(PropVariantToUInt64(value, &result))) result = 0;
    PropVariantClear(&value);
    return result;
}

HRESULT ReadShellFallback(const std::filesystem::path& path,
                          BuiltinFileInfo& result) {
    ComPtr<IPropertyStore> store;
    const HRESULT opened = SHGetPropertyStoreFromParsingName(path.c_str(),
        nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(store.Put()));
    if (FAILED(opened) || !store.Get()) return opened;
    const auto add = [&result, &store](const PROPERTYKEY& key,
                                       const wchar_t* name) {
        auto value = PropertyText(store.Get(), key);
        if (!value.empty()) result.metadata.push_back({name, std::move(value)});
    };
    add(PKEY_Title, L"Title");
    add(PKEY_Music_Artist, L"Artist");
    add(PKEY_Music_AlbumTitle, L"Album");
    add(PKEY_Music_TrackNumber, L"Tracknumber");
    add(PKEY_Music_Genre, L"Genre");
    add(PKEY_Media_Year, L"Date");
    add(PKEY_Comment, L"Comment");
    result.encoded_bits_per_second = static_cast<DWORD>(
        std::min<std::uint64_t>(PropertyUInt64(
            store.Get(), PKEY_Audio_EncodingBitrate), MAXDWORD));
    result.format.nSamplesPerSec = static_cast<DWORD>(
        std::min<std::uint64_t>(PropertyUInt64(
            store.Get(), PKEY_Audio_SampleRate), MAXDWORD));
    result.format.nChannels = static_cast<WORD>(
        std::min<std::uint64_t>(PropertyUInt64(
            store.Get(), PKEY_Audio_ChannelCount), MAXWORD));
    if (result.encoded_bits_per_second != 0)
        result.format.nAvgBytesPerSec =
            result.encoded_bits_per_second / 8U;
    const std::uint64_t duration_100ns = PropertyUInt64(
        store.Get(), PKEY_Media_Duration);
    result.duration_ms = static_cast<DWORD>(std::min<std::uint64_t>(
        duration_100ns / 10000ULL, MAXDWORD));
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) {
            return value >= L'a' && value <= L'z'
                ? static_cast<wchar_t>(value - (L'a' - L'A')) : value;
        });
    if (!extension.empty() && extension.front() == L'.')
        extension.erase(extension.begin());
    result.codec = extension.empty() ? L"Audio" : extension + L" Audio";
    return S_OK;
}

unsigned char EffectiveId3Encoding(std::uint32_t configured) noexcept {
    // Exact 004DC59D mapping: 0 -> Latin-1, 3 -> UTF-8, every other nonzero
    // persisted value -> UTF-16.  In particular historical value 2 is not
    // silently reinterpreted as UTF-8 merely because the combo selects its
    // third visible row.
    return configured == 0 ? 0U : configured == 3 ? 3U : 1U;
}

void AppendTextFrame(Bytes& frames, unsigned char major,
                     std::string_view identifier, std::wstring_view value,
                     unsigned char encoding) {
    if (value.empty()) return;
    Bytes payload{encoding};
    auto encoded = EncodeText(value, encoding);
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    frames.insert(frames.end(), identifier.begin(), identifier.end());
    if (major == 4) AppendSynchsafe(frames,
        static_cast<std::uint32_t>(payload.size()));
    else AppendBe32(frames, static_cast<std::uint32_t>(payload.size()));
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), payload.begin(), payload.end());
}

void AppendCommentFrame(Bytes& frames, unsigned char major,
                        std::wstring_view value, unsigned char encoding) {
    if (value.empty()) return;
    Bytes payload{encoding, 'e', 'n', 'g'};
    const size_t terminator = EncodedTerminatorSize(encoding);
    payload.insert(payload.end(), terminator, 0);
    auto encoded = EncodeText(value, encoding);
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    frames.insert(frames.end(), {'C', 'O', 'M', 'M'});
    if (major == 4) AppendSynchsafe(frames,
        static_cast<std::uint32_t>(payload.size()));
    else AppendBe32(frames, static_cast<std::uint32_t>(payload.size()));
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), payload.begin(), payload.end());
}

void AppendUserTextFrame(Bytes& frames, unsigned char major,
                         std::wstring_view name, std::wstring_view value,
                         unsigned char encoding) {
    if (name.empty() || value.empty()) return;
    Bytes payload{encoding};
    auto description = EncodeText(name, encoding);
    payload.insert(payload.end(), description.begin(), description.end());
    payload.insert(payload.end(), EncodedTerminatorSize(encoding), 0);
    auto encoded = EncodeText(value, encoding);
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    frames.insert(frames.end(), {'T', 'X', 'X', 'X'});
    if (major == 4) AppendSynchsafe(frames,
        static_cast<std::uint32_t>(payload.size()));
    else AppendBe32(frames, static_cast<std::uint32_t>(payload.size()));
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), payload.begin(), payload.end());
}

bool IsStandardField(std::wstring_view name) {
    static constexpr std::wstring_view names[]{
        L"Title", L"Artist", L"Album", L"Tracknumber", L"Genre",
        L"Date", L"Comment"};
    return std::any_of(std::begin(names), std::end(names),
        [name](const auto candidate) {
            return WideAsciiEqual(name, candidate);
        });
}

void AppendPictureFrame(Bytes& frames, unsigned char major,
                        std::span<const unsigned char> image) {
    if (image.empty()) return;
    std::string_view mime = "image/jpeg";
    if (image.size() >= 8 && image[0] == 0x89U && image[1] == 'P' &&
        image[2] == 'N' && image[3] == 'G') mime = "image/png";
    else if (image.size() >= 2 && image[0] == 'B' && image[1] == 'M')
        mime = "image/bmp";
    else if (image.size() >= 3 && image[0] == 'G' && image[1] == 'I' &&
             image[2] == 'F')
        mime = "image/gif";
    Bytes payload{0};
    payload.insert(payload.end(), mime.begin(), mime.end());
    payload.push_back(0);
    payload.push_back(3); // front cover
    payload.push_back(0); // empty description
    payload.insert(payload.end(), image.begin(), image.end());
    frames.insert(frames.end(), {'A', 'P', 'I', 'C'});
    if (major == 4) AppendSynchsafe(frames,
        static_cast<std::uint32_t>(payload.size()));
    else AppendBe32(frames, static_cast<std::uint32_t>(payload.size()));
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), payload.begin(), payload.end());
}

Bytes BuildId3v2(const Id3Tag& previous, const TagData& fields,
                 const Mp3TagPolicy& policy,
                 BuiltinCoverAction cover_action) {
    const unsigned char encoding = EffectiveId3Encoding(
        policy.id3v2_encoding);
    const unsigned char major = static_cast<unsigned char>(previous.present &&
            (previous.major == 3 || previous.major == 4)
        ? previous.major : encoding == 3 ? 4U : 3U);
    Bytes frames;
    std::vector<std::wstring> retained_names;
    bool retained_picture{};
    for (const auto& frame : previous.frames) {
        if (!frame.semantic.empty()) continue;
        const bool picture = frame.identifier == "APIC" ||
                             frame.identifier == "PIC";
        if (picture && cover_action != BuiltinCoverAction::unchanged)
            continue;
        frames.insert(frames.end(), frame.raw.begin(), frame.raw.end());
        if (!frame.metadata_name.empty())
            retained_names.push_back(frame.metadata_name);
        retained_picture = retained_picture || picture;
    }
    AppendTextFrame(frames, major, "TIT2", GetField(fields, L"Title"),
                    encoding);
    AppendTextFrame(frames, major, "TPE1", GetField(fields, L"Artist"),
                    encoding);
    AppendTextFrame(frames, major, "TALB", GetField(fields, L"Album"),
                    encoding);
    AppendTextFrame(frames, major, "TRCK", GetField(fields, L"Tracknumber"),
                    encoding);
    AppendTextFrame(frames, major, "TCON", GetField(fields, L"Genre"),
                    encoding);
    const auto date = GetField(fields, L"Date");
    AppendTextFrame(frames, major,
        date.find(L'-') == std::wstring::npos ? "TYER" : "TDRC", date,
        encoding);
    AppendCommentFrame(frames, major, GetField(fields, L"Comment"), encoding);
    for (const auto& field : fields.fields) {
        if (field.value.empty() || IsStandardField(field.name) ||
            std::any_of(retained_names.begin(), retained_names.end(),
                [&field](const auto& name) {
                    return WideAsciiEqual(name, field.name);
                }))
            continue;
        AppendUserTextFrame(frames, major, field.name, field.value, encoding);
    }
    if (!retained_picture) AppendPictureFrame(frames, major, fields.cover);

    std::uint64_t target_total = 10ULL + frames.size();
    if (policy.id3v2_padding) {
        const bool has_metadata = std::any_of(fields.fields.begin(),
            fields.fields.end(), [](const auto& field) {
                return !field.value.empty();
            });
        // 004DCA1E chooses 0x800 for a nonempty media-info vector and 0x400
        // otherwise, then never shrinks an existing ID3v2 allocation.
        target_total = std::max<std::uint64_t>(
            target_total, has_metadata ? 0x800U : 0x400U);
        if (previous.present)
            target_total = std::max(target_total, previous.total_size);
    }
    if (target_total - 10ULL > 0x0fffffffULL)
        return {};
    frames.resize(static_cast<size_t>(target_total - 10ULL), 0);
    Bytes output{'I', 'D', '3', major, 0, 0};
    AppendSynchsafe(output, static_cast<std::uint32_t>(frames.size()));
    output.insert(output.end(), frames.begin(), frames.end());
    return output;
}

void AppendApeTextItem(Bytes& items, std::string_view key,
                       std::wstring_view value) {
    if (value.empty()) return;
    const auto encoded = EncodeUtf8(value);
    AppendLe32(items, static_cast<std::uint32_t>(encoded.size()));
    AppendLe32(items, 0);
    items.insert(items.end(), key.begin(), key.end());
    items.push_back(0);
    items.insert(items.end(), encoded.begin(), encoded.end());
}

void AppendApeCoverItem(Bytes& items,
                        std::span<const unsigned char> image) {
    if (image.empty()) return;
    static constexpr std::string_view key = "Cover Art (Front)";
    static constexpr std::string_view filename = "cover";
    const auto value_size = static_cast<std::uint32_t>(
        filename.size() + 1U + image.size());
    AppendLe32(items, value_size);
    AppendLe32(items, 2U); // binary item
    items.insert(items.end(), key.begin(), key.end());
    items.push_back(0);
    items.insert(items.end(), filename.begin(), filename.end());
    items.push_back(0);
    items.insert(items.end(), image.begin(), image.end());
}

Bytes ApeBlock(bool header, std::uint32_t size,
               std::uint32_t count) {
    Bytes block{'A','P','E','T','A','G','E','X'};
    AppendLe32(block, 2000U);
    AppendLe32(block, size);
    AppendLe32(block, count);
    AppendLe32(block, header ? 0xa0000000U : 0x80000000U);
    AppendLe32(block, 0);
    AppendLe32(block, 0);
    return block;
}

Bytes BuildApe(const ApeTag& previous, const TagData& fields,
               BuiltinCoverAction cover_action) {
    Bytes items;
    std::uint32_t count{};
    bool retained_cover{};
    for (const auto& item : previous.items) {
        if (!item.semantic.empty()) continue;
        if (item.cover && cover_action != BuiltinCoverAction::unchanged)
            continue;
        items.insert(items.end(), item.raw.begin(), item.raw.end());
        retained_cover = retained_cover || item.cover;
        ++count;
    }
    const auto add = [&items, &count, &fields](std::string_view key,
                                               std::wstring_view name) {
        const auto value = GetField(fields, name);
        if (value.empty()) return;
        AppendApeTextItem(items, key, value);
        ++count;
    };
    add("Title", L"Title");
    add("Artist", L"Artist");
    add("Album", L"Album");
    add("Track", L"Tracknumber");
    add("Genre", L"Genre");
    add("Year", L"Date");
    add("Comment", L"Comment");
    if (!retained_cover && !fields.cover.empty()) {
        AppendApeCoverItem(items, fields.cover);
        ++count;
    }
    const std::uint64_t tag_size = items.size() + 32ULL;
    if (tag_size > std::numeric_limits<std::uint32_t>::max()) return {};
    const auto size = static_cast<std::uint32_t>(tag_size);
    auto output = ApeBlock(true, size, count);
    output.insert(output.end(), items.begin(), items.end());
    auto footer = ApeBlock(false, size, count);
    output.insert(output.end(), footer.begin(), footer.end());
    return output;
}

void EncodeFixedAnsi(std::span<unsigned char> destination,
                     std::wstring_view value) {
    std::fill(destination.begin(), destination.end(),
              static_cast<unsigned char>(0));
    if (value.empty()) return;
    const int count = WideCharToMultiByte(CP_ACP, 0, value.data(),
        static_cast<int>(std::min<size_t>(value.size(), INT_MAX)),
        reinterpret_cast<char*>(destination.data()),
        static_cast<int>(destination.size()), nullptr, nullptr);
    static_cast<void>(count);
}

Bytes BuildId3v1(const TagData& fields) {
    Bytes output(128, 0);
    std::memcpy(output.data(), "TAG", 3);
    EncodeFixedAnsi(std::span<unsigned char>(output).subspan(3, 30),
                    GetField(fields, L"Title"));
    EncodeFixedAnsi(std::span<unsigned char>(output).subspan(33, 30),
                    GetField(fields, L"Artist"));
    EncodeFixedAnsi(std::span<unsigned char>(output).subspan(63, 30),
                    GetField(fields, L"Album"));
    EncodeFixedAnsi(std::span<unsigned char>(output).subspan(93, 4),
                    GetField(fields, L"Date"));
    const auto track_text = GetField(fields, L"Tracknumber");
    wchar_t* end{};
    const long track = std::wcstol(track_text.c_str(), &end, 10);
    if (end != track_text.c_str() && track > 0 && track <= 255) {
        EncodeFixedAnsi(std::span<unsigned char>(output).subspan(97, 28),
                        GetField(fields, L"Comment"));
        output[125] = 0;
        output[126] = static_cast<unsigned char>(track);
    } else {
        EncodeFixedAnsi(std::span<unsigned char>(output).subspan(97, 30),
                        GetField(fields, L"Comment"));
    }
    const auto genre = GetField(fields, L"Genre");
    end = nullptr;
    const long genre_number = std::wcstol(genre.c_str(), &end, 10);
    output[127] = end != genre.c_str() && genre_number >= 0 &&
            genre_number <= 255
        ? static_cast<unsigned char>(genre_number) : 0xffU;
    return output;
}

bool WriteAll(HANDLE file, std::span<const unsigned char> value) noexcept {
    size_t done{};
    while (done < value.size()) {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            value.size() - done, static_cast<size_t>(MAXDWORD)));
        DWORD written{};
        if (!WriteFile(file, value.data() + done, request, &written, nullptr) ||
            written == 0)
            return false;
        done += written;
    }
    return true;
}

bool CopyRange(HANDLE source, HANDLE destination, std::uint64_t offset,
               std::uint64_t length) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(source, position, nullptr, FILE_BEGIN)) return false;
    Bytes buffer(1024U * 1024U);
    while (length != 0) {
        const DWORD wanted = static_cast<DWORD>(std::min<std::uint64_t>(
            length, buffer.size()));
        DWORD read{};
        if (!ReadFile(source, buffer.data(), wanted, &read, nullptr) ||
            read == 0)
            return false;
        if (!WriteAll(destination,
                std::span<const unsigned char>(buffer.data(), read)))
            return false;
        length -= read;
    }
    return true;
}

std::filesystem::path CreateSiblingTemporary(
    const std::filesystem::path& path, UniqueHandle& file) {
    static std::atomic_uint sequence{};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto temporary = path;
        temporary += L".ttplayer-tag-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(sequence.fetch_add(
                1, std::memory_order_relaxed)) + L".tmp";
        file.Reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file) return temporary;
        if (GetLastError() != ERROR_FILE_EXISTS) return {};
    }
    return {};
}

HRESULT ReplaceWithTags(const std::filesystem::path& path, HANDLE source,
                        const FileLayout& layout, const Bytes& id3v2,
                        const Bytes& ape, const Bytes& id3v1) {
    UniqueHandle output;
    const auto temporary = CreateSiblingTemporary(path, output);
    if (temporary.empty()) return HRESULT_FROM_WIN32(GetLastError());
    bool okay = WriteAll(output.Get(), id3v2) &&
        CopyRange(source, output.Get(), layout.body_begin,
                  layout.body_end - layout.body_begin) &&
        WriteAll(output.Get(), ape) && WriteAll(output.Get(), id3v1) &&
        FlushFileBuffers(output.Get());
    output.Reset();
    if (!okay) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        return HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT);
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(temporary.c_str(),
            attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!ReplaceFileW(path.c_str(), temporary.c_str(), nullptr,
            REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(temporary.c_str());
            return HRESULT_FROM_WIN32(error);
        }
    }
    if (attributes != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(path.c_str(), attributes);
    return S_OK;
}

HRESULT WriteMp3(const std::filesystem::path& path,
                 const Mp3TagPolicy& policy,
                 std::span<const BuiltinTagWriteField> requested,
                 std::vector<HRESULT>& field_results,
                 BuiltinCoverAction cover_action,
                 std::span<const unsigned char> cover) {
    UniqueHandle source(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!source) return HRESULT_FROM_WIN32(GetLastError());
    FileLayout layout;
    if (!ReadMp3Layout(source.Get(), layout))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    TagData fields = MergeMp3Tags(layout, policy.read_priority);
    bool has_supported = cover_action != BuiltinCoverAction::unchanged;
    field_results.reserve(requested.size());
    for (const auto& request : requested) {
        const auto canonical = CanonicalField(request.name);
        if (canonical.empty()) {
            field_results.push_back(E_NOTIMPL);
            continue;
        }
        SetField(fields, canonical, request.value);
        field_results.push_back(S_OK);
        has_supported = true;
    }
    if (cover_action == BuiltinCoverAction::replace) {
        if (cover.empty()) return E_INVALIDARG;
        fields.cover.assign(cover.begin(), cover.end());
    } else if (cover_action == BuiltinCoverAction::remove) {
        fields.cover.clear();
    }
    if (!has_supported) return requested.empty() ? S_OK : E_NOTIMPL;

    std::uint32_t write_type = policy.write_type & 0x0dU;
    if (write_type == 0) return E_INVALIDARG;
    const bool has_metadata = std::any_of(fields.fields.begin(),
        fields.fields.end(), [](const auto& field) {
            return !field.value.empty();
        });
    // 004D9B56 (004D9C09..004D9C28) forces APEv2 when nonempty metadata
    // cannot be represented by the requested ID3v1-only write.
    if (has_metadata && (write_type & 8U) == 0) write_type |= 4U;
    // A picture cannot be represented by ID3v1.  The original built-in MP3
    // writer promotes that policy to its APEv2-capable transaction just as it
    // does for extended text metadata.
    if (cover_action != BuiltinCoverAction::unchanged &&
        (write_type & (8U | 4U)) == 0)
        write_type |= 4U;
    if ((write_type & 8U) != 0 && layout.id3v2.present &&
        (!layout.id3v2.rewrite_safe || layout.id3v2.major == 2))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    Bytes id3v2;
    Bytes ape;
    Bytes id3v1;
    try {
        if (write_type & 8U)
            id3v2 = BuildId3v2(layout.id3v2, fields, policy, cover_action);
        if (write_type & 4U)
            ape = BuildApe(layout.ape, fields, cover_action);
        if (write_type & 1U) id3v1 = BuildId3v1(fields);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if ((write_type & 8U) && id3v2.empty()) return E_OUTOFMEMORY;
    if ((write_type & 4U) && ape.empty()) return E_OUTOFMEMORY;
    return ReplaceWithTags(path, source.Get(), layout, id3v2, ape, id3v1);
}

bool ExtensionIs(const std::filesystem::path& path,
                 std::wstring_view extension) {
    const auto actual = path.extension().wstring();
    return WideAsciiEqual(actual, extension);
}

} // namespace

HRESULT ReadBuiltinFileInfo(const std::filesystem::path& path,
                            const Mp3TagPolicy& policy,
                            BuiltinFileInfo& result) noexcept {
    try {
        BuiltinFileInfo decoded;
        HRESULT status = E_NOINTERFACE;
        if (ExtensionIs(path, L".mp3"))
            status = ReadMp3(path, policy, decoded);
        else if (ExtensionIs(path, L".wav") || ExtensionIs(path, L".wave"))
            status = ReadWave(path, decoded);
        if (FAILED(status)) status = ReadShellFallback(path, decoded);
        if (SUCCEEDED(status)) result = std::move(decoded);
        return status;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
}

BuiltinTagWriteResult WriteBuiltinFileInfo(
    const std::filesystem::path& path, const Mp3TagPolicy& policy,
    std::span<const BuiltinTagWriteField> fields,
    BuiltinCoverAction cover_action,
    std::span<const unsigned char> cover) noexcept {
    BuiltinTagWriteResult result;
    try {
        if (!ExtensionIs(path, L".mp3")) {
            result.status = E_NOINTERFACE;
            result.fields.assign(fields.size(), E_NOINTERFACE);
            if (cover_action != BuiltinCoverAction::unchanged)
                result.cover_status = E_NOINTERFACE;
            return result;
        }
        result.status = WriteMp3(path, policy, fields, result.fields,
                                 cover_action, cover);
        if (cover_action != BuiltinCoverAction::unchanged)
            result.cover_status = result.status;
        if (FAILED(result.status)) {
            for (auto& field : result.fields) {
                if (SUCCEEDED(field)) field = result.status;
            }
        }
    } catch (const std::bad_alloc&) {
        result.status = E_OUTOFMEMORY;
        result.fields.assign(fields.size(), E_OUTOFMEMORY);
        if (cover_action != BuiltinCoverAction::unchanged)
            result.cover_status = E_OUTOFMEMORY;
    } catch (...) {
        result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        result.fields.assign(fields.size(), result.status);
        if (cover_action != BuiltinCoverAction::unchanged)
            result.cover_status = result.status;
    }
    return result;
}

} // namespace ttplayer::audio
