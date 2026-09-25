param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$output = Join-Path $BuildDirectory 'Release'
$executable = Join-Path $output 'TTPlayerRebuild.exe'
$report = Join-Path $output 'legacy-imports.json'
if (-not (Test-Path -LiteralPath $executable) -or -not (Test-Path -LiteralPath $report)) {
    throw 'Build and audit the universal Release player before packaging.'
}
$audit = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
if ($audit.minimum_subsystem -ne '5.01' -or $audit.architecture -ne 'x86' -or
    $audit.inventories -notcontains '5.1.2600.txt' -or $audit.inventories -notcontains '6.1.7600.txt') {
    throw 'The import report is not an XP / Win7 audit.'
}
if (@($audit.imports.'ttpcomm.dll').Count -ne 1 -or $audit.imports.'ttpcomm.dll'[0] -cne '#3') {
    throw 'The universal EXE must load ttpcomm.dll at startup to initialize its XP TLS.'
}
$hash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -cne $audit.sha256) { throw 'The EXE changed after the import audit; rebuild it.' }
$package = Join-Path $BuildDirectory ('player-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath $executable -Destination $package
"$hash  TTPlayerRebuild.exe" | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding UTF8
$parent = Split-Path ([IO.Path]::GetFullPath($Destination)) -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null
Compress-Archive -LiteralPath (Join-Path $package 'TTPlayerRebuild.exe'),
    (Join-Path $package 'SHA256SUMS.txt') -DestinationPath $Destination -Force
Write-Output "Universal package: $Destination"
