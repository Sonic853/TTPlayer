#include "ttplayer/update/update.h"
#include <windows.h>
#include <wincrypt.h>
#include <zlib.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <vector>

namespace ttplayer::update {
namespace {
std::vector<unsigned char> Read(const std::filesystem::path& file,size_t limit) {
    std::ifstream in(file,std::ios::binary|std::ios::ate);
    const auto size=in.tellg();if(!in || size<0 || static_cast<uint64_t>(size)>limit) throw std::runtime_error("Invalid package file size");
    std::vector<unsigned char> data(static_cast<size_t>(size));in.seekg(0);
    if(!data.empty() && !in.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("Cannot read package");return data;
}
void Write(const std::filesystem::path& file,const std::vector<unsigned char>& data) {
    HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create staged executable");
    DWORD n{};const bool ok=WriteFile(h,data.data(),static_cast<DWORD>(data.size()),&n,nullptr) && n==data.size() && FlushFileBuffers(h);
    CloseHandle(h);if(!ok) throw std::runtime_error("Cannot save staged executable");
}
void ValidateExe(const std::filesystem::path& file,const Version& expected,bool player) {
    if(FileVersion(file)!=expected) throw std::runtime_error("Package executable version does not match release");
    const auto bytes=Read(file,64*1024*1024);
    auto u16=[&](size_t p)->unsigned {if(p+2>bytes.size()) throw std::runtime_error("Invalid PE image");return bytes[p]|unsigned(bytes[p+1])<<8;};
    auto u32=[&](size_t p)->uint32_t {return u16(p)|uint32_t(u16(p+2))<<16;};
    if(u16(0)!=0x5a4d) throw std::runtime_error("Package is not an executable");
    const auto pe=u32(0x3c);
    if(pe>bytes.size() || u32(pe)!=0x4550 || u16(pe+4)!=0x14c || u16(pe+24)!=0x10b)
        throw std::runtime_error("Package is not an x86 executable");
    if(IsXp() && (u16(pe+24+48)>5 || (u16(pe+24+48)==5 && u16(pe+24+50)>1)))
        throw std::runtime_error("Package does not support Windows XP");
    DWORD ignored{};std::vector<unsigned char> version(GetFileVersionInfoSizeW(file.c_str(),&ignored));
    if(version.empty() || !GetFileVersionInfoW(file.c_str(),0,static_cast<DWORD>(version.size()),version.data())) throw std::runtime_error("Missing product identity");
    // Both shipping resources use the same Chinese Unicode translation.
    struct Translation {WORD language,codepage;};Translation* translations{};UINT count{};
    if(!VerQueryValueW(version.data(),L"\\VarFileInfo\\Translation",reinterpret_cast<void**>(&translations),&count) || count<sizeof(Translation))
        throw std::runtime_error("Missing product translation");
    wchar_t query[128]{};swprintf_s(query,L"\\StringFileInfo\\%04x%04x\\OriginalFilename",translations[0].language,translations[0].codepage);
    wchar_t* name{};
    if(!VerQueryValueW(version.data(),query,reinterpret_cast<void**>(&name),&count) || !name ||
        _wcsicmp(name,player ? L"TTPlayerRebuild.exe" : L"TTPUpdater.exe")!=0) throw std::runtime_error("Unexpected executable identity");
}
}
std::string Sha256(const std::filesystem::path& file) {
    HCRYPTPROV provider{};HCRYPTHASH hash{};
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)) throw std::runtime_error("SHA-256 provider unavailable");
    struct Cleanup{HCRYPTPROV p;HCRYPTHASH& h;~Cleanup(){if(h) CryptDestroyHash(h);CryptReleaseContext(p,0);}}cleanup{provider,hash};
    if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)) throw std::runtime_error("Cannot initialize SHA-256");
    std::ifstream in(file,std::ios::binary);if(!in) throw std::runtime_error("Cannot read checksum input");
    unsigned char bytes[65536];
    while(in) {in.read(reinterpret_cast<char*>(bytes),sizeof(bytes));const auto count=in.gcount();
        if(count && !CryptHashData(hash,bytes,static_cast<DWORD>(count),0)) throw std::runtime_error("SHA-256 failed");}
    if(!in.eof()) throw std::runtime_error("Checksum read failed");
    unsigned char result[32]{};DWORD size=sizeof(result);
    if(!CryptGetHashParam(hash,HP_HASHVAL,result,&size,0) || size!=sizeof(result)) throw std::runtime_error("SHA-256 failed");
    constexpr char hex[]="0123456789abcdef";std::string out;
    for(auto c:result){out+=hex[c>>4];out+=hex[c&15];}return out;
}
void ExtractPackage(const std::filesystem::path& zip,const std::filesystem::path& staging) {
    const auto bytes=Read(zip,64*1024*1024);
    auto u16=[&](size_t p)->unsigned {if(p>bytes.size() || bytes.size()-p<2) throw std::runtime_error("Truncated ZIP");return bytes[p]|unsigned(bytes[p+1])<<8;};
    auto u32=[&](size_t p)->uint32_t {return u16(p)|uint32_t(u16(p+2))<<16;};
    if(bytes.size()<22) throw std::runtime_error("Invalid ZIP");
    size_t end=bytes.size()-22;const size_t minimum=bytes.size()>65557 ? bytes.size()-65557 : 0;
    for(;;) {if(u32(end)==0x06054b50 && end+22+u16(end+20)==bytes.size()) break;if(end==minimum) throw std::runtime_error("ZIP directory not found");--end;}
    const auto count=u16(end+10);const size_t directory=u32(end+16),directory_size=u32(end+12);
    if(u16(end+4) || u16(end+6) || u16(end+8)!=count || count<2 || count>3 || directory>end || directory_size!=end-directory)
        throw std::runtime_error("Unsupported update ZIP directory");
    std::map<std::string,std::vector<unsigned char>> files;
    std::vector<std::pair<size_t,size_t>> ranges;
    size_t cursor=directory,total=0;
    for(unsigned i=0;i<count;++i) {
        if(u32(cursor)!=0x02014b50) throw std::runtime_error("Invalid ZIP entry");
        const auto flags=u16(cursor+8),method=u16(cursor+10),crc=u32(cursor+16);
        const size_t packed=u32(cursor+20),size=u32(cursor+24),name_size=u16(cursor+28),extra=u16(cursor+30),comment=u16(cursor+32),local=u32(cursor+42);
        if(cursor+46+name_size+extra+comment>end || size>64*1024*1024 || total>64*1024*1024-size ||
            (flags&~0x0808U) || (method!=0 && method!=8) || u16(cursor+34)) throw std::runtime_error("Unsupported update ZIP entry");
        const std::string name(reinterpret_cast<const char*>(bytes.data()+cursor+46),name_size);
        if((name!="TTPlayerRebuild.exe" && name!="TTPUpdater.exe" && name!="SHA256SUMS.txt") || files.contains(name))
            throw std::runtime_error("Unexpected or duplicate update ZIP path");
        if(name=="SHA256SUMS.txt" && size>65536) throw std::runtime_error("Excessive checksum file");
        if(local>=directory || u32(local)!=0x04034b50 || u16(local+6)!=flags || u16(local+8)!=method || u16(local+26)!=name_size)
            throw std::runtime_error("ZIP local header mismatch");
        const size_t data=local+30+name_size+u16(local+28);
        if(data>directory || packed>directory-data || std::string_view(reinterpret_cast<const char*>(bytes.data()+local+30),name_size)!=name)
            throw std::runtime_error("ZIP data is out of bounds");
        if(!(flags&8) && (u32(local+14)!=crc || u32(local+18)!=packed || u32(local+22)!=size)) throw std::runtime_error("ZIP size mismatch");
        for(const auto& range:ranges) if(local<range.second && data+packed>range.first) throw std::runtime_error("Overlapping ZIP entries");
        ranges.emplace_back(local,data+packed);
        std::vector<unsigned char> content(size);
        if(method==0) {if(packed!=size) throw std::runtime_error("Stored ZIP size mismatch");std::copy_n(bytes.data()+data,size,content.data());}
        else {
            z_stream stream{};stream.next_in=const_cast<Bytef*>(bytes.data()+data);stream.avail_in=static_cast<uInt>(packed);
            unsigned char empty{};stream.next_out=content.empty()?&empty:content.data();stream.avail_out=static_cast<uInt>(std::max<size_t>(1,size));
            if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK) throw std::runtime_error("Cannot initialize ZIP inflater");
            const auto status=inflate(&stream,Z_FINISH);const bool valid=status==Z_STREAM_END && stream.total_out==size && stream.total_in==packed;
            inflateEnd(&stream);if(!valid) throw std::runtime_error("Invalid ZIP deflate stream");
        }
        if(crc32(0,content.data(),static_cast<uInt>(content.size()))!=crc) throw std::runtime_error("ZIP CRC mismatch");
        total+=size;files.emplace(name,std::move(content));cursor+=46+name_size+extra+comment;
    }
    if(cursor!=end || !files.contains("TTPlayerRebuild.exe") || !files.contains("SHA256SUMS.txt")) throw std::runtime_error("Incomplete update package");
    const auto& raw=files.at("SHA256SUMS.txt");const std::string sums(raw.begin(),raw.end());
    std::filesystem::create_directories(staging);
    for(const auto& [name,content]:files) {
        if(name=="SHA256SUMS.txt") continue;
        const auto path=staging/name;Write(path,content);
        if(Sha256(path)!=ExpectedHash(sums,name)) throw std::runtime_error("Executable checksum mismatch");
    }
}
std::filesystem::path PreparePackage(Http& http,const Release& release,const std::filesystem::path& staging,const Cancel& cancel,const Progress& progress) {
    std::filesystem::create_directories(staging);
    const auto expected=ExpectedHash(http.Get(release.sums_url,cancel),release.asset_name);
    const auto zip=staging/L"package.zip.part";
    http.Download(release.zip_url,zip,release.size,cancel,progress);
    if(cancel && cancel()) throw std::runtime_error("Canceled");
    if(Sha256(zip)!=expected) throw std::runtime_error("Downloaded ZIP checksum mismatch");
    ExtractPackage(zip,staging/L"payload");
    const auto exe=staging/L"payload"/L"TTPlayerRebuild.exe";ValidateExe(exe,release.version,true);
    const auto updater=staging/L"payload"/L"TTPUpdater.exe";
    if(std::filesystem::exists(updater)) ValidateExe(updater,release.version,false);
    return exe;
}
namespace {
void ReplaceExecutable(const std::filesystem::path& directory,const std::filesystem::path& prepared,const Version& expected,bool player) {
    const auto target=directory/(player ? L"TTPlayerRebuild.exe" : L"TTPUpdater.exe");
    const auto backup=std::filesystem::path(target.wstring()+L".bak");
    ValidateExe(prepared,expected,player);
    if(FileVersion(target)>=expected) {
        if(!player) return;
        throw std::runtime_error("The installed player is already this version or newer");
    }
    wchar_t temporary[MAX_PATH]{};
    if(!GetTempFileNameW(directory.c_str(),L"ttu",0,temporary)) throw std::runtime_error("Cannot write to the player directory");
    struct Temp{std::filesystem::path p;~Temp(){DeleteFileW(p.c_str());}}temp{temporary};
    if(!CopyFileW(prepared.c_str(),temp.p.c_str(),FALSE) || Sha256(temp.p)!=Sha256(prepared)) throw std::runtime_error("Cannot stage the replacement executable");
    // ReplaceFile preserves the old image as the requested .bak, on the same
    // volume. Never remove the current executable before staging succeeds.
    if(!ReplaceFileW(target.c_str(),temp.p.c_str(),backup.c_str(),REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr)) {
        const auto error=GetLastError();
        // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 can leave the old image at the
        // backup name. Restore that state without overwriting a concurrent file.
        if(error==ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 && GetFileAttributesW(target.c_str())==INVALID_FILE_ATTRIBUTES &&
            !MoveFileExW(backup.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Update failed; restore TTPlayerRebuild.exe.bak manually");
        throw std::runtime_error("Cannot replace player; close other instances and check directory permissions ("+std::to_string(error)+")");
    }
    try {ValidateExe(target,expected,player);} catch(...) {
        if(!MoveFileExW(backup.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Update verification failed; restore TTPlayerRebuild.exe.bak manually");
        throw;
    }
}
}
void ReplacePlayer(const std::filesystem::path& directory,const std::filesystem::path& prepared,const Version& expected) {
    ReplaceExecutable(directory,prepared,expected,true);
}
void ReplaceUpdater(const std::filesystem::path& directory,const std::filesystem::path& prepared,const Version& expected) {
    ReplaceExecutable(directory,prepared,expected,false);
}
}
