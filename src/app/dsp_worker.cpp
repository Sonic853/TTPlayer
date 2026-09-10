#include "ttplayer/app/worker_process.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace {

constexpr std::uint32_t kRequestMagic = 0x51534454; // TDSQ
constexpr std::uint32_t kResultMagic = 0x52534454;  // TDSR
constexpr std::uint32_t kProtocolVersion = 1;
constexpr DWORD kPerPluginTimeoutMilliseconds = 1500;

struct RequestEntry {
    std::uint32_t kind{}; // 0 = explicit DLL, 1 = directory
    std::filesystem::path path;
};

struct ProbeResult {
    std::filesystem::path path;
    std::wstring description;
};

struct WinampDspHeader {
    int version;
    const char* description;
    void* (__cdecl* get_module)(int);
};

struct WinampDspModule {
    const char* description;
    HWND parent;
    HINSTANCE instance;
    void (__cdecl* configure)(WinampDspModule*);
};

bool ReadExact(HANDLE file, void* destination, DWORD bytes) {
    auto* cursor = static_cast<std::byte*>(destination);
    while (bytes != 0) {
        DWORD read{};
        if (!ReadFile(file, cursor, bytes, &read, nullptr) || read == 0)
            return false;
        cursor += read;
        bytes -= read;
    }
    return true;
}

bool WriteExact(HANDLE file, const void* source, DWORD bytes) {
    const auto* cursor = static_cast<const std::byte*>(source);
    while (bytes != 0) {
        DWORD written{};
        if (!WriteFile(file, cursor, bytes, &written, nullptr) || written == 0)
            return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

bool ReadWideString(HANDLE file, std::wstring& value) {
    std::uint32_t size{};
    if (!ReadExact(file, &size, sizeof(size)) || size > 32768) return false;
    value.resize(size);
    return size == 0 || ReadExact(file, value.data(), size * sizeof(wchar_t));
}

bool WriteWideString(HANDLE file, std::wstring_view value) {
    if (value.size() > 32768) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return WriteExact(file, &size, sizeof(size)) &&
           (size == 0 || WriteExact(file, value.data(),
                                    size * sizeof(wchar_t)));
}

std::vector<RequestEntry> ReadRequest(const std::filesystem::path& path) {
    std::vector<RequestEntry> entries;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return entries;
    std::uint32_t magic{}, version{}, count{};
    if (!ReadExact(file, &magic, sizeof(magic)) ||
        !ReadExact(file, &version, sizeof(version)) ||
        !ReadExact(file, &count, sizeof(count)) || magic != kRequestMagic ||
        version != kProtocolVersion || count > 4096) {
        CloseHandle(file);
        return entries;
    }
    entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        RequestEntry entry;
        std::wstring value;
        if (!ReadExact(file, &entry.kind, sizeof(entry.kind)) ||
            entry.kind > 1 || !ReadWideString(file, value)) {
            entries.clear();
            break;
        }
        entry.path = std::move(value);
        entries.push_back(std::move(entry));
    }
    CloseHandle(file);
    return entries;
}

bool WriteResults(const std::filesystem::path& path,
                  const std::vector<ProbeResult>& results) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::uint32_t count = static_cast<std::uint32_t>(results.size());
    bool good = WriteExact(file, &kResultMagic, sizeof(kResultMagic)) &&
                WriteExact(file, &kProtocolVersion,
                           sizeof(kProtocolVersion)) &&
                WriteExact(file, &count, sizeof(count));
    for (const auto& result : results) {
        const auto full = result.path.wstring();
        good = good && WriteWideString(file, full) &&
               WriteWideString(file, result.description);
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return good;
}

std::wstring AnsiDescription(const char* description) {
    if (!description) return {};
    const size_t length = strnlen_s(description, 32768);
    if (length == 0 || length == 32768) return {};
    const int required = MultiByteToWideChar(
        CP_ACP, 0, description, static_cast<int>(length), nullptr, 0);
    if (required <= 0) return {};
    std::wstring converted(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_ACP, 0, description, static_cast<int>(length),
                        converted.data(), required);
    return converted;
}

WinampDspHeader* LoadHeader(const std::filesystem::path& plugin,
                            HMODULE& module) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    const auto directory = plugin.parent_path().wstring();
    if (!directory.empty()) SetDllDirectoryW(directory.c_str());
    module = LoadLibraryW(plugin.c_str());
    if (!module) return nullptr;
    const auto get_header = reinterpret_cast<WinampDspHeader* (__cdecl*)()>(
        GetProcAddress(module, "winampDSPGetHeader2"));
    if (!get_header) return nullptr;
    WinampDspHeader* header = get_header();
    return header && header->version > 0x1f ? header : nullptr;
}

int ProbeOne(const std::filesystem::path& plugin,
             const std::filesystem::path& output) {
    HMODULE module{};
    WinampDspHeader* header = LoadHeader(plugin, module);
    if (!header) {
        if (module) FreeLibrary(module);
        return 2;
    }
    const std::vector<ProbeResult> result{{plugin, AnsiDescription(
                                                       header->description)}};
    const bool written = WriteResults(output, result);
    if (module) FreeLibrary(module);
    return written ? 0 : 3;
}

std::wstring Quote(std::wstring_view argument) {
    return ttplayer::app::QuoteWorkerArgument(argument);
}

