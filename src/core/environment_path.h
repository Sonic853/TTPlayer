#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace ttplayer::core {
// Generic path expansion for stored configuration. No game-specific rules.
inline std::wstring ExpandEnvironmentPath(std::wstring_view value) {
    const std::wstring source(value);
    const DWORD count=ExpandEnvironmentStringsW(source.c_str(),nullptr,0);
    if(!count || count>32768)return source;
    std::wstring expanded(count,L'\0');
    const DWORD copied=ExpandEnvironmentStringsW(source.c_str(),expanded.data(),count);
    if(!copied || copied>count)return source;
    expanded.resize(copied-1);return expanded;
}
}
