#include "https_provider.h"
#include "ttplayer/net/ttp_https.h"
#include <windows.h>
#include <cstring>
#include <cwchar>
#include <stdexcept>

namespace ttplayer::lyrics {
namespace {
struct Provider {
    HMODULE module{};
    const ttp_https_api* api{};
    Provider() noexcept {
        wchar_t path[32768]{};
        const auto count=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
        if (!count || count>=std::size(path)) return;
        auto slash=std::wcsrchr(path,L'\\');
        if (!slash) return;
        constexpr wchar_t relative[]=L"AddIn\\ttp_https.dll";
        const auto prefix=static_cast<size_t>(slash-path)+1;
        if (prefix+std::size(relative)>std::size(path)) return;
        std::memcpy(path+prefix,relative,sizeof(relative));
        // Optional component: no PATH/current-directory search, and no hard
        // dependency that prevents startup when the DLL cannot be loaded.
        module=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) return;
        auto get=reinterpret_cast<ttp_https_get_api_fn>(GetProcAddress(module,"ttp_https_get_api"));
        api=get ? get(TTP_HTTPS_ABI_VERSION) : nullptr;
        if (!api || api->size!=sizeof(*api) || api->abi_version!=TTP_HTTPS_ABI_VERSION ||
            !api->get || !api->release) {
            api=nullptr;FreeLibrary(module);module=nullptr;
        }
        // Successful module remains loaded until process termination; pending
        // worker requests can safely release their DLL-owned response buffers.
    }
};
int __cdecl Canceled(void* context) noexcept {
    const auto& callback=*static_cast<const std::function<bool()>*>(context);
    try { return callback && callback() ? 1 : 0; } catch (...) { return 1; }
}
}
std::optional<HttpsProviderResponse> FetchHttpsProvider(const std::wstring& url,
    const settings::NetworkSettings& network,const std::function<bool()>& canceled) {
    static const Provider provider;
    if (!provider.api) return std::nullopt;
    ttp_https_request request{};request.size=sizeof(request);request.url=url.c_str();
    request.proxy_type=network.proxy_type;request.proxy_server=network.proxy_server.c_str();
    request.proxy_port=network.proxy_port;request.proxy_has_credentials=!network.proxy_username.empty();
    request.proxy_username=network.proxy_username.c_str();request.proxy_password=network.proxy_password.c_str();
    request.canceled=Canceled;request.cancel_context=const_cast<std::function<bool()>*>(&canceled);
    ttp_https_response response{};response.size=sizeof(response);
    struct Cleanup {
        const ttp_https_api* api;ttp_https_response& response;
        ~Cleanup() { api->release(&response); }
    } cleanup{provider.api,response};
    char error[512]{};
    const int status=provider.api->get(&request,&response,error,sizeof(error));
    error[sizeof(error)-1]=0;
    if (status==TTP_HTTPS_USE_WINHTTP) return std::nullopt;
    if (status!=TTP_HTTPS_OK) throw std::runtime_error(*error ? error : "HTTPS provider request failed");
    if ((!response.body && response.body_size) || response.body_size>2*1024*1024)
        throw std::runtime_error("Invalid HTTPS provider response");
    return HttpsProviderResponse{
        response.body_size ? std::string(reinterpret_cast<const char*>(response.body),response.body_size) : std::string{},
        response.title_header ? response.title_header : "",response.url_header ? response.url_header : ""};
}
}
