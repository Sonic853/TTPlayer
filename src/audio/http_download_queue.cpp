#include "ttplayer/audio/http_download_queue.h"
#include "ttplayer/audio/http_stream.h"
#include "ttplayer/core/text.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <system_error>
#include <msxml6.h>
#include <comdef.h>
#include <wrl/client.h>
#include <wincrypt.h>

namespace ttplayer::audio {
namespace {
using Microsoft::WRL::ComPtr;
struct ComInit { HRESULT hr{CoInitializeEx(nullptr,COINIT_MULTITHREADED)};
    ~ComInit(){if(SUCCEEDED(hr)) CoUninitialize();} };
struct Handle { HANDLE h{INVALID_HANDLE_VALUE}; ~Handle(){if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);} };
struct Error {HRESULT hr;};
void Check(bool ok){if(!ok){const auto error=GetLastError();throw Error{error?HRESULT_FROM_WIN32(error):E_FAIL};}}
std::wstring Attribute(IXMLDOMNode* node,const wchar_t* name) {
    ComPtr<IXMLDOMNamedNodeMap> attrs;ComPtr<IXMLDOMNode> attr;
    if(FAILED(node->get_attributes(&attrs)) || !attrs ||
       FAILED(attrs->getNamedItem(_bstr_t(name),&attr)) || !attr) return {};
    VARIANT v{};if(FAILED(attr->get_nodeValue(&v))) return {};
    _variant_t value(v,false);return value.vt==VT_BSTR && value.bstrVal ? value.bstrVal:L"";
}
ComPtr<IXMLDOMNodeList> Items(const std::filesystem::path& file,
    const wchar_t* xpath=L"/cachelist[@version='2']/items/item",
    const wchar_t* root=L"/cachelist[@version='2']") {
    std::error_code ec;const auto size=std::filesystem::file_size(file,ec);
    if(ec || size>4*1024*1024) return {};
    ComPtr<IXMLDOMDocument2> doc;
    if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&doc)))) {
        CLSID cls{};if(FAILED(CLSIDFromProgID(L"Msxml2.DOMDocument.3.0",&cls)) ||
            FAILED(CoCreateInstance(cls,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&doc)))) return {};
    }
    doc->put_async(VARIANT_FALSE);doc->put_resolveExternals(VARIANT_FALSE);
    doc->put_validateOnParse(VARIANT_FALSE);
    doc->setProperty(_bstr_t(L"ProhibitDTD"),_variant_t(true));
    VARIANT_BOOL loaded{};
    if(FAILED(doc->load(_variant_t(file.c_str()),&loaded)) || loaded!=VARIANT_TRUE) return {};
    ComPtr<IXMLDOMDocumentType> type;doc->get_doctype(&type);if(type) return {};
    ComPtr<IXMLDOMNode> catalog;
    if(FAILED(doc->selectSingleNode(_bstr_t(root),&catalog)) || !catalog) return {};
    ComPtr<IXMLDOMNodeList> result;
    doc->selectNodes(_bstr_t(xpath),&result);return result;
}
LegacyCacheEntry Entry(IXMLDOMNode* node) {
    return {Attribute(node,L"title"),Attribute(node,L"temp"),Attribute(node,L"save"),
        Attribute(node,L"sourceurl"),Attribute(node,L"excludeurl"),Attribute(node,L"tag"),Attribute(node,L"playtag")};
}
std::wstring Escape(std::wstring_view value) {
    std::wstring out;
    for(auto c:value) {
        switch(c){case L'&':out+=L"&amp;";break;case L'<':out+=L"&lt;";break;
        case L'>':out+=L"&gt;";break;case L'\"':out+=L"&quot;";break;
        case L'\r':out+=L"&#13;";break;case L'\n':out+=L"&#10;";break;
        case L'\t':out+=L"&#9;";break;default:if(c>=32) out+=c;break;}
    }return out;
}
bool Under(const std::filesystem::path& file,const std::filesystem::path& root) {
    std::error_code ec;auto f=std::filesystem::weakly_canonical(file,ec);if(ec) return false;
    auto r=std::filesystem::weakly_canonical(root,ec);if(ec || r.empty()) return false;
    auto a=f.begin();for(auto b=r.begin();b!=r.end();++b,++a)
        if(a==f.end() || _wcsicmp(a->c_str(),b->c_str())) return false;
    return a!=f.end();
}

