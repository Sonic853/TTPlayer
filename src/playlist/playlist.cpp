#include "ttplayer/playlist/playlist.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <windows.h>

namespace ttplayer::playlist {
int CompareLegacyLogicalText(const wchar_t* left,
                             const wchar_t* right) noexcept {
    using StrCmpLogicalWFn = int (WINAPI*)(PCWSTR, PCWSTR);
    static const StrCmpLogicalWFn logical = [] {
        const HMODULE shlwapi = GetModuleHandleW(L"shlwapi.dll");
        return shlwapi ? reinterpret_cast<StrCmpLogicalWFn>(
            GetProcAddress(shlwapi, "StrCmpLogicalW")) : nullptr;
    }();
    const wchar_t* const lhs = left ? left : L"";
    const wchar_t* const rhs = right ? right : L"";
    return logical ? logical(lhs, rhs) : lstrcmpiW(lhs, rhs);
}

namespace {
template <typename T> T ReadValue(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw std::runtime_error("truncated TTBL");
    return value;
}

template <typename T> void WriteValue(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) throw std::runtime_error("cannot write TTBL");
}

std::wstring ReadTtblString(std::istream& input) {
    const auto bytes = ReadValue<std::uint32_t>(input);
    if ((bytes & 1U) != 0 || bytes > 16U * 1024U * 1024U)
        throw std::runtime_error("invalid TTBL string");
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (bytes != 0) input.read(reinterpret_cast<char*>(value.data()), bytes);
    if (!input) throw std::runtime_error("truncated TTBL string");
    return value;
}

std::string ReadTtblNarrowString(std::istream& input) {
    const auto bytes = ReadValue<std::uint32_t>(input);
    if (bytes > 16U * 1024U * 1024U)
        throw std::runtime_error("invalid TTBL narrow string");
    std::string value(bytes, '\0');
    if (bytes != 0) input.read(value.data(), bytes);
    if (!input) throw std::runtime_error("truncated TTBL narrow string");
    return value;
}

void WriteTtblString(std::ostream& output, std::wstring_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(wchar_t))
        throw std::runtime_error("TTBL string too long");
    const auto bytes = static_cast<std::uint32_t>(value.size() * sizeof(wchar_t));
    WriteValue(output, bytes);
    if (bytes != 0) output.write(reinterpret_cast<const char*>(value.data()), bytes);
    if (!output) throw std::runtime_error("cannot write TTBL string");
}

void WriteTtblNarrowString(std::ostream& output, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("TTBL narrow string too long");
    WriteValue(output, static_cast<std::uint32_t>(value.size()));
    if (!value.empty())
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) throw std::runtime_error("cannot write TTBL narrow string");
}

bool EqualsAsciiInsensitive(std::string_view left,
                            std::string_view right) noexcept;

std::wstring Utf8Field(const std::string& value) {
    if (value.empty()) return {};
    try { return core::Utf8ToWide(value); }
    catch (const std::exception&) { return {}; }
}

std::wstring MetadataField(const Track& track, std::string_view name) {
    const auto found = std::find_if(track.metadata.begin(), track.metadata.end(),
        [name](const auto& entry) {
            return EqualsAsciiInsensitive(entry.first, name);
        });
    return found == track.metadata.end() ? std::wstring{} :
        Utf8Field(found->second);
}

std::string WideField(const std::wstring& value) {
    if (value.empty()) return {};
    try { return core::WideToUtf8(value); }
    catch (const std::exception&) { return {}; }
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    return extension;
}

bool IsXmlWhitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool EqualsAsciiInsensitive(std::string_view left,
                            std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

void AppendUtf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        codepoint = 0xfffdU;
    }
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

std::string DecodeXmlEntities(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t offset = 0; offset < value.size();) {
        if (value[offset] != '&') {
            decoded.push_back(value[offset++]);
            continue;
        }
        const size_t semicolon = value.find(';', offset + 1);
        if (semicolon == std::string_view::npos) {
            decoded.push_back(value[offset++]);
            continue;
        }
        const auto entity = value.substr(offset + 1, semicolon - offset - 1);
        if (entity == "amp") decoded.push_back('&');
        else if (entity == "lt") decoded.push_back('<');
        else if (entity == "gt") decoded.push_back('>');
        else if (entity == "quot") decoded.push_back('"');
        else if (entity == "apos") decoded.push_back('\'');
        else if (!entity.empty() && entity.front() == '#') {
            const bool hexadecimal = entity.size() > 2 &&
                (entity[1] == 'x' || entity[1] == 'X');
            const auto digits = entity.substr(hexadecimal ? 2 : 1);
            std::uint32_t codepoint{};
            const auto parsed = std::from_chars(
                digits.data(), digits.data() + digits.size(), codepoint,
                hexadecimal ? 16 : 10);
            if (digits.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != digits.data() + digits.size()) {
                decoded.append(value.substr(offset, semicolon - offset + 1));
            } else {
                AppendUtf8(decoded, codepoint);
            }
        } else {
            decoded.append(value.substr(offset, semicolon - offset + 1));
        }
        offset = semicolon + 1;
    }
    return decoded;
}

struct XmlTag {
    bool closing{};
    bool self_closing{};
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
};

class XmlTagReader {
public:
    explicit XmlTagReader(std::string_view source) : source_(source) {}

