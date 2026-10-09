#include "legacy_lyric_http.h"
#include "ttplayer/core/text.h"
#include <wininet.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ttplayer::lyrics {
namespace {
struct Handle {
    HINTERNET value{};
    ~Handle() { if(value) InternetCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
void Check(BOOL ok) {
    if (!ok) throw std::runtime_error("HTTP " + std::to_string(GetLastError()));
}
std::wstring Lower(std::wstring s) {
    std::transform(s.begin(),s.end(),s.begin(),towlower); return s;
}
std::wstring Trim(std::wstring_view s) {
    const auto first=s.find_first_not_of(L" \t");
    return first==s.npos ? L"" : std::wstring(s.substr(first,s.find_last_not_of(L" \t")-first+1));
}
struct Address {
    std::wstring host, path, target, origin;
    INTERNET_PORT port{};
    explicit Address(const std::wstring& url) {
        URL_COMPONENTSW c{sizeof(c)};
        c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=static_cast<DWORD>(-1);
        Check(InternetCrackUrlW(url.c_str(),static_cast<DWORD>(url.size()),0,&c));
        if(c.nScheme!=INTERNET_SCHEME_HTTP) throw std::runtime_error("Legacy lyric transport requires HTTP");
        host.assign(c.lpszHostName,c.dwHostNameLength); port=c.nPort;
        path=c.dwUrlPathLength ? std::wstring(c.lpszUrlPath,c.dwUrlPathLength) : L"/";
        target=path;
        if(c.dwExtraInfoLength) target.append(c.lpszExtraInfo,c.dwExtraInfoLength);
        origin=Lower(host)+L":"+std::to_wstring(port);
    }
};
std::wstring Header(HINTERNET request,DWORD query,const wchar_t* name=nullptr,DWORD* index=nullptr) {
    // Query into a bounded buffer: also handles repeated Set-Cookie fields.
    wchar_t buffer[8192]{};
    if(name) wcscpy_s(buffer,name);
    DWORD bytes=sizeof(buffer);
    if(!HttpQueryInfoW(request,query,buffer,&bytes,index)) {
        if(GetLastError()==ERROR_INSUFFICIENT_BUFFER) throw std::runtime_error("Lyric HTTP header too large");
        return {};
    }
    return std::wstring(buffer,bytes/sizeof(wchar_t));
}
bool PathMatches(std::wstring_view path,std::wstring_view cookie) {
    return path.starts_with(cookie) && (path.size()==cookie.size() || cookie.back()==L'/' || path[cookie.size()]==L'/');
}
ULONGLONG Now() { FILETIME f{}; GetSystemTimeAsFileTime(&f); return (ULONGLONG(f.dwHighDateTime)<<32)|f.dwLowDateTime; }
}
struct LegacyLyricSession::State {
    struct Cookie { std::wstring origin,path,name,value; ULONGLONG expires{~ULONGLONG{}}; };
    std::mutex mutex;
    std::vector<Cookie> cookies;
    std::wstring RequestCookies(const Address& url) {
        std::wstring result;
        for(const auto& c:cookies) if(c.origin==url.origin && c.expires>Now() && PathMatches(url.path,c.path)) {
            if(!result.empty()) result+=L"; "; result+=c.name+L"="+c.value;
        }
        if(result.size()>16384) throw std::runtime_error("Too many lyric cookies");
        return result.empty() ? result : L"Cookie: "+result+L"\r\n";
    }
    void Store(const Address& url,const std::wstring& header) {
        const auto semi=header.find(L';'),eq=header.find(L'=');
        if(eq==header.npos || eq==0 || (semi!=header.npos && eq>semi)) return;
        Cookie c{url.origin,url.path.substr(0,url.path.rfind(L'/')+1),Trim(std::wstring_view(header).substr(0,eq)),
            Trim(std::wstring_view(header).substr(eq+1,semi==header.npos ? header.npos : semi-eq-1))};
        if(c.path.size()>1) c.path.pop_back();
        if(c.name.empty() || c.name.find_first_of(L"()<>@,;:\\\"/[]?={} \t")!=c.name.npos) return;
        for(auto ch:c.name+c.value) if(ch<0x21 || ch>0x7e) return;
        bool secure=false,domain_ok=true; std::optional<ULONGLONG> age;
        for(size_t start=semi;start!=header.npos;) {
            const auto end=header.find(L';',start+1);
            auto attr=Trim(std::wstring_view(header).substr(start+1,end==header.npos ? header.npos : end-start-1));
            const auto split=attr.find(L'='); const auto key=Lower(Trim(std::wstring_view(attr).substr(0,split)));
            auto value=split==attr.npos ? L"" : Trim(std::wstring_view(attr).substr(split+1));
            if(key==L"secure") secure=true;
            else if(key==L"path" && !value.empty() && value[0]==L'/') c.path=value;
            else if(key==L"domain") {
                value=Lower(value); if(!value.empty() && value[0]==L'.') value.erase(0,1);
                const auto host=Lower(url.host);
                domain_ok=!value.empty() && (host==value || (host.size()>value.size() && host.ends_with(L"."+value)));
            } else if(key==L"max-age" && !value.empty()) {
                wchar_t* endptr{}; const auto seconds=_wcstoi64(value.c_str(),&endptr,10);
                if(endptr!=value.c_str() && !*endptr) age=seconds<=0 ? 0 : Now()+static_cast<ULONGLONG>(std::min<__int64>(seconds,315360000))*10000000;
            } else if(key==L"expires") {
                SYSTEMTIME time{}; FILETIME f{};
                if(InternetTimeToSystemTimeW(value.c_str(),&time,0) && SystemTimeToFileTime(&time,&f))
                    c.expires=(ULONGLONG(f.dwHighDateTime)<<32)|f.dwLowDateTime;
            }
            start=end;
        }
        if(secure || !domain_ok) return; // This jar is deliberately confined to HTTP and the response origin.
        if(age) c.expires=*age;
        std::erase_if(cookies,[&](const Cookie& old) { return old.expires<=Now() ||
            (old.origin==c.origin && old.path==c.path && old.name==c.name); });
        if(c.expires>Now()) {
            if(cookies.size()>=64) throw std::runtime_error("Too many lyric cookies");
            cookies.push_back(std::move(c));
            std::stable_sort(cookies.begin(),cookies.end(),[](const Cookie& a,const Cookie& b){return a.path.size()>b.path.size();});
        }
    }
};
LegacyLyricSession::LegacyLyricSession():state_(std::make_unique<State>()) {}
LegacyLyricSession::~LegacyLyricSession()=default;
LegacyLyricResponse LegacyLyricSession::Fetch(const std::wstring& url,
    const settings::NetworkSettings& network,const std::function<bool()>& canceled) {
    std::lock_guard lock(state_->mutex);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(65);
    auto guard=[&] {
        if(canceled && canceled()) throw std::runtime_error("Canceled");
        if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("HTTP timeout");
    };
    guard(); Address address(url);
    DWORD access=network.proxy_type==0 ? INTERNET_OPEN_TYPE_DIRECT : INTERNET_OPEN_TYPE_PRECONFIG;
    std::wstring proxy;
    if(network.proxy_type>1 && !network.proxy_server.empty()) {
        access=INTERNET_OPEN_TYPE_PROXY; proxy=network.proxy_server;
        if(network.proxy_port>0) proxy+=L":"+std::to_wstring(network.proxy_port);
    }
    Handle session{InternetOpenW(L"TTPlayerRebuild/Lyrics",access,proxy.empty()?nullptr:proxy.c_str(),proxy.empty()?nullptr:L"<local>",0)};
    Check(session.value!=nullptr);
    DWORD timeout=10000;
    for(DWORD option:{INTERNET_OPTION_CONNECT_TIMEOUT,INTERNET_OPTION_SEND_TIMEOUT,INTERNET_OPTION_RECEIVE_TIMEOUT})
        Check(InternetSetOptionW(session,option,&timeout,sizeof(timeout)));
    Handle connection{InternetConnectW(session,address.host.c_str(),address.port,nullptr,nullptr,INTERNET_SERVICE_HTTP,0,0)};
    Check(connection.value!=nullptr);
    // NO_COOKIES prevents both global reads and writes. Redirects are handled by
    // the caller so an explicitly supplied Cookie cannot escape its origin.
    const bool proxy_auth=!proxy.empty() && !network.proxy_username.empty();
    Handle request{HttpOpenRequestW(connection,L"GET",address.target.c_str(),nullptr,nullptr,nullptr,
        INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_NO_COOKIES|
        INTERNET_FLAG_NO_AUTO_REDIRECT|INTERNET_FLAG_KEEP_CONNECTION|INTERNET_FLAG_NO_UI|
        (proxy_auth ? 0 : INTERNET_FLAG_NO_AUTH),0)};
    Check(request.value!=nullptr);
    // Keep explicit proxy authentication available, including NTLM connection
    // reuse. Modern WinINet can suppress origin auth independently (XP cannot).
    InternetSetOptionW(request,INTERNET_OPTION_SUPPRESS_SERVER_AUTH,nullptr,0);
    if(proxy_auth) {
        Check(InternetSetOptionW(request,INTERNET_OPTION_PROXY_USERNAME,const_cast<wchar_t*>(network.proxy_username.c_str()),static_cast<DWORD>((network.proxy_username.size()+1)*sizeof(wchar_t))));
        Check(InternetSetOptionW(request,INTERNET_OPTION_PROXY_PASSWORD,const_cast<wchar_t*>(network.proxy_password.c_str()),static_cast<DWORD>((network.proxy_password.size()+1)*sizeof(wchar_t))));
    }
    BOOL decoding=TRUE; InternetSetOptionW(request,INTERNET_OPTION_HTTP_DECODING,&decoding,sizeof(decoding));
    const auto headers=state_->RequestCookies(address);
    DWORD status{};
    for(int attempt=0;attempt<2;++attempt) {
        guard(); Check(HttpSendRequestW(request,headers.c_str(),static_cast<DWORD>(headers.size()),nullptr,0));
        DWORD bytes=sizeof(status);
        Check(HttpQueryInfoW(request,HTTP_QUERY_STATUS_CODE|HTTP_QUERY_FLAG_NUMBER,&status,&bytes,nullptr));
        if(status!=407 || attempt || proxy.empty() || network.proxy_username.empty()) break;
        Check(InternetSetOptionW(request,INTERNET_OPTION_PROXY_USERNAME,const_cast<wchar_t*>(network.proxy_username.c_str()),static_cast<DWORD>((network.proxy_username.size()+1)*sizeof(wchar_t))));
        Check(InternetSetOptionW(request,INTERNET_OPTION_PROXY_PASSWORD,const_cast<wchar_t*>(network.proxy_password.c_str()),static_cast<DWORD>((network.proxy_password.size()+1)*sizeof(wchar_t))));
    }
    guard();
    const auto store_cookies=[&] {
        for(DWORD index=0,count=0;count<64;++count) {
            auto cookie=Header(request,HTTP_QUERY_SET_COOKIE,nullptr,&index);
            if(cookie.empty()) break;
            state_->Store(address,cookie);
        }
    };
    LegacyLyricResponse response;
    if(status==301 || status==302 || status==303 || status==307 || status==308) {
        store_cookies();
        auto target=Header(request,HTTP_QUERY_LOCATION);
        if(target.empty()) throw std::runtime_error("Empty lyric redirect");
        wchar_t combined[16384]{}; DWORD length=static_cast<DWORD>(std::size(combined));
        Check(InternetCombineUrlW(url.c_str(),target.c_str(),combined,&length,ICU_NO_ENCODE));
        response.redirect.assign(combined,length); return response;
    }
    if(status!=200) throw std::runtime_error("HTTP status "+std::to_string(status));
    char buffer[8192];
    for(;;) {
        guard(); DWORD count{}; Check(InternetReadFile(request,buffer,sizeof(buffer),&count));
        if(!count) break;
        if(response.body.size()+count>2*1024*1024) throw std::runtime_error("Lyric response exceeds 2 MiB");
        response.body.append(buffer,count);
    }
    // Follow 60352C1E: consume the body before querying optional headers.
    store_cookies();
    response.title_header=Header(request,HTTP_QUERY_CUSTOM,L"tt-title");
    response.url_header=Header(request,HTTP_QUERY_CUSTOM,L"tt-url");
    guard(); return response;
}
}
