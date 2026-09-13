# Manual transport regression; never changes a certificate trust store.
param(
    [Parameter(Mandatory=$true)][string]$Program,
    [string]$OpenSsl = 'openssl',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('TTPlayer-lyric-TLS-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$cert = Join-Path $testRoot 'cert.pem'
$key = Join-Path $testRoot 'key.pem'
# No import into CurrentUser/LocalMachine certificate stores, no verification bypass.
$opensslLog = Join-Path $testRoot 'openssl.log'
$opensslArguments = @('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', $key, '-out', $cert,
    '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1') |
    ForEach-Object { '"{0}"' -f $_ }
$generator = Start-Process -FilePath $OpenSsl -ArgumentList ($opensslArguments -join ' ') -WindowStyle Hidden -Wait -PassThru -RedirectStandardError $opensslLog
if ($generator.ExitCode -ne 0) { throw 'OpenSSL fixture generation failed' }
$ready = Join-Path $testRoot 'server.log'
$errors = Join-Path $testRoot 'server.err'
$serverScript = Join-Path $PSScriptRoot 'lyric_tls_fixture.py'
$arguments = @('-u', $serverScript, '--cert', $cert, '--key', $key) | ForEach-Object { '"{0}"' -f $_ }
$server = Start-Process -FilePath $Python -ArgumentList ($arguments -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $ready -RedirectStandardError $errors
$null = $server.Handle # Retain the exit-code handle on Windows PowerShell 5.1.
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $portText = ''
    while (!$portText) {
        if ([DateTime]::UtcNow -gt $deadline -or $server.HasExited) { throw 'TLS fixture did not start' }
        if (Test-Path -LiteralPath $ready) { $portText = Get-Content -LiteralPath $ready -Raw }
        if (!$portText) { Start-Sleep -Milliseconds 50 }
    }
    $port = [int]$portText.Trim()
    & $Program --reject-tls "https://localhost:$port/lyrics"
    if ($LASTEXITCODE -ne 0) { throw 'Untrusted HTTPS certificate regression' }
    if (!$server.WaitForExit(3000)) { throw 'TLS fixture did not complete' }
    if ($server.ExitCode -ne 0) { throw (Get-Content -LiteralPath $errors -Raw) }
    "TLS fixture: $testRoot (certificate was not installed)"
} finally {
    if (!$server.HasExited) { Stop-Process -Id $server.Id }
}
