#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <objidl.h>
#include <stdexcept>
#include <functional>
#include <memory>

namespace ttplayer::audio {

enum class ArchiveKind { zip, rar, tar, gzip, tar_gzip, seven_zip };
[[nodiscard]] bool IsArchivePath(const std::filesystem::path& path);

struct ArchiveMemberPath {
    std::filesystem::path archive;
    std::wstring member;
    ArchiveKind kind{ArchiveKind::zip};
};

// TTPlayer 5.7.9 represents an archive member as one logical path.  It only
// recognizes the marker following the archive suffix, rather than treating a
// bare '|' as a general path separator.
struct ZipMemberPath {
    std::filesystem::path archive;
    std::wstring member;
};

[[nodiscard]] bool ParseZipMemberPath(std::wstring_view logical_path,
                                      ZipMemberPath& result);
[[nodiscard]] std::filesystem::path MakeZipMemberPath(
    const std::filesystem::path& archive, std::wstring_view member);

// FUN_0041EF26 converts central-directory names with OemToCharBuffW and then
// normalizes ZIP separators to Win32 separators.
[[nodiscard]] std::wstring DecodeZipMemberName(std::string_view encoded_name);

// Opens the outer ZIP and returns the requested member without materializing
// it on disk.  Matching follows the original case-insensitive member lookup.
[[nodiscard]] std::vector<unsigned char> ReadZipMember(
    const ZipMemberPath& path, HMODULE ttpcomm);

// The original archive dispatcher (004738B5) exposes the same persistent
// `archive|member` contract for its ZIP and RAR implementations.  Member
// spelling is retained (apart from slash direction) because 0047E177 compares
// it to the enumerated name exactly and does not canonicalize `.` or `..`.
// Music ZIP and RAR4/RAR5 use the rebuilt ttpcomm archive stream service.
// Enumeration never extracts payloads or depends on a Windows Shell namespace.
class ArchiveError final : public std::runtime_error {
public:
    explicit ArchiveError(HRESULT result);
    HRESULT Result() const noexcept { return result_; }
    std::wstring Message() const;
private:
    HRESULT result_;
};
[[nodiscard]] bool ParseArchiveMemberPath(std::wstring_view logical_path,
                                          ArchiveMemberPath& result);
[[nodiscard]] std::filesystem::path MakeArchiveMemberPath(
    const std::filesystem::path& archive, std::wstring_view member);
// A process-local verified identity of all actual archive volumes. Capture can
// discover volumes on a cache miss: call before starting a long member reader.
// Fresh and TryReadCachedArchiveMember only inspect known file identities; they
// never enumerate/decompress an archive or prompt for a password/volume.
class ArchiveCacheIdentityState;
using ArchiveCacheIdentity = std::shared_ptr<const ArchiveCacheIdentityState>;
[[nodiscard]] ArchiveCacheIdentity CaptureArchiveCacheIdentity(
    const std::filesystem::path& archive, HMODULE ttpcomm = nullptr,
    const std::function<bool()>& canceled = {});
[[nodiscard]] bool ArchiveCacheIdentityFresh(
    const ArchiveCacheIdentity& identity,
    const std::function<bool()>& canceled = {}) noexcept;
[[nodiscard]] bool TryReadCachedArchiveMember(
    const ArchiveMemberPath& path, std::vector<unsigned char>& bytes,
    ArchiveCacheIdentity* identity = nullptr,
    const std::function<bool()>& canceled = {}) noexcept;
[[nodiscard]] std::vector<unsigned char> ReadArchiveMember(
    const ArchiveMemberPath& path, HMODULE ttpcomm,
    const std::function<bool()>& canceled = {});
// Embedded thumbnails remain authoritative. This fallback searches the member's
// directory only, so a multi-album archive cannot borrow another album's cover.
// OSZ instead uses the background referenced by a .osu for this audio member.
[[nodiscard]] std::vector<unsigned char> ReadArchiveSidecarCover(
    const std::filesystem::path& media, HMODULE ttpcomm,
    const std::function<bool()>& canceled = {});
// The caller owns one reference; the stream remains valid independently of
// enumeration and does not expose any extraction/cache path to the playlist.
[[nodiscard]] IStream* OpenArchiveMemberStream(
    const ArchiveMemberPath& path, HMODULE ttpcomm,
    const std::function<bool()>& canceled = {});
[[nodiscard]] int ArchiveMemberProgress(IStream* stream, HMODULE ttpcomm,
                                       HRESULT* result = nullptr);
[[nodiscard]] std::vector<std::wstring> ListArchiveMembers(
    const std::filesystem::path& archive, HMODULE ttpcomm = nullptr,
    const std::function<bool()>& canceled = {},
    const std::function<void(uint64_t, uint64_t)>& progress = {},
    std::filesystem::path* resolved_archive = nullptr);
[[nodiscard]] std::vector<std::wstring> ListRarArchiveMembers(
    const std::filesystem::path& archive, HMODULE ttpcomm = nullptr);

} // namespace ttplayer::audio