std::filesystem::path TemporaryFile() {
    std::array<wchar_t, MAX_PATH + 1> folder{};
    std::array<wchar_t, MAX_PATH + 1> file{};
    if (!GetTempPathW(static_cast<DWORD>(folder.size()), folder.data()) ||
        !GetTempFileNameW(folder.data(), L"tdp", 0, file.data())) return {};
    return file.data();
}

std::vector<ProbeResult> ReadResults(const std::filesystem::path& path) {
    std::vector<ProbeResult> results;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return results;
    std::uint32_t magic{}, version{}, count{};
    if (ReadExact(file, &magic, sizeof(magic)) &&
        ReadExact(file, &version, sizeof(version)) &&
        ReadExact(file, &count, sizeof(count)) && magic == kResultMagic &&
        version == kProtocolVersion && count <= 1) {
        for (std::uint32_t index = 0; index < count; ++index) {
            std::wstring path_value;
            ProbeResult result;
            if (!ReadWideString(file, path_value) ||
                !ReadWideString(file, result.description)) {
                results.clear();
                break;
            }
            result.path = std::move(path_value);
            results.push_back(std::move(result));
        }
    }
    CloseHandle(file);
    return results;
}

void AppendCandidate(std::vector<std::filesystem::path>& candidates,
                     std::filesystem::path path) {
    if (path.empty()) return;
    std::error_code error;
    path = std::filesystem::absolute(path, error);
    const auto value = path.wstring();
    if (_wcsicmp(path.extension().c_str(), L".dll") != 0 ||
        _wcsnicmp(path.filename().c_str(), L"dsp_", 4) != 0) return;
    const auto duplicate = std::find_if(candidates.begin(), candidates.end(),
        [&value](const auto& existing) {
            return _wcsicmp(existing.c_str(), value.c_str()) == 0;
        });
    if (duplicate == candidates.end()) candidates.push_back(std::move(path));
}

void AppendDirectory(std::vector<std::filesystem::path>& candidates,
                     const std::filesystem::path& directory) {
    if (directory.empty()) return;
    WIN32_FIND_DATAW data{};
    const auto pattern = (directory / L"dsp_*.dll").wstring();
    const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            AppendCandidate(candidates, directory / data.cFileName);
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

std::vector<ProbeResult> Scan(const std::filesystem::path& executable,
                              const std::vector<RequestEntry>& request,
                              bool embedded) {
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : request) {
        if (entry.kind == 0) AppendCandidate(candidates, entry.path);
        else AppendDirectory(candidates, entry.path);
    }

    std::vector<ProbeResult> accepted;
    for (const auto& candidate : candidates) {
        const auto scratch = TemporaryFile();
        if (scratch.empty()) continue;
        std::wstring command = ttplayer::app::WorkerCommandPrefix(
                                   executable, embedded
                                       ? ttplayer::app::kDspWorkerSwitch : L"") +
                               L" --one " +
                               Quote(candidate.wstring()) + L" " +
                               Quote(scratch.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        const auto working = candidate.parent_path().wstring();
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr,
                           working.empty() ? nullptr : working.c_str(),
                           &startup, &process)) {
            CloseHandle(process.hThread);
            const DWORD wait = WaitForSingleObject(
                process.hProcess, kPerPluginTimeoutMilliseconds);
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                WaitForSingleObject(process.hProcess, 250);
                // A few old plug-ins hang only during DLL_PROCESS_DETACH.
                // ProbeOne flushes the validated header first, so retain that
                // result after terminating the expendable helper process.
                auto result = ReadResults(scratch);
                if (!result.empty()) accepted.push_back(std::move(result[0]));
            } else if (wait == WAIT_OBJECT_0) {
                DWORD exit_code{};
                if (GetExitCodeProcess(process.hProcess, &exit_code) &&
                    exit_code == 0) {
                    auto result = ReadResults(scratch);
                    if (!result.empty()) accepted.push_back(std::move(result[0]));
                }
            }
            CloseHandle(process.hProcess);
        }
        DeleteFileW(scratch.c_str());
    }
    return accepted;
}

int Configure(const std::filesystem::path& plugin, HWND parent) {
    HMODULE module{};
    WinampDspHeader* header = LoadHeader(plugin, module);
    if (!header || !header->get_module) {
        if (module) FreeLibrary(module);
        return 2;
    }
    auto* dsp = static_cast<WinampDspModule*>(header->get_module(0));
    if (!dsp || !dsp->configure) {
        FreeLibrary(module);
        return 3;
    }
    dsp->parent = parent;
    dsp->instance = module;
    dsp->configure(dsp);
    FreeLibrary(module);
    return 0;
}

} // namespace

int ttplayer::app::RunDspWorker(int argc, wchar_t** argv, bool embedded) {
    if (argc == 4 && _wcsicmp(argv[1], L"--one") == 0)
        return ProbeOne(argv[2], argv[3]);
    if (argc == 4 && _wcsicmp(argv[1], L"--scan") == 0) {
        const auto executable = CurrentExecutablePath();
        if (executable.empty()) return 4;
        const auto results = Scan(executable, ReadRequest(argv[2]), embedded);
        return WriteResults(argv[3], results) ? 0 : 5;
    }
    if (argc == 4 && _wcsicmp(argv[1], L"--configure") == 0) {
        const auto numeric = static_cast<ULONG_PTR>(_wcstoui64(argv[3], nullptr, 10));
        return Configure(argv[2], reinterpret_cast<HWND>(numeric));
    }
    return 1;
}
