#pragma once

#include <string_view>

namespace ttplayer::integrations {

// Public Discord application snowflake registered for the rebuilt player.
// It is not a client secret. A non-empty DiscordApplicationId in TTPlayerRebuild.xml
// may replace it for private builds with their own Discord application.
inline constexpr std::wstring_view kDefaultDiscordApplicationId =
    L"1546275976676376716";

} // namespace ttplayer::integrations
