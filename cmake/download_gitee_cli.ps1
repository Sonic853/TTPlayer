[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'

$repository = 'https://github.com/Sonic853/gitee-release-cli-rust'
$api = 'https://api.github.com/repos/Sonic853/gitee-release-cli-rust/releases/latest'
$asset = 'gitee-release-rs.exe'
$maximumSize = 32MB
$directory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$executable = Join-Path $directory $asset
$metadataPath = Join-Path $directory 'gitee-cli.json'

function Assert-CliMetadata($Metadata) {
    if ($Metadata.repository -cne $repository -or $Metadata.asset -cne $asset -or
        $Metadata.version -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
        $Metadata.url -cne "$repository/releases/download/$($Metadata.version)/$asset" -or
        $Metadata.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $Metadata.size -le 0 -or $Metadata.size -gt $maximumSize -or
        $Metadata.release_id -le 0 -or $Metadata.asset_id -le 0) {
        throw 'Invalid Gitee CLI release source metadata.'
    }
}

function Assert-CliFile([string]$Path, $Metadata) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-Item -LiteralPath $Path).Length -ne $Metadata.size -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Metadata.sha256) {
        throw "Gitee CLI $($Metadata.version) executable size or SHA-256 verification failed."
    }
}

function Invoke-WithRetry([scriptblock]$Operation) {
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        try { return (& $Operation) } catch {
            if ($attempt -eq 2) { throw }
            Start-Sleep -Seconds ($attempt + 1)
        }
    }
}

if ($VerifyOnly) {
    $metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-CliMetadata $metadata
    Assert-CliFile $executable $metadata
} else {
    $headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'TTPlayerRebuild-cli-downloader' }
    if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }
    # Query latest only once. The recorded URL/digest is reused across jobs.
    $release = Invoke-WithRetry { Invoke-RestMethod -Uri $api -Headers $headers -TimeoutSec 60 }
    if ($release.draft -or $release.prerelease) { throw 'Expected a stable published Gitee CLI release.' }
    $candidates = @($release.assets | Where-Object name -CEQ $asset)
    if ($candidates.Count -ne 1) { throw 'Expected exactly one gitee-release-rs.exe release asset.' }
    $binary = $candidates[0]
    if ($binary.digest -cnotmatch '^sha256:[0-9a-f]{64}$') {
        throw 'GitHub did not supply the Gitee CLI executable SHA-256 digest.'
    }
    $metadata = [pscustomobject][ordered]@{
        repository = $repository
        version = $release.tag_name
        release_id = $release.id
        asset_id = $binary.id
        asset = $asset
        url = $binary.browser_download_url
        size = $binary.size
        sha256 = $binary.digest.Substring(7)
    }
    Assert-CliMetadata $metadata
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $temporary = Join-Path $directory ($asset + '.' + [guid]::NewGuid().ToString('N') + '.download')
    try {
        Invoke-WithRetry {
            # The API token is never passed to executable downloads.
            Invoke-WebRequest -Uri $metadata.url -OutFile $temporary -UseBasicParsing -TimeoutSec 60
        }
        Assert-CliFile $temporary $metadata
        Move-Item -LiteralPath $temporary -Destination $executable -Force
        $metadata | ConvertTo-Json | Set-Content -LiteralPath $metadataPath -Encoding UTF8
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}
Write-Output "Verified Gitee release CLI $($metadata.version): $executable"