std::wstring SafeName(std::wstring s) {
    for(auto& c:s) if(c<32 || wcschr(L"<>:\"/\\|?*",c)) c=L'_';
    while(!s.empty() && (s.back()==L'.' || s.back()==L' ')) s.pop_back();
    if(s.size()>100) s.resize(100);
    if(s.empty()) s=L"track";
    const auto stem=s.substr(0,s.find(L'.')); // DOS device basenames remain reserved even with an extension.
    if(!_wcsicmp(stem.c_str(),L"CON") || !_wcsicmp(stem.c_str(),L"PRN") ||
       !_wcsicmp(stem.c_str(),L"AUX") || !_wcsicmp(stem.c_str(),L"NUL") ||
       (stem.size()==4 && (!_wcsnicmp(stem.c_str(),L"COM",3) || !_wcsnicmp(stem.c_str(),L"LPT",3)) && stem[3]>=L'1' && stem[3]<=L'9')) s=L"_"+s;
    return s;
}
}

std::wstring HttpDownloadKey(std::wstring url) {
    if(_wcsnicmp(url.c_str(),L"http://",7) && _wcsnicmp(url.c_str(),L"https://",8)) return {};
    if(url.size()>32768 || url.find_first_of(L"\r\n")!=std::wstring::npos) return {};
    if(const auto hash=url.find(L'#');hash!=std::wstring::npos) url.resize(hash);
    return url;
}
std::wstring LegacyCacheTitle(std::wstring_view url,std::wstring_view title,std::wstring_view artist) {
    // 004C7F81 -> 004C5663: Artist + " - " + Title, with the nine filename
    // separators replaced by spaces. Without BOTH tags use uppercase MD5 of
    // the UTF-8 URL (004C544F -> ttpcomm ordinal 400), not the display title.
    if(!title.empty() && !artist.empty()) {
        std::wstring key=std::wstring(artist)+L" - "+std::wstring(title);
        for(auto& c:key) if(wcschr(L"\\/:*?\"<>|",c)) c=L' ';return key;
    }
    const auto utf8=core::WideToUtf8(url);char hex[64]{};
    using Digest=char*(__cdecl*)(const char*,unsigned,char*,int);
    const auto comm=GetModuleHandleW(L"ttpcomm.dll");
    const auto digest=comm?reinterpret_cast<Digest>(GetProcAddress(comm,MAKEINTRESOURCEA(400))):nullptr;
    if(digest && digest(utf8.data(),static_cast<unsigned>(utf8.size()),hex,sizeof(hex))) return core::Utf8ToWide(hex);
    HCRYPTPROV provider{};HCRYPTHASH hash{};std::wstring result;
    if(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)) {
        if(CryptCreateHash(provider,CALG_MD5,0,0,&hash)) {
            BYTE bytes[16];DWORD size=sizeof(bytes);
            if(CryptHashData(hash,reinterpret_cast<const BYTE*>(utf8.data()),static_cast<DWORD>(utf8.size()),0) && CryptGetHashParam(hash,HP_HASHVAL,bytes,&size,0))
                for(auto b:bytes){result+=L"0123456789ABCDEF"[b>>4];result+=L"0123456789ABCDEF"[b&15];}
            CryptDestroyHash(hash);
        }CryptReleaseContext(provider,0);
    }return result;
}
std::vector<LegacyCacheEntry> ReadLegacyCacheList(const std::filesystem::path& file) {
    ComInit com;std::vector<LegacyCacheEntry> result;
    try {auto items=Items(file);if(!items) return result;long count{};items->get_length(&count);
        for(long i=0;i<std::min(count,10000L);++i){ComPtr<IXMLDOMNode> n;items->get_item(i,&n);if(n) result.push_back(Entry(n.Get()));}
    }catch(...){}return result;
}
std::filesystem::path ResolveLegacySavedFile(const std::vector<LegacyCacheEntry>& entries,
    std::wstring_view url,const std::vector<std::filesystem::path>& roots) {
    const auto key=HttpDownloadKey(std::wstring(url));if(key.empty()) return {};
    for(const auto& entry:entries) {
        if(HttpDownloadKey(entry.sourceurl)!=key || entry.save.empty() ||
            (!entry.excludeurl.empty() && entry.excludeurl.find(key)!=std::wstring::npos)) continue;
        const std::filesystem::path path(entry.save);
        // A successful save is a location, not a format assertion. The caller
        // still opens it through the real reader; suffixless/.bin downloads
        // must not be rejected before content probing. Never use entry.temp.
        if(!path.is_absolute()) continue;
        if(std::none_of(roots.begin(),roots.end(),[&](const auto& root){return !root.empty() && Under(path,root);})) continue;
        std::error_code ec;const auto bytes=std::filesystem::file_size(path,ec);
        if(!ec && bytes && bytes<=0x80000000ULL && std::filesystem::is_regular_file(path,ec)) return path;
    }return {};
}
std::filesystem::path HttpDownloadDestination(const std::filesystem::path& folder,
    std::wstring_view url,std::wstring title,std::wstring artist,bool artist_folder) {
    auto name=std::wstring(url);name.resize(name.find_first_of(L"?#")==std::wstring::npos?name.size():name.find_first_of(L"?#"));
    name=name.substr(name.find_last_of(L'/')+1);std::filesystem::path leaf(name);
    auto ext=leaf.extension().wstring();if(!AudioEngine::IsRecoveredFormatPath(leaf)) ext=L".bin";
    if(title.empty()) title=leaf.stem().wstring();
    auto root=folder;if(artist_folder && !artist.empty()) root/=SafeName(artist);
    return root/(SafeName(title)+ext);
}

