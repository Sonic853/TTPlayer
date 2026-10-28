#include "ttplayer/audio/http_stream.h"
#include "ttplayer/net/https_provider.h"
#include "ttplayer/net/content_disposition.h"
#include <shlwapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <tuple>

namespace ttplayer::audio {
namespace {
std::mutex personal_media_mutex;
PersonalMediaResolver personal_media_resolver;
bool PersonalSource(const std::wstring& url) { return url.starts_with(L"https://ttpbrowser.invalid/"); }
HRESULT ResolvePersonalSource(const std::wstring& source,const std::function<bool()>& canceled,std::wstring& output) {
    PersonalMediaResolver resolver;
    { std::lock_guard lock(personal_media_mutex);resolver=personal_media_resolver; }
    return resolver ? resolver(source,canceled,output) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}
struct Failure { HRESULT result; };
void Check(BOOL ok) { if (!ok) throw Failure{HRESULT_FROM_WIN32(GetLastError())}; }
void Require(bool ok) { if (!ok) throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)}; }
bool Number(std::string_view value, uint64_t& out) {
    if(value.empty()) return false;
    out=0;
    for(unsigned char c:value) {
        if(c<'0' || c>'9' || out>(UINT64_MAX-(c-'0'))/10) return false;
        out=out*10+c-'0';
    }
    return true;
}
std::wstring Text(std::string_view bytes) {
    if(bytes.empty()) return {};
    UINT code=CP_UTF8; DWORD flags=MB_ERR_INVALID_CHARS;
    int n=MultiByteToWideChar(code,flags,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    if(!n) {code=CP_ACP;flags=0;n=MultiByteToWideChar(code,flags,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);}
    std::wstring result(n,0);
    if(n) MultiByteToWideChar(code,flags,bytes.data(),static_cast<int>(bytes.size()),result.data(),n);
    return result;
}
struct File {
    HANDLE value{INVALID_HANDLE_VALUE};
    void Close(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);value=INVALID_HANDLE_VALUE;}
    ~File(){Close();}
};
// A reader is a lease, not the producer. Its cancellation callback is called
// only under clients_mutex_; Detach waits for an in-flight callback before the
// owner can destroy the object captured by that callback (AudioEngine, Job).
struct Consumer {
    std::function<bool()> canceled;
    HttpStreamUse use{HttpStreamUse::playback};
    bool active{true}; // clients_mutex_
    uint64_t position{}; // CacheTask::mutex_
    std::optional<uint64_t> demand; // CacheTask::mutex_
    std::atomic<HRESULT> error{S_OK};
    std::mutex operation;
};
class CacheTask final {
public:
    CacheTask(std::wstring url, PlaybackOptions options):url_(std::move(url)),options_(std::move(options)){
        options_.network_canceled={};
        // Account changes must not revive an old authenticated cache entry.
        if(PersonalSource(url_))options_.network_cache_enabled=false;
    }
    ~CacheTask() {
        stopping_=true;changed_.notify_all();
        if(worker_.joinable()) worker_.join();
        CloseRequest();FlushPersistent();
    }
    bool Attach(const std::shared_ptr<Consumer>& consumer) {
        std::lock_guard lock(clients_mutex_);
        if(!accepting_ || FAILED(error_) || (ready_ && !shareable_)) return false;
        clients_.push_back(consumer);return true;
    }
    void Detach(Consumer& consumer) noexcept {
        {std::lock_guard lock(clients_mutex_);consumer.active=false;
            bool remaining{};
            for(auto it=clients_.begin();it!=clients_.end();) {
                const auto client=it->lock();
                if(!client || client.get()==&consumer) it=clients_.erase(it);
                else {remaining|=client->active;++it;}
            }
            if(!remaining){accepting_=false;stopping_=true;}
        }
        changed_.notify_all();
    }
    void Launch() noexcept {
        try {worker_=std::thread([this]{
            try {
                Open();
                {std::lock_guard lock(clients_mutex_);
                    shareable_=!sequential_;
                    if(!shareable_) for(auto& weak:clients_) if(auto client=weak.lock();client && client->active){live_client_=client;break;}
                }
                ready_=true;changed_.notify_all();Prefetch();return;
            }catch(const Failure& e){error_=e.result;}
             catch(const std::bad_alloc&){error_=E_OUTOFMEMORY;}
             catch(...){error_=E_FAIL;}
            CloseRequest();FlushPersistent();
            {std::lock_guard lock(mutex_);worker_done_=true;ready_=true;}
            changed_.notify_all();
        });}catch(const std::bad_alloc&){error_=E_OUTOFMEMORY;ready_=true;}
          catch(...){error_=E_FAIL;ready_=true;}
        changed_.notify_all();
    }
    HRESULT WaitOpen(Consumer& client) noexcept {
        std::unique_lock lock(mutex_);
        return Protect(client,[&]() -> HRESULT {
            while(!ready_){Guard(client);changed_.wait_for(lock,std::chrono::milliseconds(25));}
            Guard(client);
            // Concurrent opens can join before Content-Length/ICY is known.
            // Only the elected owner keeps a rolling stream; others open private.
            return shareable_ || live_client_.lock().get()==&client ? S_OK:S_FALSE;
        });
    }
private:
    void Open() {
        Guard();
        if(const auto hash=url_.find(L'#');hash!=url_.npos) url_.resize(hash);
        if(!module_.Load()) throw Failure{HRESULT_FROM_WIN32(module_.Error())};
        const auto* base=module_.Api(TTP_HTTPS_STREAM_ABI_VERSION);
        if(!base || base->abi_version!=TTP_HTTPS_STREAM_ABI_VERSION || base->size<sizeof(ttp_https_api_v7))
            throw Failure{E_NOINTERFACE};
        api_=reinterpret_cast<const ttp_https_api_v7*>(base);
        Require(api_->stream_open && api_->stream_read && api_->stream_close && api_->stream_header);
        wchar_t folder[MAX_PATH],file[MAX_PATH];const auto length=GetTempPathW(MAX_PATH,folder);
        Require(length && length<MAX_PATH);Check(GetTempFileNameW(folder,L"ttp",0,file)!=0);
        cache_.value=CreateFileW(file,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(cache_.value==INVALID_HANDLE_VALUE) {const auto e=GetLastError();DeleteFileW(file);throw Failure{HRESULT_FROM_WIN32(e)};}
        live_capacity_=std::clamp<uint64_t>(options_.network_live_cache_bytes,64*1024,64*1024*1024);
        Start(0); LoadPersistent();

    }
public:
    HRESULT Read(Consumer& client,void* out,ULONG size,ULONG* read) {
        if(read) *read=0;if(!out && size) return STG_E_INVALIDPOINTER;
        std::lock_guard operation(client.operation);
        std::unique_lock lock(mutex_);
        return Protect(client,[&]() -> HRESULT {
            Guard(client);ULONG done{};
            while(done<size && (!total_ || client.position<*total_)) {
                Guard(client);auto it=coverage_.upper_bound(client.position);
                if(it!=coverage_.begin() && (--it)->second>client.position) {
                    auto n=static_cast<DWORD>(std::min<uint64_t>(size-done,it->second-client.position));
                    const auto disk=DiskPosition(client.position);
                    if(sequential_) n=static_cast<DWORD>(std::min<uint64_t>(n,live_capacity_-disk));
                    MoveCache(disk);DWORD got{};Check(ReadFile(cache_.value,static_cast<char*>(out)+done,n,&got,nullptr));
                    Require(got==n);done+=got;client.position+=got;client.demand.reset();if(read) *read=done;
                    changed_.notify_all();continue;
                }
                if(sequential_) {
                    if(client.position<response_position_) throw Failure{STG_E_INVALIDFUNCTION};
                }
                client.demand=client.position;
                const auto wanted=Demand();
                if(!sequential_ && ranged_ &&
                   wanted && (*wanted<response_position_ || *wanted>response_position_+buffer_.size())) restart_=true;
                changed_.notify_all();
                if(worker_done_) throw Failure{HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)};
                changed_.wait_for(lock,std::chrono::milliseconds(25));
            }
            client.demand.reset();return done==size?S_OK:S_FALSE;
        });
    }
    HRESULT Seek(Consumer& client,LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER* out) {
        std::lock_guard operation(client.operation);
        std::lock_guard lock(mutex_);
        // A rolling stream is not random-access. Probes may rewind retained
        // bytes, but CUR/END must not advertise seekability to codec libraries.
        if(sequential_ && origin!=STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        return Protect(client,[&]() -> HRESULT {
            Guard(client);uint64_t base{};
            if(origin==STREAM_SEEK_CUR) base=client.position;
            else if(origin==STREAM_SEEK_END) {if(!total_) return STG_E_INVALIDFUNCTION;base=*total_;}
            else if(origin!=STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
            if(base>INT64_MAX || (move.QuadPart<0 && uint64_t(-(move.QuadPart+1))+1>base) ||
                (move.QuadPart>0 && uint64_t(move.QuadPart)>INT64_MAX-base)) return STG_E_INVALIDFUNCTION;
            const uint64_t next=base+move.QuadPart;
            if(!sequential_ && next>file_limit_) return STG_E_INVALIDFUNCTION;
            if(sequential_ && next<response_position_) {
                auto it=coverage_.upper_bound(next);
                if(it==coverage_.begin() || (--it)->second<=next) return STG_E_INVALIDFUNCTION;
            }
            client.position=next;client.demand.reset();changed_.notify_all();
            if(out) out->QuadPart=next;return S_OK;
        });
    }
    HRESULT Stat(STATSTG* out,DWORD flags) {
        if(!out) return E_POINTER;
        std::lock_guard lock(mutex_);*out={};
        if(!total_ || sequential_) return E_PENDING;
        out->type=STGTY_STREAM;out->cbSize.QuadPart=*total_;out->grfMode=STGM_READ;
        if(!(flags&STATFLAG_NONAME)) {
            const auto& name=response_filename_.empty()?url_:response_filename_;
            out->pwcsName=static_cast<wchar_t*>(CoTaskMemAlloc((name.size()+1)*sizeof(wchar_t)));
            if(!out->pwcsName) return E_OUTOFMEMORY;
            std::copy(name.begin(),name.end(),out->pwcsName);out->pwcsName[name.size()]=0;
        }
        return S_OK;
    }
    int DownloadedPercent() const noexcept {return percent_;}
    HRESULT LastError(const Consumer& client) const noexcept {return FAILED(client.error)?client.error.load():error_.load();}
    bool Sequential() const noexcept {return sequential_;}
    bool CanPersist() const noexcept {
        std::lock_guard lock(mutex_);
        return options_.network_cache_enabled && !options_.network_cache_folder.empty() &&
            !sequential_ && total_ && !validator_.empty() && options_.network_cache_space_mb>0 &&
            *total_+((*total_+16383)/16384)*48+48<=static_cast<uint64_t>(std::clamp(options_.network_cache_space_mb,1,2000))*1024*1024;
    }
    AudioMetadata Metadata(const Consumer& client) const {
        std::lock_guard lock(mutex_);auto value=metadata_;
        auto it=icy_titles_.upper_bound(client.position);
        if(it!=icy_titles_.begin()) {--it;value.artist=it->second.first;value.title=it->second.second;}
        return value;
    }
    HRESULT BufferToEnd(Consumer& client,uint64_t limit) noexcept {
        std::lock_guard operation(client.operation);
        std::unique_lock lock(mutex_);
        if(icy_interval_ || (sequential_ && response_position_>live_capacity_) || limit>file_limit_) return E_NOTIMPL;
        if(total_ && *total_>limit) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        return Protect(client,[&]() -> HRESULT {
            if(sequential_){sequential_=false;spool_limit_=limit;}UpdateProgress();changed_.notify_all();
            while(!worker_done_) {Guard(client);changed_.wait_for(lock,std::chrono::milliseconds(25));}
            Guard(client);
            client.position=0;return S_OK;
        });
    }
private:
    template<class F> HRESULT Protect(Consumer& client,F action) noexcept {
        try{return action();}
        catch(const Failure& e){client.error=e.result;}
        catch(const std::bad_alloc&){client.error=E_OUTOFMEMORY;}
        catch(...){client.error=E_FAIL;}
        client.demand.reset();changed_.notify_all();return client.error;
    }
    bool Active(Consumer& client) const noexcept { // clients_mutex_ held
        if(client.active && client.canceled) {
            try {if(client.canceled()){client.active=false;client.error=HRESULT_FROM_WIN32(ERROR_CANCELLED);}}
            catch(...){client.active=false;client.error=HRESULT_FROM_WIN32(ERROR_CANCELLED);}
        }
        return client.active;
    }
    void Guard(Consumer& client) const {
        {std::lock_guard lock(clients_mutex_);
            if(!Active(client)) throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
        }
        if(stopping_) throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
        if(FAILED(client.error)) throw Failure{client.error};
        if(FAILED(error_)) throw Failure{error_};
    }
    void Guard() {
        {std::lock_guard lock(clients_mutex_);
            bool active{};
            for(auto& weak:clients_) if(auto client=weak.lock()) active|=Active(*client);
            if(!active){accepting_=false;stopping_=true;}
        }
        if(stopping_) throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
        if(FAILED(error_)) throw Failure{error_};
    }
    std::optional<uint64_t> Demand() const { // mutex_ held, cache -> clients lock order
        std::lock_guard lock(clients_mutex_);
        std::optional<uint64_t> background;
        bool playback{};
        for(auto& weak:clients_) if(auto client=weak.lock();client && client->active) {
            if(client->use==HttpStreamUse::playback) {
                playback=true;
                if(client->demand && AfterCovered(*client->demand)==*client->demand &&
                   (!total_ || *client->demand<*total_)) return client->demand;
            } else if(!background && client->demand && AfterCovered(*client->demand)==*client->demand &&
                      (!total_ || *client->demand<*total_)) background=client->demand;
        }
        // The download reader must not repeatedly seek a foreground range back
        // to its own missing prefix. The producer will fill every remaining gap.
        return playback?std::optional<uint64_t>{}:background;
    }
    uint64_t LivePosition() const { // only used for an unshared rolling stream
        const auto client=live_client_.lock();return client?client->position:0;
    }
    static int __cdecl Canceled(void* context) noexcept {
        try {auto* self=static_cast<CacheTask*>(context);self->Guard();return self->restart_?1:0;} catch(...) {return 1;}
    }
    void TransportResult(int value) {
        if(value==TTP_HTTPS_OK) return;
        throw Failure{value==TTP_HTTPS_CANCELED?HRESULT_FROM_WIN32(ERROR_CANCELLED):
            value==TTP_HTTPS_TIMEOUT?HRESULT_FROM_WIN32(ERROR_TIMEOUT):
            value==TTP_HTTPS_TRUNCATED?HRESULT_FROM_WIN32(ERROR_HANDLE_EOF):HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
    }
    void CloseRequest() noexcept {if(request_){api_->stream_close(request_);request_=nullptr;}}
    std::string Header(const char* name) const {return api_->stream_header(request_,name);}
    void Start(uint64_t offset) {
        CloseRequest();Guard();
        // Without an entity validator keep the original response sequential.
        // A second range could silently splice a different same-length resource.
        Require(!offset || !validator_.empty());std::wstring target=url_;
        if(PersonalSource(url_)) {
            const auto hr=ResolvePersonalSource(url_,[this]{return Canceled(this)!=0;},target);
            if(FAILED(hr))throw Failure{hr};
            Require(!PersonalSource(target) &&
                (target.starts_with(L"https://")||target.starts_with(L"http://")));
        }
        uint32_t status{};
        for(unsigned redirects=0;;++redirects) {
            ttp_https_stream_request req{};req.size=sizeof(req);req.request.size=sizeof(req.request);
            req.request.url=target.c_str();req.request.proxy_type=options_.network_proxy_type;
            req.request.proxy_server=options_.network_proxy_server.c_str();req.request.proxy_port=options_.network_proxy_port;
            req.request.proxy_username=options_.network_proxy_username.c_str();req.request.proxy_password=options_.network_proxy_password.c_str();
            req.request.canceled=Canceled;req.request.cancel_context=this;
            req.request.ca_pem=options_.network_ca_pem.empty()?nullptr:options_.network_ca_pem.data();
            req.request.ca_pem_size=options_.network_ca_pem.size();
            req.offset=offset;req.if_range=offset?validator_.c_str():nullptr;req.icy_metadata=1;
            req.timeout_ms=static_cast<uint32_t>(std::clamp(options_.network_timeout_ms,100,60000));
            req.auto_config_url=options_.network_pac_url.empty()?nullptr:options_.network_pac_url.c_str();
            req.auto_detect=options_.network_auto_detect;
            TransportResult(api_->stream_open(&req,&request_,&status,nullptr,0));
            if(status!=301 && status!=302 && status!=303 && status!=307 && status!=308) break;
            Require(redirects<8);const auto location=Text(Header("location"));Require(!location.empty());
            wchar_t combined[8192];DWORD size=8192;
            Require(SUCCEEDED(UrlCombineW(target.c_str(),location.c_str(),combined,&size,0)));
            const std::wstring next=combined;
            Require(_wcsnicmp(next.c_str(),L"https://",8)==0 || _wcsnicmp(next.c_str(),L"http://",7)==0);
            Require(_wcsnicmp(target.c_str(),L"https://",8)!=0 || _wcsnicmp(next.c_str(),L"https://",8)==0);
            target=next;CloseRequest();Guard();
        }
        Require(status==200 || status==206);
        // Only the producer touches the transport. Never hold the cache lock
        // across DNS, TLS, headers or a body read: cached reads and UI metadata
        // must remain available while the server stalls.
        std::lock_guard lock(mutex_);
        const auto tag=Header("etag"), modified=Header("last-modified");
        const auto validator=!tag.empty() && !tag.starts_with("W/")?tag:modified;
        Require(validator.size()<=512 && validator.find_first_of("\r\n")==validator.npos);
        if(!validator_.empty()) Require(validator==validator_);
        else if(!opened_) validator_=validator;
        uint64_t n{};std::optional<uint64_t> length,total;
        const auto text=Header("content-length");
        if(!text.empty()){Require(Number(text,n));length=n;}
        response_position_=wire_position_=0;response_end_.reset();
        if(status==206) {
            const auto range=Header("content-range");const auto dash=range.find('-'),slash=range.find('/');
            uint64_t first{},last{},full{};
            Require(range.starts_with("bytes ") && dash>6 && slash>dash && slash<range.size());
            Require(Number(std::string_view(range).substr(6,dash-6),first) &&
                Number(std::string_view(range).substr(dash+1,slash-dash-1),last) && Number(std::string_view(range).substr(slash+1),full));
            Require(first==offset && last>=first && last<full && (!length || *length==last-first+1));
            response_position_=wire_position_=first;response_end_=last+1;total=full;ranged_=true;
        } else {response_end_=length;total=length;ranged_=false;}
        const auto icy=Header("icy-metaint");
        if(!icy.empty()) {Require(!offset && Number(icy,n) && n && n<=64*1024*1024);icy_interval_=icy_audio_left_=n;}
        if(!opened_) {
            response_filename_=net::HttpResponseFilename(Header("content-disposition"));
            sequential_=!total || icy_interval_;
            metadata_.title=Text(Header("icy-name"));
            const auto genre=Text(Header("icy-genre")), home=Text(Header("icy-url"));
            if(!genre.empty()) metadata_.entries.emplace_back(L"Genre",genre);
            if(!home.empty()) metadata_.entries.emplace_back(L"URL",home);
        }
        if(!sequential_ && total) {Require(*total<=file_limit_ && (!total_ || *total_==*total));total_=total;}
        else if(!sequential_) Require(!total_);
        if(validator_.empty()) ranged_=false;
        opened_=true;UpdateProgress();
    }
    uint64_t DiskPosition(uint64_t position) const {return sequential_?position%live_capacity_:position;}
    void MoveCache(uint64_t position) {LARGE_INTEGER n{};n.QuadPart=position;Check(SetFilePointerEx(cache_.value,n,nullptr,FILE_BEGIN));}
    void Cache(const unsigned char* bytes,size_t count) {
        Require(response_position_<=UINT64_MAX-count);
        size_t written=0;
        while(written<count) {
            const auto disk=DiskPosition(response_position_+written);
            const auto n=static_cast<DWORD>(sequential_?std::min<uint64_t>(count-written,live_capacity_-disk):count-written);
            MoveCache(disk);DWORD got{};Check(WriteFile(cache_.value,bytes+written,n,&got,nullptr));Require(got==n);written+=got;
        }
        auto first=response_position_,last=first+count;
        auto it=coverage_.lower_bound(first);
        if(it!=coverage_.begin() && std::prev(it)->second>=first) --it;
        while(it!=coverage_.end() && it->first<=last){first=std::min(first,it->first);last=std::max(last,it->second);it=coverage_.erase(it);}
        coverage_[first]=last;response_position_+=count;
        if(sequential_ && response_position_>live_capacity_) DropBefore(response_position_-live_capacity_);
    }
    void DropBefore(uint64_t oldest) {
        while(!coverage_.empty() && coverage_.begin()->second<=oldest) coverage_.erase(coverage_.begin());
        if(!coverage_.empty() && coverage_.begin()->first<oldest) {
            const auto end=coverage_.begin()->second;coverage_.erase(coverage_.begin());coverage_[oldest]=end;
        }
        // Retain the last title preceding the oldest readable audio byte.
        auto title=icy_titles_.upper_bound(oldest);
        if(title!=icy_titles_.begin()) icy_titles_.erase(icy_titles_.begin(),std::prev(title));
    }
    void IcyTitle() {
        const std::string_view text(reinterpret_cast<const char*>(icy_text_.data()),icy_text_.size());
        auto key=text.find("StreamTitle='");
        if(key!=text.npos) {
            key+=13;const auto end=text.find("';",key);
            if(end!=text.npos) {
                auto title=Text(text.substr(key,end-key));
                const auto split=title.find(L" - ");std::wstring artist;
                if(split!=title.npos){artist=title.substr(0,split);title.erase(0,split+3);}
                icy_titles_[response_position_]={std::move(artist),std::move(title)};
            }
        }
        icy_text_.clear();
    }
    void Pump() {
        Guard();Require(request_!=nullptr);uint32_t got{};
        const auto result=api_->stream_read(request_,buffer_.data(),static_cast<uint32_t>(buffer_.size()),&got,nullptr,0);
        // Resume once only when a validated range response identifies the same
        // finite resource. Live/no-range streams cannot be spliced safely.
        if((result==TTP_HTTPS_TRUNCATED || result==TTP_HTTPS_TIMEOUT) && ranged_ &&
           !sequential_ && !validator_.empty() && retries_++==0) {
            Start(response_position_);return;
        }
        TransportResult(result);
        std::unique_lock lock(mutex_);
        if(!got) {
            if((response_end_ && wire_position_!=*response_end_) || icy_meta_left_ || (icy_interval_ && !icy_audio_left_ && icy_need_length_))
                throw Failure{HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)};
            if(!total_) total_=response_position_;
            UpdateProgress();lock.unlock();CloseRequest();changed_.notify_all();return;
        }
        Require(!response_end_ || (wire_position_<=*response_end_ && got<=*response_end_-wire_position_));
        wire_position_+=got;
        if(spool_limit_ && (response_position_>*spool_limit_ || got>*spool_limit_-response_position_))
            throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
        const auto begin=response_position_;
        if(!icy_interval_) Cache(buffer_.data(),got);
        else for(size_t at=0;at<got;) {
            if(icy_audio_left_) {
                const auto n=std::min<uint64_t>(got-at,icy_audio_left_);Cache(buffer_.data()+at,static_cast<size_t>(n));
                at+=static_cast<size_t>(n);icy_audio_left_-=n;if(!icy_audio_left_) icy_need_length_=true;
            } else if(icy_need_length_) {
                icy_meta_left_=buffer_[at++]*16u;icy_need_length_=false;
                if(!icy_meta_left_) icy_audio_left_=icy_interval_;
            } else {
                const auto n=std::min<size_t>(got-at,icy_meta_left_);
                icy_text_.insert(icy_text_.end(),buffer_.begin()+at,buffer_.begin()+at+n);at+=n;icy_meta_left_-=n;
                if(!icy_meta_left_){IcyTitle();icy_audio_left_=icy_interval_;}
            }
        }
        UpdateProgress();lock.unlock();changed_.notify_all();
        if(!icy_interval_) SavePersistent(begin,buffer_.data(),got);
    }
    uint64_t AfterCovered(uint64_t position) const {
        auto it=coverage_.upper_bound(position);
        return it!=coverage_.begin() && (--it)->second>position?it->second:position;
    }
    void Prefetch() noexcept {
        for(;;) {
            try {
                std::unique_lock lock(mutex_);Guard();
                if(sequential_ && !spool_limit_ && response_position_>=LivePosition() &&
                   response_position_-LivePosition()>live_capacity_-buffer_.size()) {
                    changed_.wait_for(lock,std::chrono::milliseconds(25));continue;
                }
                if(icy_titles_.size()>=2048) {
                    // Pathological stations may send a title every audio byte.
                    // Bound metadata as well as audio, retaining the title at
                    // the consumer position. Expire the corresponding older
                    // audio rather than returning it with an incorrect title.
                    auto current=icy_titles_.upper_bound(LivePosition());
                    if(current!=icy_titles_.begin()) DropBefore(std::prev(current)->first);
                    if(icy_titles_.size()>=2048) {changed_.wait_for(lock,std::chrono::milliseconds(25));continue;}
                }
                std::optional<uint64_t> start;const auto demand=Demand();
                if(!sequential_ && total_) {
                    const auto missing=AfterCovered(0);
                    if(missing==*total_) break;
                    if(demand && AfterCovered(*demand)==*demand && *demand<*total_ &&
                       (!request_ || (ranged_ && (*demand<response_position_ ||
                                                *demand>response_position_+buffer_.size())))) start=*demand;
                    else if(!request_ || restart_) start=missing;
                    else if(ranged_) {
                        const auto next=AfterCovered(response_position_);
                        if(next!=response_position_) start=next==*total_?missing:next;
                    }
                } else if(!request_) break;
                if(start) {
                    restart_=false;response_position_=*start;
                    lock.unlock();Start(*start);
                } else {restart_=false;lock.unlock();Pump();}
            } catch(const Failure& e) {
                CloseRequest();
                // A seek interrupts just this request, not the stream. A
                // user cancellation or a real protocol failure stays terminal.
                if(e.result==HRESULT_FROM_WIN32(ERROR_CANCELLED) && restart_ && !stopping_) {
                    try {Guard();continue;} catch(...) {}
                }
                error_=e.result;break;
            } catch(const std::bad_alloc&) {error_=E_OUTOFMEMORY;break;}
              catch(...) {error_=E_FAIL;break;}
        }
        CloseRequest();FlushPersistent();
        {std::lock_guard lock(mutex_);worker_done_=true;}
        changed_.notify_all();
    }
    void UpdateProgress() {
        uint64_t received{};for(const auto& [begin,end]:coverage_) received+=end-begin;
        percent_=total_ && !sequential_?(*total_?static_cast<int>(received*100/ *total_):100):-1;
    }
    void LoadPersistent();
    void SavePersistent(uint64_t begin,const BYTE* bytes,DWORD count) noexcept;
    void FlushPersistent() noexcept;
    std::filesystem::path PersistentPath() const;
    std::atomic<HRESULT> error_{S_OK};std::atomic<int> percent_{-1};
    // mutex_: shared disk cursor, coverage, reader positions and metadata. Each
    // Consumer has its own Read/Seek lock. Network I/O never holds mutex_.
    // clients_mutex_ protects cancellation callback lifetime and lease changes.
    mutable std::mutex mutex_,clients_mutex_;std::condition_variable changed_;
    std::vector<std::weak_ptr<Consumer>> clients_;std::weak_ptr<Consumer> live_client_;
    std::atomic<bool> ready_{},shareable_{};bool accepting_{true};
    std::thread worker_;std::atomic<bool> stopping_{},restart_{},sequential_{};
    std::wstring url_,response_filename_;std::string validator_;PlaybackOptions options_;
    net::HttpsProvider module_;const ttp_https_api_v7* api_{};ttp_https_stream* request_{};
    File cache_;std::array<unsigned char,16384> buffer_{};
    bool opened_{},ranged_{},worker_done_{},persisted_{};
    unsigned retries_{};
    static constexpr uint64_t file_limit_=2ULL*1024*1024*1024;
    uint64_t response_position_{},wire_position_{},live_capacity_{};
    std::optional<uint64_t> total_,response_end_;std::map<uint64_t,uint64_t> coverage_;
    std::optional<uint64_t> spool_limit_;
    File persistent_;std::filesystem::path persistent_path_;
    std::map<uint64_t,uint64_t> stored_;
    uint64_t persistent_size_{};DWORD persistent_tick_{};size_t unflushed_{};
    uint64_t icy_interval_{},icy_audio_left_{};size_t icy_meta_left_{};bool icy_need_length_{};
    std::vector<unsigned char> icy_text_;AudioMetadata metadata_;
    std::map<uint64_t,std::pair<std::wstring,std::wstring>> icy_titles_;
};
#include "http_cache.inl"

// Include all transport/security and cache-policy inputs. Consumer role, cursor,
// destination and cancellation are deliberately NOT a part of the resource key.
// One active task owns a single response entity; every later Range still checks
// its validator and length. A new lifetime starts with a fresh HTTP response.
auto TaskKey(const std::wstring& url,const PlaybackOptions& p) {
    return std::make_tuple(url,p.network_proxy_type,p.network_proxy_server,p.network_proxy_port,
        p.network_proxy_username,p.network_proxy_password,p.network_pac_url,p.network_auto_detect,
        p.network_ca_pem,p.network_timeout_ms,p.network_cache_enabled,p.network_cache_folder.native(),
        p.network_cache_space_mb,p.network_live_cache_bytes);
}
struct TaskRegistry {
    std::mutex mutex;
    std::map<decltype(TaskKey(std::wstring{},PlaybackOptions{})),std::weak_ptr<CacheTask>> tasks;
};
std::pair<std::shared_ptr<CacheTask>,bool> AcquireTask(std::wstring url,const PlaybackOptions& options,
    const std::shared_ptr<Consumer>& consumer,bool shared) {
    if(const auto hash=url.find(L'#');hash!=url.npos) url.resize(hash);
    if(!shared) {auto task=std::make_shared<CacheTask>(std::move(url),options);task->Attach(consumer);return {task,true};}
    static TaskRegistry registry;
    const auto key=TaskKey(url,options);std::shared_ptr<CacheTask> task,retired;
    {std::lock_guard lock(registry.mutex);
        for(auto it=registry.tasks.begin();it!=registry.tasks.end();)
            if(it->second.expired()) it=registry.tasks.erase(it);else ++it;
        if(const auto it=registry.tasks.find(key);it!=registry.tasks.end()) {
            task=it->second.lock();if(task && task->Attach(consumer)) return {task,false};
            retired=std::move(task); // Any last-reference join happens outside registry.mutex.
        }
        task=std::make_shared<CacheTask>(std::move(url),options);task->Attach(consumer);registry.tasks[key]=task;
    }
    return {task,true};
}
class Stream final : public HttpAudioStream {
public:
    explicit Stream(const PlaybackOptions& options,HttpStreamUse use):client_(std::make_shared<Consumer>()) {
        client_->canceled=options.network_canceled;client_->use=use;
    }
    ~Stream() override {if(task_) task_->Detach(*client_);}
    HRESULT Open(const std::wstring& url,const PlaybackOptions& options,bool shared) {
        auto [task,created]=AcquireTask(url,options,client_,shared);task_=std::move(task);
        if(created) task_->Launch();return task_->WaitOpen(*client_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER;*out=nullptr;
        if(iid!=IID_IUnknown && iid!=IID_ISequentialStream && iid!=IID_IStream) return E_NOINTERFACE;
        *out=static_cast<IStream*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override {const auto n=--refs_;if(!n) delete this;return n;}
    HRESULT STDMETHODCALLTYPE Read(void* out,ULONG size,ULONG* read) override {return task_->Read(*client_,out,size,read);}
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER* out) override {return task_->Seek(*client_,move,origin,out);}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* out,DWORD flags) override {return task_->Stat(out,flags);}
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG*) override {return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*,ULARGE_INTEGER,ULARGE_INTEGER*,ULARGE_INTEGER*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE Revert() override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override {return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override {return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override {return E_NOTIMPL;}
    int DownloadedPercent() const noexcept override {return task_->DownloadedPercent();}
    HRESULT LastError() const noexcept override {return task_->LastError(*client_);}
    bool Sequential() const noexcept override {return task_->Sequential();}
    bool CanPersist() const noexcept override {return task_->CanPersist();}
    AudioMetadata Metadata() const override {return task_->Metadata(*client_);}
    HRESULT BufferToEnd(uint64_t limit) noexcept override {return task_->BufferToEnd(*client_,limit);}
private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<Consumer> client_;
    std::shared_ptr<CacheTask> task_;
};
}
void SetPersonalMediaResolver(PersonalMediaResolver resolver) {
    std::lock_guard lock(personal_media_mutex);personal_media_resolver=std::move(resolver);
}
HRESULT OpenHttpAudioStream(const std::wstring& url,const PlaybackOptions& options,HttpAudioStream** stream,HttpStreamUse use) noexcept {
    if(!stream) return E_POINTER;*stream=nullptr;
    try{
        for(bool shared:{true,false}) {
            auto owner=std::make_unique<Stream>(options,use);const auto result=owner->Open(url,options,shared);
            if(result==S_FALSE) continue;
            if(FAILED(result)) return result;
            *stream=owner.release();return S_OK;
        }
        return E_FAIL;
    }catch(const Failure& e){return e.result;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
}
