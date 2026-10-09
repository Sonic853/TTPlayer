#pragma once

#include <filesystem>
#include <windows.h>

namespace ttplayer {

// A commit failure must keep its destination and error until the UI has
// decided whether to retry. Success never means merely "save was attempted".
struct SaveResult {
    std::filesystem::path path;
    HRESULT error{S_OK};
    explicit operator bool() const noexcept { return SUCCEEDED(error); }

    static SaveResult Win32Failure(const std::filesystem::path& path,
                                  DWORD error = GetLastError()) {
        return {path, HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT)};
    }
};

} // namespace ttplayer
