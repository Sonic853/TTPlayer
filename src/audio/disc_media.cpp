#include "ttplayer/audio/disc_media.h"
#include "ttplayer/core/text.h"
#include <winioctl.h>
#include <wincrypt.h>
#include <msxml6.h>
#include <wrl/client.h>
#include <comutil.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

namespace ttplayer::audio {
namespace {
using Microsoft::WRL::ComPtr;
[[noreturn]] void Error(DWORD code = GetLastError()) {
    throw std::system_error(code ? code : ERROR_INVALID_DATA, std::system_category(), "CD audio");
}
struct Handle { HANDLE value{INVALID_HANDLE_VALUE}; ~Handle() { if(value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct Com { HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)}; ~Com() { if(SUCCEEDED(hr)) CoUninitialize(); } };
unsigned Address(const UCHAR* a) {
    if(a[0] || a[2] >= 60 || a[3] >= 75) Error(ERROR_INVALID_DATA);
    const unsigned n = (unsigned(a[1])*60+a[2])*75+a[3];
    if(n < 150) Error(ERROR_INVALID_DATA);
    return n;
}
std::wstring Root(const std::filesystem::path& path) {
    auto root=path.root_name().wstring();
    if(root.size()!=2 || root[1]!=L':' || !iswalpha(root[0])) Error(ERROR_INVALID_NAME);
    root[0]=towupper(root[0]); return root+L"\\";
}
std::filesystem::path CachePath(const DiscLayout& disc, const std::filesystem::path& directory) {
    if(!disc.serial) Error(ERROR_NOT_READY);
    return (directory.empty() ? DiscCacheDirectory() : directory)/(std::to_wstring(disc.serial)+L".cddb");
}
std::wstring Attribute(IXMLDOMElement* node, const wchar_t* key) {
    _variant_t value;
    if(FAILED(node->getAttribute(_bstr_t(key), &value)) || value.vt!=VT_BSTR || !value.bstrVal) return {};
    return value.bstrVal;
}
void Set(CueMetadata& fields, const std::wstring& key, const std::wstring& value) {
    const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& f){return _wcsicmp(f.first.c_str(),key.c_str())==0;});
    if(found!=fields.end()) { if(value.empty()) fields.erase(found); else found->second=value; }
    else if(!value.empty()) fields.emplace_back(key,value);
}
std::wstring Escape(std::wstring_view value) {
    std::wstring out;
    for(wchar_t c:value) {
        if(c<32 && c!=9 && c!=10 && c!=13) Error(ERROR_INVALID_DATA);
        switch(c) { case L'&':out+=L"&amp;";break;case L'<':out+=L"&lt;";break;
        case L'"':out+=L"&quot;";break;case L'\r':out+=L"&#13;";break;
        case L'\n':out+=L"&#10;";break;case L'\t':out+=L"&#9;";break;default:out+=c; }
    }
    return out;
}
}
std::wstring DiscLayout::Fingerprint() const {
    std::wstring result=std::to_wstring(serial)+(complete?L":full":L":partial");
    for(const auto& track:tracks)result+=L"/"+std::to_wstring(track.number)+L":"+
        std::to_wstring(track.start)+L":"+std::to_wstring(track.end)+(track.data?L":data":L":audio");
    return result;
}
bool DiscLayout::IsAudioDisc() const noexcept {
    if(!complete || tracks.empty() || tracks.size()>99) return false;
    unsigned previous=0,number=tracks.front().number;
    for(const auto& t:tracks) {
        if(t.data || t.number!=number++ || t.number>99 || t.start<150 ||
           t.end<=t.start || (previous && previous!=t.start)) return false;
        previous=t.end;
    }
    return tracks.front().number>0;
}
std::string DiscLayout::MusicBrainzId() const {
    if(!IsAudioDisc()) Error(ERROR_NOT_SUPPORTED); // Mixed-session lead-out needs full TOC.
    char hex[16]{};
    sprintf_s(hex,"%02X%02X",tracks.front().number,tracks.back().number);
    std::string text=hex;
    sprintf_s(hex,"%08X",tracks.back().end);text+=hex;
    for(unsigned n=1;n<=99;++n) {
        unsigned frame=0;
        for(const auto& t:tracks) if(t.number==n) frame=t.start;
        sprintf_s(hex,"%08X",frame);text+=hex;
    }
    struct Crypto { HCRYPTPROV provider{};HCRYPTHASH hash{};
        ~Crypto(){if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);} } crypto;
    if(!CryptAcquireContextW(&crypto.provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT) ||
       !CryptCreateHash(crypto.provider,CALG_SHA1,0,0,&crypto.hash) ||
       !CryptHashData(crypto.hash,reinterpret_cast<const BYTE*>(text.data()),static_cast<DWORD>(text.size()),0)) Error();
    BYTE digest[20]{};DWORD size=sizeof(digest);
    if(!CryptGetHashParam(crypto.hash,HP_HASHVAL,digest,&size,0) || size!=20) Error();
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    std::string result;
    for(size_t i=0;i<20;i+=3) {
        unsigned v=unsigned(digest[i])<<16;
        if(i+1<20)v|=unsigned(digest[i+1])<<8;
        if(i+2<20)v|=digest[i+2];
        result+=alphabet[(v>>18)&63];result+=alphabet[(v>>12)&63];
        result+=i+1<20?alphabet[(v>>6)&63]:'-';result+=i+2<20?alphabet[v&63]:'-';
    }
    return result;
}
std::wstring DiscLayout::TocParameter() const {
    if(!IsAudioDisc()) Error(ERROR_NOT_SUPPORTED);
    auto result=std::to_wstring(tracks.front().number)+L"+"+std::to_wstring(tracks.back().number)+L"+"+std::to_wstring(tracks.back().end);
    for(const auto& t:tracks) result+=L"+"+std::to_wstring(t.start);
    return result;
}
DiscLayout ParseDiscToc(const CDROM_TOC& toc, DWORD returned) {
    if(returned<4 || toc.FirstTrack<1 || toc.LastTrack<toc.FirstTrack || toc.LastTrack>99) Error(ERROR_INVALID_DATA);
    const unsigned count=toc.LastTrack-toc.FirstTrack+1;
    const auto needed=offsetof(CDROM_TOC,TrackData)+(count+1)*sizeof(TRACK_DATA);
    const unsigned length=(unsigned(toc.Length[0])<<8)|toc.Length[1];
    if(returned<needed || length+2<needed || length+2>returned || toc.TrackData[count].TrackNumber!=0xaa) Error(ERROR_INVALID_DATA);
    DiscLayout result;
    for(unsigned i=0;i<count;++i) {
        if(toc.TrackData[i].TrackNumber!=toc.FirstTrack+i) Error(ERROR_INVALID_DATA);
        const auto start=Address(toc.TrackData[i].Address), end=Address(toc.TrackData[i+1].Address);
        if(end<=start) Error(ERROR_INVALID_DATA);
        result.tracks.push_back({unsigned(toc.FirstTrack)+i,start,end,(toc.TrackData[i].Control&4)!=0});
    }
    return result;
}
HANDLE OpenCdDrive(const std::filesystem::path& path) {
    const auto root=Root(path);
    if(GetDriveTypeW(root.c_str())!=DRIVE_CDROM) Error(ERROR_INVALID_DRIVE);
    const auto device=L"\\\\.\\"+root.substr(0,2);
    HANDLE result=CreateFileW(device.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(result==INVALID_HANDLE_VALUE) Error();return result;
}
DiscLayout ReadDiscLayout(const std::filesystem::path& path) {
    Handle drive{OpenCdDrive(path)}; CDROM_TOC toc{};DWORD returned{};
    if(!DeviceIoControl(drive.value,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&returned,nullptr)) Error();
    auto result=ParseDiscToc(toc,returned);
    GetVolumeInformationW(Root(path).c_str(),nullptr,0,&result.serial,nullptr,nullptr,nullptr,0);
    return result;
}
unsigned CdaTrackNumber(const std::filesystem::path& path) {
    const auto name=path.filename().wstring();
    if(name.size()<10 || _wcsnicmp(name.c_str(),L"track",5)!=0) Error(ERROR_INVALID_NAME);
    wchar_t* end{};const auto number=wcstoul(name.c_str()+5,&end,10);
    if(number<1 || number>99 || !end || _wcsicmp(end,L".cda")) Error(ERROR_INVALID_NAME);
    return number;
}
const DiscTrack& FindDiscTrack(const DiscLayout& disc,unsigned number) {
    for(const auto& t:disc.tracks) if(t.number==number && !t.data)return t;
    Error(ERROR_INVALID_DATA);
}
CdaDescriptor ReadCdaDescriptor(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);std::array<unsigned char,44> b{};
    if(!stream.read(reinterpret_cast<char*>(b.data()),b.size())) Error(ERROR_READ_FAULT);
    const auto u32=[&](size_t p){return unsigned(b[p])|(unsigned(b[p+1])<<8)|(unsigned(b[p+2])<<16)|(unsigned(b[p+3])<<24);};
    if(memcmp(b.data(),"RIFF",4) || u32(4)!=36 || memcmp(b.data()+8,"CDDAfmt ",8) || u32(16)!=24 || b[20]!=1 || b[21]!=0) Error(ERROR_INVALID_DATA);
    CdaDescriptor result{unsigned(b[22])|(unsigned(b[23])<<8),u32(28),u32(32),u32(24)};
    if(result.number<1 || result.number>99 || !result.frames || result.start>0x7fffffff-result.frames) Error(ERROR_INVALID_DATA);
    return result;
}
std::filesystem::path DiscCacheDirectory() {
    wchar_t executable[32768]{};const auto n=GetModuleFileNameW(nullptr,executable,32768);
    if(!n || n>=32768) Error();return std::filesystem::path(executable).parent_path()/L"CDDB";
}
std::wstring DiscField(const CueMetadata& fields,const wchar_t* key) {
    for(const auto& f:fields) if(_wcsicmp(f.first.c_str(),key)==0)return f.second;return {};
}
bool IsDiscMetadataField(std::wstring_view name) noexcept {
    if (name.size()==16 && _wcsnicmp(name.data(), L"CDDBSerialNumber", 16)==0) return false;
    return CueSheet::IsWritableField(name) && !name.empty() &&
        ((name[0]>=L'A'&&name[0]<=L'Z')||(name[0]>=L'a'&&name[0]<=L'z')||name[0]==L'_');
}
DiscMetadata ReadDiscMetadata(const DiscLayout& disc,const std::filesystem::path& directory) {
    DiscMetadata result;
    if(!disc.serial) return result;
    const auto path=CachePath(disc,directory);
    if(!std::filesystem::exists(path))return result;
    if(std::filesystem::file_size(path)>2*1024*1024)Error(ERROR_FILE_TOO_LARGE);
    Com com;ComPtr<IXMLDOMDocument2> doc;
    if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&doc)))) Error(ERROR_NOT_SUPPORTED);
    doc->put_async(VARIANT_FALSE);doc->put_resolveExternals(VARIANT_FALSE);doc->put_validateOnParse(VARIANT_FALSE);
    doc->setProperty(_bstr_t(L"ProhibitDTD"),_variant_t(true));
    VARIANT_BOOL loaded{};
    if(FAILED(doc->load(_variant_t(path.c_str()),&loaded)) || loaded!=VARIANT_TRUE)Error(ERROR_INVALID_DATA);
    ComPtr<IXMLDOMElement> root;doc->get_documentElement(&root);
    if(!root)Error(ERROR_INVALID_DATA);
    BSTR tag{};root->get_tagName(&tag);const bool named=tag && wcscmp(tag,L"CD")==0;SysFreeString(tag);
    const auto version=Attribute(root.Get(),L"version");
    if(!named || version.empty() || wcstoul(version.c_str(),nullptr,10)>=4 ||
       wcstoul(Attribute(root.Get(),L"SerialNumber").c_str(),nullptr,10)!=disc.serial)Error(ERROR_INVALID_DATA);
    const auto id=Attribute(root.Get(),L"MusicBrainzDiscId");
    const auto fingerprint=Attribute(root.Get(),L"TOC");
    if(!fingerprint.empty() && disc.complete && fingerprint!=disc.Fingerprint())Error(ERROR_MEDIA_CHANGED);
    if(!id.empty() && disc.IsAudioDisc() && core::WideToUtf8(id)!=disc.MusicBrainzId())Error(ERROR_MEDIA_CHANGED);
    ComPtr<IXMLDOMNodeList> children;root->get_childNodes(&children);long count{};children->get_length(&count);
    if(count>1000)Error(ERROR_INVALID_DATA);
    for(long i=0;i<count;++i) {
        ComPtr<IXMLDOMNode> node;children->get_item(i,&node);ComPtr<IXMLDOMElement> element;
        if(FAILED(node.As(&element)))continue;
        BSTR name{};element->get_tagName(&name);std::wstring node_name=name?name:L"";SysFreeString(name);
        unsigned number{};
        if(node_name==L"Track")number=wcstoul(Attribute(element.Get(),L"ID").c_str(),nullptr,10);
        else if(node_name.starts_with(L"Track"))number=wcstoul(node_name.c_str()+5,nullptr,10);
        if(number<1 || number>99)continue;
        auto& fields=result[number];
        ComPtr<IXMLDOMNamedNodeMap> attributes;element->get_attributes(&attributes);long length{};attributes->get_length(&length);
        for(long j=0;j<length;++j) {
            ComPtr<IXMLDOMNode> attr;attributes->get_item(j,&attr);BSTR key{};attr->get_nodeName(&key);
            _variant_t value;attr->get_nodeValue(&value);
            if(key && value.vt==VT_BSTR && value.bstrVal && _wcsicmp(key,L"ID") && IsDiscMetadataField(key))Set(fields,key,value.bstrVal);
            SysFreeString(key);
        }
        if(DiscField(fields,L"Album").empty())Set(fields,L"Album",Attribute(root.Get(),L"Album"));
        Set(fields,L"Tracknumber",std::to_wstring(number));
    }
    return result;
}
bool IsDiscMetadataWritable(const DiscLayout& disc) noexcept {
    try {const auto path=CachePath(disc,{});const auto a=GetFileAttributesW(path.c_str());
        return a==INVALID_FILE_ATTRIBUTES ? GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND : !(a&FILE_ATTRIBUTE_READONLY);
    } catch(...) {return false;}
}
void WriteDiscMetadata(const DiscLayout& disc,const DiscMetadata& changes,const std::filesystem::path& directory) {
    if(!disc.complete)Error(ERROR_NOT_READY);
    const auto path=CachePath(disc,directory);
    auto lock_key=std::filesystem::absolute(path).wstring();CharLowerBuffW(lock_key.data(),static_cast<DWORD>(lock_key.size()));
    uint64_t hash=14695981039346656037ULL;for(auto c:lock_key){hash^=c;hash*=1099511628211ULL;}
    Handle mutex{CreateMutexW(nullptr,FALSE,(L"Local\\TTPlayer.CDDB."+std::to_wstring(hash)).c_str())};
    if(!mutex.value)Error();const auto wait=WaitForSingleObject(mutex.value,3000);
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)Error(ERROR_LOCK_VIOLATION);
    struct Unlock {HANDLE h;~Unlock(){ReleaseMutex(h);}} unlock{mutex.value};
    auto merged=ReadDiscMetadata(disc,directory);
    for(const auto& [number,fields]:changes) {
        FindDiscTrack(disc,number);
        for(const auto& [key,value]:fields) {
            if(!IsDiscMetadataField(key))Error(ERROR_INVALID_PARAMETER);
            // Album is a disc-level property in the original cache.
            if(_wcsicmp(key.c_str(),L"Album")==0) {
                for(const auto& t:disc.tracks)if(!t.data)Set(merged[t.number],key,value);
            }
            Set(merged[number],key,value);
        }
    }
    std::wstring album;for(const auto& [n,f]:merged){album=DiscField(f,L"Album");if(!album.empty())break;}
    std::wstring xml=L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<CD version=\"3\" SerialNumber=\""+std::to_wstring(disc.serial)+L"\" Album=\""+Escape(album)+L"\"";
    if(disc.IsAudioDisc())xml+=L" MusicBrainzDiscId=\""+core::Utf8ToWide(disc.MusicBrainzId())+L"\"";
    xml+=L" TOC=\""+disc.Fingerprint()+L"\"";
    xml+=L">\r\n";
    for(const auto& [number,fields]:merged) {
        wchar_t name[24]{};swprintf_s(name,L"  <Track%02u",number);xml+=name;
        for(const auto& [key,value]:fields)if(IsDiscMetadataField(key) && _wcsicmp(key.c_str(),L"Album"))xml+=L" "+key+L"=\""+Escape(value)+L"\"";
        xml+=L" />\r\n";
    }
    xml+=L"</CD>\r\n";const auto bytes=core::WideToUtf8(xml);
    std::filesystem::create_directories(path.parent_path());
    wchar_t temporary[MAX_PATH+1]{};if(!GetTempFileNameW(path.parent_path().c_str(),L"cdb",0,temporary))Error();
    struct Delete {wchar_t* p;~Delete(){DeleteFileW(p);}} cleanup{temporary};
    {Handle output{CreateFileW(temporary,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)};if(output.value==INVALID_HANDLE_VALUE)Error();
    DWORD written{};if(!WriteFile(output.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(output.value))Error();}
    const bool exists=std::filesystem::exists(path);
    if(exists ? !ReplaceFileW(path.c_str(),temporary,nullptr,0,nullptr,nullptr) : !MoveFileExW(temporary,path.c_str(),MOVEFILE_WRITE_THROUGH))Error();
}
unsigned CheckedCdRead(unsigned requested,DWORD returned) {
    if(!requested || !returned || returned%2352 || returned/2352>requested)Error(ERROR_READ_FAULT);
    return returned/2352;
}
bool ContainsDtsCd(std::span<const std::byte> bytes) noexcept {
    // DTS syncs in 16-bit BE/LE and 14-bit packed BE/LE forms. Detect before
    // permitting any output; this source must never emit compressed noise.
    constexpr unsigned char magic[][4]={{0x7f,0xfe,0x80,1},{0xfe,0x7f,1,0x80},{0x1f,0xff,0xe8,0},{0xff,0x1f,0,0xe8}};
    unsigned matches{};
    for(size_t i=0;i+16<=bytes.size();++i)for(const auto& m:magic)if(memcmp(bytes.data()+i,m,4)==0 && ++matches>=2)return true;
    return false;
}
} // namespace ttplayer::audio
