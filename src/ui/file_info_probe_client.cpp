#include "file_info_probe_client.h"
#include "playlist_info_session_protocol.h"
#include "ttplayer/app/file_info_worker.h"
#include "ttplayer/app/worker_process.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <list>
#include <cwctype>
#include <objbase.h>
#include <string_view>

namespace ttplayer::ui::detail {
namespace {
std::atomic_bool embedded_worker_enabled{};

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
    return app::QuoteWorkerArgument(value);
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

void EnableEmbeddedFileInfoProbe() noexcept {
    embedded_worker_enabled.store(true, std::memory_order_relaxed);
}

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
    const bool embedded = helper.empty() ||
        embedded_worker_enabled.load(std::memory_order_relaxed);
    // Prefer our own matching worker code, even if an older standalone helper
    // was left beside the executable. Copying/renaming the player remains safe.
    const auto executable = embedded ? app::CurrentExecutablePath() : helper;
    std::error_code path_error;
    if (executable.empty() ||
        !std::filesystem::is_regular_file(executable, path_error) || path_error)
        return result;

    std::wstring command = QuoteProbeArgument(executable.wstring());
    if (embedded) {
        command.push_back(L' ');
        command += app::kFileInfoWorkerSwitch;
    }
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += QuoteProbeArgument(argument);
    }

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION raw_process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
            executable.parent_path().c_str(), &startup, &raw_process))
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

struct PlaylistInfoProbeSession::State {
    UniqueHandle process, job, incoming, outgoing;
    std::filesystem::path request, output, helper, addin, comm;
    Statistics stats;
    ULONGLONG last_use{};
    std::uint64_t next_id{};
    struct Stamp {
        DWORD size_high{}, size_low{};
        FILETIME modified{}, created{};
        bool operator==(const Stamp& rhs) const noexcept {
            return size_high == rhs.size_high && size_low == rhs.size_low &&
                CompareFileTime(&modified, &rhs.modified) == 0 &&
                CompareFileTime(&created, &rhs.created) == 0;
        }
    };
    struct Cached {
        std::wstring key;
        Stamp stamp;
        FileInfoProbeReadResult result;
        size_t bytes{};
    };
    std::list<Cached> cache;
    size_t cache_bytes{};
#ifdef TTPLAYER_LEGACY_WINDOWS
    static constexpr size_t cache_limit = 128, byte_limit = 4 * 1024 * 1024;
#else
    static constexpr size_t cache_limit = 512, byte_limit = 16 * 1024 * 1024;
#endif
    ~State() { Reset(); }
    void Reset() noexcept {
        if (process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProbe(job.Get(), process.Get(), ERROR_CANCELLED);
            static_cast<void>(WaitForSingleObject(process.Get(), 1000));
        }
        process.Reset(); job.Reset(); incoming.Reset(); outgoing.Reset();
        if (!request.empty()) DeleteFileW(request.c_str());
        if (!output.empty()) DeleteFileW(output.c_str());
        request.clear(); output.clear();
    }
    static std::optional<Stamp> Fingerprint(const std::filesystem::path& path, int subtrack) {
        // A CUE/archive can depend on multiple files. Do not cache it using
        // only the container's stamp, which would miss changes to the audio.
        if (subtrack || path.native().find(L'|') != std::wstring::npos ||
            path.native().find(L"://") != std::wstring::npos || !path.is_absolute() ||
            path.native().starts_with(L"\\\\")) return {};
        // Keep potentially blocking network filesystem probes inside the
        // cancellable child as well (including mapped network drives).
        const auto drive = GetDriveTypeW(path.root_path().c_str());
        if (drive != DRIVE_FIXED && drive != DRIVE_RAMDISK) return {};
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
        return Stamp{data.nFileSizeHigh, data.nFileSizeLow,
            data.ftLastWriteTime, data.ftCreationTime};
    }
    bool Start(const std::filesystem::path& selected_helper,
               const std::filesystem::path& selected_addin,
               const std::filesystem::path& selected_comm) {
        Reset();
        helper = selected_helper; addin = selected_addin; comm = selected_comm;
        const bool embedded = helper.empty() || embedded_worker_enabled.load(std::memory_order_relaxed);
        const auto executable = embedded ? app::CurrentExecutablePath() : helper;
        GUID guid{};
        wchar_t identifier[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, identifier, 40)) return false;
        const std::wstring prefix = std::wstring(L"Local\\TTPlayer.Info.") + identifier;
        const auto in_name = prefix + L".request", out_name = prefix + L".result";
        incoming.Reset(CreateEventW(nullptr, FALSE, FALSE, in_name.c_str()));
        outgoing.Reset(CreateEventW(nullptr, FALSE, FALSE, out_name.c_str()));
        request = ProbeTemporaryFile(); output = ProbeTemporaryFile();
        if (!incoming || !outgoing || request.empty() || output.empty()) { Reset(); return false; }
        std::wstring command = QuoteProbeArgument(executable.wstring());
        if (embedded) command += L" " + std::wstring(app::kFileInfoWorkerSwitch);
        for (const auto& argument : std::vector<std::wstring>{L"playlist-session", addin.wstring(),
                comm.wstring(), request.wstring(), output.wstring(), in_name, out_name})
            command += L" " + QuoteProbeArgument(argument);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION raw{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, executable.parent_path().c_str(),
                &startup, &raw)) { Reset(); return false; }
        process.Reset(raw.hProcess);
        UniqueHandle thread(raw.hThread);
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits,
                    sizeof(limits)) || !AssignProcessToJobObject(job.Get(), process.Get())) job.Reset();
        }
        if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) { Reset(); return false; }
        ++stats.launches;
        return true;
    }
};

