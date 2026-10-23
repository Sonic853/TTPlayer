#include "ttplayer/audio/http_download_queue.h"
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
bool IsHttpDownloadMediaPath(const std::filesystem::path& path) {
    // Same file suffix set as the recovered audio boundary, without linking
    // an AudioEngine (or another network producer) into ttpbrowser.dll.
    constexpr std::wstring_view patterns =
        L".mp3;.mp2;.mp1;.mpa;.mp3pro;.wav;.wma;.wmv;.asf;.cda;.cue;.mid;.midi;.rmi;"
        L".aif;.aifc;.aiff;.au;.snd;.m4a;.mp4;.aac;.aa;.ac3;.a52;.eac3;.ec3;.dts;.dtswav;.ape;.mac;"
        L".flac;.fla;.tta;.mod;.far;.it;.s3m;.stm;.mtm;.umx;.xm;.mpc;.mp+;.ogg;.ra;.rm;.ram;.rmvb;.tak;.vqf";
    const auto extension=path.extension().wstring();
    if(extension.empty()) return false;
    size_t begin{};
    while(begin<patterns.size()) {
        const auto end=patterns.find(L';',begin);
        const auto item=patterns.substr(begin,end==patterns.npos?patterns.size()-begin:end-begin);
        if(extension.size()==item.size() && !_wcsnicmp(extension.c_str(),item.data(),item.size())) return true;
        if(end==patterns.npos) break;
        begin=end+1;
    }
    return false;
}
std::filesystem::path HttpDownloadDestination(const std::filesystem::path& folder,
    std::wstring_view url,std::wstring title,std::wstring artist,bool artist_folder) {
    auto name=std::wstring(url);name.resize(name.find_first_of(L"?#")==std::wstring::npos?name.size():name.find_first_of(L"?#"));
    name=name.substr(name.find_last_of(L'/')+1);std::filesystem::path leaf(name);
    auto ext=leaf.extension().wstring();if(!IsHttpDownloadMediaPath(leaf)) ext=L".bin";
    if(title.empty()) title=leaf.stem().wstring();
    auto root=folder;if(artist_folder && !artist.empty()) root/=SafeName(artist);
    return root/(SafeName(title)+ext);
}

} // namespace ttplayer::audio
