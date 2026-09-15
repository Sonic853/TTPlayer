#pragma once
#include "file_info_probe_protocol.h"

namespace ttplayer::ui::detail {
// Two reusable packets, synchronized by auto-reset events. No handles are
// inherited and no synchronous pipe read can outlive the per-song deadline.
struct PlaylistInfoSessionRequest {
    std::uint64_t id{};
    std::wstring path;
    int subtrack{};
    FileInfoProbeMp3Policy mp3;
};
inline bool WritePlaylistInfoSessionRequest(const std::filesystem::path& path,
                                           const PlaylistInfoSessionRequest& request) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool okay = FileInfoProbeWriteHeader(file, 5) &&
        FileInfoProbeWriteScalar(file, request.id) &&
        FileInfoProbeWriteWide(file, request.path) &&
        FileInfoProbeWriteScalar(file, request.subtrack) &&
        FileInfoProbeWriteMp3Policy(file, request.mp3);
    CloseHandle(file);
    return okay;
}
inline bool ReadPlaylistInfoSessionRequest(const std::filesystem::path& path,
                                          PlaylistInfoSessionRequest& request) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool okay = FileInfoProbeReadHeader(file, 5) &&
        FileInfoProbeReadScalar(file, request.id) && request.id != 0 &&
        FileInfoProbeReadWide(file, request.path) &&
        FileInfoProbeReadScalar(file, request.subtrack) && request.subtrack >= 0 &&
        FileInfoProbeReadMp3Policy(file, request.mp3);
    CloseHandle(file);
    return okay;
}
inline bool WritePlaylistInfoSessionResult(const std::filesystem::path& path,
    std::uint64_t id, const FileInfoProbeReadResult& result) {
    if (!WriteFileInfoProbeReadResult(path, result)) return false;
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool okay = FileInfoProbeWriteScalar(file, id);
    CloseHandle(file);
    return okay;
}
inline bool ReadPlaylistInfoSessionResult(const std::filesystem::path& path,
    std::uint64_t expected_id, FileInfoProbeReadResult& result) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}, offset{}; offset.QuadPart = -static_cast<LONGLONG>(sizeof(expected_id));
    std::uint64_t id{};
    const bool okay = GetFileSizeEx(file, &size) && size.QuadPart >= 8 && size.QuadPart <= 8 * 1024 * 1024 &&
        SetFilePointerEx(file, offset, nullptr, FILE_END) && FileInfoProbeReadScalar(file, id) && id == expected_id;
    CloseHandle(file);
    return okay && ReadFileInfoProbeReadResult(path, result) && result.cover.empty();
}
} // namespace ttplayer::ui::detail
