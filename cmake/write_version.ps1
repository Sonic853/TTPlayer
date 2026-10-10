[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Template,
    [Parameter(Mandatory)][string]$OutputPath,
    [string]$HeaderPath,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$build = Get-PlayerBuildVersion $Version
$fixed = '{0},{1},{2},{3}' -f $build.Date.Year,$build.Date.Month,$build.Date.Day,$build.Patch
$content = (Get-Content -LiteralPath $Template -Encoding UTF8 -Raw).
    Replace('@PLAYER_FIXED_VERSION@', $fixed).Replace('@PLAYER_BUILD_VERSION@', $build.Name)
if ($content -match '@[A-Z_]+@') { throw 'Unresolved version resource placeholder.' }
$path = [IO.Path]::GetFullPath($OutputPath)
if (-not [IO.File]::Exists($path) -or [IO.File]::ReadAllText($path) -cne $content) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($false))
}
if ($HeaderPath) {
    # The C++ identity and PE resource must use the same resolved release tag,
    # including pN. Do not derive the UI version from the completion date.
    $header = @"
// Generated together with version.rc. Do not edit.
#pragma once
namespace ttplayer::build {
inline constexpr wchar_t kVersion[] = L"$($build.Name)";
inline constexpr char kVersionUtf8[] = "$($build.Name)";
}
"@
    $header = $header.Replace("`r`n", "`n") + "`n"
    $path = [IO.Path]::GetFullPath($HeaderPath)
    if (-not [IO.File]::Exists($path) -or [IO.File]::ReadAllText($path) -cne $header) {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path, $header, [Text.UTF8Encoding]::new($false))
    }
}
