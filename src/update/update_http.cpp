#include "ttplayer/update/update.h"
#include "ttplayer/net/ttp_https.h"
#include "ttplayer/core/text.h"
#include "ttplayer/update_build_config.h"
#include <winhttp.h>
#include <shlwapi.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace ttplayer::update {
namespace {
struct HttpError:std::runtime_error {
    unsigned status;
    explicit HttpError(unsigned n):std::runtime_error("HTTP status "+std::to_string(n)),status(n) {}
};
struct Internet {
    HINTERNET value{};
    ~Internet(){if(value) WinHttpCloseHandle(value);}
    operator HINTERNET() const{return value;}
};
void CheckWin(BOOL ok) {if(!ok) throw std::runtime_error("Network error "+std::to_string(GetLastError()));}
void ProviderError(int code,const char* message) {
    if(code==TTP_HTTPS_CANCELED) throw std::runtime_error("Canceled");
    std::string text=message;
    if(text.starts_with("HTTP status ")) {
        unsigned status=0;
        for(char c:text.substr(12)) {if(c<'0'||c>'9') break;status=status*10+c-'0';}
        throw HttpError(status);
    }
    throw std::runtime_error(text.empty() ? "HTTPS component request failed" : text);
}
std::wstring Header(HINTERNET request,const wchar_t* name) {
    DWORD size=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,name,nullptr,&size,nullptr);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || size>65536) return {};
    std::wstring out(size/2,L'\0');
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,name,out.data(),&size,nullptr)) return {};
    out.resize(size/2);while(!out.empty() && out.back()==0) out.pop_back();return out;
}
std::wstring TokenUrl(const std::wstring& url) {
    if(!url.starts_with(L"https://gitee.com/") || !*kUpdaterGiteeToken) return {};
    std::wstring encoded;constexpr wchar_t hex[]=L"0123456789ABCDEF";
    for(const unsigned char c:std::string_view(kUpdaterGiteeToken)) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') encoded+=c;
        else {encoded+=L'%';encoded+=hex[c>>4];encoded+=hex[c&15];}
    }
    return url+(url.find(L'?')==url.npos ? L"?access_token=" : L"&access_token=")+encoded;
}
}
struct Http::Impl {
    settings::NetworkSettings network;
    HMODULE module{};
    const ttp_https_api* api{};
    const ttp_https_api_v3* streaming{};
    explicit Impl(const std::filesystem::path& runtime,settings::NetworkSettings n):network(std::move(n)) {
        const auto path=std::filesystem::absolute(runtime/L"AddIn"/L"ttp_https.dll");
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module) return;
        const auto get=reinterpret_cast<ttp_https_get_api_fn>(GetProcAddress(module,"ttp_https_get_api"));
        if(get) {
            api=get(TTP_HTTPS_ABI_VERSION);
            if(!api || api->size!=sizeof(*api) || api->abi_version!=TTP_HTTPS_ABI_VERSION || !api->get || !api->release) api=nullptr;
            const auto next=get(TTP_HTTPS_DOWNLOAD_ABI_VERSION);
            if(next && next->size==sizeof(ttp_https_api_v3) && next->abi_version==TTP_HTTPS_DOWNLOAD_ABI_VERSION) {
                streaming=reinterpret_cast<const ttp_https_api_v3*>(next);
                if(!streaming->download) streaming=nullptr;
            }
        }
    }
    ~Impl(){if(module) FreeLibrary(module);}
    static int __cdecl Canceled(void* data) noexcept {
        try {const auto& f=*static_cast<const Cancel*>(data);return f && f();} catch(...){return 1;}
    }
    ttp_https_request Request(const std::wstring& url,const Cancel& canceled) const {
        ttp_https_request r{};r.size=sizeof(r);r.url=url.c_str();
        r.proxy_type=network.proxy_type;r.proxy_server=network.proxy_server.c_str();r.proxy_port=network.proxy_port;
        r.proxy_username=network.proxy_username.c_str();r.proxy_password=network.proxy_password.c_str();
        r.proxy_has_credentials=!network.proxy_username.empty();r.canceled=Canceled;r.cancel_context=const_cast<Cancel*>(&canceled);
        return r;
    }
    using Sink=std::function<void(const unsigned char*,size_t,uint64_t,uint64_t)>;
    void Native(std::wstring url,uint64_t limit,const Cancel& canceled,const Sink& sink) {
        if(IsXp()) throw std::runtime_error(api ? "HTTPS proxy requires unsupported native transport on XP" : core::WideToUtf8(kMissingHttps));
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(10);
        auto guard=[&]{if(canceled && canceled()) throw std::runtime_error("Canceled");
            if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Download timeout");};
        for(int redirect=0;redirect<=8;++redirect) {
            guard();
            if(!url.starts_with(L"https://") || url.size()>8192 || url.find_first_of(L"\r\n\\")!=url.npos)
                throw std::runtime_error("Invalid HTTPS update URL");
            URL_COMPONENTS p{sizeof(p)};
            p.dwHostNameLength=p.dwUrlPathLength=p.dwExtraInfoLength=p.dwUserNameLength=p.dwPasswordLength=DWORD(-1);
            CheckWin(WinHttpCrackUrl(url.c_str(),static_cast<DWORD>(url.size()),0,&p));
            if(p.dwUserNameLength || p.dwPasswordLength || !p.dwHostNameLength) throw std::runtime_error("Invalid update URL credentials");
            std::wstring host(p.lpszHostName,p.dwHostNameLength),path=p.dwUrlPathLength ? std::wstring(p.lpszUrlPath,p.dwUrlPathLength) : L"/";
            if(p.dwExtraInfoLength) path.append(p.lpszExtraInfo,p.dwExtraInfoLength);
            std::wstring proxy;
            DWORD access=network.proxy_type==0 ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
            if(network.proxy_type>1 && !network.proxy_server.empty()) {
                proxy=network.proxy_server;
                if(proxy.starts_with(L"socks")) throw std::runtime_error("SOCKS proxy requires the HTTPS component");
                if(proxy.starts_with(L"http://")) proxy.erase(0,7);
                if(network.proxy_port>0) {auto colon=proxy.rfind(L':');if(colon!=proxy.npos) proxy.resize(colon);proxy+=L":"+std::to_wstring(network.proxy_port);}
                access=WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            }
            Internet session{WinHttpOpen(L"TTPlayerRebuild/Updater",access,proxy.empty()?nullptr:proxy.c_str(),nullptr,0)};
            bool resolve_ie=false;
            if(!session.value && access==WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY && GetLastError()==ERROR_INVALID_PARAMETER) {
                session.value=WinHttpOpen(L"TTPlayerRebuild/Updater",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,0);
                resolve_ie=true;
            }
            CheckWin(session.value!=nullptr);CheckWin(WinHttpSetTimeouts(session,5000,10000,15000,15000));
            DWORD protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2|WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            if(!WinHttpSetOption(session,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols))) {
                protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;CheckWin(WinHttpSetOption(session,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols)));
            }
            Internet connection{WinHttpConnect(session,host.c_str(),p.nPort,0)};CheckWin(connection.value!=nullptr);
            Internet request{WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,nullptr,nullptr,WINHTTP_FLAG_SECURE)};CheckWin(request.value!=nullptr);
            if(resolve_ie) {
                // Win7 has no AUTOMATIC_PROXY session. Resolve the same IE /
                // PAC setting requested by the player's network options.
                WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
                if(WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
                    struct FreeIe {WINHTTP_CURRENT_USER_IE_PROXY_CONFIG& ie;~FreeIe(){GlobalFree(ie.lpszAutoConfigUrl);GlobalFree(ie.lpszProxy);GlobalFree(ie.lpszProxyBypass);}} free_ie{ie};
                    WINHTTP_PROXY_INFO info{static_cast<DWORD>(ie.lpszProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY),ie.lpszProxy,ie.lpszProxyBypass};
                    if(ie.lpszAutoConfigUrl || ie.fAutoDetect) {
                        WINHTTP_AUTOPROXY_OPTIONS options{};options.fAutoLogonIfChallenged=FALSE;
                        if(ie.lpszAutoConfigUrl) {options.dwFlags=WINHTTP_AUTOPROXY_CONFIG_URL;options.lpszAutoConfigUrl=ie.lpszAutoConfigUrl;}
                        else {options.dwFlags=WINHTTP_AUTOPROXY_AUTO_DETECT;options.dwAutoDetectFlags=WINHTTP_AUTO_DETECT_TYPE_DHCP|WINHTTP_AUTO_DETECT_TYPE_DNS_A;}
                        WINHTTP_PROXY_INFO resolved{};
                        if(WinHttpGetProxyForUrl(session,url.c_str(),&options,&resolved)) {
                            const auto ok=WinHttpSetOption(request,WINHTTP_OPTION_PROXY,&resolved,sizeof(resolved));
                            const auto error=GetLastError();GlobalFree(resolved.lpszProxy);GlobalFree(resolved.lpszProxyBypass);
                            if(!ok) {SetLastError(error);CheckWin(FALSE);}
                        } else if(ie.lpszAutoConfigUrl) CheckWin(FALSE);
                        else CheckWin(WinHttpSetOption(request,WINHTTP_OPTION_PROXY,&info,sizeof(info)));
                    } else CheckWin(WinHttpSetOption(request,WINHTTP_OPTION_PROXY,&info,sizeof(info)));
                }
            }
            DWORD disable=WINHTTP_DISABLE_REDIRECTS;CheckWin(WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)));
            DWORD logon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;WinHttpSetOption(request,WINHTTP_OPTION_AUTOLOGON_POLICY,&logon,sizeof(logon));
            DWORD status{};
            for(int attempt=0;attempt<2;++attempt) {
                guard();CheckWin(WinHttpSendRequest(request,L"Accept: */*\r\nAccept-Encoding: identity\r\n",DWORD(-1),nullptr,0,0,0));
                CheckWin(WinHttpReceiveResponse(request,nullptr));DWORD size=sizeof(status);
                CheckWin(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr));
                if(status!=407 || attempt || network.proxy_username.empty()) break;
                DWORD supported{},first{},target{};CheckWin(WinHttpQueryAuthSchemes(request,&supported,&first,&target));
                DWORD scheme=supported&WINHTTP_AUTH_SCHEME_NEGOTIATE ? WINHTTP_AUTH_SCHEME_NEGOTIATE : supported&WINHTTP_AUTH_SCHEME_NTLM ? WINHTTP_AUTH_SCHEME_NTLM : supported&WINHTTP_AUTH_SCHEME_DIGEST ? WINHTTP_AUTH_SCHEME_DIGEST : WINHTTP_AUTH_SCHEME_BASIC;
                CheckWin(WinHttpSetCredentials(request,WINHTTP_AUTH_TARGET_PROXY,scheme,network.proxy_username.c_str(),network.proxy_password.c_str(),nullptr));
            }
            if(status==301 || status==302 || status==303 || status==307 || status==308) {
                const auto location=Header(request,L"Location");wchar_t combined[8192]{};DWORD count=std::size(combined);
                if(location.empty() || FAILED(UrlCombineW(url.c_str(),location.c_str(),combined,&count,URL_DONT_ESCAPE_EXTRA_INFO))) throw std::runtime_error("Invalid update redirect");
                url=combined;continue;
            }
            if(status!=200) throw HttpError(status);
            const auto encoding=Header(request,L"Content-Encoding");
            if(!encoding.empty() && _wcsicmp(encoding.c_str(),L"identity")!=0) throw std::runtime_error("Unexpected download encoding");
            const auto length=Header(request,L"Content-Length");uint64_t total=0;
            for(wchar_t c:length) {if(c<L'0'||c>L'9'||total>limit/10) throw std::runtime_error("Invalid download length");total=total*10+c-L'0';}
            if(total>limit) throw std::runtime_error("Download exceeds limit");
            unsigned char bytes[65536];uint64_t received=0;
            for(;;) {guard();DWORD count{};CheckWin(WinHttpReadData(request,bytes,sizeof(bytes),&count));if(!count) break;
                if(count>limit-received) throw std::runtime_error("Download exceeds limit");received+=count;sink(bytes,count,received,total);}
            if(!length.empty() && received!=total) throw std::runtime_error("Truncated download");return;
        }
        throw std::runtime_error("Too many download redirects");
    }
    template<class Fn> void AnonymousFirst(const std::wstring& url,Fn&& fetch) {
        try {fetch(url);} catch(const HttpError& error) {
            const auto retry=TokenUrl(url);
            if((error.status!=401 && error.status!=403 && error.status!=404) || retry.empty()) throw;
            fetch(retry);
        }
    }
};
Http::Http(std::filesystem::path runtime,settings::NetworkSettings network):impl_(std::make_unique<Impl>(runtime,std::move(network))) {}
Http::~Http()=default;
bool Http::HasProvider() const noexcept{return impl_->api!=nullptr;}
std::string Http::Get(const std::wstring& url,const Cancel& canceled) {
    std::string output;
    impl_->AnonymousFirst(url,[&](const std::wstring& address) {
        output.clear();
        if(impl_->api) {
            auto request=impl_->Request(address,canceled);ttp_https_response response{};response.size=sizeof(response);
            struct Cleanup{const ttp_https_api* api;ttp_https_response& response;~Cleanup(){api->release(&response);}}cleanup{impl_->api,response};
            char error[512]{};const int code=impl_->api->get(&request,&response,error,sizeof(error));error[511]=0;
            if(code==TTP_HTTPS_OK) {
                if(response.body_size>2*1024*1024 || (!response.body && response.body_size)) throw std::runtime_error("Invalid HTTPS response");
                if(response.body_size) output.assign(reinterpret_cast<const char*>(response.body),response.body_size);return;
            }
            if(code!=TTP_HTTPS_USE_WINHTTP) ProviderError(code,error);
        }
        impl_->Native(address,2*1024*1024,canceled,[&](const unsigned char* p,size_t n,uint64_t,uint64_t){output.append(reinterpret_cast<const char*>(p),n);});
    });
    return output;
}
void Http::Download(const std::wstring& url,const std::filesystem::path& file,uint64_t expected,const Cancel& canceled,const Progress& progress) {
    if(expected>64ULL*1024*1024) throw std::runtime_error("Update package too large");
    impl_->AnonymousFirst(url,[&](const std::wstring& address) {
        HANDLE handle=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create download file");
        struct Close{HANDLE h;~Close(){CloseHandle(h);}}close{handle};
        uint64_t written=0;
        Impl::Sink sink=[&](const unsigned char* p,size_t n,uint64_t received,uint64_t total) {
            const uint64_t limit=expected ? expected : 64ULL*1024*1024;
            if(n>limit-written || (!p && n)) throw std::runtime_error("Download exceeds limit");
            DWORD count{};if(!WriteFile(handle,p,static_cast<DWORD>(n),&count,nullptr) || count!=n) throw std::runtime_error("Cannot write download");
            written+=n;if(progress) progress(received,expected ? expected : total);
        };
        bool complete=false;
        if(impl_->streaming) {
            struct Context{Impl::Sink* sink;std::exception_ptr error;};Context context{&sink,{}};
            ttp_https_download_request request{};request.size=sizeof(request);request.request=impl_->Request(address,canceled);
            request.max_size=expected ? expected : 64ULL*1024*1024;request.write_context=&context;
            request.write=[](void* p,const unsigned char* bytes,size_t n,uint64_t received,uint64_t total)->int {
                auto& c=*static_cast<Context*>(p);try{(*c.sink)(bytes,n,received,total);return 1;}catch(...){c.error=std::current_exception();return 0;}
            };
            char error[512]{};const auto code=impl_->streaming->download(&request,error,sizeof(error));error[511]=0;
            if(context.error) std::rethrow_exception(context.error);
            if(code==TTP_HTTPS_OK) complete=true;
            else if(code!=TTP_HTTPS_USE_WINHTTP) ProviderError(code,error);
        } else if(impl_->api && (!expected || expected<=2*1024*1024)) {
            // Older ABI 2 plugins can still install existing small releases.
            const auto body=Get(address,canceled);sink(reinterpret_cast<const unsigned char*>(body.data()),body.size(),body.size(),body.size());complete=true;
        }
        if(!complete) {
            if(impl_->api && !impl_->streaming && IsXp()) throw std::runtime_error("HTTPS 组件版本过旧，请从发布页更新 ttp_https.dll。");
            impl_->Native(address,expected ? expected : 64ULL*1024*1024,canceled,sink);
        }
        if(!written || (expected && written!=expected)) throw std::runtime_error("Package download length mismatch");
        if(!FlushFileBuffers(handle)) throw std::runtime_error("Cannot flush downloaded package");
    });
}
}
