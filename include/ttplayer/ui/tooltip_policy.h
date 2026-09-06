#pragma once

#include <string>
#include <string_view>

namespace ttplayer::ui {

// Command strings in ttpres.dll use the conventional
// "long description\nshort label" form.  Menu help displays the long part;
// control tooltips continue to use the short part selected by their owner.
inline std::wstring CommandTipDescription(std::wstring_view resource_text) {
    const size_t separator = resource_text.find_first_of(L"\r\n");
    if (separator == std::wstring_view::npos)
        return std::wstring(resource_text);
    return std::wstring(resource_text.substr(0, separator));
}

inline std::wstring AppendToolTipHotKey(std::wstring text,
                                        bool enabled,
                                        std::wstring_view hotkey) {
    if (!enabled || text.empty() || hotkey.empty()) return text;
    text += L" (";
    text += hotkey;
    text += L")";
    return text;
}

inline std::wstring FormatPlaybackTipBody(
    std::wstring_view resource_template,
    std::wstring_view title,
    std::wstring_view artist,
    std::wstring_view album,
    std::wstring_view format,
    std::wstring_view duration) {
    const auto replace_all = [](std::wstring& value,
                                std::wstring_view needle,
                                std::wstring_view replacement) {
        size_t offset{};
        while ((offset = value.find(needle, offset)) != std::wstring::npos) {
            value.replace(offset, needle.size(), replacement);
            offset += replacement.size();
        }
    };
    std::wstring result;
    size_t begin{};
    while (begin <= resource_template.size()) {
        const size_t end = resource_template.find(L'|', begin);
        std::wstring line(resource_template.substr(begin,
            end == std::wstring_view::npos ? end : end - begin));
        replace_all(line, L"%(Title)", title);
        replace_all(line, L"%(Artist)", artist);
        replace_all(line, L"%(Album)", album);
        replace_all(line, L"%(Format)", format);
        replace_all(line, L"%(Duration)", duration);
        const auto colon = line.find(L':');
        const bool have_value = colon == std::wstring::npos ||
            line.find_first_not_of(L" \t", colon + 1) != std::wstring::npos;
        if (line.find(L"%(") == std::wstring::npos && have_value) {
            if (!result.empty()) result.push_back(L'\n');
            result += line;
        }
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return result;
}

} // namespace ttplayer::ui
