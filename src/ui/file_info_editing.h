#pragma once

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::ui::detail {

using FileInfoFields = std::vector<std::pair<std::string, std::wstring>>;

inline bool FileInfoNameEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) {
            if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
            if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
            return x == y;
        });
}

inline std::wstring FileInfoField(const FileInfoFields& fields, std::string_view name) {
    const auto found = std::find_if(fields.begin(), fields.end(),
        [name](const auto& field) { return FileInfoNameEquals(field.first, name); });
    return found == fields.end() ? std::wstring{} : found->second;
}

inline void SetFileInfoField(FileInfoFields& fields, std::string_view name,
                             std::wstring_view value) {
    auto found = std::find_if(fields.begin(), fields.end(),
        [name](const auto& field) { return FileInfoNameEquals(field.first, name); });
    if (found == fields.end()) {
        if (!value.empty()) fields.emplace_back(name, value);
    } else if (value.empty()) {
        fields.erase(found);
    } else {
        found->second = value;
    }
}

// Compare each file against its own snapshot: a mixed-value caption is never
// a metadata value. Empty values explicitly remove fields in the write protocol.
inline FileInfoFields FileInfoChanges(const FileInfoFields& before,
                                      const FileInfoFields& after) {
    FileInfoFields changes;
    for (const auto& field : after)
        if (FileInfoField(before, field.first) != field.second)
            changes.push_back(field);
    for (const auto& field : before)
        if (!field.second.empty() && FileInfoField(after, field.first).empty())
            changes.emplace_back(field.first, L"");
    return changes;
}

inline std::wstring FileInfoTitleCase(std::wstring value) {
    // 004C1BAA: ASCII letters only; every non-letter starts another word.
    bool first = true;
    for (auto& ch : value) {
        if (first && ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        else if (!first && ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        first = !((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'));
    }
    return value;
}

inline std::wstring FileInfoTrim(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == value.npos) return {};
    return std::wstring(value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1));
}

// 0042EE06/0042EBAB: match the extensionless path from the right, component
// by component, with %(FieldName) captures delimited by literal text.
inline std::vector<std::pair<std::wstring, std::wstring>> GuessFileInfoFields(
    std::wstring path, std::wstring pattern) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::replace(pattern.begin(), pattern.end(), L'/', L'\\');
    const auto dot = path.find_last_of(L'.');
    const auto slash = path.find_last_of(L'\\');
    if (dot != path.npos && (slash == path.npos || dot > slash)) path.resize(dot);
    std::vector<std::pair<std::wstring, std::wstring>> result;
    for (;;) {
        const auto p = pattern.find_last_of(L'\\');
        const auto s = path.find_last_of(L'\\');
        const std::wstring format = pattern.substr(p == pattern.npos ? 0 : p + 1);
        const std::wstring text = path.substr(s == path.npos ? 0 : s + 1);
        size_t fi{}, ti{};
        while (fi < format.size()) {
            const auto token = format.find(L"%(", fi);
            if (token == format.npos) break;
            const auto end = format.find(L')', token + 2);
            if (end == format.npos) break;
            const auto raw_prefix = std::wstring_view(format).substr(fi, token - fi);
            const auto prefix = FileInfoTrim(raw_prefix);
            if (!prefix.empty()) {
                const auto match = text.find(prefix, ti);
                if (match == text.npos) break;
                ti = match + prefix.size();
            } else if (!raw_prefix.empty()) {
                while (ti < text.size() && (text[ti] == L' ' || text[ti] == L'\t')) ++ti;
            }
            const auto next = format.find(L"%(", end + 1);
            const auto literal = std::wstring_view(format).substr(end + 1,
                (next == format.npos ? format.size() : next) - end - 1);
            auto delimiter = FileInfoTrim(literal);
            if (delimiter.empty() && !literal.empty()) delimiter = L" ";
            const auto value_end = delimiter.empty() ? text.size() : text.find(delimiter, ti);
            if (value_end == text.npos) break;
            auto name = FileInfoTrim(std::wstring_view(format).substr(token + 2, end - token - 2));
            if (!name.empty()) result.emplace_back(std::move(name),
                FileInfoTrim(std::wstring_view(text).substr(ti, value_end - ti)));
            ti = value_end;
            fi = end + 1;
        }
        if (p == pattern.npos || s == path.npos) break;
        pattern.resize(p);
        path.resize(s);
    }
    return result;
}

inline std::wstring FileInfoBitrate(std::uint64_t encoded, bool vbr = false) {
    if (!encoded) return {};
    wchar_t buffer[64]{};
    if (encoded > 1000000)
        swprintf_s(buffer, L"%.2f Mbps", static_cast<double>(encoded) / 1000000.0);
    else swprintf_s(buffer, L"%llu Kbps", static_cast<unsigned long long>(encoded / 1000));
    return std::wstring(buffer) + (vbr ? L" (VBR)" : L"");
}

inline std::wstring FileInfoDuration(int milliseconds) {
    if (milliseconds < 0) return {};
    const int seconds = milliseconds / 1000;
    wchar_t buffer[64]{};
    if (seconds >= 3600)
        swprintf_s(buffer, L"%d:%02d:%02d.%03d", seconds / 3600,
                   seconds / 60 % 60, seconds % 60, milliseconds % 1000);
    else if (seconds >= 60) swprintf_s(buffer, L"%d:%02d.%03d", seconds / 60,
                                      seconds % 60, milliseconds % 1000);
    else swprintf_s(buffer, L"%d.%03d", seconds, milliseconds % 1000);
    return buffer;
}

} // namespace ttplayer::ui::detail
