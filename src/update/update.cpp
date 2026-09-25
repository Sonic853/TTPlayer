#include "ttplayer/update/update.h"
#include "ttplayer/core/text.h"
#include "ttplayer/platform/windows_features.h"
#include "json.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace ttplayer::update {
std::optional<Version> ParseVersion(std::string_view s) {
    if(s.size()<10 || s.size()>16 || s[4]!='.' || s[7]!='.') return {};
    Version v;
    auto number=[](std::string_view part,unsigned& value) {
        if(part.empty() || part.find_first_not_of("0123456789")!=part.npos) return false;
        auto r=std::from_chars(part.data(),part.data()+part.size(),value);
        return r.ec==std::errc{} && r.ptr==part.data()+part.size();
    };
    if(!number(s.substr(0,4),v.parts[0]) || !number(s.substr(5,2),v.parts[1]) || !number(s.substr(8,2),v.parts[2])) return {};
    if(s.size()>10 && (s[10]!='p' || s.size()<12 || s[11]=='0' || !number(s.substr(11),v.parts[3]))) return {};
    const auto [y,m,d,p]=v.parts;
    constexpr unsigned days[]{0,31,28,31,30,31,30,31,31,30,31,30,31};
    if(!y || m<1 || m>12 || !d || p>65535) return {};
    if(d>days[m]+(m==2 && y%4==0 && (y%100!=0 || y%400==0))) return {};
    return v;
}
std::wstring Version::Text() const {
    wchar_t out[40]{};swprintf_s(out,L"%04u.%02u.%02u",parts[0],parts[1],parts[2]);
    return std::wstring(out)+(parts[3] ? L"p"+std::to_wstring(parts[3]) : L"");
}
Version FileVersion(const std::filesystem::path& file) {
    DWORD unused{};const auto size=GetFileVersionInfoSizeW(file.c_str(),&unused);
    if(!size || size>1024*1024) throw std::runtime_error("Cannot read executable version");
    std::vector<unsigned char> data(size);
    if(!GetFileVersionInfoW(file.c_str(),0,size,data.data())) throw std::runtime_error("Cannot read executable version");
    VS_FIXEDFILEINFO* info{};UINT count{};
    if(!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&count) || count<sizeof(*info) || info->dwSignature!=0xfeef04bd)
        throw std::runtime_error("Invalid executable version");
    Version v{{HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS)}};
    if(!ParseVersion(core::WideToUtf8(v.Text()))) throw std::runtime_error("Executable does not use a release date version");
    return v;
}
std::filesystem::path ExecutablePath() {
    wchar_t path[32768]{};const DWORD size=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    if(!size || size>=std::size(path)) throw std::runtime_error("Cannot locate executable");
    return path;
}
bool IsXp() noexcept { return !platform::CurrentWindowsFeatures().version.AtLeast(6); }
std::wstring ErrorText(const std::exception& e) {return core::Utf8ToWide(e.what());}
namespace {
bool AssetUrl(std::string_view url,Source source) {
    const std::string prefix=source==Source::gitee ? "https://gitee.com/Sonic853/TTPlayer/releases/download/" :
        "https://github.com/Sonic853/TTPlayer/releases/download/";
    return url.starts_with(prefix) && url.find_first_of("\r\n\\") == url.npos;
}
}
std::vector<Release> ParseReleases(std::string_view json,Source source) {
    const auto root=detail::JsonReader(json).Read();
    if(root.kind!=detail::Json::array) throw std::runtime_error("Release API did not return a list");
    std::vector<Release> result;
    for(const auto& row:root.items) {
        const auto tag=row["tag_name"].String();const auto version=ParseVersion(tag);
        if(!version || row["draft"].True() || row["prerelease"].True()) continue;
        Release r;r.version=*version;r.tag=tag;r.asset_name="TTPlayerRebuild-"+tag+".zip";
        r.page=std::wstring(source==Source::gitee ? kGiteePage : kGitHubPage)+L"/tag/"+core::Utf8ToWide(tag);
        unsigned zip_count=0,sums_count=0;
        for(const auto& asset:row["assets"].items) {
            const auto name=asset["name"].String(),url=asset["browser_download_url"].String();
            if(!AssetUrl(url,source) || (asset["state"].kind!=detail::Json::null && asset["state"].String()!="uploaded")) continue;
            if(name==r.asset_name) {++zip_count;r.zip_url=core::Utf8ToWide(url);r.size=asset["size"].Integer();}
            if(name=="SHA256SUMS.txt") {++sums_count;r.sums_url=core::Utf8ToWide(url);}
        }
        if(zip_count==1 && sums_count==1 && r.size<=64ULL*1024*1024) result.push_back(std::move(r));
    }
    return result;
}
std::optional<Release> Check(Http& http,Source source,const Cancel& cancel) {
    std::optional<Release> best;std::set<std::string> seen;
    for(unsigned page=1;page<=20;++page) {
        if(cancel && cancel()) throw std::runtime_error("Canceled");
        std::wstring url=source==Source::gitee ? L"https://gitee.com/api/v5/repos/Sonic853/TTPlayer/releases?direction=desc&" :
            L"https://api.github.com/repos/Sonic853/TTPlayer/releases?";
        url+=L"per_page=20&page="+std::to_wstring(page);
        const auto text=http.Get(url,cancel);
        const auto root=detail::JsonReader(text).Read();
        if(root.kind!=detail::Json::array) throw std::runtime_error("Invalid release list");
        if(!seen.insert(text).second && !root.items.empty()) throw std::runtime_error("Release pagination repeated a page");
        for(auto& release:ParseReleases(text,source)) if(!best || release.version>best->version) best=std::move(release);
        if(root.items.size()<20) return best;
    }
    throw std::runtime_error("Release list exceeds the check limit; open the release page");
}
std::string ExpectedHash(std::string_view text,std::string_view name) {
    if(text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::string found;
    while(!text.empty()) {
        const auto end=text.find('\n');auto line=text.substr(0,end);
        if(!line.empty() && line.back()=='\r') line.remove_suffix(1);
        if(line.size()>=66) {
            auto file=line.substr(64);const auto start=file.find_first_not_of(" \t*");
            if(start!=file.npos && file.substr(start)==name) {
                if(!found.empty() || line[64]!=' ' || line.substr(0,64).find_first_not_of("0123456789abcdefABCDEF")!=line.npos)
                    throw std::runtime_error("Invalid or duplicate package checksum");
                found=line.substr(0,64);for(auto& c:found) if(c>='A' && c<='F') c+='a'-'A';
            }
        }
        if(end==text.npos) break;text.remove_prefix(end+1);
    }
    if(found.empty()) throw std::runtime_error("Package checksum is missing");return found;
}
}
