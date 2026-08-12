[CmdletBinding()]
param(
    [ValidateSet('Development-Release', 'Publishing-Release')]
    [string] $Configuration = 'Publishing-Release',
    [string] $RepoRoot = '',
    [string] $PackageRoot = '',
    [string] $GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\SleepingDogsDefinitiveEdition',
    [switch] $ReplaceReShadeIni,
    [switch] $ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $scriptRoot
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot).TrimEnd([char[]]'\/')
$GameRoot = [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/')
if (-not [string]::IsNullOrWhiteSpace($PackageRoot)) {
    $packageRoot = [IO.Path]::GetFullPath($PackageRoot).TrimEnd([char[]]'\/')
} elseif (Test-Path -LiteralPath (Join-Path $scriptRoot 'SHA256SUMS.txt') `
        -PathType Leaf) {
    $packageRoot = [IO.Path]::GetFullPath($scriptRoot).TrimEnd([char[]]'\/')
} else {
    $packageRoot = Join-Path $RepoRoot `
        "artifacts\shenlong\$Configuration\ShenLong-Package"
}
$gameRootPrefix = $GameRoot + [IO.Path]::DirectorySeparatorChar
if ($packageRoot -ieq $GameRoot -or
    $packageRoot.StartsWith(
        $gameRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Graphics PackageRoot must be outside GameRoot; package records may not be installed as runtime files.'
}
$manifestPath = Join-Path $packageRoot 'SHA256SUMS.txt'
$gameExe = Join-Path $GameRoot 'sdhdship.exe'
$installedManifest = Join-Path $GameRoot 'ShenLong-SHA256SUMS.txt'
$legacyInstalledManifest = Join-Path $GameRoot 'SPatchGraphics-SHA256SUMS.txt'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$writeUtf8 = [Text.UTF8Encoding]::new($false)

function Get-Sha256([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-BytesSha256([byte[]] $Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
                $algorithm.ComputeHash($Bytes))).Replace('-', '')
    } finally {
        $algorithm.Dispose()
    }
}

function Assert-NoReparsePath(
    [string] $Root,
    [string] $Path,
    [string] $Label) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $pathFull = [IO.Path]::GetFullPath($Path)
    if ($pathFull -ine $rootFull -and
        -not $pathFull.StartsWith(
            $rootFull + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its reparse-check root: $pathFull"
    }

    $relative = if ($pathFull -ieq $rootFull) {
        ''
    } else {
        $pathFull.Substring($rootFull.Length + 1)
    }
    $cursor = $rootFull
    foreach ($component in @('') + @($relative -split '\\' | Where-Object {
                -not [string]::IsNullOrEmpty($_)
            })) {
        if (-not [string]::IsNullOrEmpty($component)) {
            $cursor = Join-Path $cursor $component
        }
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -eq $item) {
            break
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Label contains a reparse point: $cursor"
        }
    }
}

function Get-SafeChildPath(
    [string] $Root,
    [string] $RelativePath,
    [string] $Label) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $relative = $RelativePath.Replace('/', '\')
    if ([string]::IsNullOrWhiteSpace($relative) -or
        [IO.Path]::IsPathRooted($relative) -or
        @($relative -split '\\' | Where-Object {
                $_ -ceq '..' -or $_ -ceq '.' -or
                [string]::IsNullOrWhiteSpace($_)
            }).Count -ne 0) {
        throw "$Label contains an unsafe relative path: $RelativePath"
    }
    $path = [IO.Path]::GetFullPath((Join-Path $rootFull $relative))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $path.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its root: $RelativePath"
    }
    Assert-NoReparsePath $rootFull $path $Label
    return $path
}

function Get-Fnv1a64Utf16LePathHash([string] $Path) {
    $key = [IO.Path]::GetFullPath($Path).Replace('/', '\').ToLowerInvariant()

    # FNV-1a offset 14695981039346656037 and prime 1099511628211.
    # Keep the state as two UInt32 halves so multiplication has explicit
    # modulo-2^64 behavior in PowerShell.
    [uint32] $high = 3421674724
    [uint32] $low = 2216829733
    foreach ($character in $key.ToCharArray()) {
        $unit = [uint16][char]$character
        foreach ($byte in @(
                [byte]($unit -band 0xFF),
                [byte](($unit -shr 8) -band 0xFF))) {
            $low = [uint32]($low -bxor [uint32]$byte)
            $lowProduct = [uint64]$low * [uint64]435
            $nextLow = [uint32]($lowProduct -band [uint64]4294967295)
            $nextHigh = [uint32]((
                    ([uint64]$high * [uint64]435) +
                    ([uint64]$low * [uint64]256) +
                    ($lowProduct -shr 32)) -band [uint64]4294967295)
            $low = $nextLow
            $high = $nextHigh
        }
    }
    return $high.ToString('x8', [Globalization.CultureInfo]::InvariantCulture) +
        $low.ToString('x8', [Globalization.CultureInfo]::InvariantCulture)
}

function Get-LocalAppDataRoot {
    $value = [Environment]::GetEnvironmentVariable(
        'LOCALAPPDATA', [EnvironmentVariableTarget]::Process)
    if ([string]::IsNullOrWhiteSpace($value) -or
        -not [IO.Path]::IsPathFullyQualified($value)) {
        throw 'LOCALAPPDATA must name an absolute directory for external backups.'
    }
    $root = [IO.Path]::GetFullPath($value).TrimEnd([char[]]'\/')
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "LOCALAPPDATA directory is missing: $root"
    }
    Assert-NoReparsePath $root $root 'LOCALAPPDATA backup root'
    return $root
}

function Get-DeterministicExternalBackupPath(
    [string] $Owner,
    [string] $SourcePath,
    [string] $FileName) {
    if ([string]::IsNullOrWhiteSpace($Owner) -or
        [string]::IsNullOrWhiteSpace($FileName) -or
        [IO.Path]::GetFileName($FileName) -cne $FileName) {
        throw 'External backup owner and filename must be exact leaf names.'
    }
    $localRoot = Get-LocalAppDataRoot
    $hash = Get-Fnv1a64Utf16LePathHash $SourcePath
    $relative = "$Owner\ConfigBackups\$hash\$FileName"
    return [pscustomobject]@{
        Root = $localRoot
        Path = Get-SafeChildPath $localRoot $relative 'External backup path'
        Hash = $hash
    }
}

function Get-TreeRelativeFiles([string] $Root) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    return @(
        Get-ChildItem -LiteralPath $rootFull -File -Recurse -Force | ForEach-Object {
            $fullPath = [IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Enumerated file escaped its package root: $fullPath"
            }
            $fullPath.Substring($prefix.Length).Replace('/', '\')
        })
}

function New-TrackedDirectory(
    [string] $Path,
    [Collections.Generic.List[string]] $CreatedDirectories) {
    $missing = [Collections.Generic.List[string]]::new()
    $cursor = $Path
    while (-not (Test-Path -LiteralPath $cursor)) {
        $missing.Add($cursor)
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cursor) {
            throw "Could not resolve a safe parent for directory: $Path"
        }
        $cursor = $parent
    }
    if (-not (Test-Path -LiteralPath $cursor -PathType Container)) {
        throw "A required parent path is not a directory: $cursor"
    }
    [IO.Directory]::CreateDirectory($Path) | Out-Null
    for ($index = $missing.Count - 1; $index -ge 0; --$index) {
        $CreatedDirectories.Add($missing[$index])
    }
}

function Test-ShenLongDeploymentPath([string] $RelativePath) {
    return $RelativePath -ieq 'ShenLong.asi' -or
        $RelativePath -ieq 'ShenLong.ini' -or
        $RelativePath -ieq 'dxgi.dll' -or
        $RelativePath -ieq 'ReShade.ini' -or
        $RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
}

function Test-RetiredInstalledShenLongPath([string] $RelativePath) {
    return $RelativePath -ieq 'SHENLONG-README.md' -or
        $RelativePath -ieq 'GRAPHICS-README.md' -or
        $RelativePath -ieq 'Install-ShenLong.ps1' -or
        $RelativePath -ieq 'Install-GraphicsPackage.ps1' -or
        $RelativePath -ieq 'ReShadeIniPolicy.ps1'
}

