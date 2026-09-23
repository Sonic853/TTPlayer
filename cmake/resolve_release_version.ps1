[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReleaseDate,
    [string]$Repository,
    [switch]$PublishGitHub,
    [switch]$PublishGitee
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$base = Get-PlayerBuildVersion $ReleaseDate
if ($base.Patch -ne 0) { throw 'Expected a Beijing build date without a patch suffix.' }
$version = $base.Name
$previous = ''
if ($PublishGitHub -or $PublishGitee) {
    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Missing or invalid repository.' }
    $tags = @(gh api "repos/$Repository/tags?per_page=100" --paginate --jq '.[].name')
    if ($LASTEXITCODE -ne 0) { throw 'Could not read repository tags.' }
    $releaseTags = @(gh api "repos/$Repository/releases?per_page=100" --paginate --jq '.[].tag_name')
    if ($LASTEXITCODE -ne 0) { throw 'Could not read existing releases.' }
    $versions = @($tags | ForEach-Object { Read-PlayerVersionTag $_ })
    $occupied = @($versions) + @($releaseTags | ForEach-Object { Read-PlayerVersionTag $_ })
    if ($PublishGitee) {
        if ([string]::IsNullOrWhiteSpace($env:GITEE_REPO) -or
            [string]::IsNullOrWhiteSpace($env:GITEE_TOKEN) -or
            [string]::IsNullOrWhiteSpace($env:GITEE_RELEASE_CLI)) {
            throw 'Gitee publication requires its repository, token and built CLI.'
        }
        foreach ($kind in @('tag', 'release')) {
            # Credentials remain in the CLI environment, never command arguments.
            $json = & $env:GITEE_RELEASE_CLI --json $kind list --all --per-page 100
            if ($LASTEXITCODE -ne 0) { throw "Could not read Gitee $kind list." }
            $items = ($json -join "`n") | ConvertFrom-Json
            $occupied += @($items | ForEach-Object {
                if ($kind -eq 'tag') { Read-PlayerVersionTag $_.name }
                else { Read-PlayerVersionTag $_.tag_name }
            })
        }
    }
    $sameDay = @($occupied | Where-Object { $_.Date -eq $base.Date } | Sort-Object Patch -Descending)
    if ($sameDay.Count) {
        if ($sameDay[0].Patch -ge 65535) { throw 'Windows version patch number is exhausted.' }
        $version += 'p' + ($sameDay[0].Patch + 1).ToString([Globalization.CultureInfo]::InvariantCulture)
    }
    $latest = $versions | Sort-Object Date, Patch -Descending | Select-Object -First 1
    if ($latest) { $previous = $latest.Name }
}
# The workflow concurrency group holds this allocation through publication.
if ($env:GITHUB_OUTPUT) {
    "version=$version`nprevious=$previous" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
}
Write-Output "Selected player build version $version"
