[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Template,
    [Parameter(Mandatory)][string]$OutputPath,
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
