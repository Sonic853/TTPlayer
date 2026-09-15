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
$publication = @($blocks | Where-Object { $_.Contains('gh release create') })
if ($validation.Count -ne 1 -or $publication.Count -ne 1) { throw 'Missing or ambiguous workflow steps.' }
if ($text.Contains('inputs.release_version') -or $text -match '(?m)^      release_version:') {
    throw 'Manual version input must not remain.'
}
if (-not $text.Contains('group: manual-player-release') -or -not $text.Contains('needs.build.outputs.release_date')) {
    throw 'Publication must use a shared concurrency group and the captured build date.'
}
$publish = [scriptblock]::Create($publication[0])
$savedEnvironment = @{}
foreach ($name in @('RELEASE_DATE', 'BUILD_CONFIGURATION', 'GITHUB_SHA', 'GITHUB_REPOSITORY',
                     'GITHUB_SERVER_URL', 'RELEASE_INSTALL_NOTES')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$savedExitCode = $global:LASTEXITCODE
try {
    foreach ($case in @(
        @('2026-09-04T15:59:59Z', 'Release', '2026.09.04'),
        @('2026-09-04T16:00:00Z', 'Release', '2026.09.05'),
        @('2026-12-31T16:00:00Z', 'Release', '2027.01.01'),
        @('2024-02-28T16:00:00Z', 'Release', '2024.02.29'),
        @('2026-09-04T16:00:00Z', 'Debug', ''),
        @('2026-09-04T16:00:00Z', 'RelWithDebInfo', ''))) {
        & {
            $capture = @{ value = '' }
            function Out-File {
                param([Parameter(ValueFromPipeline)]$InputObject, $FilePath, $Encoding, [switch]$Append)
                process { $capture.value = $InputObject }
            }
            $env:BUILD_CONFIGURATION = $case[1]
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
    $notes = [regex]::Match($text, '(?m)^          RELEASE_INSTALL_NOTES: \|\r?\n((?:            .*\r?\n|\r?\n)*)')
    if (-not $notes.Success) { throw 'Installation notes are missing.' }
    $env:RELEASE_INSTALL_NOTES = $notes.Groups[1].Value -replace '(?m)^            ', ''
    $expectedInstallation = @'
使用该程序前确保本地已安装有 5.7.9 版本的千千静听，请按照以下步骤安装重建版：
1. Windows XP / Windows 7 下载并解压 TTPlayerRebuild-XP-Win7.zip；现代 Windows 下载 TTPlayerRebuild.exe。
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
        'release-api-error', 'collision', 'invalid-date', 'exhausted', 'wrong-legacy-hash', 'wrong-legacy-configuration')
    foreach ($scenario in $scenarios) {
        & {
            $env:RELEASE_DATE = '2026.09.05'
            $observed = @{ creates = 0; apis = 0; arguments = @(); notes = '' }
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
                        @{commit=$commit;configuration=$configuration;legacy_configuration=$legacy} | ConvertTo-Json
                    }
                    'artifact/SHA256SUMS.txt' { ('a' * 64) + "  TTPlayerRebuild.exe`r`n" + ('c' * 64) + '  TTPlayerRebuild-XP-Win7.zip' }
                    default { throw "Unexpected file read: $LiteralPath" }
                }
            }
            function Get-FileHash {
                param($LiteralPath, $Algorithm)
                if ($Algorithm -ne 'SHA256') { throw 'Unexpected hash algorithm.' }
                switch ($LiteralPath) {
                    'artifact/TTPlayerRebuild.exe' { @{Hash = $(if ($scenario -eq 'wrong-hash') { 'b' * 64 } else { 'a' * 64 })} }
                    'artifact/TTPlayerRebuild-XP-Win7.zip' { @{Hash = $(if ($scenario -eq 'wrong-legacy-hash') { 'b' * 64 } else { 'c' * 64 })} }
                    default { throw 'Unexpected hash request.' }
                }
            }
            function Set-Content {
                param([Parameter(ValueFromPipeline)]$Value, $LiteralPath, $Encoding)
                process {
                    if ($LiteralPath -ne 'artifact/release-notes.md') { throw 'Unexpected file write.' }
                    $observed.notes = $Value
                }
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
                $expectedNotes = "**完整更新日志**: $url`n`n" + $env:RELEASE_INSTALL_NOTES
                $expected = @('release','create',$expectedVersion,'artifact/TTPlayerRebuild.exe','artifact/TTPlayerRebuild-XP-Win7.zip','artifact/SHA256SUMS.txt',
                    '--repo',$env:GITHUB_REPOSITORY,'--target',$env:GITHUB_SHA,'--title',$expectedVersion,'--notes-file','artifact/release-notes.md')
                if ($observed.notes -cne $expectedNotes) { throw "Wrong release notes file: $scenario" }
                if (($observed.arguments -join "`0") -cne ($expected -join "`0")) { throw "Wrong version/changelog/assets: $scenario" }
            }
        }
    }
    Write-Output '6 Beijing-date/configuration cases, 19 mocked publication cases, both artifacts, installation notes and PowerShell syntax passed. No remote writes.'
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    $global:LASTEXITCODE = $savedExitCode
}