    bool Next(XmlTag& tag) {
        for (;;) {
            const size_t opening = source_.find('<', offset_);
            if (opening == std::string_view::npos) return false;
            offset_ = opening + 1;
            if (source_.substr(offset_, 3) == "!--") {
                const size_t end = source_.find("-->", offset_ + 3);
                if (end == std::string_view::npos) Invalid();
                offset_ = end + 3;
                continue;
            }
            if (source_.substr(offset_, 8) == "![CDATA[") {
                const size_t end = source_.find("]]>", offset_ + 8);
                if (end == std::string_view::npos) Invalid();
                offset_ = end + 3;
                continue;
            }
            if (offset_ < source_.size() && source_[offset_] == '?') {
                const size_t end = source_.find("?>", offset_ + 1);
                if (end == std::string_view::npos) Invalid();
                offset_ = end + 2;
                continue;
            }
            if (offset_ < source_.size() && source_[offset_] == '!') {
                SkipDeclaration();
                continue;
            }
            break;
        }

        tag = {};
        SkipWhitespace();
        if (offset_ < source_.size() && source_[offset_] == '/') {
            tag.closing = true;
            ++offset_;
            SkipWhitespace();
        }
        tag.name = ReadName();
        if (tag.name.empty()) Invalid();
        if (tag.closing) {
            SkipWhitespace();
            if (offset_ >= source_.size() || source_[offset_] != '>') Invalid();
            ++offset_;
            return true;
        }

        for (;;) {
            SkipWhitespace();
            if (offset_ >= source_.size()) Invalid();
            if (source_[offset_] == '>') {
                ++offset_;
                return true;
            }
            if (source_[offset_] == '/') {
                ++offset_;
                SkipWhitespace();
                if (offset_ >= source_.size() || source_[offset_] != '>') Invalid();
                ++offset_;
                tag.self_closing = true;
                return true;
            }

            auto name = ReadName();
            if (name.empty()) Invalid();
            SkipWhitespace();
            if (offset_ >= source_.size() || source_[offset_] != '=') Invalid();
            ++offset_;
            SkipWhitespace();
            if (offset_ >= source_.size()) Invalid();

            std::string value;
            const char quote = source_[offset_];
            if (quote == '"' || quote == '\'') {
                const size_t begin = ++offset_;
                const size_t end = source_.find(quote, begin);
                if (end == std::string_view::npos) Invalid();
                value.assign(source_.substr(begin, end - begin));
                offset_ = end + 1;
            } else {
                const size_t begin = offset_;
                while (offset_ < source_.size() &&
                       !IsXmlWhitespace(source_[offset_]) &&
                       source_[offset_] != '>' && source_[offset_] != '/') {
                    ++offset_;
                }
                if (begin == offset_) Invalid();
                value.assign(source_.substr(begin, offset_ - begin));
            }
            tag.attributes.emplace_back(std::move(name),
                                        DecodeXmlEntities(value));
        }
    }

private:
    [[noreturn]] static void Invalid() {
        throw std::runtime_error("malformed TTPlayer XML playlist");
    }

    void SkipWhitespace() noexcept {
        while (offset_ < source_.size() && IsXmlWhitespace(source_[offset_]))
            ++offset_;
    }

    std::string ReadName() {
        const size_t begin = offset_;
        while (offset_ < source_.size() &&
               !IsXmlWhitespace(source_[offset_]) &&
               source_[offset_] != '=' && source_[offset_] != '/' &&
               source_[offset_] != '>') {
            ++offset_;
        }
        return std::string(source_.substr(begin, offset_ - begin));
    }

    void SkipDeclaration() {
        char quote{};
        int subset_depth{};
        while (offset_ < source_.size()) {
            const char value = source_[offset_++];
            if (quote != '\0') {
                if (value == quote) quote = '\0';
            } else if (value == '"' || value == '\'') {
                quote = value;
            } else if (value == '[') {
                ++subset_depth;
            } else if (value == ']') {
                subset_depth = std::max(0, subset_depth - 1);
            } else if (value == '>' && subset_depth == 0) {
                return;
            }
        }
        Invalid();
    }

    std::string_view source_;
    size_t offset_{};
};

const std::string* FindAttribute(const XmlTag& tag, std::string_view name) {
    const auto found = std::find_if(tag.attributes.begin(), tag.attributes.end(),
        [name](const auto& attribute) { return attribute.first == name; });
    return found == tag.attributes.end() ? nullptr : &found->second;
}

int XmlInteger(const XmlTag& tag, std::string_view name, int fallback) {
    const auto* value = FindAttribute(tag, name);
    if (value == nullptr) return fallback;
    const char* first = value->data();
    const char* last = first + value->size();
    while (first != last && IsXmlWhitespace(*first)) ++first;
    bool explicit_plus{};
    if (first != last && *first == '+') {
        explicit_plus = true;
        ++first;
    }
    int parsed{};
    const auto result = std::from_chars(first, last, parsed);
    if (result.ptr == first || result.ec != std::errc{}) return fallback;
    return explicit_plus && parsed < 0 ? fallback : parsed;
}

bool IsUrl(std::wstring_view value) noexcept {
    const size_t separator = value.find(L"://");
    return separator != std::wstring_view::npos && separator != 0;
}

bool KeepImportedPath(const std::filesystem::path& path,
                      const LoadOptions& options) {
    if (!options.ignore_bad_files || IsUrl(path.native())) return true;
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::string ReadBinaryText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open M3U file");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::wstring DecodeM3uField(std::string_view value, UINT code_page) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("M3U line is too long");
    const int source_length = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(code_page, 0, value.data(),
                                            source_length, nullptr, 0);
    if (length <= 0) throw std::runtime_error("invalid M3U text encoding");
    std::wstring decoded(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(code_page, 0, value.data(), source_length,
                            decoded.data(), length) != length) {
        throw std::runtime_error("invalid M3U text encoding");
    }
    return decoded;
}

std::string EncodeM3uField(std::wstring_view value, UINT code_page) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("M3U field is too long");
    const int source_length = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(code_page, 0, value.data(),
                                            source_length, nullptr, 0,
                                            nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("cannot encode M3U text");
    std::string encoded(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(code_page, 0, value.data(), source_length,
                            encoded.data(), length, nullptr, nullptr) != length) {
        throw std::runtime_error("cannot encode M3U text");
    }
    return encoded;
}

std::filesystem::path ResolveM3uTrackPath(
    const std::filesystem::path& base, std::wstring_view value) {
    if (IsUrl(value)) return std::filesystem::path(value);
    std::filesystem::path resolved(value);
    if (resolved.is_relative()) resolved = base / resolved;
    return resolved.lexically_normal();
}

