#include <ttpcomm/zip.h>
#include "ttplayer/update/update.h"
#include <windows.h>
#include <objbase.h>
#include <zlib.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace ttplayer::update {
namespace {
std::wstring Fold(std::wstring value) {
    if(!value.empty()) CharUpperBuffW(value.data(),static_cast<DWORD>(value.size()));
    return value;
}
std::filesystem::path PackagePath(std::string_view name) {
    if(name.empty() || name.size()>4*MAX_PATH || name.find('\0')!=name.npos)
        throw std::runtime_error("Invalid update ZIP path");
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0);
    if(!size) throw std::runtime_error("Update ZIP path is not UTF-8");
    std::wstring value(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),value.data(),size);
    // XP can discard invalid UTF-8 instead of failing; enforce an exact round trip.
    std::string encoded(WideCharToMultiByte(CP_UTF8,0,value.data(),size,nullptr,0,nullptr,nullptr),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),size,encoded.data(),static_cast<int>(encoded.size()),nullptr,nullptr);
    if(encoded!=name || value.size()>=MAX_PATH) throw std::runtime_error("Invalid update ZIP path encoding");
    size_t start=0;
    for(size_t end=0;end<=value.size();++end) {
        if(end<value.size() && value[end]!=L'/') {
            if(value[end]<32 || std::wstring_view(L"\\:*?\"<>|").find(value[end])!=std::wstring_view::npos)
                throw std::runtime_error("Invalid update ZIP path character");
            continue;
        }
        const auto part=value.substr(start,end-start);
        if(part.empty() || part==L"." || part==L".." || part.back()==L'.' || part.back()==L' ')
            throw std::runtime_error("Unsafe update ZIP path component");
        auto base=Fold(part.substr(0,part.find(L'.')));
        while(!base.empty() && base.back()==L' ') base.pop_back();
        if(base==L"CON" || base==L"PRN" || base==L"AUX" || base==L"NUL" || base==L"CLOCK$" || base==L"CONIN$" || base==L"CONOUT$" ||
           (base.size()==4 && (base.substr(0,3)==L"COM" || base.substr(0,3)==L"LPT") &&
            ((base[3]>=L'1' && base[3]<=L'9') || base[3]==0xb9 || base[3]==0xb2 || base[3]==0xb3)))
            throw std::runtime_error("Reserved Windows device in update ZIP path");
        start=end+1;
    }
    return std::filesystem::path(value);
}
// Check every existing ancestor, including the selected installation directory.
// Never traverse a junction/symlink when extracting, installing or rolling back.
void CheckPath(const std::filesystem::path& path,bool directory=false) {
    const auto full=std::filesystem::absolute(path).lexically_normal();
    if(full.native().size()>=MAX_PATH) throw std::runtime_error("Update path exceeds the Windows path limit");
    auto current=full.root_path();
    for(const auto& part:full.relative_path()) {
        current/=part;
        const auto attr=GetFileAttributesW(current.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES) {
            const auto error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) continue;
            throw std::runtime_error("Cannot inspect update path ("+std::to_string(error)+")");
        }
        if(attr&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Update path contains a junction or symbolic link");
        if((current!=full || directory) && !(attr&FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Update directory conflicts with a file");
        if(current==full && !directory && (attr&FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Update file conflicts with a directory");
    }
}
std::vector<unsigned char> Read(const std::filesystem::path& file,size_t limit) {
    std::ifstream in(file,std::ios::binary|std::ios::ate);
    const auto size=in.tellg();if(!in || size<0 || static_cast<uint64_t>(size)>limit) throw std::runtime_error("Invalid package file size");
    std::vector<unsigned char> data(static_cast<size_t>(size));in.seekg(0);
    if(!data.empty() && !in.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("Cannot read package");return data;
}
void Write(const std::filesystem::path& file,const std::vector<unsigned char>& data) {
    HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create staged file");
    DWORD n{};const bool ok=WriteFile(h,data.data(),static_cast<DWORD>(data.size()),&n,nullptr) && n==data.size() && FlushFileBuffers(h);
    CloseHandle(h);if(!ok) throw std::runtime_error("Cannot save staged file");
}
void ValidateExe(const std::filesystem::path& file,const Version& expected,bool player) {
    if(FileVersion(file)!=expected) throw std::runtime_error("Package executable version does not match release");
    const auto bytes=Read(file,64*1024*1024);
    auto u16=[&](size_t p)->unsigned {if(p+2>bytes.size()) throw std::runtime_error("Invalid PE image");return ttpcomm::bytes::Le16(bytes.data()+p);};
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
void ExtractPackage(const std::filesystem::path& zip,const std::filesystem::path& staging) {
    const auto bytes=Read(zip,64*1024*1024);
    auto u16=[&](size_t p)->unsigned {if(p>bytes.size() || bytes.size()-p<2) throw std::runtime_error("Truncated ZIP");return ttpcomm::bytes::Le16(bytes.data()+p);};
    auto u32=[&](size_t p)->uint32_t {return u16(p)|uint32_t(u16(p+2))<<16;};
    const auto directory_end=ttpcomm::zip::FindDirectoryEnd({bytes.data(),bytes.size()},ttpcomm::zip::EndPolicy::exact_eof);
    if(!directory_end) throw std::runtime_error("ZIP directory not found");
    const size_t end=*directory_end;
    const auto count=u16(end+10);const size_t directory=u32(end+16),directory_size=u32(end+12);
    if(u16(end+4) || u16(end+6) || u16(end+8)!=count || count<2 || count>4096 || directory>end || directory_size!=end-directory)
        throw std::runtime_error("Unsupported update ZIP directory");
    std::map<std::string,std::vector<unsigned char>> files;
    std::map<std::wstring,bool> paths;
    std::vector<std::filesystem::path> folders;
    std::vector<std::pair<size_t,size_t>> ranges;
    size_t cursor=directory,total=0;
    for(unsigned i=0;i<count;++i) {
        const auto entry=ttpcomm::zip::ReadCentral({bytes.data(),bytes.size()},cursor);
        if(!entry) throw std::runtime_error("Invalid ZIP entry");
        const unsigned flags=entry->flags,method=entry->method;
        const auto crc=entry->crc;
        const size_t packed=entry->packed,size=entry->size,name_size=entry->name.size(),local=entry->local;
        if(cursor>end || entry->record_size>end-cursor || size>64*1024*1024 || total>64*1024*1024-size ||
            (flags&~0x0808U) || (method!=0 && method!=8) || entry->disk) throw std::runtime_error("Unsupported update ZIP entry");
        const std::string name(entry->name);
        const bool folder=!name.empty() && name.back()=='/';
        const auto path=PackagePath(folder ? std::string_view(name).substr(0,name.size()-1) : name);
        if(!paths.emplace(Fold(path.generic_wstring()),folder).second) throw std::runtime_error("Duplicate update ZIP path");
        const auto type=(u32(cursor+38)>>16)&0170000;
        if((type && type!=(folder?0040000U:0100000U)) || (folder && size)) throw std::runtime_error("Unsupported update ZIP file type");
        if(name=="SHA256SUMS.txt" && size>1024*1024) throw std::runtime_error("Excessive checksum file");
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
        total+=size;
        if(folder) folders.push_back(path);else files.emplace(name,std::move(content));
        cursor+=entry->record_size;
    }
    if(cursor!=end || !files.contains("TTPlayerRebuild.exe") || !files.contains("SHA256SUMS.txt")) throw std::runtime_error("Incomplete update package");
    const auto& raw=files.at("SHA256SUMS.txt");const std::string sums(raw.begin(),raw.end());
    for(const auto& [name,folder]:paths) {
        for(size_t p=name.find(L'/');p!=name.npos;p=name.find(L'/',p+1)) {
            const auto parent=paths.find(name.substr(0,p));
            if(parent!=paths.end() && !parent->second) throw std::runtime_error("Conflicting update ZIP paths");
        }
    }
    CheckPath(staging,true);
    std::filesystem::create_directories(staging);
    if(!std::filesystem::is_empty(staging)) throw std::runtime_error("Update staging directory must be empty");
    for(const auto& folder:folders) {CheckPath(staging/folder,true);std::filesystem::create_directories(staging/folder);}
    for(const auto& [name,content]:files) {
        if(name=="SHA256SUMS.txt") continue;
        const auto path=staging/PackagePath(name);
        CheckPath(path);
        std::filesystem::create_directories(path.parent_path());
        Write(path,content);
        if(Sha256(path)!=ExpectedHash(sums,name)) throw std::runtime_error("Package file checksum mismatch");
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
struct PackageInstall::Impl {
    struct File {std::filesystem::path relative;std::string next_hash,old_hash;bool existed{},touched{};};
    std::filesystem::path root,transaction;
    std::vector<File> files;
    std::vector<std::filesystem::path> folders,created;
    bool applied{},finished{},started{};
    static void Fail(const char* message,const std::filesystem::path& path,DWORD error=GetLastError()) {
        const auto name=path.u8string();
        throw std::runtime_error(std::string(message)+": "+std::string(name.begin(),name.end())+" ("+std::to_string(error)+")");
    }
    void MakeDirectories(const std::filesystem::path& relative) {
        auto path=root;
        for(const auto& part:relative) {
            path/=part;CheckPath(path,true);
            if(std::filesystem::create_directory(path)) created.push_back(path);
        }
    }
    void Preflight() {
        std::set<std::array<DWORD,3>> identities;
        for(const auto& file:files) {
            const auto target=root/file.relative;CheckPath(target);
            if(file.existed!=std::filesystem::exists(target)) Fail("Installed file changed during update",target,0);
            if(!file.existed) continue;
            const auto handle=CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE) Fail("Cannot update file; close programs using it and check permissions",target);
            BY_HANDLE_FILE_INFORMATION info{};
            const bool ok=GetFileInformationByHandle(handle,&info)!=FALSE;const auto error=GetLastError();CloseHandle(handle);
            if(!ok) Fail("Cannot inspect installed file",target,error);
            if(!identities.insert({info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow}).second)
                Fail("Update paths refer to the same installed file",target,0);
            if(Sha256(target)!=file.old_hash) Fail("Installed file changed during update",target,0);
        }
        for(const auto& folder:folders) CheckPath(root/folder,true);
    }
    void Rollback() {
        if(finished) return;
        bool failed=false;
        for(auto it=files.rbegin();it!=files.rend();++it) {
            if(!it->touched) continue;
            try {
                const auto target=root/it->relative;CheckPath(target);
                if(it->existed) {
                    // A failed replacement can leave the original untouched.
                    if(std::filesystem::exists(target) && Sha256(target)==it->old_hash) {it->touched=false;continue;}
                    const auto backup=transaction/L"backup"/it->relative;
                    const auto restore=transaction/L"new"/it->relative;CheckPath(backup);CheckPath(restore);
                    if(Sha256(backup)!=it->old_hash || !CopyFileW(backup.c_str(),restore.c_str(),FALSE) ||
                        !MoveFileExW(restore.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) || Sha256(target)!=it->old_hash)
                        throw std::runtime_error("Restore failed");
                } else if(std::filesystem::exists(target) && !DeleteFileW(target.c_str())) throw std::runtime_error("Remove failed");
                it->touched=false;
            } catch(...) {failed=true;}
        }
        for(auto it=created.rbegin();it!=created.rend();++it) {
            try {CheckPath(*it,true);if(std::filesystem::exists(*it) && !std::filesystem::remove(*it)) failed=true;}
            catch(...) {failed=true;}
        }
        if(failed) Fail("Update rollback incomplete; restore files using the preserved backup",transaction,0);
        finished=true;
        std::error_code ignored;std::filesystem::remove_all(transaction,ignored);
    }
};
PackageInstall::PackageInstall(const std::filesystem::path& directory,const std::filesystem::path& payload,const Version& expected)
    :impl_(std::make_unique<Impl>()) {
    auto& state=*impl_;state.root=std::filesystem::absolute(directory).lexically_normal();
    CheckPath(state.root,true);CheckPath(payload,true);
    ValidateExe(payload/L"TTPlayerRebuild.exe",expected,true);
    if(FileVersion(state.root/L"TTPlayerRebuild.exe")>=expected) throw std::runtime_error("The installed player is already this version or newer");
    if(std::filesystem::exists(payload/L"TTPUpdater.exe")) ValidateExe(payload/L"TTPUpdater.exe",expected,false);
    // Enumerate without following directory reparse points. The payload is private
    // verified staging, but check it again before copying anything to the install.
    size_t total=0,count=0;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(payload)) {
        if(++count>4096) throw std::runtime_error("Too many update files");
        const bool folder=entry.is_directory();CheckPath(entry.path(),folder);
        const auto relative=entry.path().lexically_relative(payload);
        const auto encoded=relative.generic_u8string();
        PackagePath(std::string_view(reinterpret_cast<const char*>(encoded.data()),encoded.size()));
        if(Fold(relative.generic_wstring())==L"SHA256SUMS.TXT") continue;
        CheckPath(state.root/relative,folder);
        if(folder) {state.folders.push_back(relative);continue;}
        if(!entry.is_regular_file()) throw std::runtime_error("Unsupported staged update file");
        const auto size=entry.file_size();
        if(size>64*1024*1024 || total>64*1024*1024-size) throw std::runtime_error("Update payload is too large");
        total+=static_cast<size_t>(size);
        const auto target=state.root/relative;
        const bool existed=std::filesystem::exists(target);
        state.files.push_back({relative,Sha256(entry.path()),existed?Sha256(target):std::string{},existed});
    }
    state.Preflight();
    GUID guid{};wchar_t unique[40]{};
    if(FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,unique,40)) throw std::runtime_error("Cannot create update transaction identity");
    state.transaction=state.root/(L".TTPlayerUpdate-"+std::wstring(unique));
    CheckPath(state.transaction,true);
    if(!std::filesystem::create_directory(state.transaction)) throw std::runtime_error("Cannot create update backup directory");
    try {
        std::ofstream journal(state.transaction/L"files.txt",std::ios::binary);
        if(!journal) throw std::runtime_error("Cannot create update recovery list");
        journal<<"Restore files marked REPLACE from backup/ to the installation directory.\r\n"
                  "Remove installed files marked ADD when rolling back. Close the player first.\r\n";
        for(const auto& file:state.files) {
            const auto next=state.transaction/L"new"/file.relative;
            CheckPath(next);std::filesystem::create_directories(next.parent_path());
            if(!CopyFileW((payload/file.relative).c_str(),next.c_str(),TRUE) || Sha256(next)!=file.next_hash)
                Impl::Fail("Cannot stage update file",next);
            if(file.existed) {
                const auto backup=state.transaction/L"backup"/file.relative;
                CheckPath(backup);std::filesystem::create_directories(backup.parent_path());
                if(!CopyFileW((state.root/file.relative).c_str(),backup.c_str(),TRUE) || Sha256(backup)!=file.old_hash)
                    Impl::Fail("Cannot back up update file",backup);
            }
            const auto name=file.relative.generic_u8string();
            journal<<(file.existed?"REPLACE ":"ADD ")<<std::string(name.begin(),name.end())<<"\r\n";
        }
        journal.flush();if(!journal) throw std::runtime_error("Cannot save update recovery list");
    } catch(...) {std::error_code ignored;std::filesystem::remove_all(state.transaction,ignored);throw;}
}
PackageInstall::~PackageInstall() {if(impl_ && !impl_->finished) {try {impl_->Rollback();} catch(...) {}}}
void PackageInstall::Apply() {
    auto& state=*impl_;
    if(state.started || state.finished) throw std::runtime_error("Update transaction has already been used");
    state.started=true;
    try {
        state.Preflight();
        for(const auto& file:state.files)
            if(Sha256(state.transaction/L"new"/file.relative)!=file.next_hash) throw std::runtime_error("Staged update file changed");
        for(const auto& folder:state.folders) state.MakeDirectories(folder);
        for(auto& file:state.files) {
            const auto target=state.root/file.relative,next=state.transaction/L"new"/file.relative;
            state.MakeDirectories(file.relative.parent_path());CheckPath(target);CheckPath(next);
            file.touched=file.existed; // Also recover partial ReplaceFile failure states.
            const bool ok=file.existed ? ReplaceFileW(target.c_str(),next.c_str(),nullptr,REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr)!=FALSE
                : MoveFileExW(next.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
            if(!ok) Impl::Fail("Cannot install update file",target);
            file.touched=true;
            if(Sha256(target)!=file.next_hash) Impl::Fail("Installed update file checksum mismatch",target,0);
        }
        state.applied=true;
    } catch(...) {state.Rollback();throw;}
}
void PackageInstall::Rollback() {impl_->Rollback();}
void PackageInstall::Commit() noexcept {
    if(!impl_->applied || impl_->finished) return;
    impl_->finished=true;
    std::error_code ignored;std::filesystem::remove_all(impl_->transaction/L"new",ignored);
}
std::filesystem::path PackageInstall::BackupDirectory() const {return impl_->transaction;}
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
