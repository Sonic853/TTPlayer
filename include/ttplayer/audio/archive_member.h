#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace ttplayer::audio {

enum class ArchiveKind { zip, rar };

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
// ZIP is read directly; RAR uses the Windows archive namespace already
// required by the rebuild's importer, but is materialized only into an
// implementation cache.
[[nodiscard]] bool ParseArchiveMemberPath(std::wstring_view logical_path,
                                          ArchiveMemberPath& result);
[[nodiscard]] std::filesystem::path MakeArchiveMemberPath(
    const std::filesystem::path& archive, std::wstring_view member);
[[nodiscard]] std::vector<unsigned char> ReadArchiveMember(
    const ArchiveMemberPath& path, HMODULE ttpcomm);
[[nodiscard]] std::vector<std::wstring> ListRarArchiveMembers(
    const std::filesystem::path& archive);

} // namespace ttplayer::audio
