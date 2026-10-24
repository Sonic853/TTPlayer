#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/archive_password.h"
#include "ttplayer/audio/osz_archive.h"
#include <ttpcomm/archive_format.h>
#include <ttpcomm/archive_volumes.h>

#include <ttpcomm/runtime_client.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <exception>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <list>

namespace ttplayer::audio {
class ArchiveCacheIdentityState {
public:
    struct File { std::filesystem::path path; BY_HANDLE_FILE_INFORMATION info{}; uint64_t change{}; };
    std::filesystem::path source, first;
    uint64_t generation{}; size_t cost{sizeof(ArchiveCacheIdentityState)};
    std::vector<File> files;
    std::vector<ArchiveVolumeMappingIdentity> mappings;
};
namespace {
constexpr size_t kSmallCacheBytes = 16U * 1024 * 1024;
constexpr size_t kSmallMemberBytes = 4U * 1024 * 1024;
constexpr size_t kDirectoryCacheBytes = 4U * 1024 * 1024;
constexpr size_t kCacheEntries = 128, kArchiveIdentities = 32;
struct SmallMemberCacheEntry {
    ArchiveCacheIdentity identity; std::wstring member; std::vector<unsigned char> bytes; size_t cost{};
};
struct DirectoryCacheEntry {
    ArchiveCacheIdentity identity; std::vector<std::wstring> names; size_t cost{};
};
std::mutex small_cache_mutex;
std::list<ArchiveCacheIdentity> archive_identities;
std::list<std::shared_ptr<const SmallMemberCacheEntry>> small_members;
std::list<std::shared_ptr<const DirectoryCacheEntry>> small_directories;
size_t small_member_bytes{}, small_directory_bytes{}, identity_bytes{};
void CheckCacheCancellation(const std::function<bool()>& canceled) {
    if(canceled && canceled())throw ArchiveError(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
std::filesystem::path CacheArchivePath(const std::filesystem::path& path) {
    const auto outer=path.native().substr(0,path.native().find(L'|'));
    const auto canonical=ttpcomm::archive_format::Canonical(outer.c_str());
    std::error_code error;auto full=std::filesystem::absolute(canonical,error);
    return (error?std::filesystem::path(canonical):full).lexically_normal();
}
bool EqualCachePath(const std::filesystem::path& a,const std::filesystem::path& b) {
    return !_wcsicmp(a.c_str(),b.c_str());
}
std::wstring CacheMemberName(std::wstring name) {
    std::replace(name.begin(),name.end(),L'/',L'\\');return name;
}
bool FileStamp(const std::filesystem::path& path,ArchiveCacheIdentityState::File& out) {
    const HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    const bool okay=GetFileInformationByHandle(file,&out.info)!=FALSE &&
        !(out.info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);
    out.change=ttpcomm::archive_volumes::ChangeTime(file);CloseHandle(file);out.path=path;
    return okay && out.change!=0;
}
bool SameFileStamp(const ArchiveCacheIdentityState::File& a,const ArchiveCacheIdentityState::File& b) {
    return a.change==b.change && a.info.dwVolumeSerialNumber==b.info.dwVolumeSerialNumber &&
        a.info.nFileIndexHigh==b.info.nFileIndexHigh && a.info.nFileIndexLow==b.info.nFileIndexLow &&
        a.info.nFileSizeHigh==b.info.nFileSizeHigh && a.info.nFileSizeLow==b.info.nFileSizeLow &&
        CompareFileTime(&a.info.ftLastWriteTime,&b.info.ftLastWriteTime)==0 &&
        CompareFileTime(&a.info.ftCreationTime,&b.info.ftCreationTime)==0;
}
ArchiveCacheIdentity FindCacheIdentity(const std::filesystem::path& path,const std::function<bool()>& canceled) {
    std::vector<ArchiveCacheIdentity> candidates;
    {std::lock_guard lock(small_cache_mutex);for(const auto& item:archive_identities)
        if(EqualCachePath(item->source,path))candidates.push_back(item);}
    for(const auto& item:candidates)if(ArchiveCacheIdentityFresh(item,canceled))return item;
    return {};
}
void RememberCacheIdentity(const ArchiveCacheIdentity& identity) {
    if(!identity)return;std::lock_guard lock(small_cache_mutex);
    for(auto i=archive_identities.begin();i!=archive_identities.end();)if(EqualCachePath((*i)->source,identity->source)){
        identity_bytes-=(*i)->cost;i=archive_identities.erase(i);}else ++i;
    identity_bytes+=identity->cost;archive_identities.push_front(identity);
    while(archive_identities.size()>kArchiveIdentities || identity_bytes>4U*1024*1024){identity_bytes-=archive_identities.back()->cost;archive_identities.pop_back();}
}
void RememberSmallMember(const ArchiveCacheIdentity& identity,const std::wstring& member,
                         const std::vector<unsigned char>& bytes,const std::function<bool()>& canceled) {
    if(!identity || bytes.size()>kSmallMemberBytes || !ArchiveCacheIdentityFresh(identity,canceled))return;
    const size_t cost=sizeof(SmallMemberCacheEntry)+identity->cost+(member.size()+1)*sizeof(wchar_t)+bytes.size();
    if(cost>kSmallCacheBytes)return;
    auto entry=std::make_shared<SmallMemberCacheEntry>();entry->identity=identity;entry->member=member;entry->bytes=bytes;entry->cost=cost;
    CheckCacheCancellation(canceled);std::lock_guard lock(small_cache_mutex);
    for(auto i=small_members.begin();i!=small_members.end();)if(EqualCachePath((*i)->identity->source,identity->source) &&
        !_wcsicmp((*i)->member.c_str(),member.c_str())){small_member_bytes-=(*i)->cost;i=small_members.erase(i);}else ++i;
    while(!small_members.empty() && (small_members.size()>=kCacheEntries || small_member_bytes+cost>kSmallCacheBytes)){
        small_member_bytes-=small_members.back()->cost;small_members.pop_back();}
    small_member_bytes+=cost;small_members.push_front(std::move(entry));
}
bool FindCachedDirectory(const std::filesystem::path& path,std::vector<std::wstring>& names,
                         ArchiveCacheIdentity& identity,const std::function<bool()>& canceled) {
    std::vector<std::shared_ptr<const DirectoryCacheEntry>> candidates;
    {std::lock_guard lock(small_cache_mutex);for(const auto& item:small_directories)
        if(EqualCachePath(item->identity->source,path))candidates.push_back(item);}
    for(const auto& item:candidates)if(ArchiveCacheIdentityFresh(item->identity,canceled)){
        names=item->names;identity=item->identity;CheckCacheCancellation(canceled);
        std::lock_guard lock(small_cache_mutex);auto at=std::find(small_directories.begin(),small_directories.end(),item);
        if(at!=small_directories.end())small_directories.splice(small_directories.begin(),small_directories,at);return true;
    }
    return false;
}
void RememberDirectory(const ArchiveCacheIdentity& identity,const std::vector<std::wstring>& names,
                       const std::function<bool()>& canceled) {
    if(!identity || !ArchiveCacheIdentityFresh(identity,canceled))return;
    size_t cost=sizeof(DirectoryCacheEntry)+identity->cost;if(cost>kDirectoryCacheBytes)return;for(const auto& n:names){
        const size_t bytes=sizeof(std::wstring)+(n.size()+1)*sizeof(wchar_t);
        if(bytes>kDirectoryCacheBytes-cost)return;cost+=bytes;}
    auto entry=std::make_shared<DirectoryCacheEntry>();entry->identity=identity;entry->names=names;entry->cost=cost;
    CheckCacheCancellation(canceled);std::lock_guard lock(small_cache_mutex);
    for(auto i=small_directories.begin();i!=small_directories.end();)if(EqualCachePath((*i)->identity->source,identity->source)){
        small_directory_bytes-=(*i)->cost;i=small_directories.erase(i);}else ++i;
    while(!small_directories.empty() && (small_directories.size()>=kArchiveIdentities || small_directory_bytes+cost>kDirectoryCacheBytes)){
        small_directory_bytes-=small_directories.back()->cost;small_directories.pop_back();}
    small_directory_bytes+=cost;small_directories.push_front(std::move(entry));
}


bool HasCoverSignature(const std::vector<unsigned char>& bytes) {
    return (bytes.size() >= 24 && !memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8)) ||
        (bytes.size() >= 4 && bytes[0]==0xff && bytes[1]==0xd8 && bytes[2]==0xff) ||
        (bytes.size() >= 26 && bytes[0]=='B' && bytes[1]=='M') ||
        (bytes.size() >= 13 && (!memcmp(bytes.data(),"GIF87a",6) || !memcmp(bytes.data(),"GIF89a",6)));
}

size_t FindZipMarker(std::wstring_view value) noexcept {
    if (value.size() < 6) return std::wstring_view::npos;
    for (size_t index = 1; index + 5 <= value.size(); ++index) {
        if (value[index] == L'.' &&
            towlower(value[index + 1]) == L'z' &&
            towlower(value[index + 2]) == L'i' &&
            towlower(value[index + 3]) == L'p' &&
            value[index + 4] == L'|') {
            return index;
        }
    }
    return std::wstring_view::npos;
}

bool PathKind(std::wstring_view value, ArchiveKind& kind) {
    const auto detected=ttpcomm::archive_format::Detect(std::wstring(value).c_str());
    switch(detected){
    case ttpcomm::archive_format::Kind::zip:kind=ArchiveKind::zip;return true;
    case ttpcomm::archive_format::Kind::rar:kind=ArchiveKind::rar;return true;
    case ttpcomm::archive_format::Kind::seven:kind=ArchiveKind::seven_zip;return true;
    case ttpcomm::archive_format::Kind::tar:kind=ArchiveKind::tar;return true;
    case ttpcomm::archive_format::Kind::gzip:kind=ArchiveKind::gzip;return true;
    case ttpcomm::archive_format::Kind::targzip:kind=ArchiveKind::tar_gzip;return true;
    default:break;
    }
    struct Suffix {const wchar_t* text; ArchiveKind kind;};
    constexpr Suffix formats[]={{L".tar.gz",ArchiveKind::tar_gzip},
        {L".zip",ArchiveKind::zip},{L".osz",ArchiveKind::zip},{L".rar",ArchiveKind::rar},
        {L".tar",ArchiveKind::tar},{L".tgz",ArchiveKind::tar_gzip},
        {L".gz",ArchiveKind::gzip},{L".7z",ArchiveKind::seven_zip}};
    for(const auto& format:formats) {
        const size_t n=wcslen(format.text);
        if(value.size()>n && _wcsnicmp(value.data()+value.size()-n,format.text,n)==0) {
            kind=format.kind;return true;
        }
    }
    return false;
}

std::filesystem::path NormalizeArchiveMember(std::wstring_view value) {
    std::wstring normalized(value);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    while (!normalized.empty() && normalized.front() == L'\\')
        normalized.erase(normalized.begin());
    auto path = std::filesystem::path(normalized).lexically_normal();
    if (path.empty() || path.has_root_name() || path.has_root_directory())
        throw std::runtime_error("invalid archive member path");
    for (const auto& component : path) {
        if (component == L"..")
            throw std::runtime_error("archive member escapes archive root");
    }
    return path;
}

TtpCommArchiveStreamApi ArchiveApi(HMODULE module) {
    TtpCommArchiveStreamApi api{};
    if(module) {
        if(ttpcomm::host::QueryArchiveStreams(module,api)) return api;
    } else if(const auto* loaded=ttpcomm::host::ArchiveStreams()) return *loaded;
    throw ArchiveError(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
}
TtpCommArchiveGroupApi GroupApi(HMODULE module){
    TtpCommArchiveGroupApi api{};
    if(module){if(!ttpcomm::host::QueryArchiveGroups(module,api))api={};}
    else if(const auto* loaded=ttpcomm::host::ArchiveGroups())api=*loaded;
    return api;
}

// Decoder clones must retain their cancellation context too. The DLL producer
// never owns a callback into a destroyed AudioEngine or metadata worker.
class OwnedArchiveStream final : public IStream, public TtpCommArchiveStreamStatusInterface {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<std::function<bool()>> canceled_;
    Microsoft::WRL::ComPtr<IStream> inner_;
    decltype(TtpCommArchiveStreamApi::status) status_;
    std::filesystem::path archive_;
    HRESULT Result(HRESULT hr){
        if(hr==HRESULT_FROM_WIN32(ERROR_CRC) || hr==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) ||
           hr==TTPCOMM_ARCHIVE_MISSING_VOLUME || hr==STG_E_READFAULT)DiscardArchiveVolumes(archive_);
        return hr;
    }
public:
    OwnedArchiveStream(std::shared_ptr<std::function<bool()>> canceled,IStream* stream,
                       decltype(TtpCommArchiveStreamApi::status) status,const std::filesystem::path& archive)
        : canceled_(std::move(canceled)),status_(status),archive_(archive) {inner_.Attach(stream);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id==IID_IUnknown || id==IID_IStream || id==IID_ISequentialStream){*out=static_cast<IStream*>(this);AddRef();return S_OK;}
        // Never expose the inner stream: it borrows our cancellation context.
        if(id==IID_TtpCommArchiveStreamStatus){*out=static_cast<TtpCommArchiveStreamStatusInterface*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--references_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Snapshot(TtpCommArchiveStreamStatus* out) override{return status_(inner_.Get(),out);}
    HRESULT STDMETHODCALLTYPE Read(void* p,ULONG n,ULONG* r) override{return Result(inner_->Read(p,n,r));}
    HRESULT STDMETHODCALLTYPE Write(const void* p,ULONG n,ULONG* r) override{return inner_->Write(p,n,r);}
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER p,DWORD w,ULARGE_INTEGER* r) override{return Result(inner_->Seek(p,w,r));}
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER n) override{return inner_->SetSize(n);}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* s,ULARGE_INTEGER n,ULARGE_INTEGER* r,ULARGE_INTEGER* w) override{return inner_->CopyTo(s,n,r,w);}
    HRESULT STDMETHODCALLTYPE Commit(DWORD f) override{return inner_->Commit(f);}
    HRESULT STDMETHODCALLTYPE Revert() override{return inner_->Revert();}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER a,ULARGE_INTEGER b,DWORD c) override{return inner_->LockRegion(a,b,c);}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER a,ULARGE_INTEGER b,DWORD c) override{return inner_->UnlockRegion(a,b,c);}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* s,DWORD f) override{return Result(inner_->Stat(s,f));}
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if(!out)return E_POINTER;*out=nullptr;Microsoft::WRL::ComPtr<IStream> clone;
        const auto hr=inner_->Clone(&clone);if(FAILED(hr))return hr;
        try{*out=new OwnedArchiveStream(canceled_,clone.Get(),status_,archive_);clone.Detach();return S_OK;}catch(...){return E_OUTOFMEMORY;}
    }
};
IStream* OpenMember(const ArchiveMemberPath& path,HMODULE module,
                    const std::function<bool()>& canceled,bool cache,uint64_t limit) {
    const auto api=ArchiveApi(module);
    const auto group=GroupApi(module);auto volumes=ArchiveVolumeOptions(&canceled);
    auto control=std::make_shared<std::function<bool()>>(canceled);
    TtpCommArchiveStreamOptions options{};options.size=sizeof(options);
    options.flags=cache?TTPCOMM_ARCHIVE_CACHE_CURRENT:0;
    options.limits.size=sizeof(options.limits);options.limits.member_limit_bytes=limit;
    options.context=control.get();options.canceled=[](void* context)->BOOL {
        const auto& fn=*static_cast<std::function<bool()>*>(context);return fn && fn();
    };
    Microsoft::WRL::ComPtr<IStream> stream;
    const auto hr=WithArchivePassword(path.archive,[&](const wchar_t* password)->HRESULT {
        stream.Reset();options.limits.password=password;
        auto result=group.open_member?group.open_member(path.archive.c_str(),path.member.c_str(),&options,&volumes,&stream)
            :api.open_member(path.archive.c_str(),path.member.c_str(),&options,&stream);
        if(FAILED(result) || !stream || !password)return result;
        // Password-capable streams already withhold encrypted plaintext until
        // the member CRC / authentication has succeeded. Do not force an
        // unencrypted member to EOF just because this archive has a saved
        // password (mixed encrypted/unencrypted ZIPs are valid).
        const auto capabilities=group.open_member?group.capabilities:api.capabilities;
        if(capabilities&TTPCOMM_ARCHIVE_CAP_PASSWORDS)return result;
        // A password verifier alone is not proof: ZipCrypto has only 8 check
        // bits. Wait for the member CRC / AES authentication before accepting
        // a candidate or exposing plaintext to a decoder.
        LARGE_INTEGER zero{};result=stream->Seek(zero,STREAM_SEEK_END,nullptr);
        if(FAILED(result))return result;unsigned char byte{};ULONG read{};
        result=stream->Read(&byte,1,&read);if(FAILED(result))return result;
        if(read)return STG_E_READFAULT;
        return stream->Seek(zero,STREAM_SEEK_SET,nullptr);
    },canceled);
    if(FAILED(hr)){if(hr!=TTPCOMM_ARCHIVE_PASSWORD && hr!=TTPCOMM_ARCHIVE_BAD_PASSWORD)DiscardArchiveVolumes(path.archive);throw ArchiveError(hr);}
    if(!stream)throw ArchiveError(E_UNEXPECTED);
    auto* result=new OwnedArchiveStream(std::move(control),stream.Get(),api.status,path.archive);stream.Detach();return result;
}

} // namespace

