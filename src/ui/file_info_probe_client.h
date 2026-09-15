#pragma once

#include "file_info_probe_protocol.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <windows.h>

namespace ttplayer::ui::detail {

// The player opts in at startup because its own entry point understands the
// private worker switch. Other hosts/tests may keep using a standalone helper.
void EnableEmbeddedFileInfoProbe() noexcept;

enum class FileInfoProbeProcessState {
    completed,
    launch_failed,
    process_failed,
    timed_out,
    cancelled,
    wait_failed
};

struct FileInfoProbeProcessResult {
    FileInfoProbeProcessState state{FileInfoProbeProcessState::launch_failed};
    DWORD exit_code{ERROR_GEN_FAILURE};

    [[nodiscard]] bool Succeeded() const noexcept {
        return state == FileInfoProbeProcessState::completed && exit_code == 0;
    }
};

[[nodiscard]] std::filesystem::path ProbeTemporaryFile();

// Empty helper selects the current EXE's private mode. An explicit helper is
// supported only for development hosts which have not opted into that mode.
[[nodiscard]] FileInfoProbeProcessResult RunFileInfoProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments, DWORD timeout_milliseconds);

[[nodiscard]] std::optional<FileInfoProbeReadResult> RunFileInfoReadProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& logical_path,
    const std::filesystem::path& ttpcomm_path,
    DWORD timeout_milliseconds = 15000,
    FileInfoProbeProcessState* process_state = nullptr,
    const FileInfoProbeMp3Policy& mp3_policy = {});

[[nodiscard]] std::optional<FileInfoProbeReadResult>
RunPlaylistInfoReadProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& logical_path,
    const std::filesystem::path& ttpcomm_path, int subtrack,
    DWORD timeout_milliseconds = 15000,
    FileInfoProbeProcessState* process_state = nullptr,
    const FileInfoProbeMp3Policy& mp3_policy = {});

// One caller thread per session. A bounded child and its lazily loaded plug-ins
// survive successive reads; cancellation/failure discards the entire child.
class PlaylistInfoProbeSession {
public:
    PlaylistInfoProbeSession();
    ~PlaylistInfoProbeSession();
    PlaylistInfoProbeSession(const PlaylistInfoProbeSession&) = delete;
    PlaylistInfoProbeSession& operator=(const PlaylistInfoProbeSession&) = delete;
    std::optional<FileInfoProbeReadResult> Read(
        std::stop_token stop, const std::filesystem::path& helper,
        const std::filesystem::path& addin_directory,
        const std::filesystem::path& logical_path,
        const std::filesystem::path& ttpcomm_path, int subtrack,
        DWORD timeout_milliseconds = 15000,
        FileInfoProbeProcessState* process_state = nullptr,
        const FileInfoProbeMp3Policy& mp3_policy = {});
    struct Statistics { size_t launches{}, reads{}, cache_hits{}; };
    [[nodiscard]] Statistics Stats() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ttplayer::ui::detail
