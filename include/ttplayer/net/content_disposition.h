#pragma once
#include <windows.h>
#include <cwchar>
#include <string>
#include <string_view>

namespace ttplayer::net {
// RFC 6266/5987: prefer filename*, decode its declared charset, and retain
// only a Windows-safe leaf name. Missing/malformed values use the caller's fallback.
inline std::wstring HttpResponseFilename(std::string_view header) {
    if(header.size()>8192 || header.find_first_of("\r\n")!=header.npos)return {};
    auto trim=[](std::string_view s){while(!s.empty()&&(s.front()==' '||s.front()=='\t'))s.remove_prefix(1);
        while(!s.empty()&&(s.back()==' '||s.back()=='\t'))s.remove_suffix(1);return s;};
    auto lower=[](std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;};
    auto decode=[](std::string_view s,UINT cp)->std::wstring {
        if(s.empty())return {};const DWORD flags=cp==CP_UTF8?MB_ERR_INVALID_CHARS:0;
        const int n=MultiByteToWideChar(cp,flags,s.data(),static_cast<int>(s.size()),nullptr,0);
        std::wstring out(n,0);if(n)MultiByteToWideChar(cp,flags,s.data(),static_cast<int>(s.size()),out.data(),n);return out;
    };
    std::string plain,extended;bool seen_plain{},seen_extended{};
    auto position=header.find(';');
    while(position!=header.npos && ++position<header.size()) {
        const auto begin=position;while(position<header.size()&&header[position]!='='&&header[position]!=';')++position;
        if(position==header.size())break;if(header[position]==';')continue;
        const auto key=lower(std::string(trim(header.substr(begin,position-begin))));++position;
        while(position<header.size()&&(header[position]==' '||header[position]=='\t'))++position;
        std::string value;
        if(position<header.size()&&header[position]=='"') {
            ++position;bool closed{};
            while(position<header.size()) {char c=header[position++];if(c=='"'){closed=true;break;}
                if(c=='\\'&&position<header.size()&&(header[position]=='"'||header[position]=='\\'))c=header[position++];value+=c;}
            if(!closed)return {};
            while(position<header.size()&&(header[position]==' '||header[position]=='\t'))++position;
            if(position<header.size()&&header[position]!=';')return {};
        }else {const auto start=position;position=header.find(';',position);value=trim(header.substr(start,position==header.npos?header.size()-start:position-start));}
        if(key=="filename"){if(seen_plain)return {};seen_plain=true;plain=std::move(value);}
        if(key=="filename*"){if(seen_extended)return {};seen_extended=true;extended=std::move(value);}
    }
    std::wstring name;
    const auto first=extended.find('\''),second=first==extended.npos?extended.npos:extended.find('\'',first+1);
    if(second!=extended.npos) {
        const auto charset=lower(extended.substr(0,first));std::string bytes;bool valid=true;
        auto hex=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
        for(size_t i=second+1;i<extended.size();++i) {
            if(extended[i]!='%'){bytes+=extended[i];continue;}
            if(i+2>=extended.size()||hex(extended[i+1])<0||hex(extended[i+2])<0){valid=false;break;}
            bytes+=static_cast<char>(hex(extended[i+1])*16+hex(extended[i+2]));i+=2;
        }
        if(valid&&(charset=="utf-8"||charset=="iso-8859-1"))name=decode(bytes,charset=="utf-8"?CP_UTF8:28591);
    }
    if(name.empty()){name=decode(plain,CP_UTF8);if(name.empty())name=decode(plain,28591);}
    if(name.empty()||name.size()>240 || name.find_first_of(L"/\\")!=name.npos)return {};
    for(auto& c:name) {if(c<32||c==127)return {};if(std::wstring_view(L":*?\"<>|").find(c)!=std::wstring_view::npos)c=L'_';}
    while(!name.empty()&&(name.back()==L'.'||name.back()==L' '))name.pop_back();
    if(name.empty()||name==L"."||name==L"..")return {};
    auto stem=name.substr(0,name.find(L'.'));while(!stem.empty()&&stem.back()==L' ')stem.pop_back();
    if(!_wcsicmp(stem.c_str(),L"CON")||!_wcsicmp(stem.c_str(),L"PRN")||!_wcsicmp(stem.c_str(),L"AUX")||!_wcsicmp(stem.c_str(),L"NUL")||
       (stem.size()==4&&(!_wcsnicmp(stem.c_str(),L"COM",3)||!_wcsnicmp(stem.c_str(),L"LPT",3))&&stem[3]>=L'1'&&stem[3]<=L'9'))name.insert(0,L"_");
    return name;
}
}