namespace {
bool CacheIdentityFilesFresh(const ArchiveCacheIdentity& identity,const std::function<bool()>& canceled) noexcept {
    try {
        if(!identity || identity->files.empty())return false;
        for(const auto& file:identity->files){CheckCacheCancellation(canceled);ArchiveCacheIdentityState::File current;
            if(!FileStamp(file.path,current) || !SameFileStamp(file,current))return false;}
        // A volume restored at its expected path takes precedence over a prior
        // replacement even when the previously selected replacement is intact.
        for(const auto& mapping:identity->mappings)if(!EqualCachePath(mapping.expected,mapping.actual)){
            CheckCacheCancellation(canceled);const auto attributes=GetFileAttributesW(mapping.expected.c_str());
            if(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY))return false;
        }
        CheckCacheCancellation(canceled);return true;
    }catch(...){return false;}
}
}
bool ArchiveCacheIdentityFresh(const ArchiveCacheIdentity& identity,const std::function<bool()>& canceled) noexcept {
    return identity && identity->generation==ArchiveSessionGeneration() &&
        CacheIdentityFilesFresh(identity,canceled) && identity->generation==ArchiveSessionGeneration();
}
ArchiveCacheIdentity CaptureArchiveCacheIdentity(const std::filesystem::path& archive,HMODULE module,
                                               const std::function<bool()>& canceled) {
    CheckCacheCancellation(canceled);const auto source=CacheArchivePath(archive);
    if(auto known=FindCacheIdentity(source,canceled))return known;
    const auto group=GroupApi(module);if(!group.volumes)return {};
    std::vector<std::wstring> actual;auto volumes=ArchiveVolumeOptions(&canceled);
    const auto hr=WithArchivePassword(source,[&](const wchar_t* password){
        actual.clear();TtpCommArchiveOptions limits{};limits.size=sizeof(limits);limits.password=password;
        limits.context=const_cast<std::function<bool()>*>(&canceled);
        limits.progress=[](void* p,uint64_t,uint64_t)->BOOL{const auto& stop=*static_cast<std::function<bool()>*>(p);return !stop || !stop();};
        return group.volumes(source.c_str(),&limits,&volumes,[](void* p,const wchar_t* path)->BOOL{
            try{auto& files=*static_cast<std::vector<std::wstring>*>(p);if(files.size()>=4096)return FALSE;files.emplace_back(path);return TRUE;}catch(...){return FALSE;}
        },&actual);
    },canceled);
    if(FAILED(hr))throw ArchiveError(hr);CheckCacheCancellation(canceled);if(actual.empty())return {};
    auto result=std::make_shared<ArchiveCacheIdentityState>();result->source=source;result->first=actual.front();
    result->generation=ArchiveSessionGeneration();result->mappings=ArchiveSessionVolumeMappings(source);
    // Keep only mappings actually used by this volume set. A restored expected
    // file supersedes a stale session replacement without disabling caching.
    std::erase_if(result->mappings,[&](const auto& mapping){
        const auto contains=[&](const auto& path){return std::any_of(actual.begin(),actual.end(),[&](const auto& file){return EqualCachePath(file,path);});};
        return !contains(mapping.actual) || contains(mapping.expected);
    });
    result->cost+=(result->source.native().size()+result->first.native().size()+2)*sizeof(wchar_t);
    for(const auto& m:result->mappings)result->cost+=sizeof(m)+(m.expected.native().size()+m.actual.native().size()+2)*sizeof(wchar_t);
    if(result->cost>512U*1024)return {};
    for(const auto& file:actual){CheckCacheCancellation(canceled);ArchiveCacheIdentityState::File stamp;
        if(!FileStamp(file,stamp))return {};
        result->cost+=sizeof(stamp)+(stamp.path.native().size()+1)*sizeof(wchar_t);
        if(result->cost>512U*1024)return {};
        result->files.push_back(std::move(stamp));}
    if(!ArchiveCacheIdentityFresh(result,canceled))return {};
    RememberCacheIdentity(result);return result;
}
bool TryReadCachedArchiveMember(const ArchiveMemberPath& path,std::vector<unsigned char>& bytes,
                               ArchiveCacheIdentity* identity,const std::function<bool()>& canceled) noexcept {
    bytes.clear();if(identity)identity->reset();
    try {
        CheckCacheCancellation(canceled);const auto source=CacheArchivePath(path.archive);const auto name=CacheMemberName(path.member);
        std::vector<std::shared_ptr<const SmallMemberCacheEntry>> candidates;
        {std::lock_guard lock(small_cache_mutex);for(const auto& item:small_members)
            if(EqualCachePath(item->identity->source,source) && !_wcsicmp(item->member.c_str(),name.c_str()))candidates.push_back(item);}
        for(const auto& item:candidates)if(ArchiveCacheIdentityFresh(item->identity,canceled)){
            bytes=item->bytes;CheckCacheCancellation(canceled);if(identity)*identity=item->identity;
            std::lock_guard lock(small_cache_mutex);auto at=std::find(small_members.begin(),small_members.end(),item);
            if(at!=small_members.end())small_members.splice(small_members.begin(),small_members,at);return true;
        }
    }catch(...){bytes.clear();if(identity)identity->reset();}
    return false;
}