struct HttpDownloadQueue::Impl {
    struct Job {
        HttpDownload info;
        std::shared_ptr<std::atomic_bool> canceled{std::make_shared<std::atomic_bool>(false)};
        bool requeue{},user_canceled{},removed{};
        ComPtr<HttpAudioStream> prefetched;
    };
    std::filesystem::path file;Handle lease;
    mutable std::mutex mutex;std::condition_variable changed,save_changed;
    std::vector<std::shared_ptr<Job>> jobs;std::vector<std::thread> workers;
    std::thread saver;
    std::filesystem::path recovery_backup;
    PlaybackOptions options;int maximum{3};bool stop{},writable{},save_stop{};
    uint64_t revision{},saved_revision{};
    std::wstring prediction;uint64_t serial{};std::atomic<HRESULT> storage{S_OK};
    explicit Impl(std::filesystem::path path):file(std::move(path)) {
        try {
            std::filesystem::create_directories(file.parent_path());
            lease.h=CreateFileW((file.wstring()+L".lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,nullptr);
            Check(lease.h!=INVALID_HANDLE_VALUE);
            ComInit com;auto items=Items(file);long count{};if(items) items->get_length(&count);
            if(!items && std::filesystem::exists(file)) throw Error{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
            for(long i=0;i<std::min(count,10000L);++i) {
                ComPtr<IXMLDOMNode> n;items->get_item(i,&n);if(!n) continue;
                auto j=std::make_shared<Job>();j->info.id=++serial;j->info.entry=Entry(n.Get());
                j->info.display_title=Attribute(n.Get(),L"caption");
                if(j->info.display_title.empty()) j->info.display_title=j->info.entry.title;
                if(HttpDownloadKey(j->info.entry.sourceurl).empty()) continue;
                j->info.destination=Attribute(n.Get(),L"destination");
                if(j->info.destination.empty()) j->info.destination=j->info.entry.save;
                if(j->info.destination.empty() || !j->info.destination.is_absolute()) continue;
                j->info.replace=Attribute(n.Get(),L"replace")==L"1";
                const auto state=Attribute(n.Get(),L"state");
                if(state==L"complete" || (state.empty() && !j->info.entry.save.empty())) j->info.state=DownloadState::complete;
                else if(state==L"canceled") j->info.state=DownloadState::canceled;
                else if(state==L"failed") j->info.state=DownloadState::failed;
                else if(state==L"paused") j->info.state=DownloadState::paused;
                if(j->info.state==DownloadState::failed) {
                    j->info.result=E_FAIL;
                    const auto result=Attribute(n.Get(),L"result");wchar_t* end{};
                    const auto code=std::wcstoull(result.c_str(),&end,10);
                    if(!result.empty() && end && !*end && code<=MAXDWORD && FAILED(static_cast<HRESULT>(code)))
                        j->info.result=static_cast<HRESULT>(code);
                } else if(j->info.state==DownloadState::complete) {
                    std::error_code ec;const auto bytes=std::filesystem::file_size(j->info.destination,ec);
                    if(!ec && bytes && bytes<=0x80000000ULL && std::filesystem::is_regular_file(j->info.destination,ec))
                        j->info.received=j->info.total=bytes;
                    else {j->info.entry.save.clear();j->info.state=DownloadState::failed;
                        j->info.result=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);}
                }
                // Interrupted running work rejoins FIFO and revalidates its journal.
                jobs.push_back(j);
            }
            writable=true; // Workers wait for Configure; credentials are never serialized.
            saver=std::thread([this]{Saver();});
        }catch(const Error& e){writable=false;storage=e.hr;}catch(...){writable=false;storage=E_FAIL;}
    }
    ~Impl(){
        {std::lock_guard lock(mutex);stop=true;for(auto& j:jobs) j->canceled->store(true);changed.notify_all();}
        for(auto& worker:workers) worker.join();
        {std::lock_guard lock(mutex);save_stop=true;save_changed.notify_all();}
        if(saver.joinable()) saver.join();
        Save();
    }
    void StartWorkers() {
        if(!workers.empty() || jobs.empty() || !writable) return;
        for(size_t i=0;i<5;++i) workers.emplace_back([this]{Worker();});
    }
    // Caller holds mutex. Only acknowledged revisions are considered saved.
    void Changed() noexcept {
        ++revision;
        changed.notify_all();
        save_changed.notify_one();
    }
    bool Save() noexcept {
        if(!writable) return false;
        try {
            std::vector<HttpDownload> snapshot;
            uint64_t snapshot_revision{};
            {std::lock_guard lock(mutex);
                snapshot_revision=revision;
                snapshot.reserve(jobs.size());
                for(const auto& job:jobs)
                    if(!job->info.prefetch && !job->removed) snapshot.push_back(job->info);
            }
            std::wstring xml=L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cachelist version=\"2\"><items>\n";
            for(const auto& d:snapshot) {
                    xml+=L"<item";
                    auto attr=[&](const wchar_t* name,std::wstring_view value){xml+=L" ";xml+=name;xml+=L"=\"";xml+=Escape(value);xml+=L"\"";};
                    attr(L"title",d.entry.title);attr(L"temp",d.entry.temp);attr(L"save",d.entry.save);
                    attr(L"caption",d.display_title);
                    attr(L"sourceurl",d.entry.sourceurl);attr(L"excludeurl",d.entry.excludeurl);attr(L"tag",d.entry.tag);attr(L"playtag",d.entry.playtag);
                    attr(L"destination",d.destination.wstring());attr(L"replace",d.replace?L"1":L"0");
                    attr(L"state",d.state==DownloadState::complete?L"complete":d.state==DownloadState::failed?L"failed":d.state==DownloadState::canceled?L"canceled":d.state==DownloadState::paused?L"paused":L"waiting");
                    attr(L"result",std::to_wstring(static_cast<uint32_t>(d.result)));
                    xml+=L"/>\n";
            }
            xml+=L"</items></cachelist>\n";const auto bytes=core::WideToUtf8(xml);
            if(bytes.size()>4*1024*1024) throw Error{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
            core::WriteFileAtomically(file,bytes);
            {std::lock_guard lock(mutex);saved_revision=snapshot_revision;}
            storage=S_OK;
            return true;
        }catch(const Error& e){storage=e.hr;}
        catch(const std::filesystem::filesystem_error& e){
            if(e.code().value()==ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 && !e.path2().empty()) {
                try {std::lock_guard lock(mutex);recovery_backup=e.path2();} catch(...) {}
            }
            storage=e.code().value()?HRESULT_FROM_WIN32(e.code().value()):E_FAIL;
        }
        catch(const std::system_error& e){storage=e.code().value()?HRESULT_FROM_WIN32(e.code().value()):E_FAIL;}
        catch(const std::bad_alloc&){storage=E_OUTOFMEMORY;}
        catch(...){storage=E_FAIL;}
        return false;
    }
    // 004D10EC has separate cache maintenance. Do not let a long HTTP Run
    // occupy the only writer; acknowledge a snapshot only after commit.
    void Saver() noexcept {
        std::unique_lock lock(mutex);
        for(;;) {
            save_changed.wait(lock,[this]{return save_stop || revision!=saved_revision;});
            if(save_stop) return;
            // Coalesce short bursts without extending the deadline on each edit.
            save_changed.wait_for(lock,std::chrono::milliseconds(50),[this]{return save_stop;});
            if(save_stop) return;
            lock.unlock();
            const bool saved=Save();
            lock.lock();
            // Failed snapshots stay dirty. Retry even without another edit,
            // but do not busy-loop while storage is unavailable.
            if(!saved) save_changed.wait_for(lock,std::chrono::seconds(1),
                                             [this]{return save_stop;});
        }
    }
    void Worker() noexcept {
        ComInit com;
        for(;;) {
            std::shared_ptr<Job> job;PlaybackOptions profile;
            {std::unique_lock lock(mutex);
                changed.wait_for(lock,std::chrono::milliseconds(50));if(stop) break;
                const auto active=std::count_if(jobs.begin(),jobs.end(),[](auto& j){return j->info.state==DownloadState::running;});
                if(active<maximum) {
                    // Explicit jobs first, stable FIFO within each class.
                    for(bool speculative:{false,true}) {
                        for(auto& j:jobs) if(!j->removed && j->info.prefetch==speculative && j->info.state==DownloadState::waiting) {job=j;break;}
                        if(job) break;
                    }
                    if(job){job->info.state=DownloadState::running;job->canceled->store(false);job->requeue=false;profile=options;Changed();}
                }
            }
            if(!job) continue;
            Run(job,profile);
        }
    }
    void Run(const std::shared_ptr<Job>& job,PlaybackOptions profile) noexcept {
        HRESULT result=S_OK;bool published{};std::filesystem::path temporary;ComPtr<HttpAudioStream> stream;
        try {
            // The callback owns only the cancellation flag. Locking a weak Job here
            // could drop its last reference inside CacheTask's callback lock,
            // destroy its retained stream and re-enter that same lock.
            profile.network_canceled=[canceled=job->canceled]{return canceled->load();};
            result=OpenHttpAudioStream(job->info.entry.sourceurl,profile,&stream,HttpStreamUse::background);if(FAILED(result)) throw Error{result};
            STATSTG stat{};result=stream->Stat(&stat,STATFLAG_NONAME);
            // Endless radio must not occupy a slot or fill a user's download disk.
            if(FAILED(result) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart>0x80000000ULL) throw Error{HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)};
            if(job->info.prefetch && !stream->CanPersist()) throw Error{HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)};
            {std::lock_guard lock(mutex);job->info.total=stat.cbSize.QuadPart;job->info.received=0;}
            Handle output;
            if(!job->info.prefetch) {
                std::filesystem::create_directories(job->info.destination.parent_path());
                for(unsigned suffix=0;suffix<100;++suffix) {
                    temporary=job->info.destination.wstring()+L".ttp-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                        std::to_wstring(job->info.id)+L"-"+std::to_wstring(suffix)+L".tmp";
                    output.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
                    if(output.h!=INVALID_HANDLE_VALUE || GetLastError()!=ERROR_FILE_EXISTS) break;
                }
                if(output.h==INVALID_HANDLE_VALUE) {temporary.clear();Check(false);}
            }
            std::array<BYTE,16384> bytes{};uint64_t total{};
            while(total<stat.cbSize.QuadPart) {
                if(job->canceled->load()) throw Error{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
                ULONG got{};result=stream->Read(bytes.data(),static_cast<ULONG>(std::min<uint64_t>(bytes.size(),stat.cbSize.QuadPart-total)),&got);
                if(FAILED(result)) throw Error{result};if(!got) throw Error{HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)};
                if(output.h!=INVALID_HANDLE_VALUE){DWORD done{};Check(WriteFile(output.h,bytes.data(),got,&done,nullptr));if(done!=got) throw Error{STG_E_WRITEFAULT};}
                total+=got;{std::lock_guard lock(mutex);job->info.received=total;}
            }
            if(job->canceled->load()) throw Error{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
            if(output.h!=INVALID_HANDLE_VALUE){Check(FlushFileBuffers(output.h));CloseHandle(output.h);output.h=INVALID_HANDLE_VALUE;
                std::lock_guard lock(mutex);
                if(job->canceled->load()) throw Error{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
                Check(MoveFileExW(temporary.c_str(),job->info.destination.c_str(),MOVEFILE_WRITE_THROUGH|(job->info.replace?MOVEFILE_REPLACE_EXISTING:0)));
                // Replacing a shared pathname must not leave another URL mapped
                // to the replacement song in the catalog.
                for(auto& other:jobs) if(other!=job && !_wcsicmp(other->info.entry.save.c_str(),job->info.destination.c_str())) other->info.entry.save.clear();
                published=true;temporary.clear();}
            result=S_OK;
        }catch(const Error& e){result=e.hr;}catch(const std::bad_alloc&){result=E_OUTOFMEMORY;}catch(...){result=E_FAIL;}
        // Keep one completed prediction alive until consumed/replaced, so playback
        // can attach without another header request after the prefetch completes.
        ComPtr<HttpAudioStream> prefetched;
        if(job->info.prefetch && SUCCEEDED(result) && !job->canceled->load()) prefetched.Swap(stream);
        stream.Reset(); // Last consumer joins/flushed; surviving readers keep the producer.
        if(!temporary.empty()) DeleteFileW(temporary.c_str());
        {std::lock_guard lock(mutex);
            if(published) {job->info.state=DownloadState::complete;job->info.entry.save=job->info.destination.wstring();}
            else if((stop && !job->user_canceled) || job->requeue) job->info.state=DownloadState::waiting;
            else if(job->canceled->load()) job->info.state=DownloadState::canceled;
            else if(FAILED(result)) job->info.state=DownloadState::failed;
            else {job->info.state=DownloadState::complete;job->info.entry.save=job->info.destination.wstring();}
            if(job->info.prefetch && job->info.state==DownloadState::complete) job->prefetched=std::move(prefetched);
            job->info.result=result;
            if(job->removed) jobs.erase(std::remove(jobs.begin(),jobs.end(),job),jobs.end());
            Changed();
        }
    }
};
HttpDownloadQueue::HttpDownloadQueue(std::filesystem::path file):impl_(std::make_unique<Impl>(std::move(file))){}
HttpDownloadQueue::~HttpDownloadQueue()=default;
void HttpDownloadQueue::Configure(const PlaybackOptions& options,int maximum) {
    std::lock_guard lock(impl_->mutex);impl_->options=options;impl_->options.network_canceled={};
    impl_->maximum=std::clamp(maximum,1,5);int keep{};
    for(auto& j:impl_->jobs) if(j->info.state==DownloadState::running && ++keep>impl_->maximum && !j->canceled->load()){j->requeue=true;j->canceled->store(true);}
    impl_->StartWorkers();
    impl_->changed.notify_all();
}
uint64_t HttpDownloadQueue::Enqueue(std::wstring url,std::wstring title,std::filesystem::path destination,bool replace,std::wstring artist) {
    url=HttpDownloadKey(std::move(url));if(url.empty() || destination.empty() || !destination.is_absolute()) return 0;
    std::lock_guard lock(impl_->mutex);if(!impl_->writable) return 0;
    for(auto& j:impl_->jobs) if(!j->removed && !j->info.prefetch && j->info.entry.sourceurl==url && j->info.destination==destination) return j->info.id;
    if(impl_->jobs.size()>=10000) return 0;
    auto job=std::make_shared<Impl::Job>();job->info.id=++impl_->serial;job->info.entry.sourceurl=std::move(url);
    job->info.entry.title=LegacyCacheTitle(job->info.entry.sourceurl,title,artist);
    job->info.display_title=std::move(title);job->info.destination=std::move(destination);job->info.replace=replace;
    if(std::count_if(impl_->jobs.begin(),impl_->jobs.end(),[](const auto& j){return j->info.state==DownloadState::running;})>=impl_->maximum)
        for(auto& j:impl_->jobs) if(j->info.prefetch && j->info.state==DownloadState::running && !j->canceled->load()) {j->requeue=true;j->canceled->store(true);break;}
    impl_->jobs.push_back(job);impl_->Changed();impl_->StartWorkers();return job->info.id;
}
void HttpDownloadQueue::Cancel(uint64_t id){std::lock_guard lock(impl_->mutex);
    if(!impl_->writable) return;
    for(auto& j:impl_->jobs) if(!j->removed && j->info.id==id &&
        (j->info.state==DownloadState::running || j->info.state==DownloadState::waiting || j->info.state==DownloadState::paused)) {
        j->canceled->store(true);j->user_canceled=true;j->requeue=false;
        if(j->info.state!=DownloadState::running) j->info.state=DownloadState::canceled;
        impl_->Changed();break;
    }}
void HttpDownloadQueue::Retry(uint64_t id){std::lock_guard lock(impl_->mutex);
    if(!impl_->writable) return;
    for(auto& j:impl_->jobs) if(!j->removed && j->info.id==id && j->info.state!=DownloadState::running && j->info.state!=DownloadState::waiting) {
        j->info.state=DownloadState::waiting;j->info.entry.save.clear();j->info.result=S_OK;
        j->canceled->store(false);j->user_canceled=false;j->requeue=false;
        impl_->Changed();impl_->StartWorkers();break;
    }}
bool HttpDownloadQueue::Remove(uint64_t id) {
    // Release a retained prefetch/stream after dropping the queue mutex.
    std::shared_ptr<Impl::Job> removed;
    {std::lock_guard lock(impl_->mutex);
        const auto found=std::find_if(impl_->jobs.begin(),impl_->jobs.end(),
            [id](const auto& job){return job->info.id==id && !job->removed && !job->info.prefetch;});
        if(found==impl_->jobs.end() || !impl_->writable) return false;
        removed=*found;removed->removed=true;removed->user_canceled=true;
        removed->requeue=false;removed->canceled->store(true);
        // A running tombstone still occupies its worker/concurrency slot.
        // Run erases it after its stream and temporary file have been released.
        if(removed->info.state!=DownloadState::running) impl_->jobs.erase(found);
        impl_->Changed();
    }
    return true;
}
size_t HttpDownloadQueue::ClearHistory() {
    std::vector<std::shared_ptr<Impl::Job>> removed;
    {std::lock_guard lock(impl_->mutex);
        if(!impl_->writable) return 0;
        for(const auto& job:impl_->jobs) if(!job->removed && !job->info.prefetch &&
            (job->info.state==DownloadState::complete || job->info.state==DownloadState::canceled ||
             job->info.state==DownloadState::failed)) removed.push_back(job);
        if(removed.empty()) return 0;
        for(const auto& job:removed) job->removed=true;
        impl_->jobs.erase(std::remove_if(impl_->jobs.begin(),impl_->jobs.end(),
            [](const auto& job){return job->removed && job->info.state!=DownloadState::running;}),impl_->jobs.end());
        impl_->Changed();
    }
    return removed.size();
}
size_t HttpDownloadQueue::ImportLegacyList(const std::filesystem::path& file,
    const std::filesystem::path& folder,bool artist_folder,bool replace) {
    if(folder.empty() || !folder.is_absolute()) return 0;
    std::filesystem::path cache_folder;
    {std::lock_guard lock(impl_->mutex);if(!impl_->writable) return 0;
        cache_folder=impl_->options.network_cache_folder;}
    ComInit com;
    const auto tasks=Items(file,L"/tasklist[@version='1']/downloading/item | /tasklist[@version='1']/complete/item",L"/tasklist[@version='1']");
    long count{};if(tasks) tasks->get_length(&count);
    std::vector<HttpDownload> imported;
    for(long i=0;i<std::min(count,10000L);++i) {
        ComPtr<IXMLDOMNode> node,parent;tasks->get_item(i,&node);if(!node) continue;
        node->get_parentNode(&parent);BSTR name{};if(parent) parent->get_nodeName(&name);
        const bool complete=name && wcscmp(name,L"complete")==0;SysFreeString(name);
        HttpDownload d;const auto title=Attribute(node.Get(),L"title"),artist=Attribute(node.Get(),L"artist");
        d.entry.sourceurl=HttpDownloadKey(Attribute(node.Get(),L"url"));if(d.entry.sourceurl.empty()) continue;
        d.entry.title=LegacyCacheTitle(d.entry.sourceurl,title,artist);d.display_title=title;d.replace=replace;
        d.destination=HttpDownloadDestination(folder,d.entry.sourceurl,title,artist,artist_folder);
        auto format=Attribute(node.Get(),L"format");if(!format.empty() && format.front()!=L'.') format=L"."+format;
        if(AudioEngine::IsRecoveredFormatPath(std::filesystem::path(L"track"+format))) d.destination.replace_extension(format);
        if(complete) {
            d.entry.save=Attribute(node.Get(),L"path");
            const auto saved=ResolveLegacySavedFile({d.entry},d.entry.sourceurl,{folder});
            if(!saved.empty()){d.destination=saved;d.state=DownloadState::complete;}
            else {d.entry.save.clear();d.state=DownloadState::failed;d.result=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);}
        }
        else d.state=DownloadState::paused; // 00421C45 restores downloading tasks to state 5, not waiting (1).
        // v1 size/progress describes UI state; it does not verify a temp prefix.
        imported.push_back(std::move(d));
    }
    if(count==0) for(const auto& entry:ReadLegacyCacheList(file)) {
        HttpDownload d;d.entry=entry;d.entry.sourceurl=HttpDownloadKey(entry.sourceurl);
        if(d.entry.sourceurl.empty()) continue;
        d.display_title=entry.title;d.replace=replace;
        const auto saved=ResolveLegacySavedFile({entry},d.entry.sourceurl,{folder,cache_folder});
        d.destination=saved.empty()?HttpDownloadDestination(folder,d.entry.sourceurl,{}, {},false):saved;
        if(saved.empty()){d.entry.save.clear();d.state=DownloadState::paused;}
        else d.state=DownloadState::complete;
        imported.push_back(std::move(d));
    }
    size_t added{};std::lock_guard lock(impl_->mutex);if(!impl_->writable) return 0;
    for(auto& d:imported) {
        if(impl_->jobs.size()>=10000) break;
        if(std::any_of(impl_->jobs.begin(),impl_->jobs.end(),[&](const auto& j){return !j->removed && j->info.entry.sourceurl==d.entry.sourceurl && j->info.destination==d.destination;})) continue;
        auto j=std::make_shared<Impl::Job>();d.id=++impl_->serial;j->info=std::move(d);
        if(j->info.state==DownloadState::complete) {
            std::error_code ec;const auto bytes=std::filesystem::file_size(j->info.destination,ec);
            if(!ec) j->info.received=j->info.total=bytes;
        }
        impl_->jobs.push_back(j);++added;
    }
    if(added) impl_->Changed();impl_->StartWorkers();return added;
}
void HttpDownloadQueue::Prefetch(std::wstring url){url=HttpDownloadKey(std::move(url));std::lock_guard lock(impl_->mutex);
    if(impl_->prediction==url) return;impl_->prediction=url;
    for(auto& j:impl_->jobs) if(j->info.prefetch){j->canceled->store(true);j->requeue=false;if(j->info.state==DownloadState::waiting) j->info.state=DownloadState::canceled;}
    impl_->jobs.erase(std::remove_if(impl_->jobs.begin(),impl_->jobs.end(),[](auto& j){return j->info.prefetch && j->info.state!=DownloadState::running;}),impl_->jobs.end());
    if(url.empty() || impl_->jobs.size()>=10000 || !impl_->options.network_cache_enabled) return;
    for(const auto& j:impl_->jobs) if(!j->removed && j->info.entry.sourceurl==url) {
        if(j->info.state==DownloadState::waiting || j->info.state==DownloadState::running) return;
        // Playback already resolves a verified completed download as a local
        // file. Predicting that same song must not reopen its HTTP resource.
        if(j->info.state==DownloadState::complete && !ResolveLegacySavedFile(
            {j->info.entry},url,{j->info.destination.parent_path()}).empty()) return;
    }
    auto j=std::make_shared<Impl::Job>();j->info.id=++impl_->serial;j->info.entry.sourceurl=std::move(url);j->info.prefetch=true;
    impl_->jobs.push_back(j);impl_->StartWorkers();impl_->changed.notify_all();}
std::vector<HttpDownload> HttpDownloadQueue::Snapshot() const {std::lock_guard lock(impl_->mutex);std::vector<HttpDownload> out;
    for(const auto& j:impl_->jobs) if(!j->removed) out.push_back(j->info);return out;}
HRESULT HttpDownloadQueue::StorageResult() const noexcept {return impl_->storage;}
std::filesystem::path HttpDownloadQueue::RecoveryBackupPath() const {
    std::lock_guard lock(impl_->mutex);return impl_->recovery_backup;
}
} // namespace ttplayer::audio
