param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\build\Release\Encoders'),
    [string]$Cache = (Join-Path $PSScriptRoot '..\build\external-encoders'),
    [switch]$IncludeApple,
    [switch]$IncludeNero,
    [switch]$RepairPresets
)
# Explicit, opt-in deployment only. Never run this from player startup or a
# normal build. No installer execution, registry writes, or PATH changes.
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
$Cache = [IO.Path]::GetFullPath($Cache)
$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
New-Item -ItemType Directory -Path $Cache -Force | Out-Null

function Get-Package($Name, $Url, $Sha256) {
    $path = Join-Path $Cache $Name
    if (-not (Test-Path -LiteralPath $path)) {
        & curl.exe --fail --location --silent --show-error --max-time 180 --output $path $Url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Sha256) {
        throw "SHA256 mismatch: $path. Existing files are never silently replaced."
    }
    return $path
}

function Expand-Package($Archive, $Folder, [string[]]$Entries = @()) {
    $path = Join-Path $Cache $Folder
    & $sevenZip x -aou "-o$path" $Archive @Entries | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $Archive" }
    return $path
}

$files = [Collections.Generic.List[object]]::new()
function Add-File($Source, $Relative, $Package) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing package entry: $Source" }
    $files.Add([pscustomobject]@{ Source=$Source; Relative=$Relative; Package=$Package })
}

$qaac = Get-Package 'qaac_3.07.zip' 'https://github.com/nu774/qaac/releases/download/v3.07/qaac_3.07.zip' '1FB3AB4AA81725E2607AC1B31AFA0E13D2617EE6EB5900F5070C092F5271CBB3'
$flac = Get-Package 'flac-1.5.0-win.zip' 'https://github.com/xiph/flac/releases/download/1.5.0/flac-1.5.0-win.zip' '53F1500F0D6E7C61379D7FEE50D4A9F7F504C650009506D9BA015530D76C0DDE'
$opus = Get-Package 'opus-tools-0.2-win32.zip' 'https://archive.mozilla.org/pub/opus/win32/opus-tools-0.2-win32.zip' '99A7C07C46C24727D9798ADFC53F5D1D82244E3C249476C22DDF0E1D76A7C86C'
$mpc = Get-Package 'musepack_windows_sv7.zip' 'https://files.musepack.net/windows/musepack_windows_sv7.zip' 'AFBE60AD8777196820F8C34AC9530122F2F98E03C808052C1DF03DF4D00A1A69'
$tta = Get-Package 'ttaenc-3.4.1.zip' 'https://downloads.sourceforge.net/project/tta/tta/ttaenc-win/ttaenc-3.4.1.zip' 'EB4ED3EF737486D8AE45C69E9866F3734C17875A3022F3EBA91DE64A1530C306'
# The author's site currently serves HTTP only. Pin the reviewed package bytes;
# do not disable TLS verification or fetch a random replacement if it changes.
$tak = Get-Package 'TAK_2.3.3.zip' 'http://www.thbeck.de/Download/TAK_2.3.3.zip' '40D26992266F1377D576B88B84B88D2AB256DC321E47F90314402A80692ABA6A'
$mac = Get-Package 'MAC_1326.exe' 'https://monkeysaudio.com/x86' 'A1AA0DF19096CFF813D7E32E1D6EDE8C4274C0F6787C6B11EC9DD85C0C5B2894'
$vorbis = Get-Package 'Free_Encoder_Pack-2026-03-13.exe' 'https://www.foobar2000.org/downloads/Free_Encoder_Pack-2026-03-13.exe' '2510D5706889C19EBF6E26D400B18C3B0962B1289B55BFBBEA52FCEFB6768627'
$faac = Get-Package 'faac-2.1-MSVC.zip' 'https://github.com/knik0/faac/releases/download/faac-2.1/faac-2.1-MSVC.zip' '7C11897AB812F3EB996DB0472DC0601424EBCFC2DB473F0043AF91A8B4073581'
$qaacLicense = Get-Package 'qaac-LICENSE.txt' 'https://raw.githubusercontent.com/nu774/qaac/v3.07/COPYING' 'EF1D7D9B16C7017851FAC1E812113D0DDF50F98BA86916B994BE433D90B8D187'
$faacLicense = Get-Package 'faac-LICENSE.txt' 'https://raw.githubusercontent.com/knik0/faac/faac-2.1/COPYING' '20E50FE7AAE3E56378EBF0417D9DE904F55A0E61E4DF315333E632A4D3555D95'
$vorbisLicense = Get-Package 'vorbis-tools-LICENSE.txt' 'https://raw.githubusercontent.com/xiph/vorbis-tools/master/COPYING' '32B1062F7DA84967E7019D01AB805935CAA7AB7321A7CED0E30EBE75E5DF1670'
Add-File $qaacLicense 'licenses\QAAC\COPYING' 'QAAC 3.07'
Add-File $faacLicense 'licenses\FAAC\COPYING' 'FAAC 2.1'
Add-File $vorbisLicense 'licenses\Vorbis\COPYING' 'Vorbis tools'

