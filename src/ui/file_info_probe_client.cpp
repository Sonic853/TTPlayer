#include "file_info_probe_client.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace ttplayer::ui::detail {
namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Release() noexcept {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void Reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{};
};

std::wstring QuoteProbeArgument(std::wstring_view value) {
    std::wstring quoted(1, L'"');
    size_t slashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

void TerminateProbe(HANDLE job, HANDLE process, DWORD reason) noexcept {
    if (!job || !TerminateJobObject(job, reason))
        static_cast<void>(TerminateProcess(process, reason));
}

std::optional<FileInfoProbeReadResult> RunReadMode(
    std::stop_token stop, const std::filesystem::path& helper,
    std::vector<std::wstring> arguments, DWORD timeout_milliseconds,
    FileInfoProbeProcessState* process_state,
    const FileInfoProbeMp3Policy& mp3_policy) {
    const auto request = ProbeTemporaryFile();
    const auto output = ProbeTemporaryFile();
    if (request.empty() || output.empty()) {
        if (!request.empty()) DeleteFileW(request.c_str());
        if (!output.empty()) DeleteFileW(output.c_str());
        if (process_state)
            *process_state = FileInfoProbeProcessState::launch_failed;
        return std::nullopt;
    }
    struct RemoveOutput {
        std::filesystem::path path;
        ~RemoveOutput() {
            if (!path.empty()) DeleteFileW(path.c_str());
        }
    } remove_output{output}, remove_request{request};
    FileInfoProbeReadRequest encoded_request;
    encoded_request.mp3 = mp3_policy;
    if (!WriteFileInfoProbeReadRequest(request, encoded_request)) {
        if (process_state)
            *process_state = FileInfoProbeProcessState::launch_failed;
        return std::nullopt;
    }
    arguments.push_back(request.wstring());
    arguments.push_back(output.wstring());
    const auto run = RunFileInfoProbe(
        stop, helper, arguments, timeout_milliseconds);
    if (process_state) *process_state = run.state;
    if (!run.Succeeded()) return std::nullopt;
    FileInfoProbeReadResult decoded;
    if (!ReadFileInfoProbeReadResult(output, decoded)) return std::nullopt;
    return decoded;
}

} // namespace

std::filesystem::path ProbeTemporaryFile() {
    wchar_t directory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(std::size(directory)), directory);
    if (length == 0 || length >= std::size(directory)) return {};
    wchar_t path[MAX_PATH + 1]{};
    if (!GetTempFileNameW(directory, L"tfp", 0, path)) return {};
    return path;
}

FileInfoProbeProcessResult RunFileInfoProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments, DWORD timeout_milliseconds) {
    FileInfoProbeProcessResult result;
    std::error_code path_error;
    if (helper.empty() ||
        !std::filesystem::is_regular_file(helper, path_error) || path_error)
        return result;

    std::wstring command = QuoteProbeArgument(helper.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += QuoteProbeArgument(argument);
    }

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION raw_process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
            helper.parent_path().c_str(), &startup, &raw_process))
        return result;
    UniqueHandle process(raw_process.hProcess);
    UniqueHandle thread(raw_process.hThread);
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.Get(),
                JobObjectExtendedLimitInformation, &limits,
                sizeof(limits)) ||
            !AssignProcessToJobObject(job.Get(), process.Get())) {
            job.Reset();
        }
    }

    if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) {
        TerminateProbe(job.Get(), process.Get(), ERROR_GEN_FAILURE);
        static_cast<void>(WaitForSingleObject(process.Get(), 1000));
        result.state = FileInfoProbeProcessState::wait_failed;
        return result;
    }
    thread.Reset();

    const ULONGLONG deadline = GetTickCount64() + timeout_milliseconds;
    DWORD wait = WAIT_TIMEOUT;
    while (wait == WAIT_TIMEOUT && !stop.stop_requested()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        wait = WaitForSingleObject(process.Get(), static_cast<DWORD>(
            std::min<ULONGLONG>(50, deadline - now)));
    }
    if (wait != WAIT_OBJECT_0) {
        const bool cancelled = stop.stop_requested();
        const DWORD reason = cancelled ? ERROR_CANCELLED :
            wait == WAIT_FAILED ? ERROR_GEN_FAILURE : ERROR_TIMEOUT;
        TerminateProbe(job.Get(), process.Get(), reason);
        static_cast<void>(WaitForSingleObject(process.Get(), 1000));
        result.state = cancelled
            ? FileInfoProbeProcessState::cancelled
            : wait == WAIT_FAILED
                ? FileInfoProbeProcessState::wait_failed
                : FileInfoProbeProcessState::timed_out;
        result.exit_code = reason;
        return result;
    }

    if (!GetExitCodeProcess(process.Get(), &result.exit_code)) {
        result.state = FileInfoProbeProcessState::wait_failed;
    } else {
        result.state = result.exit_code == 0
            ? FileInfoProbeProcessState::completed
            : FileInfoProbeProcessState::process_failed;
    }
    return result;
}

std::optional<FileInfoProbeReadResult> RunFileInfoReadProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& logical_path,
    const std::filesystem::path& ttpcomm_path,
    DWORD timeout_milliseconds,
    FileInfoProbeProcessState* process_state,
    const FileInfoProbeMp3Policy& mp3_policy) {
    return RunReadMode(stop, helper,
        {L"read", addin_directory.wstring(), logical_path.wstring(),
         ttpcomm_path.wstring()}, timeout_milliseconds, process_state,
        mp3_policy);
}

std::optional<FileInfoProbeReadResult> RunPlaylistInfoReadProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& logical_path,
    const std::filesystem::path& ttpcomm_path, int subtrack,
    DWORD timeout_milliseconds,
    FileInfoProbeProcessState* process_state,
    const FileInfoProbeMp3Policy& mp3_policy) {
    return RunReadMode(stop, helper,
        {L"playlist-read", addin_directory.wstring(), logical_path.wstring(),
         ttpcomm_path.wstring(), std::to_wstring(subtrack)},
        timeout_milliseconds, process_state, mp3_policy);
}

} // namespace ttplayer::ui::detail