ArchiveError::ArchiveError(HRESULT result)
    : std::runtime_error("Archive operation failed: " + std::to_string(static_cast<unsigned long>(result))),result_(result) {}

std::wstring ArchiveError::Message() const {
    if(result_==kOszPluginMissing) return L"无法加载 AddIn\\ttp_osu.dll。请安装完整发行包中的 OSZ 插件。";
    if(result_==kOszPluginIncompatible) return L"AddIn\\ttp_osu.dll 版本或接口不兼容。请更新 OSZ 插件后重新启动播放器。";
    if(result_==TTPCOMM_ARCHIVE_PASSWORD) return L"压缩包需要密码。请播放或重新添加该压缩包以输入密码。";
    if(result_==TTPCOMM_ARCHIVE_BAD_PASSWORD) return L"压缩包密码不正确，或加密数据已损坏。";
    if(result_==TTPCOMM_ARCHIVE_MISSING_VOLUME) return L"压缩包缺少分卷。请重新导入并选择所需分卷，或将全部分卷放在同一目录。";
    if(result_==TTPCOMM_ARCHIVE_DICTIONARY_LIMIT) return L"压缩包所需的解压字典超过播放器的内存限制。";
    if(result_==TTPCOMM_ARCHIVE_UNSUPPORTED) return L"不支持此压缩方式或成员类型。";
    if(result_==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)) return L"ttpcomm.dll 缺少归档接口，请手动替换完整发行包。";
    if(result_==HRESULT_FROM_WIN32(ERROR_CRC) || result_==HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) return L"压缩包数据损坏或校验失败。";
    if(result_==HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)) return L"压缩包成员超过允许的大小。";
    if(result_==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return L"已取消读取压缩包。";
    if(result_==E_OUTOFMEMORY) return L"读取压缩包时内存不足。";
    if(result_==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return L"压缩包内找不到此文件。";
    return L"无法读取压缩包，请检查文件是否完整且可访问。";
}

bool ParseZipMemberPath(std::wstring_view logical_path,
                        ZipMemberPath& result) {
    const size_t marker = FindZipMarker(logical_path);
    if (marker == std::wstring_view::npos || marker + 5 >= logical_path.size())
        return false;
    result.archive = std::filesystem::path(logical_path.substr(0, marker + 4));
    result.member.assign(logical_path.substr(marker + 5));
    std::replace(result.member.begin(), result.member.end(), L'/', L'\\');
    return !result.archive.empty() && !result.member.empty();
}

std::filesystem::path MakeZipMemberPath(
    const std::filesystem::path& archive, std::wstring_view member) {
    std::wstring normalized(member);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    std::wstring logical = archive.wstring();
    logical.push_back(L'|');
    logical.append(normalized);
    return std::filesystem::path(std::move(logical));
}

std::wstring DecodeZipMemberName(std::string_view encoded_name) {
    if (encoded_name.empty()) return {};
    if (encoded_name.size() > static_cast<size_t>(INT_MAX))
        throw std::runtime_error("ZIP member name is too long");
    const int size = static_cast<int>(encoded_name.size());
    const int count = MultiByteToWideChar(CP_OEMCP, 0, encoded_name.data(),
                                          size, nullptr, 0);
    if (count <= 0) throw std::runtime_error("invalid ZIP member name");
    std::wstring decoded(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_OEMCP, 0, encoded_name.data(), size,
                            decoded.data(), count) != count) {
        throw std::runtime_error("invalid ZIP member name");
    }
    std::replace(decoded.begin(), decoded.end(), L'/', L'\\');
    return decoded;
}

