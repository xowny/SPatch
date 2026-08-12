[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]] $Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Crc32 {
    param([byte[]] $Bytes)

    [uint32] $crc = [uint32]::MaxValue
    [uint32] $polynomial = 3988292384
    foreach ($byte in $Bytes) {
        $crc = $crc -bxor [uint32] $byte
        for ($bit = 0; $bit -lt 8; ++$bit) {
            if (($crc -band 1) -ne 0) {
                $crc = ($crc -shr 1) -bxor $polynomial
            } else {
                $crc = $crc -shr 1
            }
        }
    }
    return [uint32] ($crc -bxor [uint32]::MaxValue)
}

foreach ($inputPath in $Path) {
    $resolved = (Resolve-Path -LiteralPath $inputPath).Path
    $bytes = [IO.File]::ReadAllBytes($resolved)
    if ($bytes.Length -lt 20 -or
        [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -cne 'DXBC') {
        throw "Not a DXBC container: $resolved"
    }

    [pscustomobject]@{
        Path = $resolved
        Bytes = $bytes.Length
        SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolved).Hash
        CRC32 = ('{0:X8}' -f (Get-Crc32 -Bytes $bytes))
        DXBC = ([BitConverter]::ToString($bytes[4..19]) -replace '-', '')
    }
}