PlaylistInfoProbeSession::PlaylistInfoProbeSession() : state_(std::make_unique<State>()) {}
PlaylistInfoProbeSession::~PlaylistInfoProbeSession() = default;
PlaylistInfoProbeSession::Statistics PlaylistInfoProbeSession::Stats() const noexcept { return state_->stats; }

std::optional<FileInfoProbeReadResult> PlaylistInfoProbeSession::Read(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin, const std::filesystem::path& path,
    const std::filesystem::path& comm, int subtrack, DWORD timeout,
    FileInfoProbeProcessState* process_state, const FileInfoProbeMp3Policy& mp3) {
    auto& s = *state_;
    const auto fail = [&](FileInfoProbeProcessState state) -> std::optional<FileInfoProbeReadResult> {
        if (process_state) *process_state = state;
        s.Reset();
        return {};
    };
    if (stop.stop_requested()) return fail(FileInfoProbeProcessState::cancelled);
    if (s.helper != helper || s.addin != addin || s.comm != comm) {
        s.Reset(); s.cache.clear(); s.cache_bytes = 0;
        s.helper = helper; s.addin = addin; s.comm = comm;
    }
    auto key = path.lexically_normal().native();
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    key += L"\x1f" + std::to_wstring(subtrack) + L":" + std::to_wstring(mp3.read_priority) +
        L":" + std::to_wstring(mp3.write_type) + L":" + std::to_wstring(mp3.id3v2_encoding) +
        L":" + std::to_wstring(mp3.id3v2_padding);
    const auto stamp = State::Fingerprint(path, subtrack);
    for (auto item = s.cache.begin(); item != s.cache.end(); ++item) {
        if (item->key != key) continue;
        if (stamp && item->stamp == *stamp) {
            ++s.stats.cache_hits;
            s.cache.splice(s.cache.begin(), s.cache, item);
            if (process_state) *process_state = FileInfoProbeProcessState::completed;
            return s.cache.front().result;
        }
        s.cache_bytes -= item->bytes; s.cache.erase(item); break;
    }
    // Restart well before the worker's idle deadline to avoid its exit racing
    // the next request; process failure still discards both packet files.
    if (!s.process || WaitForSingleObject(s.process.Get(), 0) != WAIT_TIMEOUT ||
        GetTickCount64() - s.last_use >= 45000) {
        if (!s.Start(helper, addin, comm)) return fail(FileInfoProbeProcessState::launch_failed);
    }
    const auto deadline = GetTickCount64() + timeout;
    const auto request_id = ++s.next_id;
    if (!WritePlaylistInfoSessionRequest(s.request, {request_id, path.native(), subtrack, mp3}) ||
        !SetEvent(s.incoming.Get())) return fail(FileInfoProbeProcessState::wait_failed);
    ++s.stats.reads;
    HANDLE waits[]{s.outgoing.Get(), s.process.Get()};
    for (;;) {
        if (stop.stop_requested()) return fail(FileInfoProbeProcessState::cancelled);
        const auto now = GetTickCount64();
        if (now >= deadline) return fail(FileInfoProbeProcessState::timed_out);
        const auto wait = WaitForMultipleObjects(2, waits, FALSE,
            static_cast<DWORD>(std::min<ULONGLONG>(50, deadline - now)));
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) return fail(FileInfoProbeProcessState::process_failed);
        if (wait != WAIT_TIMEOUT) return fail(FileInfoProbeProcessState::wait_failed);
    }
    s.last_use = GetTickCount64();
    FileInfoProbeReadResult result;
    if (!ReadPlaylistInfoSessionResult(s.output, request_id, result))
        return fail(FileInfoProbeProcessState::process_failed);
    const auto after = State::Fingerprint(path, subtrack);
    if (stamp && (!after || !(*stamp == *after)))
        return fail(FileInfoProbeProcessState::cancelled); // File changed during decoding; retry later.
    if (stamp && SUCCEEDED(result.status)) {
        size_t memory = sizeof(State::Cached) + key.size() * sizeof(wchar_t) +
            result.codec.size() * sizeof(wchar_t) + result.metadata.size() * sizeof(FileInfoProbeMetadata);
        for (const auto& field : result.metadata) memory += (field.name.size() + field.value.size()) * sizeof(wchar_t);
        if (memory <= State::byte_limit) {
            while (!s.cache.empty() && (s.cache.size() >= State::cache_limit ||
                    s.cache_bytes + memory > State::byte_limit)) {
                s.cache_bytes -= s.cache.back().bytes; s.cache.pop_back();
            }
            s.cache.push_front({std::move(key), *stamp, result, memory});
            s.cache_bytes += memory;
        }
    }
    if (process_state) *process_state = FileInfoProbeProcessState::completed;
    return result;
}

} // namespace ttplayer::ui::detail
