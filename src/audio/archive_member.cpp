#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/archive_password.h"
#include "ttplayer/audio/osz_archive.h"
#include <ttpcomm/archive_format.h>

#include <ttpcomm/runtime_client.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <exception>
#include <wrl/client.h>
#include <atomic>
#include <memory>

namespace ttplayer::audio {
namespace {

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

ArchiveError::ArchiveError(HRESULT result)
    : std::runtime_error("Archive operation failed: " + std::to_string(static_cast<unsigned long>(result))),result_(result) {}

std::wstring ArchiveError::Message() const {
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
    return OpenMember(path,ttpcomm,canceled,true,0);
}

std::vector<unsigned char> ReadArchiveMember(const ArchiveMemberPath& path, HMODULE ttpcomm,
                                             const std::function<bool()>& canceled) {
    // Text reads must not evict the shared backing audio of adjacent CUE rows.
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
        const auto names = ListArchiveMembers(member.archive,module,canceled);
        if (IsOszArchive(member.archive)) {
            const auto resolved = ResolveOszMedia(member.archive,names,module,canceled,{},member.member);
            if (resolved.empty() || resolved.front().background.empty()) return {};
            auto bytes = ReadArchiveMember({member.archive,resolved.front().background,member.kind},module,canceled);
            return HasCoverSignature(bytes) ? std::move(bytes) : std::vector<unsigned char>{};
        }
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
    // 00473338 and 00474051 retain the archive's order. Never alphabetize here.
    return std::move(context.names);
}

std::vector<std::wstring> ListRarArchiveMembers(const std::filesystem::path& archive, HMODULE ttpcomm) {
    return ListArchiveMembers(archive,ttpcomm);
}

} // namespace ttplayer::audio
