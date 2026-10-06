#include "ttplayer/net/http.h"
#include "ttplayer/net/ttp_https.h"
#include "ttplayer/core/text.h"
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <chrono>
#include <stdexcept>

namespace ttplayer::net {
namespace {
struct Module { HMODULE handle{}; ~Module(){if(handle)FreeLibrary(handle);} };
struct Internet { HINTERNET handle{}; ~Internet(){if(handle)WinHttpCloseHandle(handle);} operator HINTERNET() const{return handle;} };
void Check(BOOL ok) {if(!ok)throw std::runtime_error("HTTPS error "+std::to_string(GetLastError()));}
int __cdecl Canceled(void* context) noexcept {
    try {const auto& f=*static_cast<const std::function<bool()>*>(context);return f && f();}catch(...){return 1;}
}
}
HttpResponse GetHttps(const std::wstring& url,const std::string& agent,
    const settings::NetworkSettings& network,const std::function<bool()>& canceled) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    const auto guard=[&]{if((canceled&&canceled()) || std::chrono::steady_clock::now()>deadline)throw std::runtime_error("HTTPS canceled or timed out");};
    guard();
    if(!url.starts_with(L"https://") || url.size()>8192 || url.find_first_of(L"\r\n\\")!=url.npos)
        throw std::runtime_error("Invalid HTTPS URL");
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    Module module{LoadLibraryExW((std::filesystem::path(executable).parent_path()/L"AddIn"/L"ttp_https.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH)};
    const auto get=module.handle?reinterpret_cast<ttp_https_get_api_fn>(GetProcAddress(module.handle,"ttp_https_get_api")):nullptr;
    const auto* base=get?get(TTP_HTTPS_HTTP_ABI_VERSION):nullptr;
    if(base && base->size==sizeof(ttp_https_api_v4) && base->abi_version==TTP_HTTPS_HTTP_ABI_VERSION) {
        const auto* api=reinterpret_cast<const ttp_https_api_v4*>(base);
        if(!api->get_http || !api->release_http)throw std::runtime_error("Invalid HTTPS ABI 4");
        ttp_https_http_request request{};request.size=sizeof(request);auto& r=request.request;
        r.size=sizeof(r);r.url=url.c_str();r.proxy_type=network.proxy_type;
        r.proxy_server=network.proxy_server.c_str();r.proxy_port=network.proxy_port;
        r.proxy_username=network.proxy_username.c_str();r.proxy_password=network.proxy_password.c_str();
        r.proxy_has_credentials=!network.proxy_username.empty();r.canceled=Canceled;
        r.cancel_context=const_cast<std::function<bool()>*>(&canceled);
        request.user_agent=agent.c_str();request.accept="application/json";
        ttp_https_http_response response{};response.size=sizeof(response);response.response.size=sizeof(response.response);
        struct Release {const ttp_https_api_v4* api;ttp_https_http_response& response;~Release(){api->release_http(&response);}} release{api,response};
        char error[512]{};const auto status=api->get_http(&request,&response,error,sizeof(error));error[511]=0;
        if(status==TTP_HTTPS_OK) {
            if(response.response.body_size>2*1024*1024 || (!response.response.body && response.response.body_size))throw std::runtime_error("Invalid HTTPS body");
            return {response.http_status,response.response.body_size ? std::string(reinterpret_cast<const char*>(response.response.body),response.response.body_size) : std::string{},response.retry_after?response.retry_after:""};
        }
        if(status!=TTP_HTTPS_USE_WINHTTP)throw std::runtime_error(*error?error:"HTTPS failed");
    }
    // Native fallback retains certificate verification and system proxy auth.
    Internet session{WinHttpOpen(core::Utf8ToWide(agent).c_str(),WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,0)};Check(session.handle!=nullptr);
    Check(WinHttpSetTimeouts(session,10000,10000,15000,15000));
    URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
    Check(WinHttpCrackUrl(url.c_str(),0,0,&parts));
    const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    auto path=std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength)+std::wstring(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    Internet connection{WinHttpConnect(session,host.c_str(),parts.nPort,0)};Check(connection.handle!=nullptr);
    Internet request{WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,nullptr,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};Check(request.handle!=nullptr);
    WINHTTP_PROXY_INFO proxy{};std::wstring explicit_proxy;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
    struct FreeProxy {WINHTTP_PROXY_INFO& proxy;WINHTTP_CURRENT_USER_IE_PROXY_CONFIG& ie;
        ~FreeProxy(){if(proxy.lpszProxy)GlobalFree(proxy.lpszProxy);if(proxy.lpszProxyBypass)GlobalFree(proxy.lpszProxyBypass);
        if(ie.lpszAutoConfigUrl)GlobalFree(ie.lpszAutoConfigUrl);if(ie.lpszProxy)GlobalFree(ie.lpszProxy);if(ie.lpszProxyBypass)GlobalFree(ie.lpszProxyBypass);}} free_proxy{proxy,ie};
    if(network.proxy_type==1 && WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
        WINHTTP_AUTOPROXY_OPTIONS options{};options.fAutoLogonIfChallenged=TRUE;
        if(ie.lpszAutoConfigUrl){options.dwFlags=WINHTTP_AUTOPROXY_CONFIG_URL;options.lpszAutoConfigUrl=ie.lpszAutoConfigUrl;}
        else if(ie.fAutoDetect){options.dwFlags=WINHTTP_AUTOPROXY_AUTO_DETECT;options.dwAutoDetectFlags=WINHTTP_AUTO_DETECT_TYPE_DHCP|WINHTTP_AUTO_DETECT_TYPE_DNS_A;}
        if(options.dwFlags && WinHttpGetProxyForUrl(session,url.c_str(),&options,&proxy))Check(WinHttpSetOption(request,WINHTTP_OPTION_PROXY,&proxy,sizeof(proxy)));
        else if(ie.lpszProxy){WINHTTP_PROXY_INFO configured{WINHTTP_ACCESS_TYPE_NAMED_PROXY,ie.lpszProxy,ie.lpszProxyBypass};Check(WinHttpSetOption(request,WINHTTP_OPTION_PROXY,&configured,sizeof(configured)));}
    } else if(network.proxy_type>1) {
        explicit_proxy=network.proxy_server;
        if(network.proxy_port>0)explicit_proxy+=L":"+std::to_wstring(network.proxy_port);
        WINHTTP_PROXY_INFO configured{WINHTTP_ACCESS_TYPE_NAMED_PROXY,explicit_proxy.data(),nullptr};Check(WinHttpSetOption(request,WINHTTP_OPTION_PROXY,&configured,sizeof(configured)));
    }
    DWORD status{};
    for(unsigned attempt=0;;++attempt) {
        guard();Check(WinHttpSendRequest(request,L"Accept: application/json\r\n",DWORD(-1),nullptr,0,0,0));Check(WinHttpReceiveResponse(request,nullptr));
        DWORD size=sizeof(status);Check(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr));
        if(status!=407 || attempt || network.proxy_username.empty())break;
        DWORD supported{},first{},target{};Check(WinHttpQueryAuthSchemes(request,&supported,&first,&target));
        DWORD scheme=supported&WINHTTP_AUTH_SCHEME_NEGOTIATE?WINHTTP_AUTH_SCHEME_NEGOTIATE:supported&WINHTTP_AUTH_SCHEME_NTLM?WINHTTP_AUTH_SCHEME_NTLM:WINHTTP_AUTH_SCHEME_BASIC;
        Check(WinHttpSetCredentials(request,WINHTTP_AUTH_TARGET_PROXY,scheme,network.proxy_username.c_str(),network.proxy_password.c_str(),nullptr));
    }
    HttpResponse result;result.status=status;
    wchar_t retry[256]{};DWORD size=sizeof(retry);
    if(WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,L"Retry-After",retry,&size,nullptr))result.retry_after=core::WideToUtf8(retry);
    char buffer[16384];DWORD read{};
    do {guard();Check(WinHttpReadData(request,buffer,sizeof(buffer),&read));
        if(result.body.size()+read>2*1024*1024)throw std::runtime_error("HTTPS response too large");result.body.append(buffer,read);
    }while(read);
    return result;
}
}
