# Backward-compatible script entry point; both names package the universal EXE.
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Destination
)
& (Join-Path $PSScriptRoot 'package_player.ps1') -BuildDirectory $BuildDirectory -Destination $Destination