std::vector<unsigned char> ReadZipMember(const ZipMemberPath& path,
                                         HMODULE ttpcomm) {
    return ReadArchiveMember({path.archive,path.member,ArchiveKind::zip},ttpcomm);
}

bool ParseArchiveMemberPath(std::wstring_view logical_path,
                            ArchiveMemberPath& result) {
    const size_t marker=logical_path.find(L'|');
    ArchiveKind kind{};
    if(marker==std::wstring_view::npos || marker+1>=logical_path.size() ||
       !PathKind(logical_path.substr(0,marker),kind))return false;
    result.archive = std::filesystem::path(logical_path.substr(0, marker));
    result.member.assign(logical_path.substr(marker + 1));
    std::replace(result.member.begin(), result.member.end(), L'/', L'\\');
    result.kind = kind;
    return !result.archive.empty() && !result.member.empty();
}

bool IsArchivePath(const std::filesystem::path& path) {
    ArchiveKind kind{};return PathKind(path.native(),kind);
}

std::filesystem::path MakeArchiveMemberPath(
    const std::filesystem::path& archive, std::wstring_view member) {
    if (!IsArchivePath(archive))
        throw std::runtime_error("unsupported archive path");
    std::wstring logical = archive.wstring();
    logical.push_back(L'|');
    logical.append(NormalizeArchiveMember(member).native());
    return std::filesystem::path(std::move(logical));
}

