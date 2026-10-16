#include "ttplayer/audio/osz_archive.h"
#include "ttplayer/audio/archive_member.h"
#include <algorithm>
#include <cwchar>
#include <map>
#include <vector>

namespace ttplayer::audio {
namespace {
std::wstring_view Trim(std::wstring_view value) {
    constexpr std::wstring_view space = L" \t\r\n";
    const auto first = value.find_first_not_of(space);
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(space) - first + 1);
}
std::wstring_view Unquote(std::wstring_view value) {
    value = Trim(value);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        return value.substr(1, value.size() - 2);
    return value;
}
std::vector<std::wstring> Csv(std::wstring_view line) {
    std::vector<std::wstring> fields;
    while (!line.empty() && fields.size() < 6) {
        line = Trim(line);
        if (line.empty()) break;
        if (line.front() != L'"') {
            const auto comma = line.find(L',');
            fields.emplace_back(Trim(line.substr(0, comma)));
            if (comma == line.npos) return fields;
            line.remove_prefix(comma + 1);
        } else {
            std::wstring field;
            size_t i = 1;
            bool closed = false;
            while (i < line.size()) {
                const auto c = line[i++];
                if (c != L'"') { field.push_back(c); continue; }
                if (i < line.size() && line[i] == L'"') { field.push_back(L'"'); ++i; }
                else { closed = true; break; }
            }
            if (!closed) return {};
            fields.push_back(std::move(field));
            line = Trim(line.substr(i));
            if (line.empty()) return fields;
            if (line.front() != L',') return {};
            line.remove_prefix(1);
        }
    }
    return fields;
}
std::optional<std::wstring> RelativeMember(std::wstring_view base, std::wstring_view reference) {
    reference = Unquote(reference);
    if (reference.empty() || reference.find_first_of(L":|\"<>?*") != reference.npos ||
        std::any_of(reference.begin(), reference.end(), [](wchar_t c) { return c < 32; })) return {};
    std::wstring value(reference);
    std::replace(value.begin(), value.end(), L'/', L'\\');
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return {};
    const auto combined = (std::filesystem::path(base).parent_path() / path).lexically_normal();
    for (const auto& component : combined) if (component == L"..") return {};
    if (combined.empty() || combined == L".") return {};
    return combined.native();
}
struct Insensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
void CheckCanceled(const std::function<bool()>& canceled) {
    if (canceled && canceled()) throw ArchiveError(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
}

bool IsOszArchive(const std::filesystem::path& path) {
    return _wcsicmp(path.extension().c_str(), L".osz") == 0;
}

std::optional<OsuMediaReferences> ParseOsuMedia(std::span<const unsigned char> bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
        bytes = bytes.subspan(3);
    if (bytes.empty() || bytes.size() > 16U * 1024 * 1024) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), text.data(), length) != length ||
        text.find(L'\0') != text.npos) return {};
    std::wstring_view remaining(text), section;
    OsuMediaReferences result;
    bool header = false;
    while (!remaining.empty()) {
        const auto end = remaining.find_first_of(L"\r\n");
        const auto line = Trim(remaining.substr(0, end));
        remaining = end == remaining.npos ? std::wstring_view{} : remaining.substr(end + 1);
        if (line.empty() || line.starts_with(L"//")) continue;
        if (!header) {
            constexpr std::wstring_view prefix = L"osu file format v";
            if (!line.starts_with(prefix) || line.size() == prefix.size() ||
                !std::all_of(line.begin() + prefix.size(), line.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return {};
            header = true;
        } else if (line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
        } else if (section == L"General") {
            const auto colon = line.find(L':');
            if (colon != line.npos && Trim(line.substr(0, colon)) == L"AudioFilename" && result.audio.empty())
                result.audio = Unquote(line.substr(colon + 1));
        } else if (section == L"Events" && result.background.empty()) {
            const auto fields = Csv(line);
            if (fields.size() >= 3 && (fields[0] == L"0" || fields[0] == L"Background"))
                result.background = fields[2];
        }
    }
    return header && !result.audio.empty() ? std::optional(result) : std::nullopt;
}

std::vector<OszMedia> ResolveOszMedia(const std::filesystem::path& archive,
    std::span<const std::wstring> names, HMODULE module,
    const std::function<bool()>& canceled,
    const std::function<void(uint64_t,uint64_t)>& progress, std::wstring_view required_audio) {
    CheckCanceled(canceled);
    std::map<std::wstring, const std::wstring*, Insensitive> members;
    for (const auto& name : names) {
        CheckCanceled(canceled);
        const auto normalized = RelativeMember({}, name);
        if (normalized) members.try_emplace(*normalized, &name);
    }
    const auto find = [&](std::wstring_view base, std::wstring_view value) -> const std::wstring* {
        const auto normalized = RelativeMember(base, value);
        if (!normalized) return nullptr;
        const auto found = members.find(*normalized);
        return found == members.end() ? nullptr : found->second;
    };
    std::vector<OszMedia> selected;
    std::map<std::wstring, size_t, Insensitive> selected_indices;
    const auto required = required_audio.empty() ? nullptr : find({},required_audio);
    if (!required_audio.empty() && !required) return {};
    uint64_t read_bytes = 0;
    unsigned maps = 0;
    size_t index = 0;
    for (const auto& name : names) {
        CheckCanceled(canceled);
        if (progress) progress(index++, names.size());
        if (_wcsicmp(std::filesystem::path(name).extension().c_str(), L".osu")) continue;
        if (!RelativeMember({},name)) continue;
        // Import must scan all difficulties: later maps can reference another
        // song. Fail a bounded scan instead of silently importing a partial set.
        if (++maps > 256 || read_bytes >= 64ULL * 1024 * 1024) {
            throw ArchiveError(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
        }
        const auto bytes = ReadArchiveMember({archive,name,ArchiveKind::zip},module,canceled);
        read_bytes += bytes.size();
        if (read_bytes > 64ULL * 1024 * 1024) {
            throw ArchiveError(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
        }
        const auto parsed = ParseOsuMedia(bytes);
        CheckCanceled(canceled);
        if (!parsed) continue;
        const auto audio = find(name, parsed->audio);
        if (!audio || (required && audio != required)) continue;
        const auto [entry, inserted] = selected_indices.try_emplace(*audio,selected.size());
        if (inserted) selected.push_back({name,*audio,{}});
        auto& media = selected[entry->second];
        if (media.background.empty()) {
            if (const auto background = find(name, parsed->background)) {
                media.background = *background;
                if (required) return selected;
            }
        }
    }
    if (progress) progress(names.size(),names.size());
    return selected;
}
} // namespace ttplayer::audio
