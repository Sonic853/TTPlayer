// System fallback for personal-library streams when the optional transport is
// absent/too old or explicitly requests native proxy handling. Never used to
// retry a portable TLS, HTTP status, or body-read failure.
class NativeMediaRequest {
    HINTERNET session_{},connection_{},request_{};
public:
    ~NativeMediaRequest(){Close();}
    void Close(){if(request_)InternetCloseHandle(request_);if(connection_)InternetCloseHandle(connection_);if(session_)InternetCloseHandle(session_);request_=connection_=session_=nullptr;}
    static std::wstring Origin(const std::wstring& url){
        wchar_t host[4096]{},user[4096]{},password[4096]{};URL_COMPONENTSW c{sizeof(c)};
        c.lpszHostName=host;c.dwHostNameLength=4096;c.lpszUserName=user;c.dwUserNameLength=4096;c.lpszPassword=password;c.dwPasswordLength=4096;
        Require(InternetCrackUrlW(url.c_str(),0,0,&c)&&c.dwHostNameLength&&!c.dwUserNameLength&&!c.dwPasswordLength&&(c.nScheme==INTERNET_SCHEME_HTTP||c.nScheme==INTERNET_SCHEME_HTTPS));
        std::wstring origin=std::to_wstring(c.nScheme)+L":"+host+L":"+std::to_wstring(c.nPort);std::transform(origin.begin(),origin.end(),origin.begin(),towlower);return origin;
    }
    uint32_t Open(const std::wstring& url,const std::wstring& authorization,const PlaybackOptions& options,uint64_t offset,const std::string& validator){
        Close();Origin(url);
        // WinINet uses system trust and proxy settings. Do not silently replace
        // a caller-owned CA or PAC policy with a different system policy.
        if(!options.network_ca_pem.empty()||!options.network_pac_url.empty()||options.network_auto_detect!=-1)
            throw Failure{E_NOTIMPL};
        Require(authorization.size()<=16384&&std::all_of(authorization.begin(),authorization.end(),[](wchar_t c){return c>=32&&c<=126;}));
        const DWORD type=options.network_proxy_type==0?INTERNET_OPEN_TYPE_DIRECT:options.network_proxy_type==1?INTERNET_OPEN_TYPE_PRECONFIG:INTERNET_OPEN_TYPE_PROXY;
        std::wstring proxy=options.network_proxy_server;if(type==INTERNET_OPEN_TYPE_PROXY){Require(!proxy.empty());if(options.network_proxy_port>0)proxy+=L":"+std::to_wstring(options.network_proxy_port);}
        session_=InternetOpenW(L"TTPlayer-Media/1.0",type,type==INTERNET_OPEN_TYPE_PROXY?proxy.c_str():nullptr,nullptr,0);Check(session_!=nullptr);
        DWORD timeout=static_cast<DWORD>(std::clamp(options.network_timeout_ms,100,60000));
        for(auto option:{INTERNET_OPTION_CONNECT_TIMEOUT,INTERNET_OPTION_RECEIVE_TIMEOUT,INTERNET_OPTION_SEND_TIMEOUT})Check(InternetSetOptionW(session_,option,&timeout,sizeof(timeout)));
        std::vector<wchar_t> data((url.size()+1)*3);const auto capacity=static_cast<DWORD>(url.size()+1);URL_COMPONENTSW c{sizeof(c)};
        c.lpszHostName=data.data();c.dwHostNameLength=capacity;c.lpszUrlPath=data.data()+capacity;c.dwUrlPathLength=capacity;c.lpszExtraInfo=data.data()+capacity*2;c.dwExtraInfoLength=capacity;
        Check(InternetCrackUrlW(url.c_str(),0,0,&c));std::wstring path(c.lpszUrlPath,c.dwUrlPathLength);if(path.empty())path=L"/";path.append(c.lpszExtraInfo,c.dwExtraInfoLength);
        connection_=InternetConnectW(session_,c.lpszHostName,c.nPort,nullptr,nullptr,INTERNET_SERVICE_HTTP,0,0);Check(connection_!=nullptr);
        const wchar_t* accept[]={L"*/*",nullptr};DWORD flags=INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_NO_COOKIES|INTERNET_FLAG_NO_AUTH|INTERNET_FLAG_NO_AUTO_REDIRECT|INTERNET_FLAG_NO_UI;
        if(c.nScheme==INTERNET_SCHEME_HTTPS)flags|=INTERNET_FLAG_SECURE;
        request_=HttpOpenRequestW(connection_,L"GET",path.c_str(),nullptr,nullptr,accept,flags,0);Check(request_!=nullptr);
        if(!options.network_proxy_username.empty()){
            Check(InternetSetOptionW(request_,INTERNET_OPTION_PROXY_USERNAME,const_cast<wchar_t*>(options.network_proxy_username.c_str()),static_cast<DWORD>((options.network_proxy_username.size()+1)*sizeof(wchar_t))));
            Check(InternetSetOptionW(request_,INTERNET_OPTION_PROXY_PASSWORD,const_cast<wchar_t*>(options.network_proxy_password.c_str()),static_cast<DWORD>((options.network_proxy_password.size()+1)*sizeof(wchar_t))));}
        std::wstring headers=L"Accept-Encoding: identity\r\nRange: bytes="+std::to_wstring(offset)+L"-\r\n";
        if(offset&&!validator.empty())headers+=L"If-Range: "+Text(validator)+L"\r\n";
        if(!authorization.empty())headers+=L"Authorization: "+authorization+L"\r\n";
        Check(HttpSendRequestW(request_,headers.c_str(),static_cast<DWORD>(headers.size()),nullptr,0));
        DWORD status{},size=sizeof(status);Check(HttpQueryInfoW(request_,HTTP_QUERY_STATUS_CODE|HTTP_QUERY_FLAG_NUMBER,&status,&size,nullptr));return status;
    }
    std::string Header(const char* name) const {
        std::array<char,16384> buffer{};const auto length=strlen(name);if(length>=buffer.size())return {};memcpy(buffer.data(),name,length+1);DWORD bytes=static_cast<DWORD>(buffer.size());
        if(!HttpQueryInfoA(request_,HTTP_QUERY_CUSTOM,buffer.data(),&bytes,nullptr))return {};return std::string(buffer.data(),bytes);
    }
    int Read(void* data,uint32_t capacity,uint32_t* got){DWORD n{};if(!InternetReadFile(request_,data,capacity,&n)){*got=0;return GetLastError()==ERROR_INTERNET_TIMEOUT?TTP_HTTPS_TIMEOUT:TTP_HTTPS_ERROR;}*got=n;return TTP_HTTPS_OK;}
};
