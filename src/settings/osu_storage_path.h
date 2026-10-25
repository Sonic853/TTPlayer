#pragma once

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <cwctype>

namespace ttplayer::settings::detail {
inline constexpr wchar_t kDefaultLazerFiles[]=LR"(%USERPROFILE%\AppData\Roaming\osu\files)";

inline std::wstring ExpandOsuPath(std::wstring_view value) {
    const std::wstring source(value);
    const DWORD count=ExpandEnvironmentStringsW(source.c_str(),nullptr,0);
    if(!count || count>32768)return source;
    std::wstring expanded(count,L'\0');
    const DWORD copied=ExpandEnvironmentStringsW(source.c_str(),expanded.data(),count);
    if(!copied || copied>count)return source;
    expanded.resize(copied-1);return expanded;
}

// The framework writes UTF-8 key=value lines, not Windows INI sections.
// Read only this small configuration file, never initialise game storage.
inline std::wstring ReadLazerStorageRoot(const std::filesystem::path& ini) {
    std::ifstream input(ini,std::ios::binary|std::ios::ate);
    if(!input)return {};
    const auto length=input.tellg();
    if(length<=0 || length>64*1024)return {};
    std::string bytes(static_cast<size_t>(length),'\0');input.seekg(0);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(bytes.size())))return {};
    std::wstring text;
    if(bytes.size()>=2 && ((static_cast<unsigned char>(bytes[0])==0xff && static_cast<unsigned char>(bytes[1])==0xfe) ||
        (static_cast<unsigned char>(bytes[0])==0xfe && static_cast<unsigned char>(bytes[1])==0xff))) {
        if(bytes.size()%2)return {};
        const bool little=static_cast<unsigned char>(bytes[0])==0xff;
        for(size_t i=2;i<bytes.size();i+=2) {
            const auto a=static_cast<unsigned char>(bytes[i]),b=static_cast<unsigned char>(bytes[i+1]);
            text.push_back(static_cast<wchar_t>(little?(a|(b<<8)):((a<<8)|b)));
        }
    } else {
        const auto skip=bytes.compare(0,3,"\xef\xbb\xbf")==0?3:0;
        const auto count=static_cast<int>(bytes.size()-skip);
        const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data()+skip,count,nullptr,0);
        if(!size)return {};
        text.resize(size);
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data()+skip,count,text.data(),size))return {};
    }
    if(text.find(L'\0')!=std::wstring::npos)return {};
    const auto trim=[](std::wstring_view line) {
        while(!line.empty() && iswspace(line.front()))line.remove_prefix(1);
        while(!line.empty() && iswspace(line.back()))line.remove_suffix(1);
        return line;
    };
    std::wstring root;
    for(size_t offset=0;offset<text.size();) {
        const auto end=text.find(L'\n',offset);
        auto line=trim(std::wstring_view(text).substr(offset,end==std::wstring::npos?end:end-offset));
        offset=end==std::wstring::npos?text.size():end+1;
        if(line.empty() || line.front()==L'#' || line.front()==L';')continue;
        const auto equal=line.find(L'=');
        if(equal!=std::wstring_view::npos && trim(line.substr(0,equal))==L"FullPath")
            root=trim(line.substr(equal+1));
    }
    // osu! writes an absolute data-root path. Retain an offline drive path,
    // but do not mistake a relative/malformed value for the player's cwd.
    if(root.empty() || root.find_first_of(L"\r\n\"<>|?*")!=std::wstring::npos ||
       !std::filesystem::path(root).is_absolute())return {};
    return root;
}

inline std::wstring FindLazerFiles(const std::filesystem::path& roaming_root,
                                 const std::filesystem::path& executable) {
    try {
        auto base=roaming_root;
        std::error_code error;
        const auto program=executable.parent_path();
        if(!program.empty() && std::filesystem::is_regular_file(program/L"framework.ini",error))base=program;
        if(!base.empty()) {
            const auto root=ReadLazerStorageRoot(base/L"storage.ini");
            if(!root.empty())return (std::filesystem::path(root)/L"files").wstring();
        }
    } catch(...) {
        // A broken/unreadable optional game config must not block the player.
    }
    return kDefaultLazerFiles;
}
} // namespace ttplayer::settings::detail
