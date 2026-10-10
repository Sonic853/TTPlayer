#include "ttplayer/core/build_version.h"
#include "ttplayer/build_version.h"

namespace ttplayer::build {
const wchar_t* Version() noexcept { return kVersion; }
const char* VersionUtf8() noexcept { return kVersionUtf8; }
}
