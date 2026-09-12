#pragma once

#include <windows.h>
#include <shellapi.h>
#include <array>

namespace ttplayer::ui::detail {

// The 5.7.9 menu 0x8a uses the 0x9d placeholder for related links.
inline constexpr UINT kMenuRelatedLinks = 0x009d;

struct ProjectLink {
    UINT command;
    const wchar_t* label;
    const wchar_t* url;
    UINT image_command;
};

// Shared by the main context menu and the Options window.
inline constexpr std::array<ProjectLink, 2> kProjectLinks{{
    {0xb650, L"Github仓库", L"https://github.com/Sonic853/TTPlayer", kMenuRelatedLinks},
    {0xb651, L"提交反馈", L"https://github.com/Sonic853/TTPlayer/issues", 0x802c},
}};

inline constexpr const ProjectLink* FindProjectLink(UINT command) noexcept {
    for (const auto& link : kProjectLinks)
        if (link.command == command) return &link;
    return nullptr;
}

inline bool OpenProjectLink(HWND owner, UINT command,
                           decltype(&ShellExecuteW) open = &ShellExecuteW) {
    const auto* link = FindProjectLink(command);
    if (!link) return false;
    open(owner, L"open", link->url, nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

} // namespace ttplayer::ui::detail
