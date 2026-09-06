#pragma once

#include <filesystem>
#include <span>

namespace ttplayer::ui {

inline std::filesystem::path PreferredDialogHistory(
    const std::filesystem::path& specific,
    const std::filesystem::path& fallback) {
    return specific.empty() ? fallback : specific;
}

inline std::filesystem::path FileSelectionHistory(
    std::span<const std::filesystem::path> paths) {
    if (paths.empty()) return {};
    if (paths.size() == 1) return paths.front();
    auto directory = paths.front().parent_path().wstring();
    if (!directory.empty() && directory.back() != L'\\' &&
        directory.back() != L'/') {
        directory.push_back(L'\\');
    }
    return directory;
}

} // namespace ttplayer::ui