# Fresh extraction directory on every run, including the NSIS archive with
# duplicate x86/x64 filenames. Never accidentally select a previous extraction.
$run = 'deploy-' + [guid]::NewGuid().ToString('N')
$q = Expand-Package $qaac "$run\qaac"
Add-File "$q\qaac_3.07\x86\qaac.exe" 'qaac.exe' 'QAAC 3.07 x86'
Add-File "$q\qaac_3.07\x86\libsoxr.dll" 'libsoxr.dll' 'QAAC 3.07 x86'
$f = Expand-Package $flac "$run\flac"
foreach ($name in @('flac.exe','metaflac.exe','libFLAC.dll')) {
    Add-File "$f\flac-1.5.0-win\Win32\$name" $name 'FLAC 1.5.0 x86'
}
foreach ($name in @('COPYING.GPL','COPYING.Xiph','COPYING.LGPL','README-for-these-builds.txt')) {
    Add-File "$f\flac-1.5.0-win\$name" "licenses\FLAC\$name" 'FLAC 1.5.0'
}
$o = Expand-Package $opus "$run\opus"
foreach ($name in @('opusenc.exe','opusdec.exe','opusinfo.exe')) {
    Add-File "$o\$name" $name 'Opus tools 0.2 / libopus 1.3 x86'
}
Add-File "$o\LICENSE" 'licenses\Opus\LICENSE' 'Opus tools 0.2'
$m = Expand-Package $mpc "$run\mpc"
Add-File "$m\mppenc.exe" 'mppenc.exe' 'Musepack SV7 1.16'
Add-File "$m\mppdec.exe" 'mppdec.exe' 'Musepack SV7'
$t = Expand-Package $tta "$run\tta"
Add-File "$t\ttaenc-3.4.1-win\ttaenc.exe" 'ttaenc.exe' 'TTA 3.4.1 x86'
Add-File "$t\ttaenc-3.4.1-win\COPYING" 'licenses\TTA\COPYING' 'TTA 3.4.1'
$k = Expand-Package $tak "$run\tak"
Add-File "$k\Applications\Takc.exe" 'Takc.exe' 'TAK 2.3.3 x86'
Add-File "$k\Applications\Readme.html" 'licenses\TAK\Readme.html' 'TAK 2.3.3'
$a = Expand-Package $mac "$run\mac"
Add-File "$a\MAC.exe" 'mac.exe' "Monkey's Audio 13.26 x86"
Add-File (Join-Path $a '$SYSDIR\MACDll.dll') 'MACDll.dll' "Monkey's Audio 13.26 x86"
Add-File "$a\License.txt" 'licenses\MonkeyAudio\License.txt' "Monkey's Audio 13.26"
$v = Expand-Package $vorbis "$run\vorbis" @('oggenc2.exe')
# NSIS stores x86 first. Its command-line interface is compatible with oggenc;
# this is only a filename alias, not a wrapper/substitution of the codec.
Add-File "$v\oggenc2.exe" 'oggenc.exe' 'OggEnc2 (foobar2000 encoder pack 2026-03-13) x86'
if (-not [Environment]::Is64BitOperatingSystem) { throw 'This FAAC MSVC package requires x64 Windows.' }
$c = Expand-Package $faac "$run\faac"
Add-File "$c\bin\faac.exe" 'faac.exe' 'FAAC 2.1 x64'
Add-File "$c\bin\faac-1.dll" 'faac-1.dll' 'FAAC 2.1 x64'

