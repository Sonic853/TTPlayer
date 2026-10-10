#pragma once

namespace ttplayer::build {
// Same yyyy.MM.dd[pN] tag as the EXE/package. About keeps its legacy resource.
[[nodiscard]] const wchar_t* Version() noexcept;
[[nodiscard]] const char* VersionUtf8() noexcept;
}
