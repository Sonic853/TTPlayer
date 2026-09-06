#pragma once

#include <cwchar>
#include <filesystem>
#include <span>
#include <string_view>

namespace ttplayer::ui::detail {

// CFileInfoDlg originally offered BMP/JPEG/GIF. PNG is an intentional
// compatibility extension, kept in one policy shared by the UI and the
// isolated metadata helper so the picker and MIME passed to AddIns cannot
// drift apart.
[[nodiscard]] inline bool IsSupportedCoverPath(
    const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return _wcsicmp(extension.c_str(), L".bmp") == 0 ||
           _wcsicmp(extension.c_str(), L".jpeg") == 0 ||
           _wcsicmp(extension.c_str(), L".jpg") == 0 ||
           _wcsicmp(extension.c_str(), L".gif") == 0 ||
           _wcsicmp(extension.c_str(), L".png") == 0;
}

[[nodiscard]] inline std::wstring_view CoverMimeType(
    std::span<const unsigned char> bytes) noexcept {
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
        bytes[2] == 0xff)
        return L"image/jpeg";
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
        bytes[2] == 'N' && bytes[3] == 'G' && bytes[4] == 0x0d &&
        bytes[5] == 0x0a && bytes[6] == 0x1a && bytes[7] == 0x0a)
        return L"image/png";
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
        return L"image/bmp";
    if (bytes.size() >= 6 &&
        ((bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
          bytes[3] == '8' && bytes[4] == '7' && bytes[5] == 'a') ||
         (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
          bytes[3] == '8' && bytes[4] == '9' && bytes[5] == 'a')))
        return L"image/gif";
    return {};
}

} // namespace ttplayer::ui::detail
