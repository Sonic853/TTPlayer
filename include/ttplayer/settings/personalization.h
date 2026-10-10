#pragma once
#include <string>
#include <string_view>

namespace ttplayer::settings {
struct PlayerSettings;
inline constexpr size_t kPersonalNameLimit = 64;
std::wstring NormalizePersonalName(std::wstring_view name);
std::wstring DecodeLegacyPersonalName(std::wstring_view word, std::wstring_view md5);
// Only UserWord/MD5 are persisted. Reject names CP936 cannot represent exactly,
// preserving the previous name; empty names explicitly clear both fields.
bool SetPersonalName(PlayerSettings& player, std::wstring_view name);
}