bool AppendCueTracks(std::vector<Track>& target,
                     const std::filesystem::path& cue_path) {
    try {
        const auto sheet = audio::CueSheet::Load(cue_path);
        for (const auto& cue_track : sheet.Tracks()) {
            const auto duration = cue_track.DurationMilliseconds();
            target.push_back({
                cue_path,
                cue_track.title.empty() ? std::string{} :
                    core::WideToUtf8(cue_track.title),
                cue_track.performer.empty() ? std::string{} :
                    core::WideToUtf8(cue_track.performer),
                duration < 0 || duration > std::numeric_limits<int>::max() ?
                    -1 : static_cast<int>(duration),
                cue_track.number,
                sheet.Title().empty() ? std::string{} :
                    core::WideToUtf8(sheet.Title())});
        }
        return !sheet.Tracks().empty();
    } catch (const std::exception&) {
        return false;
    }
}

std::filesystem::path ResolveXmlTrackPath(
    const std::filesystem::path& base, std::string_view encoded) {
    const auto wide = core::Utf8ToWide(encoded);
    std::filesystem::path resolved(wide);
    if (!IsUrl(wide) && resolved.is_relative()) resolved = base / resolved;
    return IsUrl(wide) ? resolved : resolved.lexically_normal();
}

std::string EscapeXmlAttribute(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::wstring XmlStoredTrackPath(const std::filesystem::path& output_path,
                                const std::filesystem::path& track_path,
                                bool save_relative_path) {
    if (IsUrl(track_path.native())) return track_path.wstring();

    std::error_code output_error;
    std::error_code track_error;
    auto output = std::filesystem::absolute(output_path, output_error);
    auto track = std::filesystem::absolute(track_path, track_error);
    if (output_error) output = output_path;
    if (track_error) track = track_path;
    auto base = output.parent_path().wstring();
    auto value = track.lexically_normal().wstring();
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
        base.push_back(L'\\');
    // 00475486 strips only the output directory's case-insensitive prefix;
    // it does not manufacture .. components for media outside that tree.
    if (save_relative_path && !base.empty() && value.size() >= base.size() &&
        _wcsnicmp(value.c_str(), base.c_str(), base.size()) == 0) {
        value.erase(0, base.size());
    }
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

struct PendingXmlTrack {
    Track track;
};

void UpsertMetadata(Track& track, std::string name, std::string value) {
    const auto found = std::find_if(track.metadata.begin(),
        track.metadata.end(), [&name](const auto& entry) {
            return EqualsAsciiInsensitive(entry.first, name);
        });
    if (found == track.metadata.end()) {
        track.metadata.emplace_back(std::move(name), std::move(value));
    } else {
        found->second = std::move(value);
    }
}

void ApplyXmlMetadata(PendingXmlTrack& pending, std::string_view name,
                      const std::string& value, bool retain = true) {
    if (value.empty()) return;
    if (retain)
        UpsertMetadata(pending.track, std::string(name), value);
    if (EqualsAsciiInsensitive(name, "Title") ||
        EqualsAsciiInsensitive(name, "SongName")) {
        pending.track.title = value;
    } else if (EqualsAsciiInsensitive(name, "Artist") ||
               EqualsAsciiInsensitive(name, "SongArtist")) {
        pending.track.artist = value;
    } else if (EqualsAsciiInsensitive(name, "Album") ||
               EqualsAsciiInsensitive(name, "AlbumTitle") ||
               EqualsAsciiInsensitive(name, "SongAlbum")) {
        pending.track.album = value;
    } else if (EqualsAsciiInsensitive(name, "Tracknumber") ||
               EqualsAsciiInsensitive(name, "Track")) {
        int parsed{};
        const auto result = std::from_chars(value.data(),
            value.data() + value.size(), parsed);
        if (result.ptr != value.data() && parsed >= 0)
            pending.track.track_number = parsed;
    } else if (EqualsAsciiInsensitive(name, "Rating")) {
        int parsed{};
        const auto result = std::from_chars(value.data(),
            value.data() + value.size(), parsed);
        if (result.ptr != value.data())
            pending.track.rating = std::clamp(parsed, 0, 5);
    }
}

std::optional<size_t> RemapRow(std::optional<size_t> row,
                               const std::vector<size_t>& order) {
    if (!row || *row >= order.size()) return std::nullopt;
    const auto found = std::find(order.begin(), order.end(), *row);
    return found == order.end() ? std::nullopt :
        std::optional<size_t>{static_cast<size_t>(found - order.begin())};
}

void ApplyTrackOrder(std::vector<Track>& tracks,
                     const std::vector<size_t>& order) {
    std::vector<Track> reordered;
    reordered.reserve(order.size());
    for (const size_t index : order)
        reordered.push_back(std::move(tracks[index]));
    tracks = std::move(reordered);
}
} // namespace

void Playlist::Add(Track track) { tracks_.push_back(std::move(track)); }
void Playlist::Insert(size_t index, Track track) {
    index = std::min(index, tracks_.size());
    tracks_.insert(tracks_.begin() + static_cast<ptrdiff_t>(index), std::move(track));
    if (playing_row_ && index <= *playing_row_) ++*playing_row_;
    if (current_row_ && index <= *current_row_) ++*current_row_;
}
void Playlist::InsertRange(size_t index, std::vector<Track> tracks) {
    if (tracks.empty()) return;
    index = std::min(index, tracks_.size());
    const size_t count = tracks.size();
    tracks_.insert(tracks_.begin() + static_cast<ptrdiff_t>(index),
                   std::make_move_iterator(tracks.begin()),
                   std::make_move_iterator(tracks.end()));
    if (playing_row_ && index <= *playing_row_) *playing_row_ += count;
    if (current_row_ && index <= *current_row_) *current_row_ += count;
}
bool Playlist::Remove(size_t index) {
    if (index >= tracks_.size()) return false;
    tracks_.erase(tracks_.begin() + static_cast<ptrdiff_t>(index));
    if (playing_row_) {
        if (index < *playing_row_) --*playing_row_;
        else if (index == *playing_row_) playing_row_.reset();
    }
    if (current_row_) {
        if (tracks_.empty()) current_row_.reset();
        else if (index < *current_row_) --*current_row_;
        else if (index == *current_row_ && *current_row_ >= tracks_.size())
            *current_row_ = tracks_.size() - 1;
    }
    return true;
}
void Playlist::Clear() {
    tracks_.clear();
    playing_row_.reset();
    current_row_.reset();
}

bool Playlist::SetCurrentRow(std::optional<size_t> row) noexcept {
    if (row && *row >= tracks_.size()) row.reset();
    if (current_row_ == row) return false;
    current_row_ = row;
    return true;
}

bool Playlist::SetPlayingRow(std::optional<size_t> row) noexcept {
    if (row && *row >= tracks_.size()) row.reset();
    if (playing_row_ == row) return false;
    playing_row_ = row;
    return true;
}

std::vector<size_t> Playlist::Sort(
    SortKey key, bool ascending,
    std::span<const std::wstring> display_titles) {
    std::vector<size_t> order(tracks_.size());
    std::iota(order.begin(), order.end(), size_t{});
    std::vector<std::filesystem::file_time_type> file_times;
    std::vector<bool> file_time_valid;
    if (key == SortKey::file_time) {
        file_times.resize(tracks_.size());
        file_time_valid.resize(tracks_.size());
        for (size_t index = 0; index < tracks_.size(); ++index) {
            std::error_code error;
            file_times[index] = std::filesystem::last_write_time(
                tracks_[index].path, error);
            file_time_valid[index] = !error;
        }
    }
    const auto compare_text = [ascending](const std::wstring& left,
                                          const std::wstring& right) {
        const int result = CompareLegacyLogicalText(
            left.c_str(), right.c_str());
        return ascending ? result < 0 : result > 0;
    };
    const auto compare_number = [ascending](auto left, auto right) {
        return ascending ? left < right : left > right;
    };
    const bool projected_titles = display_titles.size() == tracks_.size();
    std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
        const auto& lhs = tracks_[left];
        const auto& rhs = tracks_[right];
        switch (key) {
        case SortKey::display_title: {
            const auto left_name = projected_titles ? display_titles[left] :
                (lhs.title.empty() ? lhs.path.stem().wstring() :
                                     Utf8Field(lhs.title));
            const auto right_name = projected_titles ? display_titles[right] :
                (rhs.title.empty() ? rhs.path.stem().wstring() :
                                     Utf8Field(rhs.title));
            return compare_text(left_name, right_name);
        }
        case SortKey::file_name:
            return compare_text(lhs.path.filename().wstring(),
                                rhs.path.filename().wstring());
        case SortKey::path: {
            const int path_order = CompareLegacyLogicalText(
                lhs.path.c_str(), rhs.path.c_str());
            if (path_order != 0)
                return ascending ? path_order < 0 : path_order > 0;
            return compare_number(lhs.subtrack, rhs.subtrack);
        }
        case SortKey::album: {
            auto left_album = MetadataField(lhs, "Album");
            auto right_album = MetadataField(rhs, "Album");
            if (left_album.empty()) left_album = Utf8Field(lhs.album);
            if (right_album.empty()) right_album = Utf8Field(rhs.album);
            const int album_order = CompareLegacyLogicalText(
                left_album.c_str(), right_album.c_str());
            if (album_order != 0)
                return ascending ? album_order < 0 : album_order > 0;
            return compare_number(lhs.track_number, rhs.track_number);
        }
        case SortKey::rating:
            return compare_number(lhs.rating, rhs.rating);
        case SortKey::file_time: {
            if (file_time_valid[left] != file_time_valid[right])
                return ascending ? static_cast<bool>(file_time_valid[left]) :
                                   static_cast<bool>(file_time_valid[right]);
            if (!file_time_valid[left]) return false;
            return compare_number(file_times[left], file_times[right]);
        }
        case SortKey::track_number:
            return compare_number(lhs.track_number, rhs.track_number);
        case SortKey::duration:
            return compare_number(lhs.duration_ms, rhs.duration_ms);
        }
        return false;
    });
    playing_row_ = RemapRow(playing_row_, order);
    std::vector<size_t> old_to_new(order.size());
    for (size_t index = 0; index < order.size(); ++index)
        old_to_new[order[index]] = index;
    ApplyTrackOrder(tracks_, order);
    return old_to_new;
}

