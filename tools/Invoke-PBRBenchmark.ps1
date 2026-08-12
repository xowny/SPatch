[CmdletBinding()]
param(
    [string] $RepoRoot = '',
    [string] $GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\SleepingDogsDefinitiveEdition',
    [ValidateRange(1, 10)]
    [int] $Passes = 3,
    [ValidateRange(640, 16384)]
    [int] $ExpectedWidth = 3840,
    [ValidateRange(480, 16384)]
    [int] $ExpectedHeight = 2160,
    [ValidateSet('Auto', 'Development-Release', 'Publishing-Release')]
    [string] $ExpectedGraphicsConfiguration = 'Auto',
    [ValidatePattern('^[^\\/:*?"<>|]+\.dll$')]
    [string] $AsiLoaderName = 'dinput8.dll',
    [switch] $FirstPassPbrOff
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $scriptRoot
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot).TrimEnd([char[]]'\/')
$GameRoot = [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/')
$reShadePolicy = Join-Path $RepoRoot 'tools\ReShadeIniPolicy.ps1'
if (-not (Test-Path -LiteralPath $reShadePolicy -PathType Leaf)) {
    throw "ReShade INI policy is missing: $reShadePolicy"
}
. $reShadePolicy
$safetyHelper = Join-Path $RepoRoot 'tools\LiveHarnessSafety.ps1'
if (-not (Test-Path -LiteralPath $safetyHelper -PathType Leaf)) {
    throw "Live harness safety helper is missing: $safetyHelper"
}
. $safetyHelper
if ($script:SPatchLiveHarnessSafetyVersion -cne '2026.08.10.6' -or
    -not (Test-SPatchPathEqual `
        $script:SPatchLiveHarnessSafetyPath $safetyHelper)) {
    throw 'The exact live harness safety helper was not loaded.'
}
$ExpectedGraphicsConfiguration = switch ($ExpectedGraphicsConfiguration) {
    'Auto' { 'Auto' }
    'Development-Release' { 'Development-Release' }
    'Publishing-Release' { 'Publishing-Release' }
}
function Resolve-DisplaySettingsPath([string] $Root) {
    $gogPath = Join-Path $Root 'Save\DisplaySettings.xml'
    $steamPath = Join-Path $Root 'data\DisplaySettings.xml'
    if (Test-Path -LiteralPath $gogPath -PathType Leaf) {
        return [IO.Path]::GetFullPath($gogPath)
    }
    $gogMarkers = @(
        Get-ChildItem -LiteralPath $Root -Filter 'goggame-*.info' -File `
            -ErrorAction SilentlyContinue)
    if ($gogMarkers.Count -ne 0) {
        return [IO.Path]::GetFullPath($gogPath)
    }
    if (Test-Path -LiteralPath $steamPath -PathType Leaf) {
        return [IO.Path]::GetFullPath($steamPath)
    }
    return [IO.Path]::GetFullPath($steamPath)
}
$gameExe = Join-Path $GameRoot 'sdhdship.exe'
$benchmarkShortcut = $script:SPatchBenchmarkShortcutPath
$baseIni = Join-Path $GameRoot 'SPatch.ini'
$ini = Join-Path $GameRoot 'ShenLong.ini'
$previousIni = Join-Path $GameRoot 'SPatch.ini.previous.bak'
$reshadeIni = Join-Path $GameRoot 'ReShade.ini'
$installedAsi = Join-Path $GameRoot 'SPatch.asi'
$installedAddon = Join-Path $GameRoot 'ShenLong.asi'
$installedDxgi = Join-Path $GameRoot 'dxgi.dll'
$asiLoader = Join-Path $GameRoot $AsiLoaderName
$graphicsArtifactsRoot = Join-Path $RepoRoot 'artifacts\shenlong'
$displaySettings = Resolve-DisplaySettingsPath $GameRoot
$reshadeLog = Join-Path $GameRoot 'ReShade.log'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$writeUtf8 = [Text.UTF8Encoding]::new($false)
$script:ownedGameProcesses = [Collections.Generic.Dictionary[int, datetime]]::new()

$liveHarnessMutex = [Threading.Mutex]::new(
    $false, $script:SPatchLiveHarnessMutexName)
$ownsLiveHarnessMutex = $false
try {
    try {
        $ownsLiveHarnessMutex = $liveHarnessMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $ownsLiveHarnessMutex = $true
    }
    if (-not $ownsLiveHarnessMutex) {
        throw 'Another SPatch/ShenLong live mutation harness already owns the game installation.'
    }

$liveGogMarkers = @(
    Get-ChildItem -LiteralPath $GameRoot -Filter 'goggame-*.info' -File `
        -ErrorAction SilentlyContinue)
$gogDisplaySettings = [IO.Path]::GetFullPath(
    (Join-Path $GameRoot 'Save\DisplaySettings.xml'))
if ($displaySettings.Equals(
        $gogDisplaySettings, [StringComparison]::OrdinalIgnoreCase) -or
    $liveGogMarkers.Count -ne 0) {
    throw 'The unattended PBR benchmark is Steamworks-only; no live GOG launch path has been validated.'
}
if (Get-Process -Name sdhdship -ErrorAction SilentlyContinue) {
    throw 'A game process was active before the matched PBR benchmark.'
}
$userStateTargets = @(Get-SPatchUserStateTargets $GameRoot $displaySettings)
[void] (Restore-SPatchRecoveryBackup $GameRoot $userStateTargets)
if (Test-Path -LiteralPath $previousIni) {
    throw 'Legacy SPatch.ini.previous.bak remains in the game directory; migrate it externally before benchmarking.'
}
foreach ($path in @(
        $gameExe,
        $benchmarkShortcut,
        $baseIni,
        $ini,
        $reshadeIni,
        $displaySettings,
        $installedAsi,
        $installedAddon,
        $installedDxgi,
        $asiLoader)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "PBR benchmark prerequisite is missing: $path"
    }
}

function Get-Hash([string] $Path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Get-TextHash([string] $Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}

function Get-ByteHash(
    [byte[]] $Bytes,
    [int] $Offset = 0,
    [int] $Count = -1) {
    if ($Count -lt 0) {
        $Count = $Bytes.Length - $Offset
    }
    if ($Offset -lt 0 -or $Count -lt 0 -or
        $Offset + $Count -gt $Bytes.Length) {
        throw 'Invalid byte range requested for hashing.'
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $sha.ComputeHash($Bytes, $Offset, $Count))).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}

function Get-TreeRelativeFiles([string] $Root) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    return @(
        Get-ChildItem -LiteralPath $rootPath -File -Recurse | ForEach-Object {
            $fullPath = [IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Enumerated graphics file escaped its root: $fullPath"
            }
            $fullPath.Substring($prefix.Length).Replace('/', '\')
        })
}

function Test-GraphicsDeploymentPath([string] $RelativePath) {
    return $RelativePath -ieq 'ShenLong.asi' -or
        $RelativePath -ieq 'dxgi.dll' -or
        $RelativePath -ieq 'ReShade.ini' -or
        $RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
}

function Get-ImmutableGraphicsPackageIdentity(
    [string] $Root,
    [string] $Label,
    [string] $ManifestName,
    [bool] $RequireExactTree) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $manifestPath = Join-Path $rootPath $ManifestName
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "$Label graphics checksum manifest is missing: $manifestPath"
    }
    $manifestText = [IO.File]::ReadAllText($manifestPath, $strictUtf8)
    $lines = @($manifestText -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $entries = [Collections.Generic.List[object]]::new()
    $identityLines = [Collections.Generic.List[string]]::new()
    $names = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $lines) {
        if ($line -cnotmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$') {
            throw "$Label graphics checksum line is malformed: $line"
        }
        $declaredHash = $Matches['hash']
        $relativePath = $Matches['path'].Replace('/', '\')
        if ($relativePath -ieq $ManifestName -or
            [IO.Path]::IsPathRooted($relativePath) -or
            @($relativePath -split '\\' | Where-Object {
                    $_ -ceq '..' -or $_ -ceq '.' -or
                    [string]::IsNullOrWhiteSpace($_)
                }).Count -ne 0) {
            throw "$Label graphics checksum path is unsafe: $relativePath"
        }
        if (-not $names.Add($relativePath)) {
            throw "$Label graphics checksum path is duplicated: $relativePath"
        }
        $path = [IO.Path]::GetFullPath((Join-Path $rootPath $relativePath))
        if (-not $path.StartsWith(
                $rootPath + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "$Label graphics checksum path escapes the package root: $relativePath"
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "$Label immutable graphics file is missing: $relativePath"
        }
        $file = Get-Item -LiteralPath $path -ErrorAction Stop
        $actualHash = Get-Hash $path
        if ($actualHash -cne $declaredHash) {
            throw "$Label immutable graphics file $relativePath has SHA-256 $actualHash; manifest declares $declaredHash"
        }
        if ($relativePath -ieq 'ReShade.ini' -or
            -not (Test-GraphicsDeploymentPath $relativePath)) {
            continue
        }
        $entry = [pscustomobject]@{
            Name = $relativePath
            Length = [int64] $file.Length
            Sha256 = $actualHash
        }
        $entries.Add($entry)
        $identityLines.Add(('{0}|{1}|{2}' -f
                $entry.Name, $entry.Length, $entry.Sha256))
    }
    if ($entries.Count -eq 0) {
        throw "$Label graphics checksum manifest has no immutable entries."
    }
    if ($RequireExactTree) {
        $expectedTree = [Collections.Generic.HashSet[string]]::new(
            $names, [StringComparer]::OrdinalIgnoreCase)
        [void] $expectedTree.Add($ManifestName)
        $actualTree = @(Get-TreeRelativeFiles $rootPath)
        if ($actualTree.Count -ne $expectedTree.Count) {
            throw ("$Label graphics tree is not exact: expected " +
                   "$($expectedTree.Count) files, found $($actualTree.Count).")
        }
        foreach ($relativePath in $actualTree) {
            if (-not $expectedTree.Contains($relativePath)) {
                throw "$Label graphics tree contains an unmanifested file: $relativePath"
            }
        }
    }
    $canonicalIdentityLines = $identityLines.ToArray()
    [Array]::Sort($canonicalIdentityLines, [StringComparer]::Ordinal)
    $identityText = ($canonicalIdentityLines -join [char] 10) + [char] 10
    return [pscustomobject]@{
        ManifestPath = $manifestPath
        ManifestFileName = $ManifestName
        FileCount = $entries.Count
        ManifestSha256 = Get-TextHash $identityText
        Files = @($entries.ToArray())
    }
}

function Get-InstalledRuntimeGraphicsIdentity(
    [string] $Root,
    [string] $Label,
    [string] $ManifestName) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $manifestPath = Join-Path $rootPath $ManifestName
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "$Label graphics ownership manifest is missing: $manifestPath"
    }

    $ownedPaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $manifestText = [IO.File]::ReadAllText($manifestPath, $strictUtf8)
    foreach ($line in @($manifestText -split '\r?\n' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
        if ($line -cnotmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$') {
            throw "$Label graphics ownership line is malformed: $line"
        }
        $declaredHash = $Matches['hash']
        $relativePath = $Matches['path'].Replace('/', '\')
        if (-not $ownedPaths.Add($relativePath)) {
            throw "$Label graphics ownership path is duplicated: $relativePath"
        }
        if (-not (Test-GraphicsDeploymentPath $relativePath) -or
            $relativePath -ieq 'ShenLong\ReShadeCache' -or
            $relativePath.StartsWith(
                'ShenLong\ReShadeCache\',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "$Label graphics ownership path is not deployable: $relativePath"
        }
        if ([IO.Path]::IsPathRooted($relativePath) -or
            @($relativePath -split '\\' | Where-Object {
                    $_ -ceq '..' -or $_ -ceq '.' -or
                    [string]::IsNullOrWhiteSpace($_)
                }).Count -ne 0) {
            throw "$Label graphics ownership path is unsafe: $relativePath"
        }
        $path = [IO.Path]::GetFullPath((Join-Path $rootPath $relativePath))
        if (-not $path.StartsWith(
                $rootPath + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "$Label owned graphics file is missing or outside the game root: $relativePath"
        }
        $actualHash = Get-Hash $path
        if ($actualHash -cne $declaredHash) {
            throw "$Label owned graphics file changed after installation: $relativePath"
        }
    }

    $runtimePaths = [Collections.Generic.List[string]]::new()
    foreach ($relativePath in @('ShenLong.asi', 'dxgi.dll')) {
        $runtimePaths.Add($relativePath)
    }
    $shenLongRoot = Join-Path $rootPath 'ShenLong'
    if (-not (Test-Path -LiteralPath $shenLongRoot -PathType Container)) {
        throw "$Label ShenLong graphics directory is missing: $shenLongRoot"
    }
    foreach ($relativePath in @(Get-TreeRelativeFiles $shenLongRoot | Where-Object {
                $_ -ine 'ReShadeCache' -and
                -not $_.StartsWith(
                    'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase)
            })) {
        $runtimePaths.Add('ShenLong\' + $relativePath)
    }

    $entries = [Collections.Generic.List[object]]::new()
    $identityLines = [Collections.Generic.List[string]]::new()
    $seenRuntimePaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($relativePath in $runtimePaths) {
        if (-not $seenRuntimePaths.Add($relativePath)) {
            throw "$Label runtime graphics path is duplicated: $relativePath"
        }
        $path = [IO.Path]::GetFullPath((Join-Path $rootPath $relativePath))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "$Label runtime graphics file is missing: $relativePath"
        }
        $file = Get-Item -LiteralPath $path -ErrorAction Stop
        $entry = [pscustomobject]@{
            Name = $relativePath
            Length = [int64] $file.Length
            Sha256 = Get-Hash $path
        }
        $entries.Add($entry)
        $identityLines.Add(('{0}|{1}|{2}' -f
                $entry.Name, $entry.Length, $entry.Sha256))
    }
    if ($entries.Count -eq 0) {
        throw "$Label runtime graphics payload is empty."
    }
    $canonicalIdentityLines = $identityLines.ToArray()
    [Array]::Sort($canonicalIdentityLines, [StringComparer]::Ordinal)
    $identityText = ($canonicalIdentityLines -join [char] 10) + [char] 10
    return [pscustomobject]@{
        ManifestPath = $manifestPath
        ManifestFileName = $ManifestName
        FileCount = $entries.Count
        ManifestSha256 = Get-TextHash $identityText
        Files = @($entries.ToArray())
    }
}

function Get-PbrCacheIdentity([string] $Directory, [string] $Label) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "$Label PBR cache directory is missing: $Directory"
    }
    $files = @(Get-ChildItem -LiteralPath $Directory -Filter '*.cso' -File -ErrorAction Stop | Sort-Object Name)
    if ($files.Count -ne 18) {
        throw "$Label PBR cache must contain exactly 18 replaceable CSOs; found $($files.Count): $Directory"
    }
    $names = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $entries = [Collections.Generic.List[object]]::new()
    $manifestLines = [Collections.Generic.List[string]]::new()
    foreach ($file in $files) {
        if ($file.Name -cnotmatch '^PBR-0x[0-9A-F]{8}\.ps_4_0\.cso$') {
            throw "$Label PBR cache contains an unexpected CSO name: $($file.Name)"
        }
        if (-not $names.Add($file.Name)) {
            throw "$Label PBR cache contains a duplicate CSO name: $($file.Name)"
        }
        $hash = Get-Hash $file.FullName
        $entry = [pscustomobject]@{
            Name = $file.Name
            Length = [int64] $file.Length
            Sha256 = $hash
        }
        $entries.Add($entry)
        $manifestLines.Add(('{0}|{1}|{2}' -f
                $entry.Name, $entry.Length, $entry.Sha256))
    }
    $manifestText = ($manifestLines -join [char] 10) + [char] 10
    return [pscustomobject]@{
        Directory = [IO.Path]::GetFullPath($Directory)
        FileCount = $entries.Count
        ManifestSha256 = Get-TextHash $manifestText
        Files = @($entries.ToArray())
    }
}

function Get-GraphicsPayloadIdentity(
    [string] $Root,
    [string] $Label,
    [bool] $Installed) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $addonPath = Join-Path $rootPath 'ShenLong.asi'
    if (-not (Test-Path -LiteralPath $addonPath -PathType Leaf)) {
        throw "$Label graphics add-on is missing: $addonPath"
    }
    $addon = Get-Item -LiteralPath $addonPath -ErrorAction Stop
    $addonHash = Get-Hash $addon.FullName
    $manifestName = if ($Installed) {
        'ShenLong-SHA256SUMS.txt'
    } else {
        'SHA256SUMS.txt'
    }
    $immutablePackage = if ($Installed) {
        Get-InstalledRuntimeGraphicsIdentity `
            $rootPath $Label $manifestName
    } else {
        Get-ImmutableGraphicsPackageIdentity `
            $rootPath $Label $manifestName $true
    }
    $pbrCache = Get-PbrCacheIdentity (Join-Path $rootPath 'ShenLong\ShaderCache\v1\PBR') $Label
    $fingerprintText = @(
        'IMMUTABLE-PACKAGE-MANIFEST|{0}|{1}' -f
            $immutablePackage.FileCount, $immutablePackage.ManifestSha256
        'ShenLong.asi|{0}|{1}' -f $addon.Length, $addonHash
        'PBR-CSO-MANIFEST|{0}|{1}' -f
            $pbrCache.FileCount, $pbrCache.ManifestSha256
    ) -join [char] 10
    return [pscustomobject]@{
        Label = $Label
        Root = $rootPath
        Addon = [pscustomobject]@{
            Path = $addon.FullName
            Length = [int64] $addon.Length
            Sha256 = $addonHash
        }
        ImmutablePackage = $immutablePackage
        PbrCache = $pbrCache
        PayloadSha256 = Get-TextHash ($fingerprintText + [char] 10)
    }
}

function Get-GraphicsPayloadMismatch([object] $Expected, [object] $Actual) {
    $issues = [Collections.Generic.List[string]]::new()
    if ($Actual.ImmutablePackage.FileCount -ne
        $Expected.ImmutablePackage.FileCount) {
        $issues.Add(('immutable graphics file count expected {0}, found {1}' -f
                $Expected.ImmutablePackage.FileCount,
                $Actual.ImmutablePackage.FileCount))
    }
    if ($Actual.ImmutablePackage.ManifestSha256 -cne
        $Expected.ImmutablePackage.ManifestSha256) {
        $issues.Add(('immutable graphics manifest SHA-256 expected {0}, found {1}' -f
                $Expected.ImmutablePackage.ManifestSha256,
                $Actual.ImmutablePackage.ManifestSha256))
    }
    if ($Actual.Addon.Length -ne $Expected.Addon.Length) {
        $issues.Add(('ShenLong.asi length expected {0}, found {1}' -f
                $Expected.Addon.Length, $Actual.Addon.Length))
    }
    if ($Actual.Addon.Sha256 -cne $Expected.Addon.Sha256) {
        $issues.Add(('ShenLong.asi SHA-256 expected {0}, found {1}' -f
                $Expected.Addon.Sha256, $Actual.Addon.Sha256))
    }
    if ($Actual.PbrCache.FileCount -ne $Expected.PbrCache.FileCount) {
        $issues.Add(('PBR CSO count expected {0}, found {1}' -f
                $Expected.PbrCache.FileCount, $Actual.PbrCache.FileCount))
    }
    $actualByName = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::Ordinal)
    foreach ($entry in @($Actual.PbrCache.Files)) {
        $actualByName.Add([string] $entry.Name, $entry)
    }
    $expectedNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($expectedEntry in @($Expected.PbrCache.Files)) {
        [void] $expectedNames.Add([string] $expectedEntry.Name)
        if (-not $actualByName.ContainsKey([string] $expectedEntry.Name)) {
            $issues.Add("PBR CSO is missing: $($expectedEntry.Name)")
            continue
        }
        $actualEntry = $actualByName[[string] $expectedEntry.Name]
        if ($actualEntry.Length -ne $expectedEntry.Length) {
            $issues.Add(('{0} length expected {1}, found {2}' -f
                    $expectedEntry.Name, $expectedEntry.Length,
                    $actualEntry.Length))
        }
        if ($actualEntry.Sha256 -cne $expectedEntry.Sha256) {
            $issues.Add(('{0} SHA-256 expected {1}, found {2}' -f
                    $expectedEntry.Name, $expectedEntry.Sha256,
                    $actualEntry.Sha256))
        }
    }
    foreach ($actualEntry in @($Actual.PbrCache.Files)) {
        if (-not $expectedNames.Contains([string] $actualEntry.Name)) {
            $issues.Add("Unexpected PBR CSO: $($actualEntry.Name)")
        }
    }
    if ($Actual.PbrCache.ManifestSha256 -cne
        $Expected.PbrCache.ManifestSha256) {
        $issues.Add(('PBR CSO manifest SHA-256 expected {0}, found {1}' -f
                $Expected.PbrCache.ManifestSha256,
                $Actual.PbrCache.ManifestSha256))
    }
    if ($Actual.PayloadSha256 -cne $Expected.PayloadSha256) {
        $issues.Add(('graphics payload SHA-256 expected {0}, found {1}' -f
                $Expected.PayloadSha256, $Actual.PayloadSha256))
    }
    return $issues.ToArray()
}

function Assert-InstalledGraphicsCensus(
    [object] $PackagePayload,
    [string] $Context) {
    $legacyRootAddons = @(
        Get-ChildItem -LiteralPath $GameRoot -File | Where-Object {
            [IO.Path]::GetExtension($_.Name) -in @('.addon', '.addon64')
        } | Select-Object -ExpandProperty Name)
    if ($legacyRootAddons.Count -ne 0) {
        throw ("$Context legacy root graphics add-ons are installed: " +
               ($legacyRootAddons -join ', '))
    }
    foreach ($forbiddenDirectory in @(
            'Luma', 'SPatch\PCSS', 'SPatch\ShaderCache')) {
        if (Test-Path -LiteralPath (Join-Path $GameRoot $forbiddenDirectory)) {
            throw "$Context legacy/experimental graphics directory is installed: $forbiddenDirectory"
        }
    }

    $expectedShenLongFiles = @(
        $PackagePayload.ImmutablePackage.Files | Where-Object {
            $_.Name.StartsWith(
                'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
        } | ForEach-Object { $_.Name.Substring(9) })
    $shenLongRoot = Join-Path $GameRoot 'ShenLong'
    if (-not (Test-Path -LiteralPath $shenLongRoot -PathType Container)) {
        throw "$Context installed managed ShenLong graphics directory is missing."
    }
    $actualShenLongFiles = @(
        Get-TreeRelativeFiles $shenLongRoot | Where-Object {
            -not $_.StartsWith(
                'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase)
        })
    if ($actualShenLongFiles.Count -ne $expectedShenLongFiles.Count -or
        @(Compare-Object `
            -ReferenceObject $expectedShenLongFiles `
            -DifferenceObject $actualShenLongFiles).Count -ne 0) {
        throw "$Context installed ShenLong tree is not the exact package-managed file set."
    }

    $expectedDirectories = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($relativeFile in $expectedShenLongFiles) {
        $parent = [IO.Path]::GetDirectoryName($relativeFile)
        while (-not [string]::IsNullOrWhiteSpace($parent)) {
            [void] $expectedDirectories.Add($parent)
            $parent = [IO.Path]::GetDirectoryName($parent)
        }
    }
    $actualDirectories = @(
        Get-ChildItem -LiteralPath $shenLongRoot -Directory -Recurse |
            ForEach-Object {
                $_.FullName.Substring($shenLongRoot.Length + 1)
            } | Where-Object {
                $_ -ine 'ReShadeCache' -and
                -not $_.StartsWith(
                    'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase)
            })
    if ($actualDirectories.Count -ne $expectedDirectories.Count) {
        throw "$Context installed ShenLong tree contains unexpected directories."
    }
    foreach ($relativeDirectory in $actualDirectories) {
        if (-not $expectedDirectories.Contains($relativeDirectory)) {
            throw "$Context installed ShenLong tree contains an unexpected directory: $relativeDirectory"
        }
    }
}

function Get-GraphicsPackageIdentity([string] $Configuration) {
    $packageRoot = Join-Path $graphicsArtifactsRoot `
        "$Configuration\ShenLong-Package"
    $payload = Get-GraphicsPayloadIdentity `
        $packageRoot "$Configuration current package" $false
    return [pscustomobject]@{
        Configuration = $Configuration
        PerformanceEligible = [bool] (
            $Configuration -ceq 'Publishing-Release')
        PackageRoot = $packageRoot
        Payload = $payload
    }
}

function Resolve-GraphicsPackage([string] $RequestedConfiguration) {
    $installed = Get-GraphicsPayloadIdentity $GameRoot 'Installed' $true
    $configurations = if ($RequestedConfiguration -eq 'Auto') {
        @('Development-Release', 'Publishing-Release')
    } else {
        @($RequestedConfiguration)
    }
    $matches = [Collections.Generic.List[object]]::new()
    $diagnostics = [Collections.Generic.List[string]]::new()
    foreach ($configuration in $configurations) {
        try {
            $package = Get-GraphicsPackageIdentity $configuration
            $issues = @(Get-GraphicsPayloadMismatch $package.Payload $installed)
            if ($issues.Count -eq 0) {
                $matches.Add([pscustomobject]@{
                        Package = $package
                        Installed = $installed
                    })
            } else {
                $diagnostics.Add(($configuration + ': ' + ($issues -join '; ')))
            }
        } catch {
            $diagnostics.Add(($configuration + ': ' + $_.Exception.Message))
        }
    }
    if ($matches.Count -ne 1) {
        $detail = if ($diagnostics.Count -gt 0) {
            $diagnostics -join ' | '
        } else {
            'the installed payload matched multiple package configurations'
        }
        throw ('Installed graphics payload must match exactly one selected ' +
            "current package; matches=$($matches.Count). $detail")
    }
    Assert-InstalledGraphicsCensus `
        $matches[0].Package.Payload 'Graphics-package selection'
    return $matches[0]
}

function Assert-SelectedGraphicsIdentity(
    [object] $Selection,
    [string] $Context) {
    $currentPackage = Get-GraphicsPackageIdentity $Selection.Package.Configuration
    $packageDrift = @(Get-GraphicsPayloadMismatch $Selection.Package.Payload $currentPackage.Payload)
    if ($packageDrift.Count -ne 0) {
        throw "$Context selected current package changed after preflight: $($packageDrift -join '; ')"
    }
    $installed = Get-GraphicsPayloadIdentity $GameRoot 'Installed' $true
    $installDrift = @(Get-GraphicsPayloadMismatch $currentPackage.Payload $installed)
    if ($installDrift.Count -ne 0) {
        throw "$Context installed graphics payload no longer matches $($currentPackage.Configuration): $($installDrift -join '; ')"
    }
    Assert-InstalledGraphicsCensus $currentPackage.Payload $Context
    return $installed
}

function Read-Bytes([string] $Path) {
    [IO.File]::ReadAllBytes($Path)
}

function Read-SharedBytes([string] $Path) {
    $lastError = $null
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        try {
            $stream = [IO.File]::Open(
                $Path,
                [IO.FileMode]::Open,
                [IO.FileAccess]::Read,
                [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
            try {
                if ($stream.Length -gt 64MB) {
                    throw "Refusing to capture an unexpectedly large log: $Path"
                }
                $bytes = [byte[]]::new([int] $stream.Length)
                $offset = 0
                while ($offset -lt $bytes.Length) {
                    $read = $stream.Read($bytes, $offset, $bytes.Length - $offset)
                    if ($read -le 0) {
                        throw "Shared log was truncated while reading: $Path"
                    }
                    $offset += $read
                }
                return ,$bytes
            } finally {
                $stream.Dispose()
            }
        } catch {
            $lastError = $_
            Start-Sleep -Milliseconds 250
        }
    }
    throw "Could not read shared file ${Path}: $($lastError.Exception.Message)"
}

function New-LogSnapshot([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{
            Exists = $false
            Bytes = [byte[]]::new(0)
            Hash = ''
        }
    }
    [byte[]] $bytes = Read-SharedBytes $Path
    return [pscustomobject]@{
        Exists = $true
        Bytes = $bytes
        Hash = Get-ByteHash $bytes
    }
}

function Get-FreshLogSessionText(
    [string] $Path,
    [object] $Before,
    [datetime] $ProcessStartUtc,
    [string] $Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not created by the current benchmark arm: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    if ($item.LastWriteTimeUtc -lt $ProcessStartUtc.AddSeconds(-2)) {
        throw "$Label was not written by the current process session."
    }
    [byte[]] $afterBytes = Read-SharedBytes $Path
    if ($afterBytes.Length -eq 0) {
        throw "$Label is empty after the current process session."
    }

    $oldIsExactPrefix = $false
    if ($Before.Exists -and $afterBytes.Length -ge $Before.Bytes.Length) {
        $oldIsExactPrefix = (Get-ByteHash `
                $afterBytes 0 $Before.Bytes.Length) -ceq $Before.Hash
    }
    if ($oldIsExactPrefix) {
        $sessionLength = $afterBytes.Length - $Before.Bytes.Length
        if ($sessionLength -le 0) {
            throw "$Label contains no bytes from the current process session."
        }
        $sessionBytes = [byte[]]::new($sessionLength)
        [Array]::Copy(
            $afterBytes,
            $Before.Bytes.Length,
            $sessionBytes,
            0,
            $sessionLength)
    } else {
        if ($Before.Exists -and
            $afterBytes.Length -eq $Before.Bytes.Length -and
            (Get-ByteHash $afterBytes) -ceq $Before.Hash) {
            throw "$Label is byte-identical to the pre-launch log."
        }
        $sessionBytes = $afterBytes
    }
    try {
        $text = $strictUtf8.GetString($sessionBytes)
    } catch {
        throw "$Label is not valid UTF-8: $($_.Exception.Message)"
    }
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "$Label contains no current-session text."
    }
    return $text
}

function Test-SameBytes([byte[]] $Expected, [string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $actual = Read-Bytes $Path
    if ($actual.Length -ne $Expected.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Expected.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            return $false
        }
    }
    return $true
}

function Get-IniSectionMatch([string] $Text, [string] $Section) {
    $escaped = [regex]::Escape($Section)
    $matches = @([regex]::Matches(
        $Text,
        "(?ms)^\[$escaped\][^\r\n]*(?<body>.*?)(?=^\[|\z)"))
    if ($matches.Count -ne 1) {
        throw "INI must contain exactly one [$Section] section; found $($matches.Count)."
    }
    return $matches[0]
}

function Get-IniValueStrict(
    [string] $Text,
    [string] $Section,
    [string] $Key) {
    $sectionMatch = Get-IniSectionMatch $Text $Section
    $body = $sectionMatch.Groups['body'].Value
    $escapedKey = [regex]::Escape($Key)
    $matches = @([regex]::Matches(
        $body,
        "(?m)^[ \t]*$escapedKey[ \t]*=[ \t]*(?<value>[^\r\n]*?)[ \t]*(?:\r?\n|$)"))
    if ($matches.Count -ne 1) {
        throw "INI [$Section] must contain exactly one $Key key; found $($matches.Count)."
    }
    return $matches[0].Groups['value'].Value
}

function Set-IniValueStrict(
    [string] $Text,
    [string] $Section,
    [string] $Key,
    [string] $Value) {
    $sectionMatch = Get-IniSectionMatch $Text $Section
    $bodyGroup = $sectionMatch.Groups['body']
    $escapedKey = [regex]::Escape($Key)
    $keyRegex = [regex]::new(
        "(?m)^(?<prefix>[ \t]*$escapedKey[ \t]*=)[^\r\n]*(?<ending>\r?\n|$)")
    $matches = @($keyRegex.Matches($bodyGroup.Value))
    if ($matches.Count -ne 1) {
        throw "INI [$Section] must contain exactly one $Key key; found $($matches.Count)."
    }
    $newBody = $keyRegex.Replace(
        $bodyGroup.Value,
        [Text.RegularExpressions.MatchEvaluator] {
            param($match)
            $match.Groups['prefix'].Value + $Value + $match.Groups['ending'].Value
        },
        1)
    return $Text.Substring(0, $bodyGroup.Index) + $newBody +
        $Text.Substring($bodyGroup.Index + $bodyGroup.Length)
}

function Assert-BenchmarkReShadeConfiguration([string] $Text) {
    [void](Assert-ReShadeRootAddonPolicy $Text 'Packaged ReShade.ini' `
        -RequireAddonSection -RequireAddonPath)
}

function Get-BenchmarkSummary([IO.FileInfo] $Result) {
    try {
        [xml] $xml = [IO.File]::ReadAllText($Result.FullName)
        $values = @{}
        foreach ($metric in @('Average', 'Min', 'Max')) {
            $raw = [string] $xml.Benchmark.FPS.$metric.value
            $match = [regex]::Match(
                $raw,
                '^(?<fps>(?:0|[1-9][0-9]*)(?:\.[0-9]+)?)#(?<bits>[0-9A-Fa-f]{8})$')
            [single] $parsed = 0
            if (-not $match.Success -or
                -not [single]::TryParse(
                    $match.Groups['fps'].Value,
                    [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [ref] $parsed) -or
                [single]::IsNaN($parsed) -or
                [single]::IsInfinity($parsed)) {
                throw "invalid $metric value '$raw'"
            }
            $expectedBits = '{0:X8}' -f [BitConverter]::ToUInt32(
                [BitConverter]::GetBytes($parsed), 0)
            if ($match.Groups['bits'].Value.ToUpperInvariant() -cne
                $expectedBits) {
                throw "invalid $metric bit pattern '$raw'"
            }
            $values[$metric] = $parsed
        }
        $average = [double] $values['Average']
        $minimum = [double] $values['Min']
        $maximum = [double] $values['Max']
        if ($average -le 0 -or $maximum -le 0 -or $minimum -lt 0 -or
            $minimum -gt $average -or $average -gt $maximum) {
            throw 'Benchmark FPS values must satisfy 0 <= min <= average <= max with positive average and max.'
        }
        return [ordered]@{
            average_fps = $average
            minimum_fps = $minimum
            maximum_fps = $maximum
        }
    } catch {
        throw "Benchmark XML is invalid: $($Result.FullName): $($_.Exception.Message)"
    }
}

function Register-OwnedGame(
    [Diagnostics.Process] $Process,
    [datetime] $Requested) {
    $Process.Refresh()
    if ($Process.HasExited -or
        $Process.StartTime -lt $Requested.AddSeconds(-2) -or
        -not (Test-PathsEqual $Process.Path $gameExe)) {
        throw 'The desktop shortcut did not return the exact task-owned game process.'
    }
    if ($script:ownedGameProcesses.ContainsKey($Process.Id) -and
        $script:ownedGameProcesses[$Process.Id] -ne $Process.StartTime) {
        throw "The benchmark process PID was reused before registration: $($Process.Id)"
    }
    $script:ownedGameProcesses[$Process.Id] = $Process.StartTime
}

function Test-RegisteredOwnedGame([Diagnostics.Process] $Process) {
    try {
        return $script:ownedGameProcesses.ContainsKey($Process.Id) -and
            $script:ownedGameProcesses[$Process.Id] -eq $Process.StartTime -and
            -not $Process.HasExited -and
            (Test-PathsEqual $Process.Path $gameExe)
    } catch {
        return $false
    }
}

function Stop-OwnedGame([Diagnostics.Process] $Process) {
    if (-not $Process) {
        return
    }
    if (-not $script:ownedGameProcesses.ContainsKey($Process.Id)) {
        throw "Refusing to stop an unowned game process: $($Process.Id)"
    }
    $registeredStart = $script:ownedGameProcesses[$Process.Id]
    try { $Process.Refresh() } catch { return }
    if ($Process.HasExited) {
        return
    }
    if ($Process.StartTime -ne $registeredStart -or
        -not (Test-PathsEqual $Process.Path $gameExe)) {
        throw "Refusing to stop a reused or unowned game process: $($Process.Id)"
    }
    try { [void] $Process.CloseMainWindow() } catch { }
    if (-not $Process.WaitForExit(5000)) {
        try { $Process.Kill() } catch { }
        [void] $Process.WaitForExit(5000)
    }
    if (-not $Process.HasExited) {
        throw "Task-owned benchmark process did not stop: $($Process.Id)"
    }
}

function Stop-TaskGames([datetime] $RunStart) {
    $deadline = (Get-Date).AddSeconds(15)
    $quietSince = $null
    while ((Get-Date) -lt $deadline) {
        $gameProcesses = @(
            Get-Process -Name sdhdship -ErrorAction SilentlyContinue)
        foreach ($gameProcess in $gameProcesses) {
            if (-not (Test-RegisteredOwnedGame $gameProcess)) {
                # Never infer ownership from path and start time. A manual
                # launch during a long benchmark belongs to the user.
                return $false
            }
            Stop-OwnedGame $gameProcess
        }
        if (Get-Process -Name sdhdship -ErrorAction SilentlyContinue) {
            $quietSince = $null
        } elseif ($null -eq $quietSince) {
            $quietSince = Get-Date
        } elseif (((Get-Date) - $quietSince).TotalSeconds -ge 2) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return -not [bool] (Get-Process -Name sdhdship -ErrorAction SilentlyContinue)
}

function Invoke-CleanupBarrier(
    [string] $Context,
    [Diagnostics.Process] $Process,
    [datetime] $RunStart,
    [object[]] $UserStateTargets,
    [Collections.Generic.List[string]] $Failures) {
    try {
        Stop-OwnedGame $Process
    } catch {
        $Failures.Add(($Context + ': owned-process cleanup failed: ' +
                $_.Exception.Message))
    }
    try {
        if (-not (Stop-TaskGames $RunStart)) {
            $Failures.Add(($Context +
                    ': a task-owned game process resisted cleanup'))
        }
    } catch {
        $Failures.Add(($Context + ': task-process cleanup failed: ' +
                $_.Exception.Message))
    }

    if (@(Get-SPatchLiveGameProcesses).Count -ne 0) {
        $Failures.Add(($Context +
                ': configuration restoration refused while an unowned or resistant game process is live'))
        return
    }
    try {
        [void] (Restore-SPatchRecoveryBackup `
            $GameRoot $UserStateTargets -KeepBackup)
    } catch {
        $Failures.Add(($Context + ': recovery-backed restoration failed: ' +
                $_.Exception.Message))
    }
}

function Test-PathsEqual([string] $Left, [string] $Right) {
    try {
        return [IO.Path]::GetFullPath($Left).TrimEnd([char[]]'\/') -ieq
            [IO.Path]::GetFullPath($Right).TrimEnd([char[]]'\/')
    } catch {
        return $false
    }
}

function Register-LauncherSteamOwnership([Diagnostics.Process] $Process) {
    $startedProperty = $Process.PSObject.Properties['SPatchStartedSteam']
    if ($null -eq $startedProperty -or -not [bool] $startedProperty.Value) {
        return
    }
    $executableProperty =
        $Process.PSObject.Properties['SPatchStartedSteamExecutable']
    $processIdProperty =
        $Process.PSObject.Properties['SPatchStartedSteamProcessId']
    $startTimeProperty =
        $Process.PSObject.Properties['SPatchStartedSteamStartTimeUtc']
    if ($null -eq $executableProperty -or
        [string]::IsNullOrWhiteSpace([string] $executableProperty.Value) -or
        $null -eq $processIdProperty -or
        $null -eq $startTimeProperty) {
        throw 'The benchmark launcher reported incomplete task-started Steam identity.'
    }
    $steamExecutable = [IO.Path]::GetFullPath(
        [string] $executableProperty.Value)
    if ([IO.Path]::GetFileName($steamExecutable) -ine 'steam.exe') {
        throw "The benchmark launcher reported an invalid Steam executable: $steamExecutable"
    }
    $steamProcessId = [int] $processIdProperty.Value
    $steamStartTimeUtc = ([datetime] $startTimeProperty.Value).ToUniversalTime()
    if ($steamProcessId -le 0) {
        throw 'The benchmark launcher reported an invalid task-started Steam PID.'
    }
    if ($script:taskStartedSteamProcesses.ContainsKey($steamProcessId) -and
        $script:taskStartedSteamProcesses[$steamProcessId].StartTimeUtc -ne
            $steamStartTimeUtc) {
        throw "Task-started Steam PID was reused before registration: $steamProcessId"
    }
    $script:taskStartedSteamProcesses[$steamProcessId] = [pscustomobject]@{
        ProcessId = $steamProcessId
        Executable = $steamExecutable
        StartTimeUtc = $steamStartTimeUtc
    }
}

function Stop-TaskStartedSteamProcesses(
    [datetime] $RunStart,
    [Collections.Generic.List[string]] $Failures) {
    foreach ($owned in $script:taskStartedSteamProcesses.Values) {
        $steamProcess = Get-Process -Id $owned.ProcessId `
            -ErrorAction SilentlyContinue
        if (-not $steamProcess) {
            continue
        }
        $ownershipProven = $false
        try {
            $steamProcess.Refresh()
            $steamPath = [IO.Path]::GetFullPath($steamProcess.Path)
            if ($steamProcess.HasExited -or
                $steamProcess.StartTime.ToUniversalTime() -ne
                    $owned.StartTimeUtc -or
                -not (Test-PathsEqual $steamPath $owned.Executable)) {
                continue
            }
            $ownershipProven = $true
            Stop-Process -Id $steamProcess.Id -Force -ErrorAction Stop
            if (-not $steamProcess.WaitForExit(5000)) {
                throw 'the process did not exit within five seconds'
            }
        } catch {
            if ($ownershipProven) {
                $Failures.Add(
                    "Task-started Steam cleanup failed for PID $($steamProcess.Id): $($_.Exception.Message)")
            }
        }
    }
}

function Register-ShortcutSteamOwnership([object] $Receipt) {
    if (-not [bool] $Receipt.started_steam) {
        return
    }
    $steamProcessId = [int] $Receipt.steam_pid
    $steamStartTimeUtcTicks = [int64] $Receipt.steam_start_utc_ticks
    $steamExecutable = [IO.Path]::GetFullPath([string] $Receipt.steam_path)
    if ($steamProcessId -le 0 -or $steamStartTimeUtcTicks -le 0 -or
        [IO.Path]::GetFileName($steamExecutable) -ine 'steam.exe') {
        throw 'The desktop shortcut reported invalid task-started Steam identity.'
    }
    $steamProcess = Get-Process -Id $steamProcessId -ErrorAction SilentlyContinue
    if (-not $steamProcess -or
        -not (Test-SPatchProcessIdentity `
            $steamProcess $steamProcessId $steamStartTimeUtcTicks `
            $steamExecutable)) {
        throw 'The desktop shortcut task-started Steam PID/start/path identity is not live.'
    }
    $steamStartTimeUtc = [datetime]::new(
        $steamStartTimeUtcTicks, [DateTimeKind]::Utc)
    if ($script:taskStartedSteamProcesses.ContainsKey($steamProcessId) -and
        $script:taskStartedSteamProcesses[$steamProcessId].StartTimeUtc -ne
            $steamStartTimeUtc) {
        throw "Task-started Steam PID was reused before registration: $steamProcessId"
    }
    $script:taskStartedSteamProcesses[$steamProcessId] = [pscustomobject]@{
        ProcessId = $steamProcessId
        Executable = $steamExecutable
        StartTimeUtc = $steamStartTimeUtc
    }
}

function Get-ExactLoadedModuleIdentity(
    [object[]] $Modules,
    [string] $ExpectedPath,
    [string] $ExpectedHash,
    [string] $Label) {
    $expectedName = [IO.Path]::GetFileName($ExpectedPath)
    $sameName = @($Modules | Where-Object {
            [IO.Path]::GetFileName($_.FileName) -ieq $expectedName
        })
    $matches = @($Modules | Where-Object {
            Test-PathsEqual $_.FileName $ExpectedPath
        })
    if ($matches.Count -ne 1 -or $sameName.Count -ne 1) {
        throw ("Expected exactly one loaded $Label named $expectedName at " +
            "$ExpectedPath; exact=$($matches.Count) same-name=$($sameName.Count).")
    }
    $loadedPath = [IO.Path]::GetFullPath($matches[0].FileName)
    $loadedItem = Get-Item -LiteralPath $loadedPath -Force
    $loadedHash = Get-Hash $loadedPath
    if ($loadedHash -cne $ExpectedHash) {
        throw "Loaded $Label does not match its preflight SHA-256 identity."
    }
    return [pscustomobject]@{
        Path = $loadedPath
        Length = [int64] $loadedItem.Length
        Sha256 = $loadedHash
    }
}

function Get-NativeRendererModuleEvidence(
    [Diagnostics.Process] $Process,
    [string] $ExpectedGamePath,
    [string] $ExpectedGameHash,
    [string] $ExpectedSpatchPath,
    [string] $ExpectedSpatchHash,
    [string] $ExpectedLoaderPath,
    [string] $ExpectedLoaderHash,
    [string] $ExpectedAddonPath,
    [string] $ExpectedAddonHash,
    [string] $ExpectedDxgiPath,
    [string] $ExpectedDxgiHash) {
    $modules = @($Process.Modules)
    $gameIdentity = Get-ExactLoadedModuleIdentity `
        $modules $ExpectedGamePath $ExpectedGameHash 'game executable'
    $spatchIdentity = Get-ExactLoadedModuleIdentity `
        $modules $ExpectedSpatchPath $ExpectedSpatchHash 'SPatch ASI'
    $loaderIdentity = Get-ExactLoadedModuleIdentity `
        $modules $ExpectedLoaderPath $ExpectedLoaderHash 'ASI loader'
    $addonIdentity = Get-ExactLoadedModuleIdentity `
        $modules $ExpectedAddonPath $ExpectedAddonHash 'ShenLong graphics ASI'
    $dxgiNameModules = @(
        $modules | Where-Object { $_.ModuleName -ieq 'dxgi.dll' })
    $packagedDxgiModules = @(
        $dxgiNameModules | Where-Object {
            Test-PathsEqual $_.FileName $ExpectedDxgiPath
        })
    if ($packagedDxgiModules.Count -ne 1) {
        throw ("Expected exactly one loaded packaged dxgi.dll at " +
            "$ExpectedDxgiPath; found $($packagedDxgiModules.Count).")
    }
    $systemDxgi = Join-Path $env:SystemRoot 'System32\dxgi.dll'
    $unexpectedDxgi = @(
        $dxgiNameModules | Where-Object {
            -not (Test-PathsEqual $_.FileName $ExpectedDxgiPath) -and
            -not (Test-PathsEqual $_.FileName $systemDxgi)
        })
    if ($unexpectedDxgi.Count -ne 0) {
        throw ('An alternate dxgi.dll host is loaded: ' +
            (($unexpectedDxgi | ForEach-Object { $_.FileName }) -join '; '))
    }
    $packagedDxgiPath = [IO.Path]::GetFullPath(
        $packagedDxgiModules[0].FileName)
    $packagedDxgiItem = Get-Item -LiteralPath $packagedDxgiPath -Force
    $packagedDxgiHash = Get-Hash $packagedDxgiPath
    if ($packagedDxgiHash -cne $ExpectedDxgiHash) {
        throw 'Loaded game-root dxgi.dll does not match the selected package manifest.'
    }

    $forbiddenRendererModules = @(
        $modules | Where-Object {
            $_.ModuleName -ilike 'dxvk*.dll' -or
            $_.ModuleName -ieq 'd3d11on12.dll'
        })
    if ($forbiddenRendererModules.Count -ne 0) {
        throw ('A non-native renderer module is loaded: ' +
            (($forbiddenRendererModules | ForEach-Object {
                        $_.FileName
                    }) -join '; '))
    }

    $systemD3d11 = Join-Path $env:SystemRoot 'System32\d3d11.dll'
    $d3d11NameModules = @(
        $modules | Where-Object { $_.ModuleName -ieq 'd3d11.dll' })
    $systemD3d11Modules = @(
        $d3d11NameModules | Where-Object {
            Test-PathsEqual $_.FileName $systemD3d11
        })
    if ($systemD3d11Modules.Count -ne 1 -or
        $d3d11NameModules.Count -ne 1) {
        throw ('Stock Native D3D11 requires exactly one system d3d11.dll; ' +
            'loaded paths: ' +
            (($d3d11NameModules | ForEach-Object { $_.FileName }) -join '; '))
    }
    $nativeD3d11Path = [IO.Path]::GetFullPath(
        $systemD3d11Modules[0].FileName)
    $nativeD3d11Item = Get-Item -LiteralPath $nativeD3d11Path -Force
    $systemDxgiEvidence = @(
        $dxgiNameModules | Where-Object {
            Test-PathsEqual $_.FileName $systemDxgi
        } | ForEach-Object {
            $path = [IO.Path]::GetFullPath($_.FileName)
            $item = Get-Item -LiteralPath $path -Force
            [ordered]@{
                name = $item.Name
                path = $path
                length = [int64] $item.Length
                sha256 = Get-Hash $path
            }
        })
    return [pscustomobject]@{
        Game = $gameIdentity
        Spatch = $spatchIdentity
        Loader = $loaderIdentity
        Addon = $addonIdentity
        PackagedDxgiPath = $packagedDxgiPath
        PackagedDxgiLength = [int64] $packagedDxgiItem.Length
        PackagedDxgiHash = $packagedDxgiHash
        SystemDxgiModules = $systemDxgiEvidence
        NativeD3d11Path = $nativeD3d11Path
        NativeD3d11Length = [int64] $nativeD3d11Item.Length
        NativeD3d11Hash = Get-Hash $nativeD3d11Path
    }
}

function Wait-NativeRendererModuleEvidence(
    [Diagnostics.Process] $Process,
    [string] $ExpectedGamePath,
    [string] $ExpectedGameHash,
    [string] $ExpectedSpatchPath,
    [string] $ExpectedSpatchHash,
    [string] $ExpectedLoaderPath,
    [string] $ExpectedLoaderHash,
    [string] $ExpectedAddonPath,
    [string] $ExpectedAddonHash,
    [string] $ExpectedDxgiPath,
    [string] $ExpectedDxgiHash) {
    $deadline = (Get-Date).AddSeconds(60)
    $lastError = ''
    $exitRetryPerformed = $false
    while ((Get-Date) -lt $deadline) {
        try {
            $Process.Refresh()
        } catch {
            throw 'The benchmark process exited before module attestation completed.'
        }
        if ($Process.HasExited) {
            if (-not $exitRetryPerformed) {
                $exitRetryPerformed = $true
                Start-Sleep -Milliseconds 250
                continue
            }
            throw 'The benchmark process exited before module attestation completed.'
        }
        try {
            return Get-NativeRendererModuleEvidence `
                $Process `
                $ExpectedGamePath $ExpectedGameHash `
                $ExpectedSpatchPath $ExpectedSpatchHash `
                $ExpectedLoaderPath $ExpectedLoaderHash `
                $ExpectedAddonPath $ExpectedAddonHash `
                $ExpectedDxgiPath $ExpectedDxgiHash
        } catch {
            $lastError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 250
    }
    throw "The benchmark did not expose the exact Stock Native D3D11 module set: $lastError"
}

function Start-ShortcutGame([datetime] $RequestedUtc) {
    # Steam can keep the previous shortcut-initiated launch registered for several seconds
    # after its result is written and the owned process exits. Launching inside
    # that release window can bootstrap normally, then exit without benchmarking.
    $latestResult = @(Get-ChildItem -LiteralPath $GameRoot `
            -Filter 'BenchmarkResult-*.xml' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1)
    if ($latestResult.Count -eq 1) {
        $releaseDelayMilliseconds =
            Get-SPatchBenchmarkReleaseDelayMilliseconds $latestResult[0]
        if ($releaseDelayMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $releaseDelayMilliseconds
        }
    }
    $launch = Invoke-SPatchBenchmarkShortcut `
        $gameExe $RequestedUtc 120
    Register-OwnedGame $launch.Process $RequestedUtc.ToLocalTime()
    Register-ShortcutSteamOwnership $launch.Receipt
    return $launch
}

function Wait-BenchmarkResult(
    [datetime] $RequestedUtc,
    [Collections.Generic.Dictionary[string, object]] $BeforeSnapshot,
    [Diagnostics.Process] $Process) {
    $deadline = (Get-Date).AddMinutes(6)
    $lastParseError = ''
    $exitRetryPerformed = $false
    while ((Get-Date) -lt $deadline) {
        $nowUtc = [DateTime]::UtcNow
        $candidates = @(Get-ChildItem -LiteralPath $GameRoot `
                -Filter 'BenchmarkResult-*.xml' -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.LastWriteTimeUtc -ge $RequestedUtc.AddSeconds(-3) -and
                $_.LastWriteTimeUtc -le $nowUtc.AddSeconds(5)
            } | Sort-Object LastWriteTimeUtc -Descending)
        foreach ($candidate in $candidates) {
            try {
                $identity = [pscustomobject]@{
                    Name = $candidate.Name
                    Length = [int64] $candidate.Length
                    Sha256 = Get-Hash $candidate.FullName
                    LastWriteTimeUtc = $candidate.LastWriteTimeUtc
                    FullName = $candidate.FullName
                }
                if ($BeforeSnapshot.ContainsKey($candidate.Name)) {
                    $previous = $BeforeSnapshot[$candidate.Name]
                    if ($previous.Length -eq $identity.Length -and
                        $previous.Sha256 -ceq $identity.Sha256) {
                        continue
                    }
                }
                $summary = Get-BenchmarkSummary $candidate
                return [pscustomobject]@{
                    File = $candidate
                    Summary = $summary
                    Identity = $identity
                }
            } catch {
                # The game can expose the file while it is still being flushed.
                $lastParseError = $_.Exception.Message
            }
        }
        if ($Process.HasExited) {
            if (-not $exitRetryPerformed) {
                $exitRetryPerformed = $true
                Start-Sleep -Milliseconds 500
                continue
            }
            if ($candidates.Count -eq 0) {
                throw 'The shortcut-initiated benchmark exited without a new result.'
            }
            throw "The shortcut-initiated benchmark exited without a complete valid result: $lastParseError"
        }
        Start-Sleep -Milliseconds 500
    }
    throw 'Timed out waiting for the shortcut-initiated benchmark result.'
}

function Assert-PbrLog([string] $Text, [bool] $Enabled) {
    if ($Enabled) {
        $configPattern = '\[ShenLong-PBR\] configured enabled=1 replaceable_variants=18 native_passthrough_variants=2 strength=100 cache=PBR pipeline_replace=1 bind_telemetry=(?<mode>first-success-only|first-per-variant-bounded) draw_replay=0\.'
        $configMatch = [regex]::Match($Text, $configPattern)
        if (-not $configMatch.Success) {
            throw 'PBR-on evidence is missing the exact enabled configuration.'
        }
        $readyLiteral = '[ShenLong-PBR] ready=1 validated=18/18 unique=18 driver_accepted=18 replacement_mask=0xFDFFE atomic_all_or_nothing=1.'
        if ($Text.IndexOf($readyLiteral, [StringComparison]::Ordinal) -lt 0) {
            throw "PBR-on evidence is missing: $readyLiteral"
        }
        $policyLiteral = '[ShenLong-PBR] runtime replacement policy active=18/20 replacement_target_mask=0xFDFFE native_ambient_mask=0x00001 native_compatibility_mask=0x02000 direct_specular_aa=opaque-normal-derivative,vehicle-glass=none.'
        if ($Text.IndexOf($policyLiteral, [StringComparison]::Ordinal) -lt 0) {
            throw "PBR-on evidence is missing the exact runtime policy: $policyLiteral"
        }
        $pattern = '(?m)\[ShenLong-PBR\] present=(?:300|1800) enabled=1 ready=1 strength=100 validated=18/18 native_passthrough=2 discovery_mask=0x(?<discovery>[0-9A-F]{5}) replacement_mask=0x(?<replacement>[0-9A-F]{5}) first_bound_mask=0x(?<bound>(?!00000)[0-9A-F]{5}) requested=[1-9][0-9]* confirmed=[1-9][0-9]* first_bind_samples=(?<samples>[1-9][0-9]*) tag_failures=0;'
        $checkpoint = [regex]::Match($Text, $pattern)
        if (-not $checkpoint.Success) {
            throw 'PBR-on evidence has no complete exact-replacement and bounded bind checkpoint.'
        }
        $discoveryMask = [Convert]::ToUInt32(
            $checkpoint.Groups['discovery'].Value, 16)
        $replacementMask = [Convert]::ToUInt32(
            $checkpoint.Groups['replacement'].Value, 16)
        $establishedMask = [uint32] 0xFDFFE
        $replacementTargetMask = [uint32] 0xFDFFE
        $nativeAmbientMask = [uint32] 0x00001
        $nativeCompatibilityMask = [uint32] 0x02000
        $nativeMask = $nativeAmbientMask -bor $nativeCompatibilityMask
        if (($discoveryMask -band $establishedMask) -ne $establishedMask -or
            $replacementMask -ne $discoveryMask -or
            ($replacementMask -band $nativeMask) -ne 0 -or
            ($replacementMask -band $replacementTargetMask) -ne
                $replacementMask) {
            throw ('PBR discovery/replacement evidence is inconsistent: ' +
                ('discovery=0x{0:X} replacement=0x{1:X} target=0x{2:X}' -f
                    $discoveryMask, $replacementMask,
                    $replacementTargetMask))
        }
        $boundMask = [Convert]::ToUInt32($checkpoint.Groups['bound'].Value, 16)
        if (($boundMask -band $nativeMask) -ne 0 -or
            ($boundMask -band $replacementMask) -ne $boundMask) {
            throw "PBR bind evidence contains a non-replaceable shader bit: 0x$($checkpoint.Groups['bound'].Value)"
        }
        $expectedSamples = 0
        for ($remaining = $boundMask; $remaining -ne 0; $remaining = $remaining -shr 1) {
            $expectedSamples += $remaining -band 1
        }
        if ($configMatch.Groups['mode'].Value -eq 'first-success-only') {
            $expectedSamples = 1
        }
        if ([int]($checkpoint.Groups['samples'].Value) -ne $expectedSamples) {
            throw "PBR bind sample count does not match its bound mask and telemetry mode."
        }
    } else {
        $pattern = '\[ShenLong-PBR\] configured enabled=0 replaceable_variants=18 native_passthrough_variants=2 strength=100 cache=PBR pipeline_replace=1 bind_telemetry=(?:first-success-only|first-per-variant-bounded) draw_replay=0\.'
        if (-not [regex]::IsMatch($Text, $pattern)) {
            throw 'PBR-off evidence is missing the exact disabled configuration.'
        }
        if ($Text -match '\[ShenLong-PBR\] (?:ready=1|present=|runtime replacement policy)') {
            throw 'PBR-off arm initialized or replaced a pipeline.'
        }
    }
}

$graphicsSelection = Resolve-GraphicsPackage $ExpectedGraphicsConfiguration
$selectedGraphicsPackage = $graphicsSelection.Package
$preflightGraphicsIdentity = Assert-SelectedGraphicsIdentity $graphicsSelection 'Preflight'
$dxgiManifestEntries = @(
    $selectedGraphicsPackage.Payload.ImmutablePackage.Files |
        Where-Object { $_.Name -ieq 'dxgi.dll' })
if ($dxgiManifestEntries.Count -ne 1) {
    throw "Selected graphics package must manifest exactly one dxgi.dll; found $($dxgiManifestEntries.Count)."
}
$expectedDxgiPath = Join-Path $GameRoot 'dxgi.dll'
$expectedDxgiHash = $dxgiManifestEntries[0].Sha256
$expectedGameHash = Get-Hash $gameExe
$expectedSpatchHash = Get-Hash $installedAsi
$expectedLoaderHash = Get-Hash $asiLoader
$expectedAddonHash = $selectedGraphicsPackage.Payload.Addon.Sha256
if ((Get-Hash $installedAddon) -cne $expectedAddonHash) {
    throw 'Installed ShenLong.asi differs from the selected package identity.'
}
$performanceEligible = [bool] $selectedGraphicsPackage.PerformanceEligible
$benchmarkReShadeIni = Join-Path `
    $selectedGraphicsPackage.PackageRoot 'ReShade.ini'
if (-not (Test-Path -LiteralPath $benchmarkReShadeIni -PathType Leaf)) {
    throw "Selected graphics package ReShade.ini is missing: $benchmarkReShadeIni"
}
$benchmarkReShadeBytes = Read-Bytes $benchmarkReShadeIni
$benchmarkReShadeHash = Get-Hash $benchmarkReShadeIni
try {
    $benchmarkReShadeText = $strictUtf8.GetString($benchmarkReShadeBytes)
} catch {
    throw "Selected package ReShade.ini is not valid UTF-8: $($_.Exception.Message)"
}
if ($benchmarkReShadeText.Length -gt 0 -and
    $benchmarkReShadeText[0] -eq [char] 0xFEFF) {
    $benchmarkReShadeText = $benchmarkReShadeText.Substring(1)
}
Assert-BenchmarkReShadeConfiguration $benchmarkReShadeText

$originalBaseIniBytes = Read-Bytes $baseIni
$originalIniBytes = Read-Bytes $ini
$originalPreviousIniExists = Test-Path -LiteralPath $previousIni -PathType Leaf
$originalPreviousIniBytes = if ($originalPreviousIniExists) {
    Read-Bytes $previousIni
} else {
    [byte[]]::new(0)
}
$originalReShadeBytes = Read-Bytes $reshadeIni
$originalDisplayBytes = Read-Bytes $displaySettings
$originalBaseIniHash = Get-Hash $baseIni
$originalIniHash = Get-Hash $ini
$originalPreviousIniHash = if ($originalPreviousIniExists) {
    Get-Hash $previousIni
} else {
    ''
}
$originalReShadeHash = Get-Hash $reshadeIni
$originalDisplayHash = Get-Hash $displaySettings
try {
    $originalReShadeText = $strictUtf8.GetString($originalReShadeBytes)
} catch {
    throw "Existing ReShade.ini is not valid UTF-8: $($_.Exception.Message)"
}
[void](Assert-ReShadeRootAddonPolicy $originalReShadeText 'Existing ReShade.ini')
try {
    [xml] $displayDocument = [IO.File]::ReadAllText($displaySettings)
} catch {
    throw "DisplaySettings.xml is invalid: $($_.Exception.Message)"
}
if ([int] $displayDocument.DisplaySettings.ResolutionWidth -ne $ExpectedWidth -or
    [int] $displayDocument.DisplaySettings.ResolutionHeight -ne $ExpectedHeight -or
    [int] $displayDocument.DisplaySettings.Fullscreen -ne 1) {
    throw ('PBR benchmark requires fullscreen {0}x{1} in DisplaySettings.xml.' -f
        $ExpectedWidth, $ExpectedHeight)
}
$baseIniOffset = if ($originalBaseIniBytes.Length -ge 3 -and
    $originalBaseIniBytes[0] -eq 0xEF -and
    $originalBaseIniBytes[1] -eq 0xBB -and
    $originalBaseIniBytes[2] -eq 0xBF) { 3 } else { 0 }
$baseIniText = $strictUtf8.GetString(
    $originalBaseIniBytes, $baseIniOffset,
    $originalBaseIniBytes.Length - $baseIniOffset)
[void] (Get-IniSectionMatch $baseIniText 'SPatch')
$baseIniText = Set-IniValueStrict `
    $baseIniText 'Debug' 'WriteCrashDumps' '1'
if (-not $baseIniText.EndsWith('WriteCrashDumps=1',
        [StringComparison]::Ordinal)) {
    throw 'The SPatch benchmark INI no longer ends with WriteCrashDumps=1.'
}

$iniOffset = if ($originalIniBytes.Length -ge 3 -and
    $originalIniBytes[0] -eq 0xEF -and $originalIniBytes[1] -eq 0xBB -and
    $originalIniBytes[2] -eq 0xBF) { 3 } else { 0 }
$originalIniText = $strictUtf8.GetString(
    $originalIniBytes, $iniOffset, $originalIniBytes.Length - $iniOffset)
if ((Get-IniValueStrict $originalIniText 'ShenLong' 'ConfigVersion') -cne '1') {
    throw 'ShenLong.ini must contain exactly one [ShenLong] ConfigVersion=1 setting.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRunId = '{0}-{1}-{2}' -f
    $stamp, $PID, [Guid]::NewGuid().ToString('N')
$evidenceRoot = Join-Path $RepoRoot (
    "artifacts\benchmarks\pbr-ab-$evidenceRunId")
[void] (New-Item -ItemType Directory -Force -Path $evidenceRoot)
$sequence = [Collections.Generic.List[object]]::new()
$taskStartedSteamProcesses = [Collections.Generic.Dictionary[int, object]]::new()
$runStart = Get-Date
for ($pass = 1; $pass -le $Passes; ++$pass) {
    $firstEnabled = ($pass % 2) -eq 1
    if ($FirstPassPbrOff) {
        $firstEnabled = -not $firstEnabled
    }
    $sequence.Add([pscustomobject]@{ Pass = $pass; Enabled = $firstEnabled })
    $sequence.Add([pscustomobject]@{ Pass = $pass; Enabled = -not $firstEnabled })
}
$results = [Collections.Generic.List[object]]::new()
$primaryFailure = $null
$postflightFailure = $null
$cleanupFailures = [Collections.Generic.List[string]]::new()
$finalGraphicsIdentity = $null
$recovery = New-SPatchRecoveryBackup `
    $GameRoot $userStateTargets 'Invoke-PBRBenchmark.ps1'

try {
    foreach ($arm in $sequence) {
        $cleanupFailureCount = $cleanupFailures.Count
        $process = $null
        $launch = $null
        $armFailure = $null
        $dumpBefore = $null
        $dumpAfter = $null
        $dumpChanges = @()
        $dumpFailureRecorded = $false
        $gameExitCode = $null
        try {
            if (-not (Stop-TaskGames $runStart)) {
                throw "A game process was active before PBR pass $($arm.Pass) ($($arm.Enabled))."
            }
            $armGraphicsIdentity = Assert-SelectedGraphicsIdentity $graphicsSelection ("Before PBR pass $($arm.Pass) ($($arm.Enabled))")
            if ((Get-Hash $baseIni) -cne $originalBaseIniHash) {
                throw "SPatch.ini was not restored before PBR pass $($arm.Pass) ($($arm.Enabled))."
            }
            if ((Get-Hash $ini) -cne $originalIniHash) {
                throw "ShenLong.ini was not restored before PBR pass $($arm.Pass) ($($arm.Enabled))."
            }
            $armText = Set-IniValueStrict $originalIniText 'PhysicallyBasedRendering' 'PhysicallyBasedRendering' ($(if ($arm.Enabled) { '1' } else { '0' }))
            Write-SPatchAtomicText `
                $GameRoot $baseIni $baseIniText $writeUtf8 `
                'SPatch.ini benchmark diagnostics'
            Write-SPatchAtomicText `
                $GameRoot $ini $armText $writeUtf8 'ShenLong.ini benchmark arm'
            Write-SPatchAtomicBytes `
                $GameRoot $reshadeIni $benchmarkReShadeBytes `
                'ReShade.ini benchmark arm'
            if ((Get-Hash $reshadeIni) -cne $benchmarkReShadeHash) {
                throw 'Failed to install the selected package ReShade.ini for the benchmark arm.'
            }

            $resultBefore = Get-SPatchFileIdentitySnapshot `
                $GameRoot 'BenchmarkResult-*.xml'
            $dumpBefore = Get-SPatchFileIdentitySnapshot `
                $GameRoot 'SPatch-*.dmp'
            $requestedUtc = [DateTime]::UtcNow
            $reshadeBefore = New-LogSnapshot $reshadeLog
            $armDir = Join-Path $evidenceRoot ('pass-{0:D2}-{1}' -f
                $arm.Pass, $(if ($arm.Enabled) { 'pbr-on' } else { 'pbr-off' }))
            [void] (New-Item -ItemType Directory -Force -Path $armDir)
            $launch = Start-ShortcutGame $requestedUtc
            $process = $launch.Process
            $processStartUtc = $process.StartTime.ToUniversalTime()
            $rendererEvidence = Wait-NativeRendererModuleEvidence `
                $process `
                $gameExe $expectedGameHash `
                $installedAsi $expectedSpatchHash `
                $asiLoader $expectedLoaderHash `
                $installedAddon $expectedAddonHash `
                $expectedDxgiPath $expectedDxgiHash
            $result = Wait-BenchmarkResult `
                $requestedUtc $resultBefore $process
            $naturalExit = Wait-SPatchNaturalProcessExit `
                $process $launch.ProcessIdentity 30
            if (-not $naturalExit.Exited) {
                throw 'The task-owned benchmark did not exit naturally within 30 seconds of its result.'
            }
            $gameExitCode = [int] $naturalExit.ExitCode
            if ($gameExitCode -notin @(0, 1)) {
                throw "The benchmark exited naturally with unexpected code $gameExitCode."
            }
            $resultFile = Get-Item -LiteralPath $result.File.FullName -Force
            $result.Identity = [pscustomobject]@{
                Name = $resultFile.Name
                Length = [int64] $resultFile.Length
                Sha256 = Get-Hash $resultFile.FullName
                LastWriteTimeUtc = $resultFile.LastWriteTimeUtc
                FullName = $resultFile.FullName
            }
            if ($result.Identity.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(5)) {
                throw 'The completed benchmark result timestamp is too far in the future.'
            }
            if ($resultBefore.ContainsKey($result.Identity.Name)) {
                $oldResultIdentity = $resultBefore[$result.Identity.Name]
                if ($oldResultIdentity.Length -eq $result.Identity.Length -and
                    $oldResultIdentity.Sha256 -ceq $result.Identity.Sha256) {
                    throw 'The completed benchmark result reverted to its pre-attempt identity.'
                }
            }
            $dumpAfter = Get-SPatchFileIdentitySnapshot `
                $GameRoot 'SPatch-*.dmp'
            $dumpChanges = @(Compare-SPatchFileIdentitySnapshot `
                $dumpBefore $dumpAfter)
            if ($dumpChanges.Count -ne 0) {
                $dumpFailureRecorded = $true
                throw ('The benchmark created or changed SPatch crash dumps: ' +
                    (($dumpChanges | ForEach-Object {
                                "$($_.Kind):$($_.Name):$($_.After.Length):$($_.After.Sha256)"
                            }) -join '; '))
            }
            $benchmarkEvidencePath = Join-Path $armDir 'BenchmarkResult.xml'
            Copy-Item -LiteralPath $result.Identity.FullName `
                -Destination $benchmarkEvidencePath -Force
            $benchmarkSummary = Get-BenchmarkSummary (
                Get-Item -LiteralPath $benchmarkEvidencePath)
            $reshadeText = Get-FreshLogSessionText `
                $reshadeLog $reshadeBefore $processStartUtc 'ReShade.log'
            Assert-PbrLog $reshadeText $arm.Enabled
            [IO.File]::WriteAllText(
                (Join-Path $armDir 'ReShade.log'), $reshadeText, $writeUtf8)
            $entry = [ordered]@{
                pass = $arm.Pass
                pbr_enabled = $arm.Enabled
                launcher = $benchmarkShortcut
                arguments = '-benchmark -skipStartScreen'
                process_id = $process.Id
                process_start_utc_ticks = $launch.ProcessIdentity.StartTimeUtcTicks
                game_exit_code = $gameExitCode
                benchmark_result = $benchmarkEvidencePath
                benchmark_result_name = $result.Identity.Name
                benchmark_result_length = $result.Identity.Length
                benchmark_result_sha256 = $result.Identity.Sha256
                reshade_log = (Join-Path $armDir 'ReShade.log')
                graphics_configuration = $selectedGraphicsPackage.Configuration
                performance_eligible = $performanceEligible
                selected_package_payload_sha256 = $selectedGraphicsPackage.Payload.PayloadSha256
                installed_graphics_payload_sha256 = $armGraphicsIdentity.PayloadSha256
                graphics_addon_length = $armGraphicsIdentity.Addon.Length
                graphics_addon_sha256 = $armGraphicsIdentity.Addon.Sha256
                immutable_graphics_file_count = $armGraphicsIdentity.ImmutablePackage.FileCount
                immutable_graphics_manifest_sha256 = $armGraphicsIdentity.ImmutablePackage.ManifestSha256
                pbr_cache_file_count = $armGraphicsIdentity.PbrCache.FileCount
                pbr_cache_manifest_sha256 = $armGraphicsIdentity.PbrCache.ManifestSha256
                graphics_identity_revalidated_before_arm = $true
                graphics_identity_revalidated_after_arm = $false
                benchmark_reshade_ini_sha256 = $benchmarkReShadeHash
                loaded_game_path = $rendererEvidence.Game.Path
                loaded_game_length = $rendererEvidence.Game.Length
                loaded_game_sha256 = $rendererEvidence.Game.Sha256
                loaded_spatch_path = $rendererEvidence.Spatch.Path
                loaded_spatch_length = $rendererEvidence.Spatch.Length
                loaded_spatch_sha256 = $rendererEvidence.Spatch.Sha256
                loaded_asi_loader_path = $rendererEvidence.Loader.Path
                loaded_asi_loader_length = $rendererEvidence.Loader.Length
                loaded_asi_loader_sha256 = $rendererEvidence.Loader.Sha256
                loaded_graphics_addon_path = $rendererEvidence.Addon.Path
                loaded_graphics_addon_length = $rendererEvidence.Addon.Length
                loaded_graphics_addon_sha256 = $rendererEvidence.Addon.Sha256
                loaded_reshade_host_path = $rendererEvidence.PackagedDxgiPath
                loaded_reshade_host_length = $rendererEvidence.PackagedDxgiLength
                loaded_reshade_host_sha256 = $rendererEvidence.PackagedDxgiHash
                loaded_system_dxgi_modules = @($rendererEvidence.SystemDxgiModules)
                loaded_native_d3d11_path = $rendererEvidence.NativeD3d11Path
                loaded_native_d3d11_length = $rendererEvidence.NativeD3d11Length
                loaded_native_d3d11_sha256 = $rendererEvidence.NativeD3d11Hash
                stock_native_d3d11_attested = $true
                display_settings_path = $displaySettings
                display_settings_sha256 = $originalDisplayHash
                crash_dumps_before = @(Convert-SPatchSnapshotToEvidence $dumpBefore)
                crash_dumps_after = @(Convert-SPatchSnapshotToEvidence $dumpAfter)
                new_or_changed_crash_dumps = 0
                average_fps = $benchmarkSummary.average_fps
                minimum_fps = $benchmarkSummary.minimum_fps
                maximum_fps = $benchmarkSummary.maximum_fps
            }
            $results.Add([pscustomobject] $entry)
            [IO.File]::WriteAllText(
                (Join-Path $armDir 'summary.json'),
                ($entry | ConvertTo-Json -Depth 5), $writeUtf8)
        } catch {
            $armFailure = $_
        } finally {
            $cleanupArguments = @{
                Context = "PBR pass $($arm.Pass) ($($arm.Enabled))"
                Process = $process
                RunStart = $runStart
                UserStateTargets = $userStateTargets
                Failures = $cleanupFailures
            }
            try {
                Invoke-CleanupBarrier @cleanupArguments
            } catch {
                $cleanupFailures.Add(
                    "PBR pass $($arm.Pass) cleanup barrier failed unexpectedly: $($_.Exception.Message)")
            }
            if ($null -ne $dumpBefore) {
                try {
                    $dumpAfter = Get-SPatchFileIdentitySnapshot `
                        $GameRoot 'SPatch-*.dmp'
                    $dumpChanges = @(Compare-SPatchFileIdentitySnapshot `
                        $dumpBefore $dumpAfter)
                    if ($dumpChanges.Count -ne 0 -and -not $dumpFailureRecorded) {
                        $cleanupFailures.Add(
                            "PBR pass $($arm.Pass) created or changed crash dumps: " +
                            (($dumpChanges | ForEach-Object {
                                        "$($_.Kind):$($_.Name):$($_.After.Length):$($_.After.Sha256)"
                                    }) -join '; '))
                    }
                } catch {
                    $cleanupFailures.Add(
                        "PBR pass $($arm.Pass) crash-dump postflight failed: $($_.Exception.Message)")
                }
            }
        }
        try {
            [void] (Assert-SelectedGraphicsIdentity `
                $graphicsSelection `
                ("After PBR pass $($arm.Pass) ($($arm.Enabled))"))
            if ($results.Count -ne 0 -and
                $results[$results.Count - 1].pass -eq $arm.Pass -and
                $results[$results.Count - 1].pbr_enabled -eq $arm.Enabled) {
                $results[$results.Count - 1].graphics_identity_revalidated_after_arm = $true
                [IO.File]::WriteAllText(
                    (Join-Path $armDir 'summary.json'),
                    ($results[$results.Count - 1] |
                        ConvertTo-Json -Depth 5),
                    $writeUtf8)
            }
        } catch {
            if ($armFailure) {
                $cleanupFailures.Add(
                    "PBR pass $($arm.Pass) post-arm graphics identity failed: $($_.Exception.Message)")
            } else {
                $armFailure = $_
            }
        }
        if ($armFailure) {
            throw $armFailure
        }
        if ($cleanupFailures.Count -gt $cleanupFailureCount) {
            break
        }
    }
} catch {
    $primaryFailure = $_
} finally {
    $finalCleanupArguments = @{
        Context = 'Final cleanup barrier'
        Process = $null
        RunStart = $runStart
        UserStateTargets = $userStateTargets
        Failures = $cleanupFailures
    }
    try {
        Invoke-CleanupBarrier @finalCleanupArguments
    } catch {
        $cleanupFailures.Add(
            "Final cleanup barrier failed unexpectedly: $($_.Exception.Message)")
    }
    Stop-TaskStartedSteamProcesses $runStart $cleanupFailures
    try {
        Complete-SPatchRecoveryBackup $GameRoot $userStateTargets
    } catch {
        $cleanupFailures.Add(
            "Final recovery completion failed: $($_.Exception.Message)")
    }
}

try {
    $finalGraphicsIdentity = Assert-SelectedGraphicsIdentity $graphicsSelection 'Postflight'
} catch {
    $postflightFailure = $_
}

if ($primaryFailure -or $postflightFailure -or $cleanupFailures.Count -ne 0) {
    $failureMessages = [Collections.Generic.List[string]]::new()
    if ($primaryFailure) {
        $failureMessages.Add(
            'Primary failure: ' + $primaryFailure.Exception.Message)
    }
    if ($postflightFailure) {
        $failureMessages.Add(
            'Postflight graphics identity failure: ' +
                $postflightFailure.Exception.Message)
    }
    foreach ($cleanupFailure in $cleanupFailures) {
        $failureMessages.Add('Cleanup failure: ' + $cleanupFailure)
    }
    $innerException = if ($primaryFailure) {
        $primaryFailure.Exception
    } elseif ($postflightFailure) {
        $postflightFailure.Exception
    } else {
        $null
    }
    throw [InvalidOperationException]::new(
        ('Matched PBR benchmark failed after the final restoration barrier. ' +
            ($failureMessages -join ' | ')),
        $innerException)
}


$on = @($results | Where-Object pbr_enabled)
$off = @($results | Where-Object { -not $_.pbr_enabled })
if ($on.Count -ne $Passes -or $off.Count -ne $Passes) {
    throw "Expected $Passes completed passes per arm; found on=$($on.Count), off=$($off.Count)."
}
$onMean = ($on | Measure-Object -Property average_fps -Average).Average
$offMean = ($off | Measure-Object -Property average_fps -Average).Average
if ($null -eq $onMean -or
    [double]::IsNaN([double] $onMean) -or
    [double]::IsInfinity([double] $onMean) -or
    [double] $onMean -le 0) {
    throw 'PBR-on mean average FPS must be finite and greater than zero.'
}
if ($null -eq $offMean -or
    [double]::IsNaN([double] $offMean) -or
    [double]::IsInfinity([double] $offMean) -or
    [double] $offMean -le 0) {
    throw 'PBR-off mean average FPS must be finite and greater than zero.'
}
$deltaPercent = (($onMean - $offMean) / $offMean) * 100.0
$summary = [ordered]@{
    generated_at = (Get-Date).ToString('o')
    game_root = $GameRoot
    resolution = "${ExpectedWidth}x${ExpectedHeight}"
    launcher = $benchmarkShortcut
    arguments = '-benchmark -skipStartScreen'
    steam_applaunch_used = $false
    passes_per_arm = $Passes
    first_pass_pbr_enabled = -not [bool] $FirstPassPbrOff
    counterbalanced_order = (($Passes % 2) -eq 0)
    alternating_pair_order = $true
    first_position_order_imbalance = ($Passes % 2)
    fullscreen_window_enforced = $true
    requested_graphics_configuration = $ExpectedGraphicsConfiguration
    graphics_configuration = $selectedGraphicsPackage.Configuration
    performance_eligible = $performanceEligible
    performance_eligibility = $(if ($performanceEligible) {
            'Publishing-Release payload; performance evidence eligible'
        } else {
            'Development-Release payload; functional evidence only'
        })
    graphics_package_root = $selectedGraphicsPackage.PackageRoot
    selected_package_payload_sha256 = $selectedGraphicsPackage.Payload.PayloadSha256
    preflight_installed_graphics_payload_sha256 = $preflightGraphicsIdentity.PayloadSha256
    final_installed_graphics_payload_sha256 = $finalGraphicsIdentity.PayloadSha256
    graphics_addon_length = $finalGraphicsIdentity.Addon.Length
    graphics_addon_sha256 = $finalGraphicsIdentity.Addon.Sha256
    immutable_graphics_file_count = $finalGraphicsIdentity.ImmutablePackage.FileCount
    immutable_graphics_manifest_sha256 = $finalGraphicsIdentity.ImmutablePackage.ManifestSha256
    pbr_cache_file_count = $finalGraphicsIdentity.PbrCache.FileCount
    pbr_cache_manifest_sha256 = $finalGraphicsIdentity.PbrCache.ManifestSha256
    graphics_identity_preflight_verified = $true
    graphics_identity_revalidated_before_each_arm = $true
    graphics_identity_revalidated_after_each_arm = $true
    graphics_identity_postflight_verified = $true
    benchmark_reshade_ini_sha256 = $benchmarkReShadeHash
    write_crash_dumps_forced = $true
    exact_shortcut_launch_enforced = $true
    pbr_active_replacements = '18/20'
    pbr_replacement_target_mask = '0xFDFFE'
    pbr_established_runtime_mask = '0xFDFFE'
    pbr_native_ambient_mask = '0x00001'
    pbr_native_compatibility_mask = '0x02000'
    original_spatch_ini_sha256 = $originalBaseIniHash
    original_shenlong_ini_sha256 = $originalIniHash
    original_previous_ini_exists = $originalPreviousIniExists
    original_previous_ini_sha256 = $originalPreviousIniHash
    original_reshade_ini_sha256 = $originalReShadeHash
    original_display_settings_sha256 = $originalDisplayHash
    display_settings_path = $displaySettings
    pbr_on_mean_average_fps = $onMean
    pbr_off_mean_average_fps = $offMean
    pbr_mean_delta_percent = $deltaPercent
    arms = @($results)
    evidence_directory = $evidenceRoot
    exact_user_files_restored = $true
}
[IO.File]::WriteAllText(
    (Join-Path $evidenceRoot 'summary.json'),
    ($summary | ConvertTo-Json -Depth 8), $writeUtf8)
$results | Select-Object pass,pbr_enabled,average_fps,minimum_fps,maximum_fps |
    Format-Table -AutoSize
Write-Host ('PBR on mean average FPS:  {0:F3}' -f $onMean)
Write-Host ('PBR off mean average FPS: {0:F3}' -f $offMean)
Write-Host ('PBR mean delta:           {0:F3}%' -f $deltaPercent)
Write-Host ("Graphics payload: configuration=$($selectedGraphicsPackage.Configuration) performance_eligible=$performanceEligible payload_sha256=$($finalGraphicsIdentity.PayloadSha256)")
Write-Host "PBR A/B PASS evidence=$evidenceRoot"
} finally {
    if ($ownsLiveHarnessMutex) {
        $liveHarnessMutex.ReleaseMutex()
    }
    $liveHarnessMutex.Dispose()
}
