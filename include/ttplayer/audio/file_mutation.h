#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <windows.h>

namespace ttplayer::audio {

// A cheap physical-file revision, shared by the player and its isolated
// writers. Attributes/access time are deliberately excluded: a read or the
// manual scanner's read-only override does not change the source audio.
struct FileSourceVersion {
    BY_HANDLE_FILE_INFORMATION info{};
    bool valid{};
};

[[nodiscard]] HRESULT ReadFileSourceVersion(
    const std::filesystem::path& path, FileSourceVersion& version) noexcept;
[[nodiscard]] bool SameFileSourceVersion(
    const FileSourceVersion& left, const FileSourceVersion& right) noexcept;
[[nodiscard]] std::wstring FileSourceVersionToken(const FileSourceVersion& version);
[[nodiscard]] std::wstring CaptureFileSourceVersion(const std::filesystem::path& path);

// The path mutex survives atomic replacement; the physical-ID mutex also
// coordinates existing aliases. Both are recursive Windows mutexes so the
// worker can retain the transaction through verification while a built-in or
// plug-in writer joins it. Always acquire before opening a metadata snapshot.
class FileMutationGuard {
public:
    FileMutationGuard() = default;
    ~FileMutationGuard();
    FileMutationGuard(const FileMutationGuard&) = delete;
    FileMutationGuard& operator=(const FileMutationGuard&) = delete;
    [[nodiscard]] HRESULT Acquire(const std::filesystem::path& path,
                                  std::stop_token stop = {}, DWORD timeout_ms = 10000) noexcept;
    [[nodiscard]] HRESULT Validate(HANDLE source = INVALID_HANDLE_VALUE) const noexcept;
    [[nodiscard]] const FileSourceVersion& Version() const noexcept { return version_; }

private:
    void Reset() noexcept;
    std::filesystem::path path_;
    HANDLE path_mutex_{};
    HANDLE identity_mutex_{};
    FileSourceVersion version_;
};

} // namespace ttplayer::audio
