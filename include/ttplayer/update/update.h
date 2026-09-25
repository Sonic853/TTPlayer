#pragma once
#include "ttplayer/settings/settings.h"
#include <array>
#include <compare>
#include <exception>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::update {
enum class Source { gitee = 0, github = 1 };
inline constexpr wchar_t kMissingHttps[] = L"缺少 HTTPS 组件，请前往发布页手动下载更新";
inline constexpr wchar_t kGitHubPage[] = L"https://github.com/Sonic853/TTPlayer/releases";
inline constexpr wchar_t kGiteePage[] = L"https://gitee.com/Sonic853/TTPlayer/releases";
inline constexpr UINT kPrepareInstallMessage = WM_APP + 0x351;
struct Version {
    std::array<unsigned,4> parts{};
    auto operator<=>(const Version&) const = default;
    std::wstring Text() const;
};
std::optional<Version> ParseVersion(std::string_view text);
Version FileVersion(const std::filesystem::path& file);
struct Release {
    Version version;
    std::string tag, asset_name;
    std::wstring page, zip_url, sums_url;
    uint64_t size{};
};
std::vector<Release> ParseReleases(std::string_view json, Source source);
std::string ExpectedHash(std::string_view sums, std::string_view name);
std::string Sha256(const std::filesystem::path& file);
using Cancel = std::function<bool()>;
using Progress = std::function<void(uint64_t,uint64_t)>;
class Http {
public:
    Http(std::filesystem::path runtime, settings::NetworkSettings network);
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;
    bool HasProvider() const noexcept;
    std::string Get(const std::wstring& url, const Cancel& canceled = {});
    void Download(const std::wstring& url, const std::filesystem::path& file,
                  uint64_t expected_size, const Cancel& canceled, const Progress& progress);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::optional<Release> Check(Http& http, Source source, const Cancel& canceled = {});
// Only player/updater payloads are installed. An optional bundled HTTPS DLL is
// checksum-verified and discarded; installed DLLs and settings remain unchanged.
std::filesystem::path PreparePackage(Http& http, const Release& release,
    const std::filesystem::path& staging, const Cancel& canceled, const Progress& progress);
void ExtractPackage(const std::filesystem::path& zip, const std::filesystem::path& staging);
void ReplacePlayer(const std::filesystem::path& directory,
                   const std::filesystem::path& prepared, const Version& expected);
void ReplaceUpdater(const std::filesystem::path& directory,
                    const std::filesystem::path& prepared, const Version& expected);
bool IsXp() noexcept;
std::filesystem::path ExecutablePath();
std::wstring ErrorText(const std::exception& error);
}
