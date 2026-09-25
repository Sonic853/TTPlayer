param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$output = Join-Path $BuildDirectory 'Release'
$executable = Join-Path $output 'TTPlayerRebuild.exe'
$updater = Join-Path $output 'TTPUpdater.exe'
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
$updaterAudit = Get-Content -LiteralPath (Join-Path $output 'updater-imports.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$updaterHash = (Get-FileHash -LiteralPath $updater -Algorithm SHA256).Hash.ToLowerInvariant()
if ($updaterAudit.sha256 -cne $updaterHash -or $updaterAudit.minimum_subsystem -ne '5.01' -or
    $updaterAudit.architecture -ne 'x86' -or
    $updaterAudit.inventories -notcontains '5.1.2600.txt' -or $updaterAudit.inventories -notcontains '6.1.7600.txt') {
    throw 'Build and audit TTPUpdater.exe before packaging.'
}
if ((Get-Item -LiteralPath $executable).VersionInfo.FileVersion -cne
    (Get-Item -LiteralPath $updater).VersionInfo.FileVersion) {
    throw 'Player and updater versions must match.'
}
Copy-Item -LiteralPath $updater -Destination $package
$https = Join-Path $output 'AddIn/ttp_https.dll'
$component = Get-Content -LiteralPath (Join-Path $output 'https-component.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$httpsHash = (Get-FileHash -LiteralPath $https -Algorithm SHA256).Hash.ToLowerInvariant()
if ($component.repository -cne 'https://github.com/Sonic853/TTPlayerHttps' -or
    $component.sha256 -cne $httpsHash -or
    $component.inventories -notcontains '5.1.2600.txt' -or $component.inventories -notcontains '6.1.7600.txt') {
    throw 'Download and verify the HTTPS release component before packaging.'
}
New-Item -ItemType Directory -Path (Join-Path $package 'AddIn') | Out-Null
Copy-Item -LiteralPath $https -Destination (Join-Path $package 'AddIn/ttp_https.dll')
"$hash  TTPlayerRebuild.exe`n$updaterHash  TTPUpdater.exe`n$httpsHash  AddIn/ttp_https.dll" | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding UTF8
$parent = Split-Path ([IO.Path]::GetFullPath($Destination)) -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null
# Explicit names preserve AddIn/ without directory entries or unrelated files.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporary = $Destination + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
$zip = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($name in @('TTPlayerRebuild.exe', 'TTPUpdater.exe', 'AddIn/ttp_https.dll', 'SHA256SUMS.txt')) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $package $name), $name,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
Move-Item -LiteralPath $temporary -Destination $Destination -Force
Write-Output "Universal package: $Destination"
