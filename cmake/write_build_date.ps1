[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    # Explicit timestamps are for deterministic boundary tests. Normal CMake
    # builds omit this parameter and always use the current build instant.
    [DateTimeOffset]$Timestamp = [DateTimeOffset]::UtcNow
)

$ErrorActionPreference = 'Stop'
$date = $Timestamp.ToOffset([TimeSpan]::FromHours(8)).ToString(
    'yyyy-M-d', [Globalization.CultureInfo]::InvariantCulture)
$content = @"
// Generated at build time using Beijing time (UTC+08:00). Do not edit.
#pragma once

namespace ttplayer::build {
inline constexpr wchar_t kCompletionDate[] = L"$date";
}
"@
$content = $content.Replace("`r`n", "`n") + "`n"
$path = [IO.Path]::GetFullPath($OutputPath)
if (-not [IO.File]::Exists($path) -or
    [IO.File]::ReadAllText($path) -cne $content) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($false))
}