void Playlist::SortByDisplayTitle() {
    Sort(SortKey::display_title, true);
}

void Playlist::SortByFileName() {
    Sort(SortKey::file_name, true);
}

std::vector<size_t> Playlist::Shuffle() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    std::vector<size_t> order(tracks_.size());
    std::iota(order.begin(), order.end(), size_t{});
    std::shuffle(order.begin(), order.end(), generator);
    playing_row_ = RemapRow(playing_row_, order);
    std::vector<size_t> old_to_new(order.size());
    for (size_t index = 0; index < order.size(); ++index)
        old_to_new[order[index]] = index;
    ApplyTrackOrder(tracks_, order);
    return old_to_new;
}

std::set<size_t> Playlist::Reorder(const std::set<size_t>& selected,
                                   size_t insertion) {
    std::vector<size_t> moved;
    std::vector<size_t> remaining;
    moved.reserve(selected.size());
    remaining.reserve(tracks_.size());
    size_t selected_before_insertion = 0;
    for (size_t index = 0; index < tracks_.size(); ++index) {
        if (selected.contains(index)) {
            moved.push_back(index);
            if (index < insertion) ++selected_before_insertion;
        } else {
            remaining.push_back(index);
        }
    }
    if (moved.empty()) return {};
    insertion = std::min(insertion, tracks_.size());
    insertion = std::min(insertion - std::min(insertion, selected_before_insertion),
                         remaining.size());
    remaining.insert(remaining.begin() + static_cast<ptrdiff_t>(insertion),
                     moved.begin(), moved.end());
    playing_row_ = RemapRow(playing_row_, remaining);
    current_row_ = RemapRow(current_row_, remaining);
    ApplyTrackOrder(tracks_, remaining);
    std::set<size_t> reordered;
    for (size_t index = 0; index < moved.size(); ++index)
        reordered.insert(insertion + index);
    return reordered;
}

bool Playlist::SetDuration(size_t index, int duration_ms) {
    if (index >= tracks_.size()) return false;
    if (tracks_[index].duration_ms == duration_ms) return false;
    tracks_[index].duration_ms = duration_ms;
    return true;
}

