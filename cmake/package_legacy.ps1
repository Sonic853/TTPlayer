param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$source = Split-Path $PSScriptRoot -Parent
$output = Join-Path $BuildDirectory 'Release'
$executable = Join-Path $output 'TTPlayerRebuild.exe'
$report = Join-Path $output 'legacy-imports.json'
if (-not (Test-Path -LiteralPath $executable) -or -not (Test-Path -LiteralPath $report)) {
    throw 'Build and audit the legacy Release player before packaging.'
}
$audit = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
if ($audit.minimum_subsystem -ne '5.01' -or $audit.architecture -ne 'x86' -or
    $audit.inventories -notcontains '5.1.2600.txt' -or $audit.inventories -notcontains '6.1.7600.txt') {
    throw 'The import report is not an XP / Win7 audit.'
}
$hash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -cne $audit.sha256) { throw 'The EXE changed after the import audit; rebuild it.' }
$package = Join-Path $BuildDirectory ('legacy-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath $executable, $report -Destination $package
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination $package
Copy-Item -LiteralPath (Join-Path $source 'docs/LEGACY_WINDOWS.md') -Destination $package
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'legacy-licenses/YY-Thunks-LICENSE.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $source 'docs/licenses/VC-LTL-LICENSE.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $source 'docs/licenses/legacy-third-party.md') -Destination $licenses
"$hash  TTPlayerRebuild.exe" | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding UTF8
$parent = Split-Path ([IO.Path]::GetFullPath($Destination)) -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $Destination -Force
Write-Output "Legacy package: $Destination"