IStream* OpenArchiveMemberStream(const ArchiveMemberPath& path, HMODULE ttpcomm,
                                 const std::function<bool()>& canceled) {
    // Seed known volume identity before a producer takes the RAR engine lock.
    // Cached CUE/directory lookups then need only filesystem stamp checks.
    static_cast<void>(CaptureArchiveCacheIdentity(path.archive,ttpcomm,canceled));
    return OpenMember(path,ttpcomm,canceled,true,0);
}

std::vector<unsigned char> ReadArchiveMember(const ArchiveMemberPath& path, HMODULE ttpcomm,
                                             const std::function<bool()>& canceled) {
    std::vector<unsigned char> cached;
    if(TryReadCachedArchiveMember(path,cached,nullptr,canceled))return cached;
    CheckCacheCancellation(canceled);
    auto identity=CaptureArchiveCacheIdentity(path.archive,ttpcomm,canceled);
    // Text/image reads must not evict the shared backing audio of adjacent CUE rows.
    constexpr uint64_t limit=16ULL*1024*1024;
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(OpenMember(path,ttpcomm,canceled,false,limit));
    STATSTG stat{}; HRESULT result=stream->Stat(&stat,STATFLAG_NONAME);
    if(FAILED(result))throw ArchiveError(result);
    if(stat.cbSize.QuadPart>limit) throw ArchiveError(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    std::vector<unsigned char> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read{}; result=stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read);
    if(FAILED(result) || read!=bytes.size()) throw ArchiveError(FAILED(result) ? result : STG_E_READFAULT);
    unsigned char end{};result=stream->Read(&end,1,&read); // Empty members also require final verification.
    if(FAILED(result) || read)throw ArchiveError(FAILED(result)?result:STG_E_READFAULT);
    CheckCacheCancellation(canceled);
    // Credential acceptance during extraction can advance the session epoch.
    // Refresh only after this small member has fully passed CRC/authentication.
    if(!ArchiveCacheIdentityFresh(identity,canceled)) {
        if(CacheIdentityFilesFresh(identity,canceled))identity=CaptureArchiveCacheIdentity(path.archive,ttpcomm,canceled);
        else identity.reset();
    }
    RememberSmallMember(identity,CacheMemberName(path.member),bytes,canceled);
    return bytes;
}

