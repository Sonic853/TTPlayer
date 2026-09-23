# Shared version rules for the EXE resource, packaging and Actions.
function Read-PlayerVersionTag([string]$Name) {
    $match = [regex]::Match($Name, '^(?<date>[0-9]{4}\.[0-9]{2}\.[0-9]{2})(?:p(?<patch>[1-9][0-9]*))?$')
    if (-not $match.Success) { return }
    $date = [datetime]::MinValue
    if (-not [datetime]::TryParseExact($match.Groups['date'].Value, 'yyyy.MM.dd',
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::None, [ref]$date)) { return }
    $patch = 0L
    if ($match.Groups['patch'].Success -and -not [long]::TryParse($match.Groups['patch'].Value, [ref]$patch)) {
        throw 'Version patch number is too large.'
    }
    [pscustomobject]@{ Name = $Name; Date = $date; Patch = $patch }
}

function Get-PlayerBuildVersion([string]$Version) {
    if ([string]::IsNullOrEmpty($Version)) {
        $Version = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString(
            'yyyy.MM.dd', [Globalization.CultureInfo]::InvariantCulture)
    }
    $parsed = Read-PlayerVersionTag $Version
    if (-not $parsed) { throw 'Build version must be a valid yyyy.MM.dd or yyyy.MM.ddpN date.' }
    # Windows fixed file/product versions contain four unsigned WORDs.
    if ($parsed.Patch -gt 65535) { throw 'Windows version patch must not exceed 65535.' }
    return $parsed
}

function Assert-PlayerFileVersion([string]$Path, [string]$Version) {
    $parsed = Get-PlayerBuildVersion $Version
    $Version = $parsed.Name
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo([IO.Path]::GetFullPath($Path))
    $expected = '{0}.{1}.{2}.{3}' -f $parsed.Date.Year,$parsed.Date.Month,$parsed.Date.Day,$parsed.Patch
    $file = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart,$info.FileMinorPart,$info.FileBuildPart,$info.FilePrivatePart
    $product = '{0}.{1}.{2}.{3}' -f $info.ProductMajorPart,$info.ProductMinorPart,$info.ProductBuildPart,$info.ProductPrivatePart
    if ($info.FileVersion -cne $Version -or $info.ProductVersion -cne $Version -or
        $file -cne $expected -or $product -cne $expected) {
        throw "EXE version does not match package version $Version."
    }
    # The resource template owns the display name (including edition suffixes).
    # Keep packaging validation in sync when that name is edited.
    $template = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../src/app/version.rc.in') -Raw -Encoding UTF8
    $description = [regex]::Match($template, 'VALUE\s+"FileDescription",\s*"([^"\\]+)\\0"')
    if (-not $description.Success -or $info.CompanyName -cne 'Sonic853' -or
        $info.FileDescription -cne $description.Groups[1].Value) {
        throw 'EXE author/company or file description is missing.'
    }
}
