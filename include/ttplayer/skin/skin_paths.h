#pragma once

#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>

namespace ttplayer::skin {

[[nodiscard]] inline bool IsNativeSkinPackage(const std::filesystem::path& path) {
    const auto extension=path.extension().wstring();
    return extension.empty() || _wcsicmp(extension.c_str(),L".skn")==0 ||
           _wcsicmp(extension.c_str(),L".zip")==0;
}

[[nodiscard]] inline bool IsRelativeSkinPath(const std::filesystem::path& path) {
    if(path.empty() || path.has_root_path()) return false;
    for(const auto& part:path) {
        if(part==L".." || part.native().find_first_of(L":*?\"<>|")!=std::wstring::npos) return false;
    }
    return true;
}

// Configuration uses a complete Skin-relative selector. Provider directories
// and file suffixes are declared by the DLL, never inferred by the settings reader.
[[nodiscard]] inline std::filesystem::path NormalizePluginSkinPackageName(std::wstring_view selector) {
    const std::filesystem::path path(selector);
    if(!IsRelativeSkinPath(path)) return {};
    const auto normalized=path.lexically_normal();
    if(normalized.parent_path().empty() || normalized.parent_path()==L"." ||
       normalized.filename().empty() || normalized.filename()==L"." || IsNativeSkinPackage(normalized)) return {};
    return normalized;
}

// Native filename-only resolution is retained; valid relative namespaces are
// kept verbatim so each plugin can own a different child directory.
[[nodiscard]] inline std::filesystem::path NormalizeSkinPackageName(std::wstring_view selector) {
    const auto path=std::filesystem::path(selector).lexically_normal();
    auto name=path.filename();
    if(name.empty() || name==L"." || name==L"..") return {};
    if(name.extension().empty()) name+=L".skn";
    if(IsRelativeSkinPath(std::filesystem::path(selector)) &&
       (!IsNativeSkinPackage(name) || _wcsicmp(path.parent_path().c_str(),L"new")==0)) return path.parent_path()/name;
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
    auto parent=package.parent_path().lexically_normal();
    const auto root=skin_directory.lexically_normal();
    auto relative=package.filename();
    while(!parent.empty() && _wcsicmp(parent.c_str(),root.c_str())!=0) {
        const auto next=parent.parent_path();
        if(next==parent) break;
        relative=parent.filename()/relative;parent=next;
    }
    if(!parent.empty() && _wcsicmp(parent.c_str(),root.c_str())==0 && IsRelativeSkinPath(relative)) {
        if(!IsNativeSkinPackage(package)) return relative.wstring();
        if(_wcsicmp(relative.parent_path().c_str(),L"new")==0)
            return (std::filesystem::path(L"new")/package.filename()).wstring();
    }
    return package.filename().wstring();
}

[[nodiscard]] inline std::array<std::filesystem::path, 2> SkinSearchDirectories(
    const std::filesystem::path& skin_directory) {
    if (skin_directory.empty()) return {};
    return {skin_directory, skin_directory / L"new"};
}

} // namespace ttplayer::skin
