#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

namespace ttplayer::app {

inline constexpr wchar_t kDspWorkerSwitch[] = L"--ttplayer-dsp-worker";
inline constexpr wchar_t kOutputDeviceWorkerSwitch[] = L"--ttplayer-output-device-worker";

// argv[0] is either the standalone development tool or the private switch.
// embedded must also be propagated to DSP scan's per-DLL child processes.
int RunDspWorker(int count, wchar_t** arguments, bool embedded);
int RunOutputDeviceWorker(int count, wchar_t** arguments);

inline std::filesystem::path CurrentExecutablePath() {
    std::wstring name(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, name.data(),
                                         static_cast<DWORD>(name.size()));
    if (!size || size >= name.size()) return {};
    name.resize(size);
    return name;
}

// Windows argv quoting, including quotes and trailing backslashes. Never
// resolve worker executables through CWD, PATH or a hard-coded player name.
inline std::wstring QuoteWorkerArgument(std::wstring_view value) {
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

inline std::wstring WorkerCommandPrefix(const std::filesystem::path& executable,
                                        std::wstring_view mode) {
    auto command = QuoteWorkerArgument(executable.wstring());
    if (!mode.empty()) {
        command.push_back(L' ');
        command += QuoteWorkerArgument(mode);
    }
    return command;
}

} // namespace ttplayer::app
