param([string]$Workflow = (Join-Path $PSScriptRoot '../.github/workflows/manual-build.yml'))
$ErrorActionPreference = 'Stop'

# Execute the workflow's own PowerShell with mocked APIs. No remote writes.
$text = Get-Content -LiteralPath $Workflow -Raw -Encoding UTF8
$blocks = [regex]::Matches($text, '(?m)^        run: \|\r?\n((?:          .*\r?\n|\r?\n)*)') |
    ForEach-Object { $_.Groups[1].Value -replace '(?m)^          ', '' }
foreach ($block in $blocks) {
    $tokens = $null; $errors = $null
    [void][Management.Automation.Language.Parser]::ParseInput($block, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Invalid workflow PowerShell: $errors" }
}
$validation = @($blocks | Where-Object { $_.Contains('$releaseDate =') })
$preparation = @($blocks | Where-Object { $_.Contains('$package = Join-Path build modern-package') })
$publication = @($blocks | Where-Object { $_.Contains('gh release create') })
if ($validation.Count -ne 1 -or $preparation.Count -ne 1 -or $publication.Count -ne 1) {
    throw 'Missing or ambiguous workflow steps.'
}
if ($text.Contains('inputs.release_version') -or $text -match '(?m)^      release_version:') {
    throw 'Manual version input must not remain.'
}
if (-not $text.Contains('group: manual-player-release') -or -not $text.Contains('needs.build.outputs.release_date')) {
    throw 'Publication must use a shared concurrency group and the captured build date.'
}
$publish = [scriptblock]::Create($publication[0])
$savedEnvironment = @{}
foreach ($name in @('RELEASE_DATE', 'BUILD_CONFIGURATION', 'GITHUB_SHA', 'GITHUB_REPOSITORY',
                     'GITHUB_SERVER_URL', 'RELEASE_INSTALL_NOTES', 'PUBLISH_RELEASE', 'PACKAGE_VERSION')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$savedExitCode = $global:LASTEXITCODE
try {
    foreach ($case in @(
        @('2026-09-04T15:59:59Z', 'Release', '2026.09.04', 'true'),
        @('2026-09-04T16:00:00Z', 'Release', '2026.09.05', 'true'),
        @('2026-12-31T16:00:00Z', 'Release', '2027.01.01', 'true'),
        @('2024-02-28T16:00:00Z', 'Release', '2024.02.29', 'true'),
        @('2026-09-04T16:00:00Z', 'Debug', '', 'true'),
        @('2026-09-04T16:00:00Z', 'RelWithDebInfo', '', 'true'),
        @('2026-09-04T16:00:00Z', 'Debug', '2026.09.05', 'false'),
        @('2026-09-04T16:00:00Z', 'RelWithDebInfo', '2026.09.05', 'false'))) {
        & {
            $capture = @{ value = '' }
            function Out-File {
                param([Parameter(ValueFromPipeline)]$InputObject, $FilePath, $Encoding, [switch]$Append)
                process { $capture.value = $InputObject }
            }
            $env:BUILD_CONFIGURATION = $case[1]
            $env:PUBLISH_RELEASE = $case[3]
            $code = $validation[0].Replace('[DateTimeOffset]::UtcNow', "([DateTimeOffset]'$($case[0])')")
            $succeeded = $true
            try { & ([scriptblock]::Create($code)) } catch { $succeeded = $false }
            if ($succeeded -ne [bool]$case[2] -or ($succeeded -and $capture.value -ne "value=$($case[2])")) {
                throw "Incorrect Beijing date/configuration result: $case"
            }
        }
    }
    $env:GITHUB_SHA = '0123456789012345678901234567890123456789'
    $env:GITHUB_REPOSITORY = 'fixture/TTPlayer'
    $env:GITHUB_SERVER_URL = 'https://github.com'
    # Exercise the actual ZIP operations in isolated directories with synthetic
    # EXE/audit inputs. A raw EXE must not leak into the uploaded artifact root.
    foreach ($configuration in @('Release', 'Debug')) {
        $fixture = Join-Path ([IO.Path]::GetTempPath()) ('TTPlayerReleasePackaging-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $fixture | Out-Null
        Push-Location $fixture
        try {
            $env:BUILD_CONFIGURATION = $configuration
            $env:PACKAGE_VERSION = '2026.09.05'
            foreach ($directory in @("build/$configuration", 'build-legacy/Release', 'build-legacy/legacy-licenses', 'cmake', 'docs/licenses')) {
                New-Item -ItemType Directory -Path $directory -Force | Out-Null
            }
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot package_legacy.ps1) -Destination cmake
            foreach ($file in @('LICENSE', 'docs/BUILDING.md', 'docs/LEGACY_WINDOWS.md',
                                'docs/licenses/VC-LTL-LICENSE.txt', 'docs/licenses/legacy-third-party.md')) {
                Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../$file") -Destination $file
            }
            'modern fixture' | Set-Content -LiteralPath "build/$configuration/TTPlayerRebuild.exe"
            'legacy fixture' | Set-Content -LiteralPath build-legacy/Release/TTPlayerRebuild.exe
            'license fixture' | Set-Content -LiteralPath build-legacy/legacy-licenses/YY-Thunks-LICENSE.txt
            if ($configuration -eq 'Release') { 'symbols' | Set-Content -LiteralPath "build/$configuration/TTPlayerRebuild.pdb" }
            $legacyHash = (Get-FileHash -LiteralPath build-legacy/Release/TTPlayerRebuild.exe -Algorithm SHA256).Hash.ToLowerInvariant()
            @{architecture='x86';minimum_subsystem='5.01';inventories=@('5.1.2600.txt','6.1.7600.txt');sha256=$legacyHash} |
                ConvertTo-Json | Set-Content -LiteralPath build-legacy/Release/legacy-imports.json
            & ([scriptblock]::Create($preparation[0])) | Out-Null
            $expectedHashes = @()
            foreach ($edition in @('modern', 'legacy')) {
                $name = if ($edition -eq 'modern') { 'TTPlayerRebuild-2026.09.05.zip' }
                        else { 'TTPlayerRebuild-XP-Win7-2026.09.05.zip' }
                $original = if ($edition -eq 'modern') { "build/$configuration/TTPlayerRebuild.exe" }
                            else { 'build-legacy/Release/TTPlayerRebuild.exe' }
                Expand-Archive -LiteralPath "artifact/$name" -DestinationPath "expanded/$edition"
                $originalHash = (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant()
                $extractedHash = (Get-FileHash -LiteralPath "expanded/$edition/TTPlayerRebuild.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
                $innerManifest = (Get-Content -LiteralPath "expanded/$edition/SHA256SUMS.txt" -Raw).Trim()
                if ($originalHash -cne $extractedHash -or $innerManifest -cne "$originalHash  TTPlayerRebuild.exe") {
                    throw "ZIP content or inner checksum mismatch: $edition"
                }
                $zipHash = (Get-FileHash -LiteralPath "artifact/$name" -Algorithm SHA256).Hash.ToLowerInvariant()
                $expectedHashes += "$zipHash  $name"
            }
            $actualHashes = (Get-Content -LiteralPath artifact/SHA256SUMS.txt -Raw).Trim() -replace "`r", ''
            $info = Get-Content -LiteralPath artifact/build-info.json -Raw | ConvertFrom-Json
            if ($actualHashes -cne ($expectedHashes -join "`n") -or $info.package_version -cne '2026.09.05' -or
                (Test-Path -LiteralPath artifact/TTPlayerRebuild.exe) -or
                (Test-Path -LiteralPath artifact/TTPlayerRebuild.pdb) -ne ($configuration -eq 'Release') -or
                -not (Test-Path -LiteralPath expanded/modern/LICENSE) -or
                -not (Test-Path -LiteralPath expanded/modern/BUILDING.md) -or
                -not (Test-Path -LiteralPath expanded/legacy/legacy-imports.json)) {
                throw "Incorrect prepared artifact: $configuration"
            }
        } finally { Pop-Location }
    }
    $notes = [regex]::Match($text, '(?m)^          RELEASE_INSTALL_NOTES: \|\r?\n((?:            .*\r?\n|\r?\n)*)')
    if (-not $notes.Success) { throw 'Installation notes are missing.' }
    $env:RELEASE_INSTALL_NOTES = $notes.Groups[1].Value -replace '(?m)^            ', ''
    $expectedInstallation = @'
使用该程序前确保本地已安装有 5.7.9 版本的千千静听，请按照以下步骤安装重建版：
1. Windows XP / Windows 7 下载并解压 TTPlayerRebuild-XP-Win7-{version}.zip；现代 Windows 下载并解压 TTPlayerRebuild-{version}.zip。
2. 打开开始菜单。
3. 打开所有程序——千千静听——右键千千静听程序图标。
4. 在菜单选择“打开文件位置”
5. 复制 TTPlayerRebuild.exe 到与 TTPlayer.exe 同一路径。
6. 启动 TTPlayerRebuild.exe
7. 享受重建版千千静听。
'@
    if (($env:RELEASE_INSTALL_NOTES -replace "`r", '').TrimEnd() -cne ($expectedInstallation -replace "`r", '').TrimEnd()) {
        throw 'Installation instructions changed.'
    }
    $scenarios = @('first', 'previous', 'same-day', 'numeric-patch', 'gaps', 'patch-only', 'draft',
        'many-tags', 'invalid-tags', 'wrong-artifact', 'wrong-configuration', 'wrong-hash', 'tag-api-error',
        'release-api-error', 'collision', 'invalid-date', 'exhausted', 'wrong-legacy-hash', 'wrong-legacy-configuration',
        'wrong-package-version', 'wrong-checksum-name')
    foreach ($scenario in $scenarios) {
        & {
            $env:RELEASE_DATE = '2026.09.05'
            $observed = @{ creates = 0; apis = 0; arguments = @(); notes = ''; hashes = ''; renames = @() }
            $tagNames = @('2026.01.03', '2026.09.04p2'); $releaseNames = @()
            $expectedVersion = '2026.09.05'; $previous = '2026.09.04p2'
            switch ($scenario) {
                'first' { $tagNames = @(); $previous = '' }
                'same-day' { $tagNames += '2026.09.05'; $expectedVersion += 'p1'; $previous = '2026.09.05' }
                'numeric-patch' { $tagNames += @('2026.09.05p2','2026.09.05p10','2026.09.05'); $expectedVersion += 'p11'; $previous = '2026.09.05p10' }
                'gaps' { $tagNames += @('2026.09.05','2026.09.05p3','2026.09.05p1'); $expectedVersion += 'p4'; $previous = '2026.09.05p3' }
                'patch-only' { $tagNames += '2026.09.05p1'; $expectedVersion += 'p2'; $previous = '2026.09.05p1' }
                'draft' { $releaseNames = @('2026.09.05','2026.09.05p2'); $expectedVersion += 'p3' }
                'many-tags' { $tagNames = @(1..150 | ForEach-Object { "other-$_" }) + '2026.09.05'; $expectedVersion += 'p1'; $previous = '2026.09.05' }
                'invalid-tags' { $tagNames += @('2099.02.30','2099.99.99','2026.09.05-beta','2026.09.05p0','v2026.09.05') }
                'invalid-date' { $env:RELEASE_DATE = '2026.02.30' }
                'exhausted' { $tagNames += '2026.09.05p9223372036854775807' }
            }
            function Get-Content {
                param($LiteralPath, [switch]$Raw)
                switch ($LiteralPath) {
                    'artifact/build-info.json' {
                        $commit = if ($scenario -eq 'wrong-artifact') { 'different' } else { $env:GITHUB_SHA }
                        $configuration = if ($scenario -eq 'wrong-configuration') { 'Debug' } else { 'Release' }
                        $legacy = if ($scenario -eq 'wrong-legacy-configuration') { 'Debug' } else { 'Release' }
                        $packageVersion = if ($scenario -eq 'wrong-package-version') { '2026.09.04' } else { '2026.09.05' }
                        @{commit=$commit;configuration=$configuration;legacy_configuration=$legacy;package_version=$packageVersion} | ConvertTo-Json
                    }
                    'artifact/SHA256SUMS.txt' {
                        $name = if ($scenario -eq 'wrong-checksum-name') { 'TTPlayerRebuild.exe' } else { 'TTPlayerRebuild-2026.09.05.zip' }
                        ('a' * 64) + "  $name`r`n" + ('c' * 64) + '  TTPlayerRebuild-XP-Win7-2026.09.05.zip'
                    }
                    default { throw "Unexpected file read: $LiteralPath" }
                }
            }
            function Get-FileHash {
                param($LiteralPath, $Algorithm)
                if ($Algorithm -ne 'SHA256') { throw 'Unexpected hash algorithm.' }
                switch ($LiteralPath) {
                    'artifact/TTPlayerRebuild-2026.09.05.zip' { @{Hash = $(if ($scenario -eq 'wrong-hash') { 'b' * 64 } else { 'a' * 64 })} }
                    'artifact/TTPlayerRebuild-XP-Win7-2026.09.05.zip' { @{Hash = $(if ($scenario -eq 'wrong-legacy-hash') { 'b' * 64 } else { 'c' * 64 })} }
                    default { throw 'Unexpected hash request.' }
                }
            }
            function Set-Content {
                param([Parameter(ValueFromPipeline)]$Value, $LiteralPath, $Encoding)
                process {
                    switch ($LiteralPath) {
                        'artifact/release-notes.md' { $observed.notes = $Value }
                        'artifact/SHA256SUMS.txt' { $observed.hashes = $Value }
                        default { throw 'Unexpected file write.' }
                    }
                }
            }
            function Rename-Item {
                param($LiteralPath, $NewName)
                $observed.renames += "$LiteralPath -> $NewName"
            }
            function gh {
                $global:LASTEXITCODE = 0
                if ($args[0] -eq 'api') {
                    ++$observed.apis
                    if ($args -notcontains '--paginate') { throw 'Must read all pages, not just the first 100 tags/releases.' }
                    if ($args[1] -eq "repos/$env:GITHUB_REPOSITORY/tags?per_page=100") {
                        if ($scenario -eq 'tag-api-error') { $global:LASTEXITCODE = 1; return }
                        $tagNames
                    } elseif ($args[1] -eq "repos/$env:GITHUB_REPOSITORY/releases?per_page=100") {
                        if ($scenario -eq 'release-api-error') { $global:LASTEXITCODE = 1; return }
                        $releaseNames
                    } else { throw 'Unexpected API request.' }
                } elseif ($args[0] -eq 'release' -and $args[1] -eq 'create') {
                    ++$observed.creates; $observed.arguments = @($args)
                    if ($scenario -eq 'collision') { $global:LASTEXITCODE = 1 }
                } else { throw 'Must not edit releases, overwrite assets or push existing tags.' }
            }
            $succeeded = $true
            try { & $publish | Out-Null } catch { $succeeded = $false }
            $shouldSucceed = $scenario -in @('first','previous','same-day','numeric-patch','gaps','patch-only','draft','many-tags','invalid-tags')
            if ($succeeded -ne $shouldSucceed -or $observed.creates -ne [int]($shouldSucceed -or $scenario -eq 'collision')) {
                throw "Incorrect publication behavior: $scenario"
            }
            if ($observed.creates) {
                $url = if ($previous) { "https://github.com/fixture/TTPlayer/compare/$previous...$expectedVersion" }
                       else { "https://github.com/fixture/TTPlayer/commits/$expectedVersion" }
                $modernName = "TTPlayerRebuild-$expectedVersion.zip"
                $legacyName = "TTPlayerRebuild-XP-Win7-$expectedVersion.zip"
                $expectedNotes = "**完整更新日志**: $url`n`n" + $env:RELEASE_INSTALL_NOTES.Replace('{version}', $expectedVersion)
                $expected = @('release','create',$expectedVersion,"artifact/$modernName","artifact/$legacyName",'artifact/SHA256SUMS.txt',
                    '--repo',$env:GITHUB_REPOSITORY,'--target',$env:GITHUB_SHA,'--title',$expectedVersion,'--notes-file','artifact/release-notes.md')
                $expectedHashes = ('a' * 64) + "  $modernName`n" + ('c' * 64) + "  $legacyName"
                $expectedRenames = if ($expectedVersion -eq '2026.09.05') { @() } else {
                    @("artifact/TTPlayerRebuild-2026.09.05.zip -> $modernName",
                      "artifact/TTPlayerRebuild-XP-Win7-2026.09.05.zip -> $legacyName")
                }
                if ($observed.hashes -cne $expectedHashes -or
                    ($observed.renames -join "`n") -cne ($expectedRenames -join "`n")) {
                    throw "Wrong renamed packages or checksum filenames: $scenario"
                }
                if ($observed.notes -cne $expectedNotes) { throw "Wrong release notes file: $scenario" }
                if (($observed.arguments -join "`0") -cne ($expected -join "`0")) { throw "Wrong version/changelog/assets: $scenario" }
            }
        }
    }
    Write-Output '8 Beijing-date/configuration cases, 2 real ZIP packaging cases, 21 mocked publication cases, versioned assets/checksums, installation notes and PowerShell syntax passed. No remote writes.'
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    $global:LASTEXITCODE = $savedExitCode
}
