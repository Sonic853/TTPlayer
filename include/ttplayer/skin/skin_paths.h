#pragma once

#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>

namespace ttplayer::skin {

// Selectors are relative to the EXE's Skin directory. Keep the legacy
// filename-only fallback for other/absolute paths, but retain the supported
// new/ namespace so identically named BMP/PNG packages cannot share profiles.
[[nodiscard]] inline std::filesystem::path NormalizeSkinPackageName(std::wstring_view selector) {
    const auto path = std::filesystem::path(selector).lexically_normal();
    auto name = path.filename();
    if (name.empty() || name == L"." || name == L"..") return {};
    if (name.extension().empty()) name += L".skn";
    if (!path.has_root_path() &&
        _wcsicmp(path.parent_path().lexically_normal().c_str(), L"new") == 0)
        return std::filesystem::path(L"new") / name;
    return name;
}

[[nodiscard]] inline std::filesystem::path ResolveSkinPackagePath(
    const std::filesystem::path& skin_directory, std::wstring_view selector) {
    if (skin_directory.empty() || selector.empty() ||
        _wcsicmp(std::wstring(selector).c_str(), L"<Default_Skin>") == 0) return {};
    const auto name = NormalizeSkinPackageName(selector);
    return name.empty() ? std::filesystem::path{} : skin_directory / name;
}

[[nodiscard]] inline std::wstring SkinPackageSelector(
    const std::filesystem::path& skin_directory, const std::filesystem::path& package) {
    const auto new_directory = (skin_directory / L"new").lexically_normal();
    if (_wcsicmp(package.parent_path().lexically_normal().c_str(), new_directory.c_str()) == 0)
        return (std::filesystem::path(L"new") / package.filename()).wstring();
    return package.filename().wstring();
}

[[nodiscard]] inline std::array<std::filesystem::path, 2> SkinSearchDirectories(
    const std::filesystem::path& skin_directory) {
    if (skin_directory.empty()) return {};
    return {skin_directory, skin_directory / L"new"};
}

} // namespace ttplayer::skin
