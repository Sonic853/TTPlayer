#include "ttplayer/audio/archive_member.h"

#include "ttplayer/skin/skin_package.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <comdef.h>
#include <shldisp.h>
#include <shellapi.h>
#include <shlobj.h>
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

void HashByte(std::uint64_t& hash, unsigned char value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

std::filesystem::path RarCacheDirectory(
    const std::filesystem::path& archive) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(archive, error);
    if (error) absolute = archive;
    const auto identity = absolute.lexically_normal().wstring();
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t character : identity) {
        const auto folded = static_cast<std::uint16_t>(towlower(character));
        HashByte(hash, static_cast<unsigned char>(folded));
        HashByte(hash, static_cast<unsigned char>(folded >> 8));
    }
    error.clear();
    const auto size = std::filesystem::file_size(archive, error);
    if (!error) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            HashByte(hash, static_cast<unsigned char>(size >> shift));
    }
    error.clear();
    const auto modified = std::filesystem::last_write_time(archive, error);
    if (!error) {
        const auto ticks = static_cast<std::uint64_t>(
            modified.time_since_epoch().count());
        for (unsigned shift = 0; shift < 64; shift += 8)
            HashByte(hash, static_cast<unsigned char>(ticks >> shift));
    }
    wchar_t key[17]{};
    swprintf_s(key, L"%016llx", static_cast<unsigned long long>(hash));
    return std::filesystem::temp_directory_path() / L"TTPlayerRebuild" /
        L"ImportedArchives" / key;
}

struct DirectorySnapshot {
    std::uint64_t files{};
    std::uint64_t bytes{};

    bool operator==(const DirectorySnapshot&) const = default;
};

DirectorySnapshot SnapshotDirectory(const std::filesystem::path& directory) {
    DirectorySnapshot snapshot;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error &&
            _wcsicmp(iterator->path().filename().c_str(), L".ttp-complete") != 0) {
            ++snapshot.files;
            snapshot.bytes += iterator->file_size(error);
        }
        error.clear();
        iterator.increment(error);
    }
    return snapshot;
}

std::filesystem::path EnsureRarExtracted(
    const std::filesystem::path& archive) {
    static std::mutex extraction_mutex;
    std::scoped_lock lock(extraction_mutex);
    const auto destination = RarCacheDirectory(archive);
    const auto marker = destination / L".ttp-complete";
    std::error_code error;
    if (std::filesystem::is_regular_file(marker, error) && !error)
        return destination;
    error.clear();
    std::filesystem::create_directories(destination, error);
    if (error) throw std::runtime_error("cannot create RAR member cache");

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        throw std::runtime_error("COM is unavailable for RAR extraction");
    struct ScopedComApartment {
        bool owns{};
        ~ScopedComApartment() { if (owns) CoUninitialize(); }
    } apartment{SUCCEEDED(initialized)};

    Microsoft::WRL::ComPtr<IShellDispatch> shell;
    Microsoft::WRL::ComPtr<Folder> source;
    Microsoft::WRL::ComPtr<Folder> target;
    Microsoft::WRL::ComPtr<FolderItems> items;
    if (FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(shell.GetAddressOf()))) || !shell) {
        throw std::runtime_error("Windows archive namespace is unavailable");
    }
    _variant_t source_path(std::filesystem::absolute(archive).wstring().c_str());
    _variant_t target_path(std::filesystem::absolute(destination).wstring().c_str());
    if (FAILED(shell->NameSpace(source_path, source.GetAddressOf())) || !source ||
        FAILED(shell->NameSpace(target_path, target.GetAddressOf())) || !target ||
        FAILED(source->Items(items.GetAddressOf())) || !items) {
        throw std::runtime_error("Windows cannot open this RAR archive");
    }
    long source_count{};
    if (FAILED(items->get_Count(&source_count)))
        throw std::runtime_error("cannot enumerate RAR archive");
    _variant_t content(static_cast<IDispatch*>(items.Get()));
    _variant_t options(static_cast<long>(
        FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT));
    if (FAILED(target->CopyHere(content, options))) {
        throw std::runtime_error("RAR extraction failed");
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    DirectorySnapshot previous{};
    unsigned stable_polls{};
    bool complete = source_count == 0;
    while (!complete && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto current = SnapshotDirectory(destination);
        if (current.files != 0 && current == previous) {
            ++stable_polls;
        } else {
            stable_polls = 0;
            previous = current;
        }
        complete = stable_polls >= 15; // 750 ms with no size/count change.
    }
    if (!complete) throw std::runtime_error("RAR extraction timed out");
    std::ofstream complete_marker(marker, std::ios::binary | std::ios::trunc);
    if (!complete_marker.put('\n'))
        throw std::runtime_error("cannot finalize RAR member cache");
    return destination;
}

} // namespace

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

std::vector<unsigned char> ReadArchiveMember(
    const ArchiveMemberPath& path, HMODULE ttpcomm) {
    if (path.kind == ArchiveKind::zip)
        return ReadZipMember({path.archive, path.member}, ttpcomm);

    // 0047E177 enumerates the archive and compares the requested member with
    // _wcsicmp.  In particular, it does not fold `directory\\..\\file` before
    // looking it up.  Resolve the extracted RAR member through the same exact
    // name match so the filesystem cannot silently canonicalize that spelling.
    const auto members = ListRarArchiveMembers(path.archive);
    const auto found = std::ranges::find_if(members, [&](const auto& member) {
        return _wcsicmp(member.c_str(), path.member.c_str()) == 0;
    });
    if (found == members.end()) throw std::runtime_error("RAR member not found");
    const auto root = EnsureRarExtracted(path.archive);
    const auto file = root / NormalizeArchiveMember(*found);
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("RAR member not found");
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

std::vector<std::wstring> ListRarArchiveMembers(
    const std::filesystem::path& archive) {
    const auto root = EnsureRarExtracted(archive);
    std::vector<std::wstring> members;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error &&
            _wcsicmp(iterator->path().filename().c_str(), L".ttp-complete") != 0) {
            const auto relative = std::filesystem::relative(iterator->path(), root,
                                                             error);
            if (!error && !relative.empty()) members.push_back(relative.native());
        }
        error.clear();
        iterator.increment(error);
    }
    std::ranges::sort(members, [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    return members;
}

} // namespace ttplayer::audio
