[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string[]]$Path,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{40}$')]
    [string]$CertificateThumbprint,
    [uri]$TimestampUrl = 'http://timestamp.digicert.com'
    )

$ErrorActionPreference = 'Stop'
$kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signtool = Get-ChildItem -LiteralPath $kitsBin -Directory -ErrorAction Stop |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe was not found in the Windows 10 SDK.' }

foreach ($item in $Path) {
    $resolved = Resolve-Path -LiteralPath $item -ErrorAction Stop
    if ($PSCmdlet.ShouldProcess($resolved, 'Authenticode sign')) {
        & $signtool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl.AbsoluteUri /td SHA256 /v $resolved
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $signtool verify /pa /v $resolved
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