bool Playlist::SetMetadata(size_t index, std::string title,
                           std::string artist, std::string album) {
    if (index >= tracks_.size()) return false;
    auto& track = tracks_[index];
    bool changed{};
    if (!title.empty() && title != track.title) {
        track.title = std::move(title);
        changed = true;
    }
    if (!artist.empty() && artist != track.artist) {
        track.artist = std::move(artist);
        changed = true;
    }
    if (!album.empty() && album != track.album) {
        track.album = std::move(album);
        changed = true;
    }
    return changed;
}

bool Playlist::SetExtendedMetadata(
    size_t index,
    std::vector<std::pair<std::string, std::string>> metadata,
    std::string media_type, std::uint32_t bitrate_bps,
    std::uint32_t sample_rate_hz) {
    if (index >= tracks_.size()) return false;
    auto& track = tracks_[index];
    bool changed{};
    for (auto& [name, value] : metadata) {
        if (EqualsAsciiInsensitive(name, "Tracknumber") ||
            EqualsAsciiInsensitive(name, "Track")) {
            int parsed{};
            const auto result = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (result.ptr != value.data() && parsed >= 0 &&
                track.track_number != parsed) {
                track.track_number = parsed;
                changed = true;
            }
        }
        const auto found = std::find_if(track.metadata.begin(),
            track.metadata.end(), [&name](const auto& entry) {
                return EqualsAsciiInsensitive(entry.first, name);
            });
        if (found == track.metadata.end()) {
            track.metadata.emplace_back(std::move(name), std::move(value));
            changed = true;
        } else if (found->second != value) {
            found->second = std::move(value);
            changed = true;
        }
    }
    if (track.media_type != media_type) {
        track.media_type = std::move(media_type);
        changed = true;
    }
    if (track.bitrate_bps != bitrate_bps) {
        track.bitrate_bps = bitrate_bps;
        changed = true;
    }
    if (track.sample_rate_hz != sample_rate_hz) {
        track.sample_rate_hz = sample_rate_hz;
        changed = true;
    }
    return changed;
}

bool Playlist::SetRating(size_t index, int rating) {
    // CPlayItem::SetRating (004AE10A) performs an unsigned "rating < 6"
    // guard. Out-of-range values are rejected rather than clamped.
    if (index >= tracks_.size() || rating < 0 || rating > 5) return false;
    if (tracks_[index].rating == rating) return false;
    tracks_[index].rating = rating;
    return true;
}

bool Playlist::SetPath(size_t index, std::filesystem::path path) {
    if (index >= tracks_.size() || path.empty() || tracks_[index].path == path)
        return false;
    tracks_[index].path = std::move(path);
    return true;
}

std::optional<size_t> Playlist::Next(size_t current, PlayMode mode) {
    if (tracks_.empty()) return std::nullopt;
    if (mode == PlayMode::repeat_one && current < tracks_.size()) return current;
    if (mode == PlayMode::shuffle && tracks_.size() > 1) {
        static thread_local std::mt19937 generator{std::random_device{}()};
        std::uniform_int_distribution<size_t> pick(0, tracks_.size() - 2);
        const size_t value = pick(generator); return value >= current ? value + 1 : value;
    }
    if (current + 1 < tracks_.size()) return current + 1;
    return mode == PlayMode::repeat_all ? std::optional<size_t>{0} : std::nullopt;
}

std::optional<size_t> Playlist::Previous(size_t current, PlayMode mode) {
    if (tracks_.empty()) return std::nullopt;
    if (mode == PlayMode::repeat_one && current < tracks_.size()) return current;
    if (mode == PlayMode::shuffle) return Next(current, mode);
    if (current > 0 && current <= tracks_.size()) return current - 1;
    return mode == PlayMode::repeat_all ? std::optional<size_t>{tracks_.size() - 1} : std::nullopt;
}

void Playlist::LoadM3u8(const std::filesystem::path& path,
                         const LoadOptions& options) {
    tracks_.clear();
    playing_row_.reset();
    current_row_.reset();
    std::string bytes = ReadBinaryText(path);
    const UINT code_page = LowerExtension(path) == L".m3u8" ? CP_UTF8 : CP_ACP;
    if (code_page == CP_UTF8 && bytes.size() < 4) return;
    if (code_page == CP_UTF8 && bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3);

    std::error_code path_error;
    const auto absolute_playlist = std::filesystem::absolute(path, path_error);
    const auto base = (path_error ? path : absolute_playlist).parent_path();

    std::istringstream input(bytes);
    std::string line, pending_title;
    int pending_duration = -2;
    bool expect_extinf_path{};
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!expect_extinf_path &&
            _strnicmp(line.c_str(), "#EXTINF:", 8) == 0) {
            // The native parser searches backwards from the physical end of
            // the line, so a title containing commas starts after the last
            // comma rather than the first one. It then consumes the next
            // non-empty physical line as the path in the same iteration.
            const auto comma = line.rfind(',');
            if (comma != std::string::npos) {
                const char* first = line.data() + 8;
                const char* last = line.data() + comma;
                while (first != last && *first == ' ') ++first;
                int seconds{};
                const auto parsed = std::from_chars(first, last, seconds);
                if (parsed.ptr == first || parsed.ec != std::errc{}) seconds = 0;
                const auto milliseconds = static_cast<std::int64_t>(seconds) * 1000;
                pending_duration = static_cast<int>(milliseconds);
                pending_title = line.substr(comma + 1);
            }
            expect_extinf_path = true;
            continue;
        }
        if (!expect_extinf_path && line.front() == '#') continue;
        {
            const auto decoded_path = DecodeM3uField(line, code_page);
            const auto resolved = ResolveM3uTrackPath(base, decoded_path);
            if (KeepImportedPath(resolved, options)) {
                const auto extension = LowerExtension(resolved);
                if (extension != L".cue" ||
                    !AppendCueTracks(tracks_, resolved)) {
                    std::string title;
                    if (!pending_title.empty()) {
                        title = core::WideToUtf8(
                            DecodeM3uField(pending_title, code_page));
                    }
                    Add({resolved, std::move(title), {}, pending_duration});
                }
            }
            pending_title.clear();
            pending_duration = -2;
            expect_extinf_path = false;
        }
    }
}

