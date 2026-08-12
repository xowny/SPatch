[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameExecutablePath,

    [Parameter(Mandatory = $true)]
    [string]$BuildInfoPath,

    [Parameter(Mandatory = $true)]
    [string]$AsiLoaderPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-NoReparsePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $pathRoot = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot)) {
        throw "$Description has no filesystem root: $fullPath"
    }
    $cursor = $pathRoot
    foreach ($component in @($fullPath.Substring($pathRoot.Length) `
            -split '[\\/]' | Where-Object {
                -not [string]::IsNullOrEmpty($_)
            })) {
        $cursor = Join-Path $cursor $component
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -eq $item) {
            break
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description contains a reparse point: $cursor"
        }
    }
    return $fullPath
}

function Get-ResolvedLeafPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $checkedPath = Assert-NoReparsePath -Path $Path -Description $Description
    if (-not (Test-Path -LiteralPath $checkedPath -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $resolvedPath = [IO.Path]::GetFullPath(
        (Resolve-Path -LiteralPath $checkedPath).Path)
    [void](Assert-NoReparsePath -Path $resolvedPath -Description $Description)
    return $resolvedPath
}

function Assert-X64PeImage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x100 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Description is not a valid PE image: $Path"
    }

    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 26 -gt $bytes.Length -or
        $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "$Description has an invalid PE header: $Path"
    }

    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $optionalMagic -ne 0x20B) {
        throw ("$Description must be a native x64 PE32+ image; found " +
               ('machine=0x{0:X4}, optionalMagic=0x{1:X4}' -f
                    $machine, $optionalMagic))
    }
    return $bytes
}

function Convert-PeRvaToFileOffset {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes,

        [Parameter(Mandatory = $true)]
        [int]$PeOffset,

        [Parameter(Mandatory = $true)]
        [uint32]$Rva,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $optionalHeaderOffset = $PeOffset + 24
    $numberOfSections = [BitConverter]::ToUInt16($Bytes, $PeOffset + 6)
    $optionalHeaderSize = [BitConverter]::ToUInt16($Bytes, $PeOffset + 20)
    if ($optionalHeaderSize -lt 0x70 -or
        $optionalHeaderOffset + $optionalHeaderSize -gt $Bytes.Length) {
        throw "$Description has a truncated PE optional header."
    }

    $sizeOfHeaders = [BitConverter]::ToUInt32($Bytes, $optionalHeaderOffset + 60)
    if ($Rva -lt $sizeOfHeaders) {
        if ([uint64]$Rva -ge [uint64]$Bytes.Length) {
            throw "$Description maps an RVA beyond the file."
        }
        return [int64]$Rva
    }

    $sectionTableOffset = $optionalHeaderOffset + $optionalHeaderSize
    $sectionTableSize = [int64]$numberOfSections * 40
    if ([int64]$sectionTableOffset + $sectionTableSize -gt $Bytes.Length) {
        throw "$Description has a truncated PE section table."
    }

    for ($index = 0; $index -lt $numberOfSections; ++$index) {
        $sectionOffset = $sectionTableOffset + ($index * 40)
        $virtualSize =
            [BitConverter]::ToUInt32($Bytes, $sectionOffset + 8)
        $virtualAddress =
            [BitConverter]::ToUInt32($Bytes, $sectionOffset + 12)
        $rawSize = [BitConverter]::ToUInt32($Bytes, $sectionOffset + 16)
        $rawOffset = [BitConverter]::ToUInt32($Bytes, $sectionOffset + 20)
        $mappedSize = [Math]::Max([uint64]$virtualSize, [uint64]$rawSize)
        $rva64 = [uint64]$Rva
        $sectionStart = [uint64]$virtualAddress
        if ($rva64 -lt $sectionStart -or
            $rva64 -ge $sectionStart + $mappedSize) {
            continue
        }

        $delta = $rva64 - $sectionStart
        if ($delta -ge [uint64]$rawSize) {
            throw "$Description maps an RVA into uninitialized section data."
        }
        $fileOffset = [uint64]$rawOffset + $delta
        if ($fileOffset -ge [uint64]$Bytes.Length) {
            throw "$Description maps an RVA beyond the file."
        }
        return [int64]$fileOffset
    }

    throw ("$Description contains an RVA that is not backed by a PE section: " +
           ('0x{0:X8}' -f $Rva))
}

function Get-PeExportNames {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $peOffset = [BitConverter]::ToInt32($Bytes, 0x3C)
    $optionalHeaderOffset = $peOffset + 24
    $optionalHeaderSize = [BitConverter]::ToUInt16($Bytes, $peOffset + 20)
    if ($optionalHeaderSize -lt 0x78 -or
        $optionalHeaderOffset + $optionalHeaderSize -gt $Bytes.Length) {
        throw "$Description has no complete PE32+ data-directory table."
    }

    $directoryCount =
        [BitConverter]::ToUInt32($Bytes, $optionalHeaderOffset + 108)
    if ($directoryCount -lt 1) {
        return @()
    }
    $exportRva = [BitConverter]::ToUInt32($Bytes, $optionalHeaderOffset + 112)
    $exportSize = [BitConverter]::ToUInt32($Bytes, $optionalHeaderOffset + 116)
    if ($exportRva -eq 0 -or $exportSize -lt 40) {
        return @()
    }

    $exportOffset = Convert-PeRvaToFileOffset -Bytes $Bytes `
        -PeOffset $peOffset -Rva $exportRva -Description $Description
    if ($exportOffset + 40 -gt $Bytes.Length) {
        throw "$Description has a truncated PE export directory."
    }

    $nameCount = [BitConverter]::ToUInt32($Bytes, [int]$exportOffset + 24)
    $nameTableRva =
        [BitConverter]::ToUInt32($Bytes, [int]$exportOffset + 32)
    if ($nameCount -gt 65536) {
        throw "$Description declares an unreasonable PE export-name count."
    }
    if ($nameCount -eq 0) {
        return @()
    }

    $nameTableOffset = Convert-PeRvaToFileOffset -Bytes $Bytes `
        -PeOffset $peOffset -Rva $nameTableRva -Description $Description
    if ([uint64]$nameTableOffset + ([uint64]$nameCount * 4) -gt
        [uint64]$Bytes.Length) {
        throw "$Description has a truncated PE export-name table."
    }

    $names = [Collections.Generic.List[string]]::new()
    for ([uint32]$index = 0; $index -lt $nameCount; ++$index) {
        $nameRva = [BitConverter]::ToUInt32(
            $Bytes, [int]($nameTableOffset + ([int64]$index * 4)))
        $nameOffset = Convert-PeRvaToFileOffset -Bytes $Bytes `
            -PeOffset $peOffset -Rva $nameRva -Description $Description
        $endOffset = $nameOffset
        while ($endOffset -lt $Bytes.Length -and
               $Bytes[[int]$endOffset] -ne 0 -and
               $endOffset - $nameOffset -lt 4096) {
            ++$endOffset
        }
        if ($endOffset -ge $Bytes.Length -or
            $Bytes[[int]$endOffset] -ne 0) {
            throw "$Description contains an unterminated PE export name."
        }
        $names.Add([Text.Encoding]::ASCII.GetString(
            $Bytes, [int]$nameOffset, [int]($endOffset - $nameOffset)))
    }
    return $names.ToArray()
}