int ArchiveMemberProgress(IStream* stream,HMODULE ttpcomm,HRESULT* result) {
    if(result)*result=S_OK;if(!stream)return -1;
    try {
        TtpCommArchiveStreamStatus status{};status.size=sizeof(status);
        const auto hr=ArchiveApi(ttpcomm).status(stream,&status);
        if(FAILED(hr)){if(result)*result=hr;return -1;}
        if(result && status.complete)*result=status.result;
        if(status.complete)return SUCCEEDED(status.result)?100:-1;
        return status.total_bytes?static_cast<int>(std::min<uint64_t>(99,status.completed_bytes*100/status.total_bytes)):0;
    }catch(const ArchiveError& error){if(result)*result=error.Result();return -1;}
}

std::vector<unsigned char> ReadArchiveSidecarCover(const std::filesystem::path& media,
    HMODULE module, const std::function<bool()>& canceled) {
    ArchiveMemberPath member;
    if (!ParseArchiveMemberPath(media.native(),member)) return {};
    try {
        if (IsOszArchive(member.archive)) {
            return ReadOszCover(member.archive,module,canceled,member.member);
        }
        const auto names = ListArchiveMembers(member.archive,module,canceled);
        const std::filesystem::path file(member.member);
        for (const auto& stem : {file.stem().native(),std::wstring(L"cover"),std::wstring(L"folder"),std::wstring(L"front")}) {
            for (const auto* extension : {L".png",L".jpg",L".jpeg",L".bmp",L".gif"}) {
                if (canceled && canceled()) return {};
                const auto wanted = (file.parent_path() / (stem + extension)).native();
                const auto found = std::find_if(names.begin(),names.end(),[&](const auto& n){return !_wcsicmp(n.c_str(),wanted.c_str());});
                if (found == names.end()) continue;
                try {
                    auto bytes = ReadArchiveMember({member.archive,*found,member.kind},module,canceled);
                    if (HasCoverSignature(bytes)) return bytes;
                } catch (...) { if(canceled && canceled())return {}; }
            }
        }
    } catch (...) {}
    return {};
}

