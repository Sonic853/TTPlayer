#include "ttplayer/audio/archive_member.h"

#include "ttplayer/skin/skin_package.h"
#include <ttpcomm/runtime_client.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <exception>
#include <wrl/client.h>

namespace ttplayer::audio {
namespace {

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

size_t FindRarMarker(std::wstring_view value) noexcept {
    if (value.size() < 6) return std::wstring_view::npos;
    for (size_t index = 1; index + 5 <= value.size(); ++index) {
        if (value[index] == L'.' &&
            towlower(value[index + 1]) == L'r' &&
            towlower(value[index + 2]) == L'a' &&
            towlower(value[index + 3]) == L'r' &&
            value[index + 4] == L'|') {
            return index;
        }
    }
    return std::wstring_view::npos;
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

TtpCommArchiveApi ArchiveApi(HMODULE module) {
    TtpCommArchiveApi api{};
    if(module) {
        if(ttpcomm::host::QueryArchive(module,api)) return api;
    } else if(const auto* loaded=ttpcomm::host::Archive()) return *loaded;
    throw ArchiveError(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
}

} // namespace

ArchiveError::ArchiveError(HRESULT result)
    : std::runtime_error("Archive operation failed: " + std::to_string(static_cast<unsigned long>(result))),result_(result) {}

std::wstring ArchiveError::Message() const {
    if(result_==TTPCOMM_ARCHIVE_PASSWORD) return L"压缩包需要密码，当前播放器未提供密码输入。";
    if(result_==TTPCOMM_ARCHIVE_BAD_PASSWORD) return L"压缩包密码不正确。";
    if(result_==TTPCOMM_ARCHIVE_MISSING_VOLUME) return L"压缩包缺少分卷，请将全部分卷放在同一目录。";
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
    const auto package = skin::SkinPackage::Open(path.archive);
    std::wstring requested = path.member;
    std::replace(requested.begin(), requested.end(), L'/', L'\\');
    for (const auto& entry : package.Entries()) {
        const auto decoded = DecodeZipMemberName(entry.name);
        if (_wcsicmp(decoded.c_str(), requested.c_str()) == 0)
            return package.ReadEntry(entry.name, ttpcomm);
    }
    throw std::runtime_error("ZIP member not found");
}

bool ParseArchiveMemberPath(std::wstring_view logical_path,
                            ArchiveMemberPath& result) {
    const size_t zip = FindZipMarker(logical_path);
    const size_t rar = FindRarMarker(logical_path);
    const bool use_zip = zip != std::wstring_view::npos &&
        (rar == std::wstring_view::npos || zip < rar);
    const size_t marker = use_zip ? zip : rar;
    if (marker == std::wstring_view::npos || marker + 5 >= logical_path.size())
        return false;
    result.archive = std::filesystem::path(logical_path.substr(0, marker + 4));
    result.member.assign(logical_path.substr(marker + 5));
    std::replace(result.member.begin(), result.member.end(), L'/', L'\\');
    result.kind = use_zip ? ArchiveKind::zip : ArchiveKind::rar;
    return !result.archive.empty() && !result.member.empty();
}

std::filesystem::path MakeArchiveMemberPath(
    const std::filesystem::path& archive, std::wstring_view member) {
    auto extension = archive.extension().wstring();
    std::ranges::transform(extension, extension.begin(), towlower);
    if (extension != L".zip" && extension != L".rar")
        throw std::runtime_error("unsupported archive path");
    std::wstring logical = archive.wstring();
    logical.push_back(L'|');
    logical.append(NormalizeArchiveMember(member).native());
    return std::filesystem::path(std::move(logical));
}

IStream* OpenArchiveMemberStream(const ArchiveMemberPath& path, HMODULE ttpcomm) {
    IStream* stream{};
    if(path.kind==ArchiveKind::rar) {
        const auto api=ArchiveApi(ttpcomm);
        const HRESULT result=api.open_member(path.archive.c_str(),path.member.c_str(),nullptr,&stream);
        if(FAILED(result)) throw ArchiveError(result);
        if(!stream) throw ArchiveError(E_UNEXPECTED);
        return stream;
    }
    const auto bytes=ReadZipMember({path.archive,path.member},ttpcomm);
    if(bytes.size()>ULONG_MAX) throw ArchiveError(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    HRESULT result=CreateStreamOnHGlobal(nullptr,TRUE,&stream);
    if(FAILED(result)) throw ArchiveError(result);
    Microsoft::WRL::ComPtr<IStream> owner; owner.Attach(stream);
    ULONG written{};
    result=stream->Write(bytes.data(),static_cast<ULONG>(bytes.size()),&written);
    if(FAILED(result) || written!=bytes.size()) throw ArchiveError(FAILED(result) ? result : STG_E_WRITEFAULT);
    LARGE_INTEGER zero{};
    result=stream->Seek(zero,STREAM_SEEK_SET,nullptr);
    if(FAILED(result)) throw ArchiveError(result);
    return owner.Detach();
}

std::vector<unsigned char> ReadArchiveMember(const ArchiveMemberPath& path, HMODULE ttpcomm) {
    // This buffered adapter is for CUE/text consumers. Audio readers use the
    // seekable member stream directly, including its bounded disk backing.
    if(path.kind==ArchiveKind::zip) return ReadZipMember({path.archive,path.member},ttpcomm);
    const auto api=ArchiveApi(ttpcomm);
    TtpCommArchiveOptions options{}; options.size=sizeof(options);
    options.member_limit_bytes=64ULL*1024*1024;
    Microsoft::WRL::ComPtr<IStream> stream;
    HRESULT result=api.open_member(path.archive.c_str(),path.member.c_str(),&options,stream.GetAddressOf());
    if(FAILED(result)) throw ArchiveError(result);
    if(!stream) throw ArchiveError(E_UNEXPECTED);
    STATSTG stat{}; result=stream->Stat(&stat,STATFLAG_NONAME);
    if(FAILED(result)) throw ArchiveError(result);
    if(stat.cbSize.QuadPart>options.member_limit_bytes) throw ArchiveError(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    std::vector<unsigned char> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read{}; result=stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read);
    if(FAILED(result) || read!=bytes.size()) throw ArchiveError(FAILED(result) ? result : STG_E_READFAULT);
    return bytes;
}

std::vector<std::wstring> ListRarArchiveMembers(const std::filesystem::path& archive, HMODULE ttpcomm) {
    struct Context { std::vector<std::wstring> names; std::exception_ptr error; } context;
    const auto visitor=[](void* opaque,const TtpCommArchiveEntry* entry)->BOOL {
        auto& out=*static_cast<Context*>(opaque);
        try {
            if(!(entry->flags&(TTPCOMM_ARCHIVE_DIRECTORY|TTPCOMM_ARCHIVE_LINK))) out.names.emplace_back(entry->name);
            return TRUE;
        } catch(...) { out.error=std::current_exception(); return FALSE; }
    };
    const auto api=ArchiveApi(ttpcomm);
    const HRESULT result=api.enumerate(archive.c_str(),nullptr,visitor,&context);
    if(context.error) std::rethrow_exception(context.error);
    if(FAILED(result)) throw ArchiveError(result);
    // 00473338 and 00474051 retain the archive's order. Never alphabetize here.
    return std::move(context.names);
}

} // namespace ttplayer::audio