function Assert-ReplaceableShenLongTree(
    [string] $ShenLongRoot,
    [object[]] $PackageEntries,
    [string] $InstalledManifestPath,
    [string] $InstallRoot,
    [Collections.Generic.HashSet[string]] $PreservedUnownedPaths) {
    $priorEntries = [Collections.Generic.List[object]]::new()
    $packagePaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $allowedFiles = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $allowedDirectories = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $packageShenLongEntries = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $PackageEntries) {
        [void] $packagePaths.Add($entry.RelativePath)
        if (-not $entry.RelativePath.StartsWith(
                'ShenLong\', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $relative = $entry.RelativePath.Substring('ShenLong\'.Length)
        $packageShenLongEntries.Add($relative, $entry)
        [void] $allowedFiles.Add($relative)
        $parent = [IO.Path]::GetDirectoryName($relative)
        while (-not [string]::IsNullOrWhiteSpace($parent)) {
            [void] $allowedDirectories.Add($parent)
            $next = [IO.Path]::GetDirectoryName($parent)
            if ($next -eq $parent) {
                break
            }
            $parent = $next
        }
    }

    if (Test-Path -LiteralPath $InstalledManifestPath) {
        Assert-NoReparsePath `
            $InstallRoot $InstalledManifestPath 'Installed graphics manifest'
        if (-not (Test-Path -LiteralPath $InstalledManifestPath -PathType Leaf)) {
            throw "Installed graphics manifest path is not a file: $InstalledManifestPath"
        }
        $seenInstalledPaths = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        $installedManifestText = [IO.File]::ReadAllText(
            $InstalledManifestPath, $strictUtf8)
        foreach ($line in @($installedManifestText -split '\r?\n' |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
            if ($line -cnotmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$') {
                throw "Malformed installed graphics checksum line: $line"
            }
            $installedRelative = $Matches['path'].Replace('/', '\')
            if (-not $seenInstalledPaths.Add($installedRelative)) {
                throw "Duplicate installed graphics path: $installedRelative"
            }
            $installedPath = Get-SafeChildPath `
                $InstallRoot $installedRelative 'Installed graphics manifest'
            $exists = Test-Path -LiteralPath $installedPath
            if ($exists -and
                -not (Test-Path -LiteralPath $installedPath -PathType Leaf)) {
                throw "Previously managed graphics path is not a file: $installedRelative"
            }
            $verifiedOwned = $false
            if ($exists) {
                $verifiedOwned = (Get-Sha256 $installedPath) -ceq $Matches['hash']
            }
            $priorEntry = [pscustomobject]@{
                RelativePath = $installedRelative
                Path = $installedPath
                Sha256 = $Matches['hash']
                Exists = $exists
                VerifiedOwned = $verifiedOwned
            }
            $priorEntries.Add($priorEntry)

            if (-not $packagePaths.Contains($installedRelative) -and
                $exists -and -not $verifiedOwned) {
                throw ("Previously managed graphics file changed since installation: " +
                       $installedRelative)
            }
            if (-not $installedRelative.StartsWith(
                    'ShenLong\', [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            $shenLongRelative = $installedRelative.Substring('ShenLong\'.Length)
            if ($allowedFiles.Contains($shenLongRelative) -or -not $exists) {
                continue
            }
            if (-not $verifiedOwned) {
                throw ("Previously managed ShenLong file changed since installation: " +
                       $installedRelative)
            }
            [void] $allowedFiles.Add($shenLongRelative)
            $parent = [IO.Path]::GetDirectoryName($shenLongRelative)
            while (-not [string]::IsNullOrWhiteSpace($parent)) {
                [void] $allowedDirectories.Add($parent)
                $next = [IO.Path]::GetDirectoryName($parent)
                if ($next -eq $parent) {
                    break
                }
                $parent = $next
            }
        }
    }

    $verifiedOwnedFiles = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($priorEntry in $priorEntries) {
        if (-not $priorEntry.VerifiedOwned -or
            -not $priorEntry.RelativePath.StartsWith(
                'ShenLong\', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $ownedRelative = $priorEntry.RelativePath.Substring('ShenLong\'.Length)
        [void] $verifiedOwnedFiles.Add($ownedRelative)
    }

    if (-not (Test-Path -LiteralPath $ShenLongRoot)) {
        return $priorEntries
    }
    if (-not (Test-Path -LiteralPath $ShenLongRoot -PathType Container)) {
        throw "Installed ShenLong path is not a directory: $ShenLongRoot"
    }
    Assert-NoReparsePath $InstallRoot $ShenLongRoot 'Installed ShenLong root'

    $shenLongFull = [IO.Path]::GetFullPath($ShenLongRoot).TrimEnd([char[]]'\/')
    $shenLongPrefix = $shenLongFull + [IO.Path]::DirectorySeparatorChar
    foreach ($item in Get-ChildItem -LiteralPath $shenLongFull -Force -Recurse) {
        $fullPath = [IO.Path]::GetFullPath($item.FullName)
        if (-not $fullPath.StartsWith(
                $shenLongPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Enumerated installed graphics path escaped ShenLong: $fullPath"
        }
        $relative = $fullPath.Substring($shenLongPrefix.Length).Replace('/', '\')
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Installed ShenLong tree contains a reparse point: $relative"
        }
        $underRuntimeOutput = $relative -ieq 'ReShadeCache' -or
            $relative.StartsWith(
                'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase) -or
            $relative -ieq 'ShaderDump' -or
            $relative.StartsWith(
                'ShaderDump\', [StringComparison]::OrdinalIgnoreCase)
        if ($underRuntimeOutput) {
            if (($relative -ieq 'ReShadeCache' -or
                    $relative -ieq 'ShaderDump') -and -not $item.PSIsContainer) {
                throw "Installed ShenLong runtime-output path is not a directory: $relative"
            }
            continue
        }
        if ($item.PSIsContainer) {
            if (-not $allowedDirectories.Contains($relative)) {
                throw "Installed ShenLong tree contains an unmanaged directory: $relative"
            }
        } else {
            if (-not $allowedFiles.Contains($relative)) {
                throw "Installed ShenLong tree contains an unmanaged file: $relative"
            }
            if (-not $verifiedOwnedFiles.Contains($relative)) {
                if (-not $packageShenLongEntries.ContainsKey($relative)) {
                    throw "Installed ShenLong tree contains an unowned file: $relative"
                }
                $packageEntry = $packageShenLongEntries[$relative]
                if ((Get-Sha256 $fullPath) -cne $packageEntry.Sha256) {
                    throw "Installed ShenLong tree contains a conflicting unowned file: $relative"
                }
                [void] $PreservedUnownedPaths.Add($packageEntry.RelativePath)
            }
        }
    }
    return $priorEntries
}

function Assert-CompatibleAsiLoader([string] $InstallRoot) {
    $loader = Join-Path $InstallRoot 'dinput8.dll'
    Assert-NoReparsePath $InstallRoot $loader 'ASI loader'
    if (-not (Test-Path -LiteralPath $loader -PathType Leaf)) {
        throw ('ShenLong.asi requires an existing x64 dinput8.dll ASI loader ' +
               'beside sdhdship.exe; install a compatible x64 ASI loader ' +
               'separately.')
    }
    $bytes = [IO.File]::ReadAllBytes($loader)
    if ($bytes.Length -lt 0x100 -or $bytes[0] -ne 0x4D -or
        $bytes[1] -ne 0x5A) {
        throw "ASI loader is not a valid PE image: $loader"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 26 -gt $bytes.Length -or
        $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "ASI loader has an invalid PE header: $loader"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $optionalMagic -ne 0x20B) {
        throw ('ASI loader must be a native x64 PE32+ image; found ' +
               ('machine=0x{0:X4}, optionalMagic=0x{1:X4}' -f
                    $machine, $optionalMagic))
    }
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    if ($ascii.IndexOf('DirectInput8Create',
            [StringComparison]::Ordinal) -lt 0 -or
        $ascii.IndexOf('.asi',
            [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw ('dinput8.dll does not contain the DirectInput8Create export ' +
               'marker and static .asi loader marker required by ShenLong.')
    }
    return [pscustomobject]@{
        Path = $loader
        Sha256 = Get-Sha256 $loader
    }
}

function Read-IniMap([string] $Path, [string] $Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $sections = [Collections.Generic.Dictionary[
        string, Collections.Generic.Dictionary[string, string]]]::new(
            [StringComparer]::OrdinalIgnoreCase)
    $section = ''
    foreach ($rawLine in [IO.File]::ReadAllLines($Path, $strictUtf8)) {
        $line = $rawLine.Trim()
        if ([string]::IsNullOrWhiteSpace($line) -or
            $line.StartsWith(';') -or $line.StartsWith('#') -or
            $line.StartsWith('/')) {
            continue
        }
        if ($line -match '^\[(?<section>[^\]\r\n]+)\]') {
            $section = $Matches['section'].Trim()
            if (-not $sections.ContainsKey($section)) {
                $sections.Add($section,
                    [Collections.Generic.Dictionary[string, string]]::new(
                        [StringComparer]::OrdinalIgnoreCase))
            }
            continue
        }
        if ([string]::IsNullOrWhiteSpace($section) -or
            $line -notmatch '^(?<key>[^=]+?)\s*=\s*(?<value>.*)$') {
            continue
        }
        $key = $Matches['key'].Trim()
        $value = $Matches['value'].Trim()
        $comment = $value.IndexOf(';')
        if ($comment -ge 0) {
            $value = $value.Substring(0, $comment).Trim()
        }
        $sections[$section][$key] = $value
    }
    return $sections
}

function Get-IniCandidateValue(
    [object] $Sections,
    [string[]] $Candidates) {
    foreach ($candidate in $Candidates) {
        $parts = $candidate.Split([char]'|', 2)
        if ($parts.Count -ne 2 -or -not $Sections.ContainsKey($parts[0])) {
            continue
        }
        $values = $Sections[$parts[0]]
        if ($values.ContainsKey($parts[1])) {
            return $values[$parts[1]]
        }
    }
    return $null
}

function Get-ShenLongMigrationSpecs {
    return @(
        [pscustomobject]@{ Target='Tonemapping|AgX'; Pattern='^(?:0|1)$'; Sources=@('Tonemapping|AgX','Tonemapping|ACES','SPatch|AgX','SPatch|agx_enable','SPatch|aces_enable') },
        [pscustomobject]@{ Target='Tonemapping|AgXLook'; Pattern='^(?:Neutral|MediumHigh)$'; Sources=@('Tonemapping|AgXLook','SPatch|AgXLook','SPatch|agx_look') },
        [pscustomobject]@{ Target='Tonemapping|AgXStrength'; Pattern='^-?[0-9]+$'; Sources=@('Tonemapping|AgXStrength','SPatch|AgXStrength','SPatch|agx_strength_percent') },
        [pscustomobject]@{ Target='Tonemapping|AgXExposure'; Pattern='^-?[0-9]+$'; Sources=@('Tonemapping|AgXExposure','SPatch|AgXExposure','SPatch|agx_exposure_scale_percent') },
        [pscustomobject]@{ Target='Shadows|ShadowResolution'; Pattern='^(?:0|2048|4096)$'; Sources=@('Shadows|ShadowResolution','SPatch|ShadowResolution','SPatch|shadow_resolution') },
        [pscustomobject]@{ Target='AmbientOcclusion|AmbientOcclusion'; Pattern='^(?:Original|SDAO|GTAOLite)$'; Sources=@('AmbientOcclusion|AmbientOcclusion','Graphics|AmbientOcclusion','SPatch|AmbientOcclusion') },
        [pscustomobject]@{ Target='AmbientOcclusion|OriginalAOQuality'; Pattern='^-?[0-9]+$'; Sources=@('AmbientOcclusion|OriginalAOQuality','Graphics|OriginalAOQuality','SPatch|OriginalAOQuality','SPatch|override_ssao') },
        [pscustomobject]@{ Target='AmbientOcclusion|SDAOQuality'; Pattern='^-?[0-9]+$'; Sources=@('AmbientOcclusion|SDAOQuality','SPatch|SDAOQuality','SPatch|sdao_quality') },
        [pscustomobject]@{ Target='AmbientOcclusion|SDAORadius'; Pattern='^-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)$'; Sources=@('AmbientOcclusion|SDAORadius','SPatch|SDAORadius','SPatch|sdao_radius') },
        [pscustomobject]@{ Target='AmbientOcclusion|SDAOStrength'; Pattern='^-?[0-9]+$'; Sources=@('AmbientOcclusion|SDAOStrength','SPatch|SDAOStrength','SPatch|sdao_strength_percent') },
        [pscustomobject]@{ Target='AmbientOcclusion|GTAOLiteQuality'; Pattern='^-?[0-9]+$'; Sources=@('AmbientOcclusion|GTAOLiteQuality','AmbientOcclusion|GTAOQuality','SPatch|GTAOLiteQuality','SPatch|gtao_lite_quality') },
        [pscustomobject]@{ Target='AmbientOcclusion|GTAOLiteRadius'; Pattern='^-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)$'; Sources=@('AmbientOcclusion|GTAOLiteRadius','AmbientOcclusion|GTAORadius','SPatch|GTAOLiteRadius','SPatch|gtao_lite_radius') },
        [pscustomobject]@{ Target='AmbientOcclusion|GTAOLiteStrength'; Pattern='^-?[0-9]+$'; Sources=@('AmbientOcclusion|GTAOLiteStrength','AmbientOcclusion|GTAOStrength','SPatch|GTAOLiteStrength','SPatch|gtao_lite_strength_percent') },
        [pscustomobject]@{ Target='GlobalIllumination|GlobalIllumination'; Pattern='^(?:0|1)$'; Sources=@('GlobalIllumination|GlobalIllumination','SPatch|GlobalIllumination','SPatch|global_illumination') },
        [pscustomobject]@{ Target='GlobalIllumination|GIQuality'; Pattern='^-?[0-9]+$'; Sources=@('GlobalIllumination|GIQuality','SPatch|GIQuality','SPatch|gi_quality') },
        [pscustomobject]@{ Target='GlobalIllumination|GIStrength'; Pattern='^-?[0-9]+$'; Sources=@('GlobalIllumination|GIStrength','SPatch|GIStrength','SPatch|gi_strength_percent') },
        [pscustomobject]@{ Target='GlobalIllumination|GIRadius'; Pattern='^-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)$'; Sources=@('GlobalIllumination|GIRadius','SPatch|GIRadius','SPatch|gi_radius') },
        [pscustomobject]@{ Target='PhysicallyBasedRendering|PhysicallyBasedRendering'; Pattern='^(?:0|1)$'; Sources=@('PhysicallyBasedRendering|PhysicallyBasedRendering','Graphics|PhysicallyBasedRendering','SPatch|PhysicallyBasedRendering','SPatch|physically_based_rendering','SPatch|pbr') },
        [pscustomobject]@{ Target='SubsurfaceScattering|SubsurfaceScattering'; Pattern='^(?:0|1)$'; Sources=@('SubsurfaceScattering|SubsurfaceScattering','SPatch|SubsurfaceScattering','SPatch|subsurface_scattering') },
        [pscustomobject]@{ Target='SubsurfaceScattering|StockHairBlur'; Pattern='^(?:0|1)$'; Sources=@('SubsurfaceScattering|StockHairBlur','SPatch|StockHairBlur','SPatch|stock_hair_blur') },
        [pscustomobject]@{ Target='SubsurfaceScattering|SSSQuality'; Pattern='^-?[0-9]+$'; Sources=@('SubsurfaceScattering|SSSQuality','SPatch|SSSQuality','SPatch|sss_quality') },
        [pscustomobject]@{ Target='SubsurfaceScattering|SSSStrength'; Pattern='^-?[0-9]+$'; Sources=@('SubsurfaceScattering|SSSStrength','SPatch|SSSStrength','SPatch|sss_strength_percent') },
        [pscustomobject]@{ Target='SubsurfaceScattering|SSSRadius'; Pattern='^-?[0-9]+$'; Sources=@('SubsurfaceScattering|SSSRadius','SPatch|SSSRadius','SPatch|sss_radius_percent') },
        [pscustomobject]@{ Target='MaterialScattering|EyeScattering'; Pattern='^(?:0|1)$'; Sources=@('MaterialScattering|EyeScattering','SPatch|EyeScattering','SPatch|eye_scattering') },
        [pscustomobject]@{ Target='MaterialScattering|HairScattering'; Pattern='^(?:0|1)$'; Sources=@('MaterialScattering|HairScattering','SPatch|HairScattering','SPatch|hair_scattering') },
        [pscustomobject]@{ Target='MaterialScattering|TeethScattering'; Pattern='^(?:0|1)$'; Sources=@('MaterialScattering|TeethScattering','SPatch|TeethScattering','SPatch|teeth_scattering') },
        [pscustomobject]@{ Target='MaterialScattering|FoliageTransmission'; Pattern='^(?:0|1)$'; Sources=@('MaterialScattering|FoliageTransmission','SPatch|FoliageTransmission','SPatch|foliage_transmission') },
        [pscustomobject]@{ Target='MaterialScattering|WaterScattering'; Pattern='^(?:0|1)$'; Sources=@('MaterialScattering|WaterScattering','SPatch|WaterScattering','SPatch|water_scattering') },
        [pscustomobject]@{ Target='Debug|DumpShaders'; Pattern='^(?:0|1)$'; Sources=@('Debug|DumpShaders') },
        [pscustomobject]@{ Target='Debug|CensusShadowConsumers'; Pattern='^(?:0|1)$'; Sources=@('Debug|CensusShadowConsumers') }
    )
}

function Assert-ShenLongConfigVersion([string] $Path, [string] $Label) {
    $text = [IO.File]::ReadAllText($Path, $strictUtf8)
    $sectionMatches = @([regex]::Matches(
        $text,
        '(?ms)^[ \t]*\[ShenLong\][^\r\n]*(?:\r?\n|$)(?<body>.*?)(?=^[ \t]*\[[^\]\r\n]+\][^\r\n]*(?:\r?\n|$)|\z)'))
    if ($sectionMatches.Count -ne 1) {
        throw "$Label must contain exactly one [ShenLong] section; found $($sectionMatches.Count)."
    }
    $versionMatches = @([regex]::Matches(
        $sectionMatches[0].Groups['body'].Value,
        '(?m)^[ \t]*ConfigVersion[ \t]*=[ \t]*(?<value>[^;#\r\n]*?)[ \t]*(?:[;#].*)?\r?$'))
    if ($versionMatches.Count -ne 1 -or
        $versionMatches[0].Groups['value'].Value.Trim() -cne '1') {
        throw "$Label must contain exactly one [ShenLong] ConfigVersion=1 setting."
    }
}

function Assert-NoSPatchTextureFiltering([string] $Text, [string] $Label) {
    if ($Text -match
        '(?im)^\s*\[TextureFiltering\]\s*$|^\s*(?:AnisotropicFiltering|ForceAnisotropicFiltering)\s*=') {
        throw "$Label contains SPatch-owned texture-filter configuration."
    }
}

function Get-CanonicalExistingShenLongConfig([string] $Path) {
    $sourceBytes = [IO.File]::ReadAllBytes($Path)
    $sourceText = $strictUtf8.GetString($sourceBytes)
    $hadBom = $sourceText.Length -ne 0 -and
        $sourceText[0] -eq [char]0xFEFF
    $body = if ($hadBom) {
        $sourceText.Substring(1)
    } else {
        $sourceText
    }

    $sectionPattern =
        '(?ms)^[ \t]*\[TextureFiltering\][^\r\n]*(?:\r\n|\n|\r|$).*?(?=^[ \t]*\[[^\]\r\n]+\][^\r\n]*(?:\r\n|\n|\r|$)|\z)'
    $sectionCount = [regex]::Matches($body, $sectionPattern).Count
    $canonicalBody = [regex]::Replace($body, $sectionPattern, '')

    $keyPattern =
        '(?m)^[ \t]*(?:AnisotropicFiltering|ForceAnisotropicFiltering)[ \t]*=[^\r\n]*(?:\r\n|\n|\r|$)'
    $keyCount = [regex]::Matches($canonicalBody, $keyPattern).Count
    $canonicalBody = [regex]::Replace($canonicalBody, $keyPattern, '')

    $canonicalText = if ($hadBom) {
        ([string][char]0xFEFF) + $canonicalBody
    } else {
        $canonicalBody
    }
    Assert-NoSPatchTextureFiltering $canonicalText 'Canonical ShenLong.ini'
    $canonicalBytes = $writeUtf8.GetBytes($canonicalText)
    return [pscustomobject]@{
        Bytes = $canonicalBytes
        Sha256 = Get-BytesSha256 $canonicalBytes
        Changed = (Get-BytesSha256 $sourceBytes) -cne
            (Get-BytesSha256 $canonicalBytes)
        RemovedSections = $sectionCount
        RemovedKeys = $keyCount
    }
}

function Assert-ShenLongPackageConfig([string] $Path) {
    $text = [IO.File]::ReadAllText($Path, $strictUtf8)
    if ($text -match '(?im)^\s*(?:ShadowFilterScale|PCSS\w*)\s*=') {
        throw 'Packaged ShenLong.ini contains a retired ShadowFilterScale or PCSS key.'
    }
    Assert-NoSPatchTextureFiltering $text 'Packaged ShenLong.ini'
    Assert-ShenLongConfigVersion $Path 'Packaged ShenLong.ini'
    $sections = Read-IniMap $Path 'Packaged ShenLong.ini'
    foreach ($required in @('ShenLong|Enabled')) {
        if ($null -eq (Get-IniCandidateValue $sections @($required))) {
            throw "Packaged ShenLong.ini omits required key: $required"
        }
    }
    foreach ($spec in Get-ShenLongMigrationSpecs) {
        if ($null -eq (Get-IniCandidateValue $sections @($spec.Target))) {
            throw "Packaged ShenLong.ini omits required key: $($spec.Target)"
        }
    }
}

function Get-MigratedShenLongConfig(
    [string] $PackageConfig,
    [object[]] $Sources) {
    $selected = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $sourcesUsed = [Collections.Generic.List[string]]::new()
    foreach ($source in $Sources) {
        $sourcePath = [string]$source.Path
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            continue
        }
        Assert-NoReparsePath ([string]$source.Root) $sourcePath `
            ([string]$source.Label)
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "$($source.Label) is not a file: $sourcePath"
        }
        $sourceSections = Read-IniMap $sourcePath ([string]$source.Label)
        $sourceContributed = $false
        foreach ($spec in Get-ShenLongMigrationSpecs) {
            if ($selected.ContainsKey($spec.Target)) {
                continue
            }
            $value = Get-IniCandidateValue $sourceSections $spec.Sources
            if ($null -ne $value -and $value -cmatch $spec.Pattern) {
                $selected[$spec.Target] = $value
                $sourceContributed = $true
            }
        }
        if ($sourceContributed) {
            $sourcesUsed.Add($sourcePath)
        }
    }

    if ($selected.Count -eq 0) {
        return [pscustomobject]@{
            Bytes = [IO.File]::ReadAllBytes($PackageConfig)
            MigratedValues = 0
            Sources = @()
        }
    }

    $lines = [Collections.Generic.List[string]]::new()
    $section = ''
    foreach ($rawLine in [IO.File]::ReadAllLines($PackageConfig, $strictUtf8)) {
        $line = $rawLine
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(?<section>[^\]\r\n]+)\]') {
            $section = $Matches['section'].Trim()
        } elseif (-not [string]::IsNullOrWhiteSpace($section) -and
            $trimmed -match '^(?<key>[^=]+?)\s*=') {
            $target = $section + '|' + $Matches['key'].Trim()
            if ($selected.ContainsKey($target)) {
                $line = $Matches['key'].Trim() + '=' + $selected[$target]
            }
        }
        $lines.Add($line)
    }
    $text = [string]::Join("`r`n", $lines) + "`r`n"
    return [pscustomobject]@{
        Bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
        MigratedValues = $selected.Count
        Sources = $sourcesUsed.ToArray()
    }
}

function Get-StrictInstalledEntries([string] $Path, [string] $Label) {
    $result = [Collections.Generic.List[object]]::new()
    if (-not (Test-Path -LiteralPath $Path)) {
        return $result
    }
    Assert-NoReparsePath $GameRoot $Path $Label
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label path is not a file: $Path"
    }
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($Path, $strictUtf8)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -cnotmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$') {
            throw "Malformed $Label checksum line: $line"
        }
        $relative = $Matches['path'].Replace('/', '\')
        if (-not $seen.Add($relative)) {
            throw "Duplicate $Label path: $relative"
        }
        $installedPath = Get-SafeChildPath $GameRoot $relative $Label
        $exists = Test-Path -LiteralPath $installedPath
        if ($exists -and -not (Test-Path -LiteralPath $installedPath -PathType Leaf)) {
            throw "$Label path is not a file: $relative"
        }
        $result.Add([pscustomobject]@{
                RelativePath = $relative
                Path = $installedPath
                Sha256 = $Matches['hash']
                Exists = $exists
                VerifiedOwned = $exists -and
                    (Get-Sha256 $installedPath) -ceq $Matches['hash']
            })
    }
    return $result
}

function Test-LegacyGraphicsRuntimePath([string] $RelativePath) {
    return $RelativePath -imatch ('^(?:SPatchGraphics.*|SPatchGI|SPatchGTAO|' +
            'SPatchSDAO|SPatchSSS|SPatchPBR|SPatchPCSS|' +
            'Luma-Sleeping Dogs Definitive Edition|ShenLong)\.' +
            '(?:addon|addon64)$') -or
        $RelativePath -ieq 'dxgi.dll' -or
        $RelativePath -ieq 'ReShade.ini' -or
        $RelativePath.StartsWith(
            'SPatch\ShaderCache\v1\', [StringComparison]::OrdinalIgnoreCase) -or
        $RelativePath.StartsWith(
            'SPatch\GI\', [StringComparison]::OrdinalIgnoreCase) -or
        $RelativePath.StartsWith(
            'SPatch\PBR\', [StringComparison]::OrdinalIgnoreCase) -or
        $RelativePath.StartsWith(
            'SPatch\SDAO\', [StringComparison]::OrdinalIgnoreCase) -or
        $RelativePath.StartsWith(
            'SPatch\SSS\', [StringComparison]::OrdinalIgnoreCase) -or
        $RelativePath.StartsWith(
            'SPatch\Water\', [StringComparison]::OrdinalIgnoreCase)
}

if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "Graphics package root is missing: $packageRoot"
}
Assert-NoReparsePath $packageRoot $packageRoot 'Graphics package root'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Graphics package manifest is missing: $manifestPath"
}
Assert-NoReparsePath $packageRoot $manifestPath 'Graphics package manifest'
foreach ($packageItem in Get-ChildItem -LiteralPath $packageRoot -Force -Recurse) {
    if (($packageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        $packageRelative = $packageItem.FullName.Substring(
            $packageRoot.Length + 1)
        throw "Graphics package contains a reparse point: $packageRelative"
    }
}

$manifestText = [IO.File]::ReadAllText($manifestPath, $strictUtf8)
$entries = [Collections.Generic.List[object]]::new()
$entriesByPath = [Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($line in @($manifestText -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
    if ($line -cnotmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$') {
        throw "Malformed graphics checksum line: $line"
    }
    $declaredHash = $Matches['hash']
    $relative = $Matches['path'].Replace('/', '\')
    if ($relative -ieq 'SHA256SUMS.txt') {
        throw 'The graphics manifest may not contain itself.'
    }
    if ($entriesByPath.ContainsKey($relative)) {
        throw "Duplicate graphics package path: $relative"
    }
    $source = Get-SafeChildPath $packageRoot $relative 'Package manifest'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Graphics package file is missing: $relative"
    }
    $actualHash = Get-Sha256 $source
    if ($actualHash -cne $declaredHash) {
        throw ("Graphics package file $relative has SHA-256 $actualHash; " +
               "expected $declaredHash.")
    }
    $entry = [pscustomobject]@{
        RelativePath = $relative
        ManifestPath = $relative.Replace('\', '/')
        Source = $source
        Sha256 = $actualHash
        Length = (Get-Item -LiteralPath $source).Length
    }
    $entries.Add($entry)
    $entriesByPath.Add($relative, $entry)
}
if ($entries.Count -eq 0) {
    throw 'Graphics package manifest is empty.'
}

$expectedPackageFiles = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $entries) {
    [void] $expectedPackageFiles.Add($entry.RelativePath)
}
[void] $expectedPackageFiles.Add('SHA256SUMS.txt')
$actualPackageFiles = @(Get-TreeRelativeFiles $packageRoot)
if ($actualPackageFiles.Count -ne $expectedPackageFiles.Count) {
    throw ("$Configuration package tree is not exact: expected " +
           "$($expectedPackageFiles.Count) files, found $($actualPackageFiles.Count).")
}
foreach ($relative in $actualPackageFiles) {
    if (-not $expectedPackageFiles.Contains($relative)) {
        throw "$Configuration package contains an unmanifested file: $relative"
    }
}

foreach ($required in @(
        'ShenLong.asi',
        'ShenLong.ini',
        'dxgi.dll',
        'ReShade.ini',
        'SHENLONG-README.md',
        'Install-ShenLong.ps1',
        'ReShadeIniPolicy.ps1',
        'THIRD_PARTY_NOTICES.md',
        'ShenLong\ShaderCache\v1\manifest.tsv')) {
    if (-not $entriesByPath.ContainsKey($required)) {
        throw "$Configuration manifest omits required file: $required"
    }
}
$reShadePolicyPath = $entriesByPath['ReShadeIniPolicy.ps1'].Source
. $reShadePolicyPath
$reShadeText = [IO.File]::ReadAllText(
    $entriesByPath['ReShade.ini'].Source, $strictUtf8)
[void](Assert-ReShadeRootAddonPolicy $reShadeText 'Packaged ReShade.ini' `
    -RequireAddonSection -RequireAddonPath)
$packagedShenLongConfig = $entriesByPath['ShenLong.ini'].Source
Assert-ShenLongPackageConfig $packagedShenLongConfig
if ($Configuration -eq 'Publishing-Release') {
    $packagedSources = @($entries | Where-Object {
            [IO.Path]::GetExtension($_.RelativePath) -in @(
                '.hlsl', '.hlsli', '.fx')
        })
    if ($packagedSources.Count -ne 0) {
        throw ("Publishing-Release contains runtime shader source: " +
               ($packagedSources.RelativePath -join ', '))
    }
}

$cacheManifestEntry = $entriesByPath['ShenLong\ShaderCache\v1\manifest.tsv']
$cacheLines = [IO.File]::ReadAllLines($cacheManifestEntry.Source, $strictUtf8)
$cacheHeader = "Configuration`tSSSDevelopment`tSHA256`tBytes`tPath`tSource`tEntryPoint`tProfile`tDefines"
if ($cacheLines.Count -lt 2 -or $cacheLines[0] -cne $cacheHeader) {
    throw 'The precompiled shader-cache manifest header is invalid.'
}
$expectedSssDevelopment = if ($Configuration -eq 'Development-Release') {
    '1'
} else {
    '0'
}
$featureCounts = [ordered]@{
    PBR = 0
    GI = 0
    SDAO = 0
    SSS = 0
    Water = 0
}
$expectedFeatureCounts = [ordered]@{
    PBR = 18
    GI = 36
    SDAO = 26
    SSS = 11
    Water = 3
}
$seenCachePaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
for ($index = 1; $index -lt $cacheLines.Count; ++$index) {
    $fields = $cacheLines[$index].Split([char]9)
    if ($fields.Count -ne 9 -or
        $fields[0] -cne $Configuration -or
        $fields[1] -cne $expectedSssDevelopment -or
        $fields[2] -cnotmatch '^[0-9A-F]{64}$') {
        throw "Malformed precompiled shader-cache manifest row $($index + 1)."
    }
    [long] $byteLength = 0
    if (-not [long]::TryParse(
            $fields[3],
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$byteLength) -or
        $byteLength -le 0) {
        throw "Invalid shader byte length at cache-manifest row $($index + 1)."
    }
    $cacheRelative = $fields[4].Replace('/', '\')
    [void] (Get-SafeChildPath 'C:\ShenLongCacheValidationRoot' $cacheRelative `
        'Shader-cache manifest')
    if (-not $seenCachePaths.Add($cacheRelative)) {
        throw "Duplicate precompiled shader-cache path: $cacheRelative"
    }
    $feature = $cacheRelative.Split([char]'\')[0]
    if (-not $featureCounts.Contains($feature) -or
        [IO.Path]::GetExtension($cacheRelative) -cne '.cso') {
        throw "Unexpected precompiled shader-cache path: $cacheRelative"
    }
    ++$featureCounts[$feature]
    $outerRelative = "ShenLong\ShaderCache\v1\$cacheRelative"
    if (-not $entriesByPath.ContainsKey($outerRelative)) {
        throw "Outer graphics manifest omits cached shader: $outerRelative"
    }
    $outerEntry = $entriesByPath[$outerRelative]
    if ($outerEntry.Sha256 -cne $fields[2] -or
        $outerEntry.Length -ne $byteLength) {
        throw "Nested shader-cache identity mismatch: $cacheRelative"
    }
}
foreach ($feature in $expectedFeatureCounts.Keys) {
    if ($featureCounts[$feature] -ne $expectedFeatureCounts[$feature]) {
        throw ("$Configuration $feature cache is incomplete: expected " +
               "$($expectedFeatureCounts[$feature]), found $($featureCounts[$feature]).")
    }
}
$expectedCacheCount = 0
foreach ($count in $expectedFeatureCounts.Values) {
    $expectedCacheCount += $count
}
if ($seenCachePaths.Count -ne $expectedCacheCount) {
    throw ("$Configuration cache is incomplete: expected $expectedCacheCount " +
           "packaged shaders, found $($seenCachePaths.Count).")
}
$outerCacheShaders = @($entries | Where-Object {
        $_.RelativePath.StartsWith(
            'ShenLong\ShaderCache\v1\',
            [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetExtension($_.RelativePath) -ieq '.cso'
    })
if ($outerCacheShaders.Count -ne $seenCachePaths.Count) {
    throw 'The outer and nested graphics manifests cover different CSO sets.'
}

$validationResult = [ordered]@{
    Configuration = $Configuration
    PackageRoot = $packageRoot
    PackageManifestSha256 = Get-Sha256 $manifestPath
    ManifestedFiles = $entries.Count
    PrecompiledShaders = $seenCachePaths.Count
    MutationPerformed = $false
}
if ($ValidateOnly) {
    [pscustomobject]$validationResult
    return
}

$liveHarnessMutex = [Threading.Mutex]::new(
    $false, 'Local\SPatch.LiveGraphicsHarness')
$ownsLiveHarnessMutex = $false
try {
    try {
        $ownsLiveHarnessMutex = $liveHarnessMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $ownsLiveHarnessMutex = $true
    }
    if (-not $ownsLiveHarnessMutex) {
        throw 'Another SPatch/ShenLong live mutation already owns the game installation.'
    }

Assert-NoReparsePath $GameRoot $GameRoot 'Game root'

if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw "Sleeping Dogs: Definitive Edition executable is missing: $gameExe"
}
$asiLoader = Assert-CompatibleAsiLoader $GameRoot
if (Get-Process -Name sdhdship -ErrorAction SilentlyContinue) {
    throw 'Sleeping Dogs is running; the ShenLong package was not changed.'
}
if (Test-Path -LiteralPath (Join-Path $GameRoot 'Luma')) {
    throw 'A legacy Luma directory must be removed before ShenLong installation.'
}

$legacyInstalledEntries = @(Get-StrictInstalledEntries `
    $legacyInstalledManifest 'Legacy SPatchGraphics manifest')
$legacyByPath = [Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($legacyEntry in $legacyInstalledEntries) {
    $legacyByPath.Add($legacyEntry.RelativePath, $legacyEntry)
    if ($legacyEntry.Exists -and
        (Test-LegacyGraphicsRuntimePath $legacyEntry.RelativePath) -and
        -not $legacyEntry.VerifiedOwned) {
        throw ("Legacy SPatchGraphics manifest cannot prove ownership of " +
               "changed runtime file: $($legacyEntry.RelativePath)")
    }
}
$legacyModuleNames = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($stem in @(
        'SPatchGI', 'SPatchGTAO', 'SPatchSDAO', 'SPatchSSS', 'SPatchPBR',
        'SPatchPCSS', 'Luma-Sleeping Dogs Definitive Edition', 'ShenLong')) {
    foreach ($extension in @('.addon', '.addon64')) {
        [void] $legacyModuleNames.Add($stem + $extension)
    }
}
foreach ($candidate in Get-ChildItem -LiteralPath $GameRoot -File -Force) {
    $legacyModule = $legacyModuleNames.Contains($candidate.Name) -or
        $candidate.Name -imatch '^SPatchGraphics.*\.(?:addon|addon64)$'
    if (-not $legacyModule) {
        continue
    }
    Assert-NoReparsePath $GameRoot $candidate.FullName 'Legacy graphics module'
    if (-not $legacyByPath.ContainsKey($candidate.Name) -or
        -not $legacyByPath[$candidate.Name].VerifiedOwned) {
        throw ("Legacy graphics module is unowned or changed; refusing to " +
               "remove it: $($candidate.Name)")
    }
}
$legacyRemovalEntries = @($legacyInstalledEntries | Where-Object {
        # Legacy full-package manifests could claim these generic records, but
        # the SPatch base package owns the same game-root paths. Preserve them.
        $_.Exists -and $_.VerifiedOwned -and
        ((Test-LegacyGraphicsRuntimePath $_.RelativePath) -or
            (Test-RetiredInstalledShenLongPath $_.RelativePath)) -and
        $_.RelativePath -ine 'dxgi.dll' -and
        $_.RelativePath -ine 'ReShade.ini' -and
        $_.RelativePath -ine 'THIRD_PARTY_NOTICES.md' -and
        -not $_.RelativePath.StartsWith(
            'licenses\', [StringComparison]::OrdinalIgnoreCase)
    })

$shenLongRoot = Join-Path $GameRoot 'ShenLong'
$preservedUnownedShenLongPaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$priorInstalledEntries = @(Assert-ReplaceableShenLongTree `
    $shenLongRoot @($entries) $installedManifest $GameRoot `
    $preservedUnownedShenLongPaths)
$priorByPath = [Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($priorEntry in $priorInstalledEntries) {
    $priorByPath.Add($priorEntry.RelativePath, $priorEntry)
}
foreach ($legacyEntry in $legacyInstalledEntries) {
    if (-not $priorByPath.ContainsKey($legacyEntry.RelativePath)) {
        $priorByPath.Add($legacyEntry.RelativePath, $legacyEntry)
    }
}
$deploymentEntries = @($entries | Where-Object {
        Test-ShenLongDeploymentPath $_.RelativePath
    })
if ($deploymentEntries.Count -eq 0) {
    throw 'The graphics package contains no deployable runtime files.'
}
$deploymentPaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $deploymentEntries) {
    [void] $deploymentPaths.Add($entry.RelativePath)
}

$shenLongConfigDestination = Join-Path $GameRoot 'ShenLong.ini'
$preserveShenLongConfig = Test-Path -LiteralPath $shenLongConfigDestination
$preservedShenLongConfigHash = $null
$canonicalExistingShenLongConfig = $null
$persistentShenLongConfigBackup = $null
$shenLongConfigBackupLocation = $null
$needsPersistentShenLongConfigBackup = $false
$configMigration = $null
if ($preserveShenLongConfig) {
    Assert-NoReparsePath $GameRoot $shenLongConfigDestination `
        'Existing ShenLong.ini'
    if (-not (Test-Path -LiteralPath $shenLongConfigDestination -PathType Leaf)) {
        throw "Existing ShenLong.ini is not a file: $shenLongConfigDestination"
    }
    Assert-ShenLongConfigVersion $shenLongConfigDestination `
        'Existing ShenLong.ini'
    $preservedShenLongConfigHash = Get-Sha256 $shenLongConfigDestination
    $canonicalExistingShenLongConfig =
        Get-CanonicalExistingShenLongConfig $shenLongConfigDestination
    if ($canonicalExistingShenLongConfig.Changed) {
        $backupName = 'ShenLong-pre-texture-filtering-{0}.ini' -f
            $preservedShenLongConfigHash
        $shenLongConfigBackupLocation = Get-DeterministicExternalBackupPath `
            'ShenLong' $shenLongConfigDestination $backupName
        $persistentShenLongConfigBackup = $shenLongConfigBackupLocation.Path
        $backupFull = [IO.Path]::GetFullPath($persistentShenLongConfigBackup)
        if ($backupFull -ieq $GameRoot -or
            $backupFull.StartsWith(
                $gameRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The persistent ShenLong.ini backup must be outside GameRoot.'
        }
        if (Test-Path -LiteralPath $persistentShenLongConfigBackup) {
            if (-not (Test-Path -LiteralPath $persistentShenLongConfigBackup `
                    -PathType Leaf)) {
                throw ("Persistent ShenLong.ini backup path is not a file: " +
                       $persistentShenLongConfigBackup)
            }
            if ((Get-Sha256 $persistentShenLongConfigBackup) -cne
                $preservedShenLongConfigHash) {
                throw ("Persistent ShenLong.ini backup contains unexpected " +
                       "bytes: $persistentShenLongConfigBackup")
            }
        } else {
            $needsPersistentShenLongConfigBackup = $true
        }
    }
} else {
    $currentSpatchConfig = Get-SafeChildPath `
        $GameRoot 'SPatch.ini' 'Current SPatch graphics configuration'
    $externalSpatchBackup = Get-DeterministicExternalBackupPath `
        'SPatch' $currentSpatchConfig 'SPatch-pre-v42.ini'
    $legacySpatchConfig = Get-SafeChildPath `
        $GameRoot 'SPatch.ini.previous.bak' `
        'Legacy in-game SPatch graphics configuration'
    $configMigration = Get-MigratedShenLongConfig `
        $packagedShenLongConfig @(
            [pscustomobject]@{
                Root = $GameRoot
                Path = $currentSpatchConfig
                Label = 'Current SPatch graphics configuration'
            },
            [pscustomobject]@{
                Root = $externalSpatchBackup.Root
                Path = $externalSpatchBackup.Path
                Label = 'External pre-v42 SPatch graphics configuration'
            },
            [pscustomobject]@{
                Root = $GameRoot
                Path = $legacySpatchConfig
                Label = 'Legacy in-game SPatch graphics configuration'
            })
}

$reShadeDestination = Join-Path $GameRoot 'ReShade.ini'
$preserveReShade = $false
$retainReShadeOwnership = $false
$preservedReShadeHash = $null
$persistentReShadeBackup = $null
$needsPersistentReShadeBackup = $false
if (Test-Path -LiteralPath $reShadeDestination) {
    Assert-NoReparsePath $GameRoot $reShadeDestination 'Existing ReShade.ini'
    if (-not (Test-Path -LiteralPath $reShadeDestination -PathType Leaf)) {
        throw "ReShade.ini destination is not a file: $reShadeDestination"
    }
    $preservedReShadeHash = Get-Sha256 $reShadeDestination
    $priorReShade = if ($priorByPath.ContainsKey('ReShade.ini')) {
        $priorByPath['ReShade.ini']
    } else {
        $null
    }
    $reShadeIsVerifiedOwned = $null -ne $priorReShade -and
        $priorReShade.VerifiedOwned
    if (-not $ReplaceReShadeIni) {
        $preservedReShadeText = [IO.File]::ReadAllText(
            $reShadeDestination, $strictUtf8)
        [void](Assert-ReShadeRootAddonPolicy `
            $preservedReShadeText 'Existing ReShade.ini')
        $preserveReShade = $true
        $retainReShadeOwnership = $reShadeIsVerifiedOwned -and
            $preservedReShadeHash -ceq $entriesByPath['ReShade.ini'].Sha256
    } else {
        $reShadeBackupLocation = Get-DeterministicExternalBackupPath `
            'ShenLong' $reShadeDestination 'ReShade-pre-ShenLong.ini'
        $persistentReShadeBackup = $reShadeBackupLocation.Path
        $backupFull = [IO.Path]::GetFullPath($persistentReShadeBackup)
        if ($backupFull -ieq $GameRoot -or
            $backupFull.StartsWith(
                $gameRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The persistent ReShade.ini backup must be outside GameRoot.'
        }
        if (Test-Path -LiteralPath $persistentReShadeBackup) {
            if (-not (Test-Path -LiteralPath $persistentReShadeBackup `
                    -PathType Leaf)) {
                throw ("Persistent ReShade.ini backup path is not a file: " +
                       $persistentReShadeBackup)
            }
            if ((Get-Sha256 $persistentReShadeBackup) -cne
                $preservedReShadeHash) {
                throw ("Persistent ReShade.ini backup path already contains " +
                       "different bytes: $persistentReShadeBackup")
            }
        } else {
            $needsPersistentReShadeBackup = $true
        }
    }
}

$preservedUnownedRootPaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $deploymentEntries) {
    if ($entry.RelativePath -ieq 'ReShade.ini' -or
        $entry.RelativePath -ieq 'ShenLong.ini' -or
        $entry.RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $destination = Get-SafeChildPath `
        $GameRoot $entry.RelativePath 'Game destination preflight'
    if (-not (Test-Path -LiteralPath $destination)) {
        continue
    }
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        throw "Graphics destination is not a file: $destination"
    }
    $destinationHash = Get-Sha256 $destination
    $verifiedPriorOwner = $priorByPath.ContainsKey($entry.RelativePath) -and
        $priorByPath[$entry.RelativePath].VerifiedOwned
    if (-not $verifiedPriorOwner -and $destinationHash -ceq $entry.Sha256) {
        [void] $preservedUnownedRootPaths.Add($entry.RelativePath)
        continue
    }
    if (-not $verifiedPriorOwner) {
        throw ("Refusing to overwrite an unowned graphics root file: " +
               $entry.RelativePath)
    }
}

$serial = '{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N')
$backupRelative = ".ShenLong-install-backup-$serial"
$backupRoot = Get-SafeChildPath $GameRoot $backupRelative 'Install backup'
$createdFiles = [Collections.Generic.List[string]]::new()
$createdDirectories = [Collections.Generic.List[string]]::new()
$backupRecords = [Collections.Generic.List[object]]::new()
$preservedShenLongMoveRecords = [Collections.Generic.List[object]]::new()
$shenLongBackup = Join-Path $backupRoot 'ShenLong'
$shenLongBackedUp = $false
$installSucceeded = $false

try {
    New-TrackedDirectory $backupRoot $createdDirectories

    if (Test-Path -LiteralPath $shenLongRoot) {
        if (-not (Test-Path -LiteralPath $shenLongRoot -PathType Container)) {
            throw "Installed ShenLong path is not a directory: $shenLongRoot"
        }
        Assert-NoReparsePath $GameRoot $shenLongRoot 'Installed ShenLong root'
        Move-Item -LiteralPath $shenLongRoot -Destination $shenLongBackup
        $shenLongBackedUp = $true
    }

    foreach ($legacyEntry in $legacyRemovalEntries) {
        $backup = Get-SafeChildPath `
            $backupRoot $legacyEntry.RelativePath 'Legacy graphics backup'
        New-TrackedDirectory ([IO.Path]::GetDirectoryName($backup)) `
            $createdDirectories
        Move-Item -LiteralPath $legacyEntry.Path -Destination $backup
        $backupRecords.Add([pscustomobject]@{
                Original = $legacyEntry.Path
                Backup = $backup
            })
    }
    if (Test-Path -LiteralPath $legacyInstalledManifest -PathType Leaf) {
        $legacyManifestBackup = Join-Path `
            $backupRoot 'SPatchGraphics-SHA256SUMS.txt'
        Move-Item -LiteralPath $legacyInstalledManifest `
            -Destination $legacyManifestBackup
        $backupRecords.Add([pscustomobject]@{
                Original = $legacyInstalledManifest
                Backup = $legacyManifestBackup
            })
    }

    if ($needsPersistentShenLongConfigBackup) {
        New-TrackedDirectory `
            ([IO.Path]::GetDirectoryName($persistentShenLongConfigBackup)) `
            $createdDirectories
        Assert-NoReparsePath $shenLongConfigBackupLocation.Root `
            $persistentShenLongConfigBackup `
            'Persistent ShenLong.ini backup'
        $createdFiles.Add($persistentShenLongConfigBackup)
        [IO.File]::Copy(
            $shenLongConfigDestination,
            $persistentShenLongConfigBackup,
            $false)
        if ((Get-Sha256 $persistentShenLongConfigBackup) -cne
            $preservedShenLongConfigHash) {
            throw 'The persistent ShenLong.ini backup changed during copy.'
        }
    }

    if ($needsPersistentReShadeBackup) {
        New-TrackedDirectory `
            ([IO.Path]::GetDirectoryName($persistentReShadeBackup)) `
            $createdDirectories
        Assert-NoReparsePath $reShadeBackupLocation.Root `
            $persistentReShadeBackup 'Persistent ReShade.ini backup'
        $createdFiles.Add($persistentReShadeBackup)
        [IO.File]::Copy(
            $reShadeDestination, $persistentReShadeBackup, $false)
        if ((Get-Sha256 $persistentReShadeBackup) -cne $preservedReShadeHash) {
            throw 'The persistent ReShade.ini backup changed during copy.'
        }
    }

    foreach ($priorEntry in $priorInstalledEntries) {
        if (-not $priorEntry.Exists -or -not $priorEntry.VerifiedOwned -or
            $deploymentPaths.Contains($priorEntry.RelativePath) -or
            (-not (Test-ShenLongDeploymentPath $priorEntry.RelativePath) -and
                -not (Test-RetiredInstalledShenLongPath `
                    $priorEntry.RelativePath)) -or
            $priorEntry.RelativePath.StartsWith(
                'ShenLong\', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $backup = Get-SafeChildPath `
            $backupRoot $priorEntry.RelativePath 'Obsolete graphics backup'
        New-TrackedDirectory ([IO.Path]::GetDirectoryName($backup)) `
            $createdDirectories
        Move-Item -LiteralPath $priorEntry.Path -Destination $backup
        $backupRecords.Add([pscustomobject]@{
                Original = $priorEntry.Path
                Backup = $backup
            })
    }

    $installedEntries = [Collections.Generic.List[object]]::new()
    foreach ($entry in $deploymentEntries) {
        if ($entry.RelativePath -ieq 'ShenLong.ini') {
            continue
        }
        if ($entry.RelativePath -ieq 'ReShade.ini' -and $preserveReShade) {
            if ($retainReShadeOwnership) {
                $installedEntries.Add($entry)
            }
            continue
        }

        if ($preservedUnownedRootPaths.Contains($entry.RelativePath)) {
            $preservedDestination = Get-SafeChildPath `
                $GameRoot $entry.RelativePath 'Preserved graphics destination'
            if ((Get-Sha256 $preservedDestination) -cne $entry.Sha256) {
                throw "A preserved unowned graphics file changed during installation: $($entry.RelativePath)"
            }
            continue
        }

        if ($preservedUnownedShenLongPaths.Contains($entry.RelativePath)) {
            if (-not $shenLongBackedUp) {
                throw "The preserved ShenLong source tree is unavailable: $($entry.RelativePath)"
            }
            $shenLongRelative = $entry.RelativePath.Substring('ShenLong\'.Length)
            $preservedSource = Get-SafeChildPath `
                $shenLongBackup $shenLongRelative 'Preserved ShenLong source'
            $preservedDestination = Get-SafeChildPath `
                $GameRoot $entry.RelativePath 'Preserved ShenLong destination'
            if ((Get-Sha256 $preservedSource) -cne $entry.Sha256) {
                throw "A preserved unowned ShenLong file changed during installation: $($entry.RelativePath)"
            }
            New-TrackedDirectory ([IO.Path]::GetDirectoryName($preservedDestination)) `
                $createdDirectories
            $preservedShenLongMoveRecords.Add([pscustomobject]@{
                    Source = $preservedSource
                    Destination = $preservedDestination
                })
            Move-Item -LiteralPath $preservedSource `
                -Destination $preservedDestination
            continue
        }

        $destination = Get-SafeChildPath $GameRoot $entry.RelativePath `
            'Game destination'
        $underShenLong = $entry.RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
        if (-not $underShenLong -and (Test-Path -LiteralPath $destination)) {
            if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
                throw "Graphics destination is not a file: $destination"
            }
            $backup = Get-SafeChildPath $backupRoot $entry.RelativePath `
                'Install backup destination'
            New-TrackedDirectory ([IO.Path]::GetDirectoryName($backup)) `
                $createdDirectories
            Move-Item -LiteralPath $destination -Destination $backup
            $backupRecords.Add([pscustomobject]@{
                    Original = $destination
                    Backup = $backup
                })
        }

        New-TrackedDirectory ([IO.Path]::GetDirectoryName($destination)) `
            $createdDirectories
        $createdFiles.Add($destination)
        Copy-Item -LiteralPath $entry.Source -Destination $destination
        if ((Get-Sha256 $destination) -cne $entry.Sha256) {
            throw "Installed graphics hash mismatch: $($entry.RelativePath)"
        }
        $installedEntries.Add($entry)
    }

    if ($preserveShenLongConfig) {
        if ($canonicalExistingShenLongConfig.Changed) {
            $configTransactionBackup = Join-Path $backupRoot 'ShenLong.ini'
            Move-Item -LiteralPath $shenLongConfigDestination `
                -Destination $configTransactionBackup
            $backupRecords.Add([pscustomobject]@{
                    Original = $shenLongConfigDestination
                    Backup = $configTransactionBackup
                })
            $createdFiles.Add($shenLongConfigDestination)
            [IO.File]::WriteAllBytes(
                $shenLongConfigDestination,
                [byte[]]$canonicalExistingShenLongConfig.Bytes)
            if ((Get-Sha256 $shenLongConfigDestination) -cne
                $canonicalExistingShenLongConfig.Sha256) {
                throw 'Canonical ShenLong.ini changed while it was published.'
            }
        } elseif ((Get-Sha256 $shenLongConfigDestination) -cne
            $preservedShenLongConfigHash) {
            throw 'The installer changed existing ShenLong.ini despite preserve mode.'
        }
    } else {
        $createdFiles.Add($shenLongConfigDestination)
        [IO.File]::WriteAllBytes(
            $shenLongConfigDestination, [byte[]]$configMigration.Bytes)
    }
    Assert-ShenLongConfigVersion $shenLongConfigDestination `
        'Installed ShenLong.ini'
    Assert-NoSPatchTextureFiltering `
        ([IO.File]::ReadAllText($shenLongConfigDestination, $strictUtf8)) `
        'Installed ShenLong.ini'

    if (Test-Path -LiteralPath $installedManifest) {
        if (-not (Test-Path -LiteralPath $installedManifest -PathType Leaf)) {
            throw "Installed graphics manifest path is not a file: $installedManifest"
        }
        $manifestBackup = Join-Path $backupRoot 'ShenLong-SHA256SUMS.txt'
        Move-Item -LiteralPath $installedManifest -Destination $manifestBackup
        $backupRecords.Add([pscustomobject]@{
                Original = $installedManifest
                Backup = $manifestBackup
            })
    }
    $installedManifestLines = @($installedEntries |
        Sort-Object ManifestPath | ForEach-Object {
            "$($_.Sha256) *$($_.ManifestPath)"
        })
    $createdFiles.Add($installedManifest)
    [IO.File]::WriteAllLines(
        $installedManifest, $installedManifestLines, $writeUtf8)

    foreach ($entry in $installedEntries) {
        $destination = Get-SafeChildPath $GameRoot $entry.RelativePath `
            'Installed graphics verification'
        if ((Get-Sha256 $destination) -cne $entry.Sha256) {
            throw "Installed graphics changed after copy: $($entry.RelativePath)"
        }
    }
    if ($preserveReShade -and
        (Get-Sha256 $reShadeDestination) -cne $preservedReShadeHash) {
        throw 'The installer changed ReShade.ini despite preserve mode.'
    }

    if ($shenLongBackedUp) {
        foreach ($runtimeOutputName in @('ReShadeCache', 'ShaderDump')) {
            $runtimeOutputBackup = Join-Path `
                $shenLongBackup $runtimeOutputName
            $runtimeOutputDestination = Join-Path `
                $shenLongRoot $runtimeOutputName
            if (Test-Path -LiteralPath $runtimeOutputBackup -PathType Container) {
                if (Test-Path -LiteralPath $runtimeOutputDestination) {
                    throw "The package unexpectedly owns ShenLong\$runtimeOutputName."
                }
                Copy-Item -LiteralPath $runtimeOutputBackup `
                    -Destination $runtimeOutputDestination -Recurse
            }
        }
    }

    foreach ($legacyDirectory in @($legacyRemovalEntries |
            ForEach-Object { [IO.Path]::GetDirectoryName($_.Path) } |
            Where-Object {
                -not [string]::IsNullOrWhiteSpace($_) -and $_ -ine $GameRoot
            } | Sort-Object Length -Descending -Unique)) {
        $cursor = $legacyDirectory
        while ($cursor -ine $GameRoot -and
            (Test-Path -LiteralPath $cursor -PathType Container)) {
            Assert-NoReparsePath $GameRoot $cursor 'Legacy graphics cleanup'
            if (@(Get-ChildItem -LiteralPath $cursor -Force).Count -ne 0) {
                break
            }
            Remove-Item -LiteralPath $cursor -Force
            $cursor = [IO.Path]::GetDirectoryName($cursor)
        }
    }

    $installSucceeded = $true
} catch {
    $installError = $_
    $rollbackErrors = [Collections.Generic.List[string]]::new()

    for ($index = $preservedShenLongMoveRecords.Count - 1;
        $index -ge 0; --$index) {
        $record = $preservedShenLongMoveRecords[$index]
        try {
            if (Test-Path -LiteralPath $record.Destination -PathType Leaf) {
                if (Test-Path -LiteralPath $record.Source) {
                    throw "Both preserved ShenLong rollback paths exist: $($record.Source)"
                }
                New-TrackedDirectory `
                    ([IO.Path]::GetDirectoryName($record.Source)) `
                    $createdDirectories
                Move-Item -LiteralPath $record.Destination `
                    -Destination $record.Source
            }
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    try {
        if (Test-Path -LiteralPath $shenLongRoot) {
            Remove-Item -LiteralPath $shenLongRoot -Recurse -Force
        }
    } catch {
        $rollbackErrors.Add($_.Exception.Message)
    }
    foreach ($createdFile in $createdFiles) {
        try {
            if (Test-Path -LiteralPath $createdFile -PathType Leaf) {
                Remove-Item -LiteralPath $createdFile -Force
            }
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    for ($index = $backupRecords.Count - 1; $index -ge 0; --$index) {
        $record = $backupRecords[$index]
        try {
            New-TrackedDirectory ([IO.Path]::GetDirectoryName($record.Original)) `
                $createdDirectories
            if (Test-Path -LiteralPath $record.Original) {
                Remove-Item -LiteralPath $record.Original -Force
            }
            Move-Item -LiteralPath $record.Backup -Destination $record.Original
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    if ($shenLongBackedUp) {
        try {
            if (Test-Path -LiteralPath $shenLongBackup -PathType Container) {
                Move-Item -LiteralPath $shenLongBackup -Destination $shenLongRoot
            } else {
                throw "ShenLong rollback backup is missing: $shenLongBackup"
            }
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    if ($rollbackErrors.Count -eq 0 -and
        (Test-Path -LiteralPath $backupRoot)) {
        try {
            Remove-Item -LiteralPath $backupRoot -Recurse -Force
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    for ($index = $createdDirectories.Count - 1; $index -ge 0; --$index) {
        $directory = $createdDirectories[$index]
        try {
            if (Test-Path -LiteralPath $directory -PathType Container) {
                $items = @(Get-ChildItem -LiteralPath $directory -Force)
                if ($items.Count -eq 0) {
                    Remove-Item -LiteralPath $directory -Force
                }
            }
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    if ($rollbackErrors.Count -ne 0) {
        throw ("$($installError.Exception.Message) Rollback also failed; " +
               "backup retained at ${backupRoot}: " +
               ($rollbackErrors -join ' | '))
    }
    throw $installError
} finally {
    if ($installSucceeded -and (Test-Path -LiteralPath $backupRoot)) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
    }
    if ($installSucceeded) {
        for ($index = $createdDirectories.Count - 1; $index -ge 0; --$index) {
            $directory = $createdDirectories[$index]
            if (Test-Path -LiteralPath $directory -PathType Container) {
                $items = @(Get-ChildItem -LiteralPath $directory -Force)
                if ($items.Count -eq 0) {
                    Remove-Item -LiteralPath $directory -Force
                }
            }
        }
    }
}

[pscustomobject]@{
    Configuration = $Configuration
    InstalledFiles = $installedEntries.Count
    ReShadeIniPreserved = $preserveReShade
    ShenLongIniPreserved = $preserveShenLongConfig -and
        -not $canonicalExistingShenLongConfig.Changed
    ShenLongIniCanonicalized = $preserveShenLongConfig -and
        $canonicalExistingShenLongConfig.Changed
    RemovedTextureFilteringSections = if ($preserveShenLongConfig) {
        $canonicalExistingShenLongConfig.RemovedSections
    } else {
        0
    }
    RemovedTextureFilteringKeys = if ($preserveShenLongConfig) {
        $canonicalExistingShenLongConfig.RemovedKeys
    } else {
        0
    }
    MigratedGraphicsValues = if ($null -eq $configMigration) {
        0
    } else {
        $configMigration.MigratedValues
    }
    MigrationSources = if ($null -eq $configMigration) {
        @()
    } else {
        @($configMigration.Sources)
    }
    PreservedUnownedRootFiles = @($preservedUnownedRootPaths | Sort-Object)
    PreservedUnownedShenLongFiles = @($preservedUnownedShenLongPaths | Sort-Object)
    PersistentReShadeBackup = $persistentReShadeBackup
    PersistentShenLongConfigBackup = $persistentShenLongConfigBackup
    InstalledManifestPath = $installedManifest
    InstalledManifestSha256 = Get-Sha256 $installedManifest
    PackageManifestSha256 = Get-Sha256 $manifestPath
    AsiLoaderSha256 = $asiLoader.Sha256
    ShenLongSha256 = Get-Sha256 (Join-Path $GameRoot 'ShenLong.asi')
    MutationPerformed = $true
}
} finally {
    if ($ownsLiveHarnessMutex) {
        $liveHarnessMutex.ReleaseMutex()
    }
    $liveHarnessMutex.Dispose()
}
