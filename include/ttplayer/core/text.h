#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ttplayer::core {
std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
std::string ReadUtf8Text(const std::filesystem::path& path);
void WriteUtf8Text(const std::filesystem::path& path, std::string_view text);
}