if ($IncludeNero) {
    # Original vendor archive, retrieved through Wayback; hash and byte size
    # match FreeBSD ports 2017Q2/audio/linux-neroaaccodec/distinfo.
    # Local personal/evaluation use only; DO NOT bundle these in public builds.
    $nero = Get-Package 'NeroAACCodec-1.5.1.zip' 'https://web.archive.org/web/20160222054250id_/http://ftp6.nero.com/tools/NeroAACCodec-1.5.1.zip' 'E0496AD856E2803001A59985368D21B22F4FBDD55589C7F313D6040CEFFF648B'
    $n = Expand-Package $nero "$run\nero"
    foreach ($name in @('neroAacEnc.exe','neroAacDec.exe','neroAacTag.exe')) {
        Add-File "$n\win32\$name" $name 'Nero AAC 1.5.4.0 / archive 1.5.1 x86 (local use only)'
    }
    foreach ($name in @('license.txt','readme.txt','changelog.txt')) {
        Add-File "$n\$name" "licenses\Nero\$name" 'Nero AAC archive 1.5.1'
    }
}

if ($IncludeApple) {
    $apple = Get-Package 'iTunesSetup.exe' 'https://secure-appldnld.apple.com/itunes12/001-80042-20210422-E8A351F2-A3B2-11EB-9A8F-CF1B67FC6302/iTunesSetup.exe' '71033CC461DED34DCF7D98045386EDB9B1B4587039E53DC40289830897518EB6'
    $signature = Get-AuthenticodeSignature -LiteralPath $apple
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Apple Inc\.') {
        throw 'Apple installer signature verification failed.'
    }
    $ap = Expand-Package $apple "$run\apple" @('iTunes.msi')
    # Read MSI's File table only; never execute MSI/custom actions. Cabinet
    # member names are IDs, not usable DLL names. Resolve them using that table.
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $db = $installer.OpenDatabase("$ap\iTunes.msi", 0)
    $view = $db.OpenView('SELECT `File`, `FileName` FROM `File`')
    $view.Execute()
    $members = [ordered]@{}
    try {
        while ($record = $view.Fetch()) {
            $name = ($record.StringData(2) -split '\|')[-1]
            if ($name -match '^(ASL|CoreAudioToolbox|CoreFoundation|libdispatch|libicuin|libicuuc|objc|icudt\d+|msvc[pr]\d+|vcruntime\d+|ucrtbase)\.dll$|^api-ms-win-.*\.dll$') {
                if (-not $members.Contains($name)) { $members[$name] = $record.StringData(1) }
            }
        }
    } finally {
        $view.Close()
        foreach ($com in @($view,$db,$installer)) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($com) }
    }
    $ac = Expand-Package "$ap\iTunes.msi" "$run\apple\contents" @($members.Values)
    foreach ($name in $members.Keys) {
        Add-File (Join-Path $ac $members[$name]) "QTfiles\$name" 'Apple iTunes 12.10.11 x86 (local QAAC dependency)'
    }
}

# Validate all conflicts before installing anything. Preserve custom encoders.
foreach ($file in $files) {
    $target = Join-Path $Destination $file.Relative
    if ((Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file.Source).Hash) {
        throw "Existing different file preserved: $target"
    }
}
foreach ($file in $files) {
    $target = Join-Path $Destination $file.Relative
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $file.Source -Destination $target }
    Write-Output ("{0}: {1}" -f $file.Relative, (Get-FileHash -LiteralPath $target).Hash)
}
if ($RepairPresets) {
    $presetPath = Join-Path (Split-Path $Destination) 'AddIn\ttp_clienc.xml'
    if (Test-Path -LiteralPath $presetPath) {
        $xml = [xml]::new()
        $xml.PreserveWhitespace = $true
        $xml.Load($presetPath)
        $changed = 0
        foreach ($preset in $xml.ttp_cmdline_encoder.preset) {
            if ([IO.Path]::GetFileName([string]$preset.encoder) -ieq 'neroAacEnc.exe' -and
                [string]$preset.param -match '(?<!\S)-ignorelenth(?!\S)') {
                $preset.SetAttribute('param', ([string]$preset.param -replace '(?<!\S)-ignorelenth(?!\S)', '-ignorelength'))
                ++$changed
            }
        }
        if ($changed) {
            $backup = Join-Path $Cache ($run + '\ttp_clienc.before.xml')
            Copy-Item -LiteralPath $presetPath -Destination $backup
            $xml.Save($presetPath)
            Write-Output "Corrected $changed Nero -ignorelenth typos; preset backup: $backup"
        }
    }
}
if (-not $IncludeApple) { Write-Warning 'QAAC AAC encoding still requires Apple CoreAudioToolbox; use -IncludeApple for local extraction.' }
if (-not $IncludeNero) { Write-Warning 'Nero CLI is optional: use -IncludeNero to extract the original Wayback archive for local use only.' }
