$ErrorActionPreference = 'Stop'
$generator = Join-Path $PSScriptRoot 'write_build_date.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('TTPlayer-build-date-' + [guid]::NewGuid().ToString('N'))
$header = Join-Path $testRoot 'generated path\build_date.h'
$cases = @(
    @('2026-09-11T15:59:59Z', '2026-9-11'),
    @('2026-09-11T16:00:00Z', '2026-9-12'),
    @('2026-09-11T09:00:00-07:00', '2026-9-12'),
    @('2026-09-12T00:00:00+08:00', '2026-9-12'),
    @('2026-12-31T16:00:00Z', '2027-1-1'),
    @('2028-02-28T16:00:00Z', '2028-2-29'),
    @('2028-02-29T16:00:00Z', '2028-3-1'),
    @('2026-02-28T16:00:00Z', '2026-3-1')
)
foreach ($case in $cases) {
    $timestamp = [DateTimeOffset]::Parse($case[0], [Globalization.CultureInfo]::InvariantCulture)
    & $generator -OutputPath $header -Timestamp $timestamp
    $content = [IO.File]::ReadAllText($header)
    if (-not $content.Contains('kCompletionDate[] = L"' + $case[1] + '";')) {
        throw "Wrong Beijing date for $($case[0]): $content"
    }
}

# Do not rewrite identical dates. Give the file a known old write time to
# avoid relying on the filesystem's timestamp resolution or a timed sleep.
$writeTime = [datetime]::SpecifyKind([datetime]'2001-01-01', [DateTimeKind]::Utc)
[IO.File]::SetLastWriteTimeUtc($header, $writeTime)
& $generator -OutputPath $header -Timestamp ([DateTimeOffset]'2026-03-01T12:00:00Z')
if ([IO.File]::GetLastWriteTimeUtc($header) -ne $writeTime) {
    throw 'Same-day generation rewrote the header'
}
& $generator -OutputPath $header -Timestamp ([DateTimeOffset]'2026-03-01T16:00:00Z')
if (-not [IO.File]::ReadAllText($header).Contains('L"2026-3-2"')) {
    throw 'Next-day generation did not refresh the date'
}

$before = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy-M-d', [Globalization.CultureInfo]::InvariantCulture)
& $generator -OutputPath $header
$after = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy-M-d', [Globalization.CultureInfo]::InvariantCulture)
$content = [IO.File]::ReadAllText($header)
if (-not ($content.Contains('L"' + $before + '"') -or $content.Contains('L"' + $after + '"'))) {
    throw 'Default generation did not use the current Beijing date'
}
Write-Output 'Build-date boundary, offset, incremental and current-date tests passed.'