void Playlist::SaveM3u8(const std::filesystem::path& path) const {
    // FUN_00477446 receives the .m3u8 comparison from 00475486: UTF-8 is
    // used only for M3U8, while legacy .m3u is written in the process ANSI
    // code page that LoadM3u8 already selects for that suffix.
    const UINT code_page = LowerExtension(path) == L".m3u8" ? CP_UTF8 : CP_ACP;
    std::string output = "#EXTM3U\n";
    for (const auto& track : tracks_) {
        const auto title = track.title.empty()
            ? track.path.stem().wstring() : Utf8Field(track.title);
        output += "#EXTINF:";
        output += std::to_string(
            track.duration_ms < 0 ? -1 : track.duration_ms / 1000);
        output.push_back(',');
        output += EncodeM3uField(title, code_page);
        output.push_back('\n');
        output += EncodeM3uField(track.path.wstring(), code_page);
        output.push_back('\n');
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write M3U file");
    stream.write(output.data(), static_cast<std::streamsize>(output.size()));
    if (!stream) throw std::runtime_error("cannot write M3U file");
}

void Playlist::LoadTtbl(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open TTBL");
    std::array<char, 4> magic{};
    input.read(magic.data(), magic.size());
    const auto version = ReadValue<std::uint32_t>(input);
    static_cast<void>(ReadValue<std::int32_t>(input)); // selected/sort field
    if (magic != std::array<char, 4>{'T', 'T', 'B', 'L'} || version > 5)
        throw std::runtime_error("unsupported TTBL version");
    // Versions 0/1 have a 12-byte header.  Version 2 introduced CPlayList
    // +0x1c as the fourth DWORD; the native loader conditionally advances by
    // that field at 004778xx.
    const auto loaded_playing_row = version > 1
        ? ReadValue<std::int32_t>(input) : -1;

    const auto loaded_title = ReadTtblString(input);
    // FUN_00477787 reads the two additional playlist strings only for v4+.
    // Versions 0..3 continue directly from the title to the first item.
    if (version > 3) {
        static_cast<void>(ReadTtblString(input));
        static_cast<void>(ReadTtblString(input));
    }

    std::vector<Track> loaded;
    while (input.peek() != std::char_traits<char>::eof()) {
        const auto filename = ReadTtblString(input);
        if (filename.empty()) break;
        // v0..4 store a 16-bit item mask; v5 widened it to 32 bits.
        const std::uint32_t flags = version < 5
            ? ReadValue<std::uint16_t>(input)
            : ReadValue<std::uint32_t>(input);
        int subtrack{};
        if ((flags & 1U) != 0)
            subtrack = ReadValue<std::uint16_t>(input);
        if ((flags & 0x100U) != 0) static_cast<void>(ReadTtblString(input));
        std::wstring title;
        if ((flags & 2U) != 0) title = ReadTtblString(input);
        int duration = -2;
        if ((flags & 4U) != 0) duration = ReadValue<std::int32_t>(input);
        if ((flags & 8U) != 0) static_cast<void>(ReadValue<std::int32_t>(input));
        if ((flags & 0x10U) != 0) {
            static_cast<void>(ReadTtblString(input));
            std::array<char, 12> metadata{};
            input.read(metadata.data(), metadata.size());
            if (!input) throw std::runtime_error("truncated TTBL metadata");
        }
        if ((flags & 0x20U) != 0) {
            const auto pairs = ReadValue<std::int16_t>(input);
            if (pairs < 0 || pairs > 4096) throw std::runtime_error("invalid TTBL metadata count");
            std::string artist;
            std::string album;
            int track_number{};
            std::vector<std::pair<std::string, std::string>> metadata_entries;
            metadata_entries.reserve(static_cast<size_t>(pairs));
            for (int index = 0; index < pairs; ++index) {
                const auto key = ReadTtblNarrowString(input);
                const auto value = ReadTtblString(input);
                const auto encoded = WideField(value);
                metadata_entries.emplace_back(key, encoded);
                if (EqualsAsciiInsensitive(key, "Artist"))
                    artist = encoded;
                else if (EqualsAsciiInsensitive(key, "Album"))
                    album = encoded;
                else if (EqualsAsciiInsensitive(key, "Tracknumber")) {
                    const auto result = std::from_chars(encoded.data(),
                        encoded.data() + encoded.size(), track_number);
                    if (result.ptr == encoded.data() || track_number < 0)
                        track_number = 0;
                }
            }
            int rating{};
            if ((flags & 0x40U) != 0)
                rating = std::clamp(ReadValue<std::int32_t>(input), 0, 5);
            if ((flags & 0x80U) != 0) static_cast<void>(ReadValue<std::uint8_t>(input));
            if (version > 4)
                static_cast<void>(ReadValue<std::uint32_t>(input));
            Track track;
            track.path = std::filesystem::path(filename);
            track.title = WideField(title);
            track.artist = std::move(artist);
            track.duration_ms = duration;
            track.subtrack = subtrack;
            track.album = std::move(album);
            track.rating = rating;
            track.track_number = track_number;
            track.metadata = std::move(metadata_entries);
            loaded.push_back(std::move(track));
            continue;
        }
        int rating{};
        if ((flags & 0x40U) != 0)
            rating = std::clamp(ReadValue<std::int32_t>(input), 0, 5);
        if ((flags & 0x80U) != 0) static_cast<void>(ReadValue<std::uint8_t>(input));
        if (version > 4)
            static_cast<void>(ReadValue<std::uint32_t>(input));
        loaded.push_back({std::filesystem::path(filename), WideField(title), {},
                          duration, subtrack, {}, rating, 0});
    }
    tracks_ = std::move(loaded);
    title_ = std::move(loaded_title);
    playing_row_ = loaded_playing_row >= 0 &&
        static_cast<size_t>(loaded_playing_row) < tracks_.size()
        ? std::optional<size_t>{static_cast<size_t>(loaded_playing_row)}
        : std::nullopt;
    current_row_.reset();
}

void Playlist::SaveTtbl(const std::filesystem::path& path) const {
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create TTBL");
    output.write("TTBL", 4);
    WriteValue<std::uint32_t>(output, 5);
    WriteValue<std::int32_t>(output, -1);
    const auto saved_playing_row = playing_row_ &&
        *playing_row_ <= static_cast<size_t>(
            std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int32_t>(*playing_row_) : -1;
    WriteValue<std::int32_t>(output, saved_playing_row);
    WriteTtblString(output, title_);
    WriteTtblString(output, L"");
    WriteTtblString(output, L"");
    for (const auto& track : tracks_) {
        WriteTtblString(output, track.path.wstring());
        const auto title = Utf8Field(track.title);
        auto metadata = track.metadata;
        const auto merge_metadata = [&metadata](std::string_view name,
                                                std::string value) {
            if (value.empty()) return;
            const auto found = std::find_if(metadata.begin(), metadata.end(),
                [name](const auto& entry) {
                    return EqualsAsciiInsensitive(entry.first, name);
                });
            if (found == metadata.end())
                metadata.emplace_back(name, std::move(value));
            else
                found->second = std::move(value);
        };
        merge_metadata("Artist", track.artist);
        merge_metadata("Album", track.album);
        if (track.track_number > 0)
            merge_metadata("Tracknumber", std::to_string(track.track_number));
        if (metadata.size() > static_cast<size_t>(
                std::numeric_limits<std::int16_t>::max())) {
            metadata.resize(static_cast<size_t>(
                std::numeric_limits<std::int16_t>::max()));
        }
        std::uint32_t flags = 0x40U;
        if (track.subtrack > 0 && track.subtrack <= 0xffff) flags |= 1U;
        if (!title.empty()) flags |= 2U;
        if (track.duration_ms >= 0) flags |= 4U;
        if (!metadata.empty()) flags |= 0x20U;
        WriteValue(output, flags);
        if ((flags & 1U) != 0)
            WriteValue<std::uint16_t>(output,
                static_cast<std::uint16_t>(track.subtrack));
        if ((flags & 2U) != 0) WriteTtblString(output, title);
        if ((flags & 4U) != 0) WriteValue<std::int32_t>(output, track.duration_ms);
        if ((flags & 0x20U) != 0) {
            const auto count = static_cast<std::int16_t>(metadata.size());
            WriteValue(output, count);
            for (const auto& [name, value] : metadata) {
                WriteTtblNarrowString(output, name);
                WriteTtblString(output, Utf8Field(value));
            }
        }
        // CPlayList_SaveTtbl 00476550 always emits flag 0x40 and stores the
        // item's rating here.  The previous rebuild mislabeled this DWORD as
        // a source kind and consequently rewrote every item as rating 2.
        WriteValue<std::int32_t>(output, std::clamp(track.rating, 0, 5));
        WriteValue<std::uint32_t>(output, 0);
    }
}

void Playlist::LoadXml(const std::filesystem::path& path,
                       const LoadOptions& options) {
    std::error_code path_error;
    const auto absolute_playlist = std::filesystem::absolute(path, path_error);
    const auto base = (path_error ? path : absolute_playlist).parent_path();

    std::vector<Track> loaded;
    std::wstring loaded_title = title_;
    std::optional<PendingXmlTrack> pending;
    std::vector<std::string> elements;
    bool saw_root{};

    const auto finish_element = [&](std::string_view name) {
        if (name == "item") {
            if (pending && KeepImportedPath(pending->track.path, options))
                loaded.push_back(std::move(pending->track));
            pending.reset();
        }
    };

    // XmlTagReader is a non-owning scanner. Keep the decoded bytes alive for
    // the whole SAX-style pass; constructing it directly from ReadUtf8Text's
    // temporary leaves a dangling string_view after the full-expression.
    const std::string xml = core::ReadUtf8Text(path);
    XmlTagReader reader(xml);
    XmlTag tag;
    while (reader.Next(tag)) {
        if (tag.closing) {
            if (elements.empty() || elements.back() != tag.name)
                throw std::runtime_error("mismatched TTPlayer XML playlist element");
            finish_element(tag.name);
            elements.pop_back();
            continue;
        }

        const std::string_view parent = elements.empty()
            ? std::string_view{} : std::string_view(elements.back());
        if (elements.empty()) {
            if (saw_root || tag.name != "ttplaylist")
                throw std::runtime_error("not a TTPlayer XML playlist");
            saw_root = true;
            const int version = XmlInteger(tag, "version", -1);
            if (version == -1 || version > 4)
                throw std::runtime_error("unsupported TTPlayer XML playlist version");
            if (const auto* title = FindAttribute(tag, "title");
                title != nullptr && !title->empty()) {
                loaded_title = core::Utf8ToWide(*title);
            }
        } else if (tag.name == "items" && parent == "ttplaylist") {
            const int count = XmlInteger(tag, "count", 0);
            if (count > 0 && count <= 1000000)
                loaded.reserve(static_cast<size_t>(count));
        } else if (tag.name == "item" && parent == "items") {
            pending.reset();
            const auto* file = FindAttribute(tag, "file");
            if (file != nullptr && !file->empty()) {
                PendingXmlTrack item;
                item.track.path = ResolveXmlTrackPath(base, *file);
                item.track.duration_ms = XmlInteger(tag, "len", -2);
                item.track.subtrack = XmlInteger(tag, "subtk", 0);
                item.track.rating = std::clamp(
                    XmlInteger(tag, "rating", 0), 0, 5);
                item.track.track_number = std::max(
                    XmlInteger(tag, "track", 0), 0);
                if (const auto* title = FindAttribute(tag, "title");
                    title != nullptr && !title->empty()) {
                    item.track.title = *title;
                } else {
                    item.track.title = WideField(
                        std::filesystem::path(core::Utf8ToWide(*file))
                            .filename().wstring());
                }
                // The native callback reads these legacy aliases after the
                // cached display title, independent of XML attribute order.
                if (const auto* song_name = FindAttribute(tag, "SongName"))
                    ApplyXmlMetadata(item, "SongName", *song_name, false);
                if (const auto* song_artist = FindAttribute(tag, "SongArtist"))
                    ApplyXmlMetadata(item, "SongArtist", *song_artist, false);
                if (const auto* song_album = FindAttribute(tag, "SongAlbum"))
                    ApplyXmlMetadata(item, "SongAlbum", *song_album, false);
                if (const auto* album = FindAttribute(tag, "Album"))
                    ApplyXmlMetadata(item, "Album", *album, false);
                pending = std::move(item);
            }
        } else if (pending && tag.name == "tag" && parent == "item") {
            // Versions 1-3 store tag fields as attributes on <tag>.  Version
            // 4 moved them into child <f name="..." val="..."/> elements.
            for (const auto& attribute : tag.attributes)
                ApplyXmlMetadata(*pending, attribute.first, attribute.second);
        } else if (pending && tag.name == "f" && parent == "tag") {
            const auto* name = FindAttribute(tag, "name");
            const auto* value = FindAttribute(tag, "val");
            if (name != nullptr && value != nullptr)
                ApplyXmlMetadata(*pending, *name, *value);
        }

        if (tag.self_closing) {
            finish_element(tag.name);
        } else {
            elements.push_back(tag.name);
        }
    }
    if (!saw_root || !elements.empty())
        throw std::runtime_error("incomplete TTPlayer XML playlist");

    tracks_ = std::move(loaded);
    title_ = std::move(loaded_title);
    playing_row_.reset();
    current_row_.reset();
}

void Playlist::SaveXml(const std::filesystem::path& path,
                       std::wstring_view tag_title_format,
                       std::wstring_view default_title_format,
                       bool save_relative_path,
                       bool save_tags) const {
    // CPlayList_SaveToFile (00475486) creates a standalone UTF-8 version-4
    // ttplaylist document for every suffix other than TTBL/M3U/M3U8.  Keep
    // the fields represented by the rebuilt Track model and the original
    // generator/format nodes; unsupported private fmt/tid fields are omitted.
    std::ostringstream output;
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
           << "<ttplaylist title=\""
           << EscapeXmlAttribute(core::WideToUtf8(title_))
           << "\" version=\"4\" generator=\"TTPlayer -- 5.7.9\">\r\n"
           << "\t<format tagtitle=\""
           << EscapeXmlAttribute(core::WideToUtf8(tag_title_format))
           << "\" deftitle=\""
           << EscapeXmlAttribute(core::WideToUtf8(default_title_format))
           << "\" />\r\n";
    if (tracks_.empty()) {
        output << "\t<items />\r\n</ttplaylist>\r\n";
        core::WriteUtf8Text(path, output.str());
        return;
    }
    output << "\t<items count=\"" << tracks_.size() << "\">\r\n";
    for (const auto& track : tracks_) {
        const auto stored_path = core::WideToUtf8(
            XmlStoredTrackPath(path, track.path, save_relative_path));
        output << "\t\t<item file=\"" << EscapeXmlAttribute(stored_path) << '"';
        if (track.subtrack > 0)
            output << " subtk=\"" << track.subtrack << '"';
        if (!track.title.empty())
            output << " title=\"" << EscapeXmlAttribute(track.title) << '"';
        if (!track.title.empty())
            output << " SongName=\"" << EscapeXmlAttribute(track.title) << '"';
        if (!track.artist.empty())
            output << " SongArtist=\"" << EscapeXmlAttribute(track.artist) << '"';
        if (track.duration_ms > 0)
            output << " len=\"" << track.duration_ms << '"';
        auto metadata = track.metadata;
        const auto merge_metadata = [&metadata](std::string_view name,
                                                std::string value) {
            const auto found = std::find_if(metadata.begin(), metadata.end(),
                [name](const auto& entry) {
                    return EqualsAsciiInsensitive(entry.first, name);
                });
            if (value.empty()) {
                if (found != metadata.end()) metadata.erase(found);
                return;
            }
            if (found == metadata.end())
                metadata.emplace_back(name, std::move(value));
            else
                found->second = std::move(value);
        };
        merge_metadata("Title", track.title);
        merge_metadata("Artist", track.artist);
        merge_metadata("Album", track.album);
        merge_metadata("Tracknumber", track.track_number > 0
            ? std::to_string(track.track_number) : std::string{});
        merge_metadata("Rating", track.rating > 0
            ? std::to_string(track.rating) : std::string{});
        const bool has_tags = save_tags && std::any_of(
            metadata.begin(), metadata.end(), [](const auto& entry) {
                return !entry.first.empty() && !entry.second.empty();
            });
        if (!has_tags) {
            output << " />\r\n";
            continue;
        }
        output << ">\r\n\t\t\t<tag>\r\n";
        const auto write_tag = [&output](std::string_view name,
                                         std::string_view value) {
            if (name.empty() || value.empty()) return;
            output << "\t\t\t\t<f name=\""
                   << EscapeXmlAttribute(name) << "\" val=\""
                   << EscapeXmlAttribute(value) << "\" />\r\n";
        };
        for (const auto& [name, value] : metadata) write_tag(name, value);
        output << "\t\t\t</tag>\r\n\t\t</item>\r\n";
    }
    output << "\t</items>\r\n</ttplaylist>\r\n";
    core::WriteUtf8Text(path, output.str());
}

void Playlist::LoadFromFile(const std::filesystem::path& path,
                            const LoadOptions& options) {
    const auto extension = LowerExtension(path);
    if (extension == L".ttbl") LoadTtbl(path);
    else if (extension == L".m3u" || extension == L".m3u8")
        LoadM3u8(path, options);
    else LoadXml(path, options);
}

void Playlist::SaveToFile(const std::filesystem::path& path,
                          std::wstring_view tag_title_format,
                          std::wstring_view default_title_format,
                          bool save_relative_path,
                          bool save_tags) const {
    const auto extension = LowerExtension(path);
    if (extension.empty())
        throw std::invalid_argument("playlist output has no extension");
    if (extension == L".ttbl") SaveTtbl(path);
    else if (extension == L".m3u" || extension == L".m3u8") SaveM3u8(path);
    else SaveXml(path, tag_title_format, default_title_format,
                 save_relative_path, save_tags);
}
}