std::vector<std::wstring> ListArchiveMembers(const std::filesystem::path& archive, HMODULE ttpcomm,
    const std::function<bool()>& canceled, const std::function<void(uint64_t,uint64_t)>& progress,
    std::filesystem::path* resolved_archive) {
    CheckCacheCancellation(canceled);
    std::vector<std::wstring> cached;ArchiveCacheIdentity identity;
    const auto source=CacheArchivePath(archive);
    if(FindCachedDirectory(source,cached,identity,canceled)){
        if(progress)progress(cached.size(),cached.size());CheckCacheCancellation(canceled);
        if(resolved_archive){AliasArchiveSession(archive,identity->first);*resolved_archive=identity->first;}
        return cached;
    }
    identity=CaptureArchiveCacheIdentity(archive,ttpcomm,canceled);
    std::filesystem::path resolved=archive;
    struct Context {
        std::vector<std::wstring> names; std::exception_ptr error;
        const std::function<bool()>& canceled;
        const std::function<void(uint64_t,uint64_t)>& progress;
    } context{{},{},canceled,progress};
    const auto visitor=[](void* opaque,const TtpCommArchiveEntry* entry)->BOOL {
        auto& out=*static_cast<Context*>(opaque);
        try {
            if(out.canceled && out.canceled())return FALSE;
            if(!(entry->flags&(TTPCOMM_ARCHIVE_DIRECTORY|TTPCOMM_ARCHIVE_LINK|TTPCOMM_ARCHIVE_SPARSE))) out.names.emplace_back(entry->name);
            return TRUE;
        } catch(...) { out.error=std::current_exception(); return FALSE; }
    };
    const auto api=ArchiveApi(ttpcomm);
    const auto group=GroupApi(ttpcomm);auto volumes=ArchiveVolumeOptions(&canceled);
    const HRESULT result=WithArchivePassword(archive,[&](const wchar_t* password) {
        context.names.clear();context.error={};
        TtpCommArchiveOptions options{};options.size=sizeof(options);options.password=password;
        options.context=&context;
        options.progress=[](void* opaque,uint64_t completed,uint64_t total)->BOOL {
            auto& c=*static_cast<Context*>(opaque);
            try {
                if(c.canceled && c.canceled())return FALSE;
                if(c.progress)c.progress(completed,total);
                return TRUE;
            }catch(...){c.error=std::current_exception();return FALSE;}
        };
        if(resolved_archive && group.volumes && ttpcomm::archive_format::Detect(archive.c_str())==ttpcomm::archive_format::Kind::rar){
            std::vector<std::wstring> actual;
            const auto hr=group.volumes(archive.c_str(),&options,&volumes,[](void* p,const wchar_t* name)->BOOL{
                try{static_cast<std::vector<std::wstring>*>(p)->emplace_back(name);return TRUE;}catch(...){return FALSE;}
            },&actual);
            if(FAILED(hr))return hr;if(!actual.empty())resolved=actual.front();
        }
        return group.enumerate?group.enumerate(archive.c_str(),&options,&volumes,visitor,&context)
            :api.enumerate(archive.c_str(),&options,visitor,&context);
    },canceled);
    if(context.error) std::rethrow_exception(context.error);
    if(FAILED(result)){if(result!=TTPCOMM_ARCHIVE_PASSWORD && result!=TTPCOMM_ARCHIVE_BAD_PASSWORD)DiscardArchiveVolumes(archive);throw ArchiveError(result);}
    if(resolved_archive){AliasArchiveSession(archive,resolved);*resolved_archive=std::move(resolved);}
    CheckCacheCancellation(canceled);
    if(!ArchiveCacheIdentityFresh(identity,canceled)) {
        if(CacheIdentityFilesFresh(identity,canceled))identity=CaptureArchiveCacheIdentity(archive,ttpcomm,canceled);
        else identity.reset();
    }
    RememberDirectory(identity,context.names,canceled);
    // 00473338 and 00474051 retain the archive's order. Never alphabetize here.
    return std::move(context.names);
}

std::vector<std::wstring> ListRarArchiveMembers(const std::filesystem::path& archive, HMODULE ttpcomm) {
    return ListArchiveMembers(archive,ttpcomm);
}

} // namespace ttplayer::audio