function Get-Sha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($stream))).Replace('-', '')
    } finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

$resolvedExecutable =
    Get-ResolvedLeafPath -Path $GameExecutablePath -Description 'Game executable'
$resolvedBuildInfo =
    Get-ResolvedLeafPath -Path $BuildInfoPath -Description 'BuildInfo contract'
$resolvedLoader =
    Get-ResolvedLeafPath -Path $AsiLoaderPath `
        -Description 'x64 dinput8 ASI-loader candidate'
$resolvedGameDirectory = [IO.Path]::GetDirectoryName($resolvedExecutable)
[void](Assert-NoReparsePath -Path $resolvedGameDirectory `
    -Description 'Game directory')

if ([IO.Path]::GetFileName($resolvedExecutable) -ine 'sdhdship.exe') {
    throw 'The deployment executable must be sdhdship.exe in the selected game directory.'
}
if ([IO.Path]::GetFileName($resolvedLoader) -ine 'dinput8.dll' -or
    [IO.Path]::GetDirectoryName($resolvedLoader) -ine $resolvedGameDirectory) {
    throw 'SPatch deployment requires an x64 dinput8.dll ASI-loader candidate beside sdhdship.exe.'
}

$expectedHashes = [ordered]@{
    kLegacySha256 =
        'C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035'
    kLatestSteamSha256 =
        '2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA'
}
$buildInfoText = [IO.File]::ReadAllText($resolvedBuildInfo)
$declaredHashes = [Collections.Generic.List[string]]::new()
foreach ($entry in $expectedHashes.GetEnumerator()) {
    $declaration = [regex]::Match(
        $buildInfoText,
        ('(?s)\b{0}\s*=\s*\{{(?<bytes>.*?)\}};' -f
            [regex]::Escape($entry.Key)))
    if (-not $declaration.Success) {
        throw "BuildInfo is missing the $($entry.Key) SHA-256 declaration."
    }
    $byteMatches = [regex]::Matches(
        $declaration.Groups['bytes'].Value, '0x(?<hex>[0-9A-Fa-f]{2})')
    if ($byteMatches.Count -ne 32) {
        throw "BuildInfo $($entry.Key) must contain exactly 32 hash bytes."
    }
    $declaredHash = (($byteMatches | ForEach-Object {
        $_.Groups['hex'].Value.ToUpperInvariant()
    }) -join '')
    if ($declaredHash -cne $entry.Value) {
        throw ("BuildInfo $($entry.Key) drifted. Expected $($entry.Value), " +
               "found $declaredHash.")
    }
    $declaredHashes.Add($declaredHash)
}

$null = Assert-X64PeImage -Path $resolvedExecutable -Description 'Game executable'
$gameHash = Get-Sha256 -Path $resolvedExecutable
if (-not $declaredHashes.Contains($gameHash)) {
    throw ("Unsupported sdhdship.exe SHA-256 $gameHash. Expected one of: " +
           ($declaredHashes -join ', '))
}

$loaderBytes =
    Assert-X64PeImage -Path $resolvedLoader `
        -Description 'ASI-loader candidate'
$loaderExports = @(Get-PeExportNames -Bytes $loaderBytes `
    -Description 'ASI-loader candidate')
if (-not $loaderExports.Contains('DirectInput8Create')) {
    throw 'dinput8.dll does not export DirectInput8Create.'
}
$loaderText = [Text.Encoding]::ASCII.GetString($loaderBytes)
if ($loaderText.IndexOf('.asi',
                        [StringComparison]::OrdinalIgnoreCase) -lt 0) {
    throw 'dinput8.dll exports DirectInput8Create but lacks a static ASI-loader marker.'
}

$loaderHash = Get-Sha256 -Path $resolvedLoader
Write-Host "Validated supported sdhdship.exe SHA-256: $gameHash"
Write-Host "Recognized x64 dinput8 ASI-loader candidate SHA-256: $loaderHash"
Write-Host 'This static preflight does not prove that the candidate loads SPatch; use the final runtime smoke for module-load proof.'
