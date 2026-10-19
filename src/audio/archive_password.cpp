#include "ttplayer/audio/archive_password.h"
#include <ttpcomm/archive_api.h>
#include <ttpcomm/archive_volumes.h>
#include <ttpcomm/runtime_client.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>

namespace ttplayer::audio {
namespace {
using Bytes=std::vector<unsigned char>;
constexpr size_t kMaximumFile=2*1024*1024,kMaximumRecords=256;
struct Record {std::wstring path,name,md5,sha,password;};
std::mutex mutex;
std::atomic<uint64_t> session_generation{1};
std::filesystem::path storage;
ArchivePasswordPrompt prompt;
std::function<void(HRESULT)> storage_error;
std::vector<Record> remembered,session;
struct VolumeMapping {std::wstring archive,expected,actual;};
std::vector<VolumeMapping> volume_mappings;
ArchiveVolumePrompt volume_prompt;
thread_local unsigned background_reads{};
struct CachedIdentity {Record record;std::wstring stamp;};
std::vector<CachedIdentity> fingerprints;
HRESULT storage_result=S_OK;
void Wipe(std::wstring& text){if(!text.empty())SecureZeroMemory(text.data(),text.size()*sizeof(wchar_t));text.clear();}
std::filesystem::path Outer(const std::filesystem::path& p){return std::filesystem::path(p.native().substr(0,p.native().find(L'|')));}
std::wstring Key(const std::filesystem::path& input){const std::filesystem::path p=ttpcomm::archive_format::Canonical(input.c_str());std::error_code e;auto full=std::filesystem::absolute(p,e);return (e?p:full).lexically_normal().native();}
bool Same(std::wstring_view a,std::wstring_view b){return a.size()==b.size() && !_wcsnicmp(a.data(),b.data(),a.size());}
std::wstring Hex(const unsigned char* p,size_t n){constexpr wchar_t digits[]=L"0123456789abcdef";std::wstring s;s.reserve(n*2);for(size_t i=0;i<n;++i){s+=digits[p[i]>>4];s+=digits[p[i]&15];}return s;}
Bytes Unhex(std::wstring_view s){
    if(s.size()%2 || s.size()>kMaximumFile*2)return {};Bytes out;out.reserve(s.size()/2);
    const auto digit=[](wchar_t c){return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;};
    for(size_t i=0;i<s.size();i+=2){const int a=digit(s[i]),b=digit(s[i+1]);if(a<0 || b<0)return {};out.push_back(static_cast<unsigned char>(a*16+b));}return out;
}
std::wstring Protect(std::wstring_view text){
    DATA_BLOB in{static_cast<DWORD>(text.size()*sizeof(wchar_t)),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(text.data()))},out{};
    if(!CryptProtectData(&in,L"TTPlayer archive password",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return {};
    auto result=Hex(out.pbData,out.cbData);SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);return result;
}
std::optional<std::wstring> Unprotect(std::wstring_view encoded){
    auto bytes=Unhex(encoded);if(bytes.empty())return {};DATA_BLOB in{static_cast<DWORD>(bytes.size()),bytes.data()},out{};
    if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return {};
    std::optional<std::wstring> result;
    if(out.cbData%2==0 && out.cbData<=kMaximumFile)result=std::wstring(reinterpret_cast<wchar_t*>(out.pbData),out.cbData/2);
    SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);return result;
}
void Put(Bytes& b,std::wstring_view value){uint32_t n=static_cast<uint32_t>(value.size());const auto* count=reinterpret_cast<const unsigned char*>(&n);b.insert(b.end(),count,count+4);
    const auto* p=reinterpret_cast<const unsigned char*>(value.data());b.insert(b.end(),p,p+n*2);}
bool Get(const Bytes& b,size_t& at,std::wstring& value){
    if(at>b.size() || b.size()-at<4)return false;uint32_t n{};memcpy(&n,b.data()+at,4);at+=4;
    if(n>65536 || n>(b.size()-at)/2)return false;value.resize(n);memcpy(value.data(),b.data()+at,n*2);at+=n*2;return true;
}
void Load(){
    remembered.clear();if(storage.empty())return;
    std::ifstream file(storage,std::ios::binary|std::ios::ate);if(!file)return;const auto n=file.tellg();if(n<4 || n>kMaximumFile)return;
    Bytes bytes(static_cast<size_t>(n));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),n);if(!file)return;
    size_t at=0;std::wstring magic;if(!Get(bytes,at,magic) || magic!=L"TTPlayerArchivePasswords/1")return;
    std::vector<Record> records;
    while(at<bytes.size() && records.size()<kMaximumRecords){Record r;
        if(!Get(bytes,at,r.name) || !Get(bytes,at,r.md5) || !Get(bytes,at,r.sha) || !Get(bytes,at,r.password) || r.name.empty() || r.md5.size()!=32 || r.sha.size()!=64 || r.password.empty())return;
        records.push_back(std::move(r));}
    if(at==bytes.size())remembered=std::move(records);
}
HRESULT Save(){
    if(storage.empty())return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    Bytes bytes;Put(bytes,L"TTPlayerArchivePasswords/1");for(const auto& r:remembered){Put(bytes,r.name);Put(bytes,r.md5);Put(bytes,r.sha);Put(bytes,r.password);}
    if(bytes.size()>kMaximumFile)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    const auto temp=storage.wstring()+L".tmp-"+std::to_wstring(GetCurrentProcessId());
    HANDLE f=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());DWORD n{};
    bool okay=WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr) && n==bytes.size() && FlushFileBuffers(f);
    DWORD error=GetLastError();CloseHandle(f);
    if(okay){okay=MoveFileExW(temp.c_str(),storage.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;error=GetLastError();}
    if(!okay)DeleteFileW(temp.c_str());return okay?S_OK:HRESULT_FROM_WIN32(error?error:ERROR_WRITE_FAULT);
}
Record Fingerprint(const std::filesystem::path& path,const std::function<bool()>& canceled,const wchar_t* password=nullptr){
    Record r;r.path=Key(path);r.name=std::filesystem::path(r.path).filename().native();
    std::unique_ptr<ttpcomm::archive_volumes::Set> files;
    try{
        if(const auto* api=ttpcomm::host::ArchiveGroups()){
            TtpCommArchiveOptions limits{};limits.size=sizeof(limits);limits.password=password;
            limits.context=const_cast<std::function<bool()>*>(&canceled);
            limits.progress=[](void* p,uint64_t,uint64_t)->BOOL{const auto& c=*static_cast<std::function<bool()>*>(p);return !c || !c();};
            auto options=ArchiveVolumeOptions(&canceled);std::vector<std::wstring> names;
            const auto hr=api->volumes(path.c_str(),&limits,&options,[](void* p,const wchar_t* name)->BOOL{
                try{static_cast<std::vector<std::wstring>*>(p)->emplace_back(name);return TRUE;}catch(...){return FALSE;}
            },&names);
            if(FAILED(hr))return r;
            files=std::make_unique<ttpcomm::archive_volumes::Set>(r.path,names,canceled);
        }else files=std::make_unique<ttpcomm::archive_volumes::Set>(path.c_str(),canceled);
    }catch(...){return r;}
    const auto& stamp=files->Key();
    {
        std::lock_guard lock(mutex);
        for(const auto& cached:fingerprints){
            if(files->Reliable() && Same(cached.record.path,r.path) && cached.stamp==stamp){
                r.md5=cached.record.md5;r.sha=cached.record.sha;return r;
            }
        }
    }
    HCRYPTPROV provider{};HCRYPTHASH md5{},sha{};
    struct End{HCRYPTPROV& p;HCRYPTHASH& a;HCRYPTHASH& b;~End(){if(a)CryptDestroyHash(a);if(b)CryptDestroyHash(b);if(p)CryptReleaseContext(p,0);}}end{provider,md5,sha};
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) ||
       !CryptCreateHash(provider,CALG_MD5,0,0,&md5) || !CryptCreateHash(provider,CALG_SHA_256,0,0,&sha))return r;
    unsigned char buffer[65536];
    for(uint64_t at=0;at<files->Length();){if(canceled && canceled())return r;
        const auto read=static_cast<DWORD>(std::min<uint64_t>(sizeof(buffer),files->Length()-at));
        try{files->Read(at,buffer,read);}catch(...){return r;}
        if(!CryptHashData(md5,buffer,read,0) || !CryptHashData(sha,buffer,read,0))return r;at+=read;}
    unsigned char m[16],s[32];DWORD mn=16,sn=32;
    if(CryptGetHashParam(md5,HP_HASHVAL,m,&mn,0) && CryptGetHashParam(sha,HP_HASHVAL,s,&sn,0)){
        r.md5=Hex(m,16);r.sha=Hex(s,32);
        {std::lock_guard lock(mutex);std::erase_if(fingerprints,[&](const auto& old){return Same(old.record.path,r.path);});
            fingerprints.push_back({r,stamp});if(fingerprints.size()>64)fingerprints.erase(fingerprints.begin());}
    }return r;
}
bool PasswordFailure(HRESULT hr){return hr==TTPCOMM_ARCHIVE_PASSWORD || hr==TTPCOMM_ARCHIVE_BAD_PASSWORD;}
std::vector<std::wstring> Candidates(const Record& identity){
    std::vector<std::wstring> out;std::lock_guard lock(mutex);
    const auto add=[&](const Record& r){if(Same(r.name,identity.name) || (!identity.md5.empty() && r.md5==identity.md5 && r.sha==identity.sha))
        if(auto pw=Unprotect(r.password);pw && std::find(out.begin(),out.end(),*pw)==out.end())out.push_back(std::move(*pw));};
    for(auto i=session.rbegin();i!=session.rend();++i)add(*i);
    for(auto i=remembered.rbegin();i!=remembered.rend();++i)add(*i);return out;
}
void Accept(Record identity,const std::wstring& password,bool remember){
    identity.password=Protect(password);
    const auto protection_error=GetLastError();HRESULT result=S_OK;std::function<void(HRESULT)> report;
    {
        std::lock_guard lock(mutex);
        if(identity.password.empty())result=HRESULT_FROM_WIN32(protection_error?protection_error:ERROR_ENCRYPTION_FAILED);
        else {
            bool already_current=false;
            if(!session.empty() && Same(session.back().path,identity.path)) {
                auto old=Unprotect(session.back().password);already_current=old && *old==password;if(old)Wipe(*old);
            }
            if(!already_current)++session_generation;
            std::erase_if(session,[&](const Record& r){if(!Same(r.path,identity.path))return false;
                auto old=Unprotect(r.password);const bool same=old && *old==password;if(old)Wipe(*old);return same;});
            session.push_back(identity);if(session.size()>kMaximumRecords)session.erase(session.begin());
            if(remember){
                if(identity.md5.empty() || identity.sha.empty())result=HRESULT_FROM_WIN32(ERROR_READ_FAULT);
                else {
                    std::erase_if(remembered,[&](const Record& r){return Same(r.name,identity.name) && r.md5==identity.md5 && r.sha==identity.sha;});
                    remembered.push_back(std::move(identity));if(remembered.size()>kMaximumRecords)remembered.erase(remembered.begin());result=Save();
                }
            }
        }
        if(remember || FAILED(result))storage_result=result;
        if(remember && FAILED(result))report=storage_error;
    }
    if(report)report(result);
}
}
ScopedArchiveBackgroundRead::ScopedArchiveBackgroundRead() noexcept { ++background_reads; }
ScopedArchiveBackgroundRead::~ScopedArchiveBackgroundRead() { --background_reads; }
void ConfigureArchivePasswords(const std::filesystem::path& path,ArchivePasswordPrompt callback,std::function<void(HRESULT)> error){
    std::lock_guard lock(mutex);storage=path;prompt=std::move(callback);storage_error=std::move(error);
    session.clear();fingerprints.clear();++session_generation;storage_result=S_OK;Load();
}
void ConfigureArchiveVolumes(ArchiveVolumePrompt callback){std::lock_guard lock(mutex);volume_prompt=std::move(callback);volume_mappings.clear();++session_generation;}
void DiscardArchiveVolumes(const std::filesystem::path& archive){const auto key=Key(archive);std::lock_guard lock(mutex);
    std::erase_if(volume_mappings,[&](const auto& v){return Same(v.archive,key);});++session_generation;
}
void AliasArchiveSession(const std::filesystem::path& before,const std::filesystem::path& after){
    const auto a=Key(before),b=Key(after);if(Same(a,b))return;std::lock_guard lock(mutex);bool changed=false;
    auto mapped=volume_mappings;for(auto& v:mapped)if(Same(v.archive,a)){
        if(std::none_of(volume_mappings.begin(),volume_mappings.end(),[&](const auto& old){return Same(old.archive,b) && Same(old.expected,v.expected) && Same(old.actual,v.actual);})){v.archive=b;volume_mappings.push_back(std::move(v));changed=true;}}
    if(volume_mappings.size()>4096)volume_mappings.erase(volume_mappings.begin(),volume_mappings.end()-4096);
    auto passwords=session;for(auto& r:passwords)if(Same(r.path,a)){
        bool exists=false;auto candidate=Unprotect(r.password);
        for(const auto& old:session)if(Same(old.path,b)){auto pw=Unprotect(old.password);exists=pw && candidate && *pw==*candidate;if(pw)Wipe(*pw);if(exists)break;}
        if(candidate)Wipe(*candidate);
        if(!exists){r.path=b;r.name=std::filesystem::path(b).filename().native();session.push_back(std::move(r));changed=true;}}
    if(session.size()>kMaximumRecords)session.erase(session.begin(),session.end()-kMaximumRecords);
    if(changed)++session_generation;
}
TtpCommArchiveVolumeOptions ArchiveVolumeOptions(const std::function<bool()>* canceled){
    TtpCommArchiveVolumeOptions options{};options.size=sizeof(options);options.context=const_cast<std::function<bool()>*>(canceled);
    options.resolve=[](void* opaque,const wchar_t* archive,const wchar_t* expected,wchar_t* replacement,uint32_t capacity)->HRESULT{
        try{
            const std::function<bool()> empty;const auto& stop=opaque?*static_cast<std::function<bool()>*>(opaque):empty;
            if(stop && stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            const auto key=Key(archive);std::filesystem::path actual;ArchiveVolumePrompt ask;
            const auto exists=[](const auto& p){const auto a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY);};
            {
                std::lock_guard lock(mutex);ask=volume_prompt;
                for(auto i=volume_mappings.rbegin();i!=volume_mappings.rend();++i)if(Same(i->archive,key)){
                    if(Same(i->expected,expected) && exists(std::filesystem::path(i->actual))){actual=i->actual;break;}
                    // Selecting one missing volume supplies the directory for
                    // its conventionally named siblings, including in workers.
                    const auto sibling=std::filesystem::path(i->actual).parent_path()/std::filesystem::path(expected).filename();
                    if(exists(sibling)){actual=sibling;break;}
                }
            }
            if(actual.empty()){
                if(!ask || background_reads)return S_FALSE;
                const auto selected=ask(archive,expected,stop);if(!selected)return HRESULT_FROM_WIN32(ERROR_CANCELLED);actual=*selected;
            }
            if(stop && stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if(!exists(actual))return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            if(actual.native().size()>=capacity)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            {
                std::lock_guard lock(mutex);
                const bool unchanged=std::any_of(volume_mappings.begin(),volume_mappings.end(),[&](const auto& v){
                    return Same(v.archive,key) && Same(v.expected,expected) && Same(v.actual,actual.native());});
                if(!unchanged)++session_generation;
                std::erase_if(volume_mappings,[&](const auto& v){return Same(v.archive,key) && Same(v.expected,expected);});
                volume_mappings.push_back({key,expected,actual.native()});
                if(volume_mappings.size()>4096)volume_mappings.erase(volume_mappings.begin());
            }
            wcscpy_s(replacement,capacity,actual.c_str());return S_OK;
        }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
    };return options;
}
uint64_t ArchiveSessionGeneration() noexcept { return session_generation.load(); }
std::vector<ArchiveVolumeMappingIdentity> ArchiveSessionVolumeMappings(const std::filesystem::path& archive) {
    const auto key=Key(archive);std::vector<ArchiveVolumeMappingIdentity> out;std::lock_guard lock(mutex);
    for(const auto& v:volume_mappings)if(Same(v.archive,key))out.push_back({v.expected,v.actual});
    return out;
}
HRESULT ArchivePasswordStorageResult(){std::lock_guard lock(mutex);return storage_result;}
HRESULT WithArchivePassword(const std::filesystem::path& path,const std::function<HRESULT(const wchar_t*)>& operation,const std::function<bool()>& canceled){
    const auto stop=[&]{return canceled && canceled();};if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    // Exact in-process candidates avoid rehashing a large album per CUE track.
    std::vector<std::wstring> tried;const auto key=Key(path);
    {std::lock_guard lock(mutex);for(auto i=session.rbegin();i!=session.rend();++i)if(Same(i->path,key))if(auto pw=Unprotect(i->password))tried.push_back(std::move(*pw));}
    struct Wiper{std::vector<std::wstring>& p;~Wiper(){for(auto& s:p)Wipe(s);}}wipe{tried};
    for(const auto& pw:tried){if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);const auto hr=operation(pw.c_str());if(!PasswordFailure(hr))return hr;}
    HRESULT hr=operation(nullptr);if(!PasswordFailure(hr))return hr;
    // Filename candidates can be verified immediately. Hash a potentially huge
    // archive only for a renamed saved record or an explicit Remember request.
    Record identity;identity.path=key;identity.name=std::filesystem::path(key).filename().native();
    auto candidates=Candidates(identity);Wiper wipe_candidates{candidates};
    for(const auto& pw:candidates){if(std::find(tried.begin(),tried.end(),pw)!=tried.end())continue;if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        hr=operation(pw.c_str());if(SUCCEEDED(hr)){Accept(identity,pw,false);return hr;}if(!PasswordFailure(hr))return hr;tried.push_back(pw);}
    bool hashes_needed=false;
    {std::lock_guard lock(mutex);for(const auto& r:remembered)if(!Same(r.name,identity.name)){hashes_needed=true;break;}}
    if(hashes_needed){
        identity=Fingerprint(path,canceled);if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        // Encrypted RAR headers hide the volume chain itself. A saved password
        // may be used to discover that chain, but is accepted only after both
        // full-group hashes match and the requested member passes validation.
        if(identity.md5.empty() && ttpcomm::archive_format::Detect(path.c_str())==ttpcomm::archive_format::Kind::rar){
            std::vector<std::wstring> header_candidates;
            {std::lock_guard lock(mutex);for(const auto& r:remembered)if(auto pw=Unprotect(r.password))
                if(std::find(header_candidates.begin(),header_candidates.end(),*pw)==header_candidates.end())header_candidates.push_back(std::move(*pw));}
            Wiper wipe_headers{header_candidates};
            for(const auto& pw:header_candidates){
                if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                auto candidate=Fingerprint(path,canceled,pw.c_str());
                if(!candidate.md5.empty()){identity=std::move(candidate);break;}
            }
        }
        auto renamed=Candidates(identity);Wiper wipe_renamed{renamed};
        for(const auto& pw:renamed){if(std::find(tried.begin(),tried.end(),pw)!=tried.end())continue;
            if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            hr=operation(pw.c_str());if(SUCCEEDED(hr)){Accept(identity,pw,false);return hr;}
            if(!PasswordFailure(hr))return hr;tried.push_back(pw);}
    }
    ArchivePasswordPrompt ask;{std::lock_guard lock(mutex);ask=prompt;}
    if(!ask || background_reads)return hr;
    for(;;){if(stop())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        // A simultaneous importer/decoder may already have verified a password
        // while this request was reading headers or trying saved candidates.
        auto recent=Candidates(identity);Wiper wipe_recent{recent};
        for(const auto& pw:recent){if(std::find(tried.begin(),tried.end(),pw)!=tried.end())continue;
            hr=operation(pw.c_str());if(!PasswordFailure(hr))return hr;tried.push_back(pw);}
        auto answer=ask(path,!tried.empty(),canceled);if(!answer)return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        hr=operation(answer->password.c_str());
        if(SUCCEEDED(hr)){
            if(answer->remember && identity.md5.empty())identity=Fingerprint(path,canceled,answer->password.c_str());
            if(stop()){Wipe(answer->password);return HRESULT_FROM_WIN32(ERROR_CANCELLED);}
            Accept(identity,answer->password,answer->remember);
        }
        tried.push_back(std::move(answer->password));
        if(!PasswordFailure(hr))return hr;
    }
}
std::wstring ExportArchivePasswordPacket(const std::filesystem::path& logical,const std::function<bool()>& canceled){
    if(canceled && canceled())return {};
    if(logical.native().find(L'|')==std::wstring::npos)return {};
    const auto key=Key(Outer(logical));std::vector<std::wstring> passwords;bool saved{};
    {std::lock_guard lock(mutex);saved=!remembered.empty();for(auto i=session.rbegin();i!=session.rend() && passwords.size()<8;++i)if(Same(i->path,key))
        if(auto pw=Unprotect(i->password);pw && std::find(passwords.begin(),passwords.end(),*pw)==passwords.end())passwords.push_back(std::move(*pw));}
    // Saved candidates are also needed by metadata workers after restart.
    if(passwords.empty() && saved){auto identity=Fingerprint(Outer(logical),canceled);passwords=Candidates(identity);}
    if(canceled && canceled()){for(auto& pw:passwords)Wipe(pw);return {};}
    std::vector<VolumeMapping> volumes;
    {std::lock_guard lock(mutex);for(const auto& v:volume_mappings)if(Same(v.archive,key))volumes.push_back(v);}
    if(passwords.empty() && volumes.empty())return {};
    Bytes packet;Put(packet,L"TTPlayerArchiveSession/2");Put(packet,key);Put(packet,std::to_wstring(std::min<size_t>(8,passwords.size())));
    for(size_t i=0;i<passwords.size() && i<8;++i)Put(packet,passwords[i]);
    // Keep the existing bounded worker packet. Typical volume directories need
    // one mapping; arbitrary renamed volumes are included up to this limit.
    for(const auto& v:volumes){if(packet.size()+(v.expected.size()+v.actual.size())*2+8>240*1024)break;Put(packet,v.expected);Put(packet,v.actual);}
    auto encoded=Hex(packet.data(),packet.size());auto blob=Protect(encoded);Wipe(encoded);SecureZeroMemory(packet.data(),packet.size());
    for(auto& pw:passwords)Wipe(pw);return blob;
}
void ImportArchivePasswordPacket(const std::filesystem::path& logical,std::wstring_view packet){
    std::vector<Record> next;std::vector<VolumeMapping> decoded_volumes;
    std::wstring accepted_key;bool replace_volumes=false;
    if(logical.native().find(L'|')!=std::wstring::npos && !packet.empty())if(auto decoded=Unprotect(packet)) {
        auto bytes=Unhex(*decoded);Wipe(*decoded);size_t at=0;std::wstring key,count;unsigned passwords=8;
        bool header=Get(bytes,at,key);const bool version2=key==L"TTPlayerArchiveSession/2";
        if(version2){header=Get(bytes,at,key) && Get(bytes,at,count) && count.size()==1 && count[0]>=L'0' && count[0]<=L'8';if(header)passwords=count[0]-L'0';}
        if(header && Same(key,Key(Outer(logical)))) {
            for(unsigned i=0;i<passwords && (version2 || at<bytes.size());++i){std::wstring pw;if(!Get(bytes,at,pw)){header=false;break;}
                Record r;r.path=key;r.name=Outer(logical).filename().native();r.password=Protect(pw);Wipe(pw);
                if(r.password.empty()){header=false;break;}next.push_back(std::move(r));}
            if(header && version2)while(at<bytes.size() && decoded_volumes.size()<4096){std::wstring expected,actual;
                if(!Get(bytes,at,expected) || !Get(bytes,at,actual) || !std::filesystem::path(expected).is_absolute() || !std::filesystem::path(actual).is_absolute()){header=false;break;}
                decoded_volumes.push_back({key,std::move(expected),std::move(actual)});
            }
            if(header && at==bytes.size()){accepted_key=key;replace_volumes=version2;}else next.clear();
        }
        SecureZeroMemory(bytes.data(),bytes.size());
    }
    std::lock_guard lock(mutex);
    bool same=session.size()==next.size();
    for(size_t i=0;same && i<next.size();++i){
        auto a=Unprotect(session[i].password),b=Unprotect(next[i].password);
        same=Same(session[i].path,next[i].path) && a && b && *a==*b;
        if(a)Wipe(*a);if(b)Wipe(*b);
    }
    if(!same){session=std::move(next);++session_generation;}
    if(replace_volumes){
        std::vector<VolumeMapping> previous;for(const auto& v:volume_mappings)if(Same(v.archive,accepted_key))previous.push_back(v);
        bool equal=previous.size()==decoded_volumes.size();
        for(size_t i=0;equal && i<previous.size();++i)equal=Same(previous[i].expected,decoded_volumes[i].expected) && Same(previous[i].actual,decoded_volumes[i].actual);
        if(!equal){std::erase_if(volume_mappings,[&](const auto& v){return Same(v.archive,accepted_key);});
            volume_mappings.insert(volume_mappings.end(),decoded_volumes.begin(),decoded_volumes.end());++session_generation;}
    }
}
void RenameArchivePasswordSession(const std::filesystem::path& before,const std::filesystem::path& after){
    const auto a=Key(before),b=Key(after);std::function<void(HRESULT)> report;HRESULT error=S_OK;
    {
        std::lock_guard lock(mutex);bool changed=false;++session_generation;
        for(auto& r:session)if(Same(r.path,a)){
            for(auto& saved:remembered)if(!r.md5.empty() && r.md5==saved.md5 && r.sha==saved.sha && Same(saved.name,r.name)){
                saved.name=after.filename().native();changed=true;
            }
            r.path=b;r.name=after.filename().native();
        }
        std::erase_if(fingerprints,[&](const auto& cached){return Same(cached.record.path,a) || Same(cached.record.path,b);});
        if(changed){storage_result=error=Save();if(FAILED(error))report=storage_error;}
    }
    if(report)report(error);
}
}
