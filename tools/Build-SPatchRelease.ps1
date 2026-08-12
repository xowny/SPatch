[CmdletBinding(DefaultParameterSetName = 'Publish')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string]$ArtifactPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string]$NormalTestsPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string]$FinalTestsPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string]$BuildAttestationPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-f]{32}$')]
    [string]$BuildInvocationId,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-f]{32}$')]
    [string]$BuildLockToken,

    [Parameter(Mandatory = $true, ParameterSetName = 'BuildLock')]
    [ValidateSet('Acquire', 'Release')]
    [string]$BuildLockAction,

    [Parameter(Mandatory = $true, ParameterSetName = 'BuildLock')]
    [ValidatePattern('^[0-9a-f]{32}$')]
    [string]$LockToken,

    [Parameter(ParameterSetName = 'BuildLock')]
    [ValidateRange(0, [int]::MaxValue)]
    [int]$LockOwnerPid = 0,

    [Parameter(Mandatory = $true, ParameterSetName = 'ArchiveFixture')]
    [string]$ArchiveFixtureRoot,

    [Parameter(Mandatory = $true, ParameterSetName = 'ArchiveFixture')]
    [string]$ArchiveFixtureOutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'artifacts\release'))
$noticeRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'artifacts\release-notices'))
$activePackage = Join-Path $releaseRoot 'SPatch-Base'
$archivePath = Join-Path $releaseRoot 'SPatch-Base.zip'
$serial = '{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N')
$stagingPackage = Join-Path $releaseRoot ".SPatch-Base.staging-$serial"
$validationRoot = Join-Path $releaseRoot ".SPatch-Base.validation-$serial"
$archiveValidationRoot =
    Join-Path $releaseRoot ".SPatch-Base.archive-validation-$serial"
$temporaryArchive = Join-Path $releaseRoot ".SPatch-Base-$serial.zip"
$repeatArchive = Join-Path $releaseRoot ".SPatch-Base-$serial.repeat.zip"
$packageBackup = Join-Path $releaseRoot ".SPatch-Base.previous-$serial"
$archiveBackup = Join-Path $releaseRoot ".SPatch-Base.zip.previous-$serial"

$keyAlgorithm = [Security.Cryptography.SHA256]::Create()
try {
    $repoKey = ([BitConverter]::ToString($keyAlgorithm.ComputeHash(
        [Text.Encoding]::UTF8.GetBytes($repoRoot.ToUpperInvariant())))).Replace(
            '-', '').Substring(0, 24)
} finally {
    $keyAlgorithm.Dispose()
}
$buildLockPath = Join-Path $releaseRoot ".SPatch.final-build.$repoKey.lock"
$publicationMutexName = "Local\SPatch.BaseReleasePublication.$repoKey"

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
    $rootItem = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
    if ($null -ne $rootItem -and
        ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description contains a reparse point: $cursor"
    }
    $relative = $fullPath.Substring($pathRoot.Length)
    foreach ($component in @($relative -split '[\\/]' | Where-Object {
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

function Assert-NoReparseTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $rootFull = Assert-NoReparsePath -Path $Root -Description $Description
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) {
        throw "$Description is not a directory: $rootFull"
    }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootFull)
    while ($pending.Count -ne 0) {
        $directory = $pending.Pop()
        foreach ($entryPath in [IO.Directory]::EnumerateFileSystemEntries(
                $directory)) {
            $entry = Get-Item -LiteralPath $entryPath -Force
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) {
                $pending.Push($entry.FullName)
            }
        }
    }
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

function Get-DeclaredConfigVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $text = [IO.File]::ReadAllText($Path)
    $matches = @([regex]::Matches(
        $text,
        '(?m)^\s*inline\s+constexpr\s+int\s+kConfigVersion\s*=\s*(?<version>[1-9][0-9]*)\s*;\s*$'))
    if ($matches.Count -ne 1) {
        throw "Config.h must declare exactly one positive kConfigVersion: $Path"
    }

    [int]$version = 0
    if (-not [int]::TryParse(
            $matches[0].Groups['version'].Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$version)) {
        throw "Config.h declares an unsupported kConfigVersion: $Path"
    }
    return $version
}

function Assert-ReleaseChildPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $releaseRoot.TrimEnd([char[]]'\/') +
              [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the release root: $fullPath"
    }
    [void](Assert-NoReparsePath -Path $fullPath `
        -Description 'Release publication path')
    return $fullPath
}

function Remove-ReleaseDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $checkedPath = Assert-ReleaseChildPath -Path $Path
    if (Test-Path -LiteralPath $checkedPath) {
        if (-not (Test-Path -LiteralPath $checkedPath -PathType Container)) {
            throw "Expected a release directory but found another item: $checkedPath"
        }
        Assert-NoReparseTree -Root $checkedPath `
            -Description 'Release directory cleanup'
        Remove-Item -LiteralPath $checkedPath -Recurse -Force
    }
}

function Remove-ReleaseFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $checkedPath = Assert-ReleaseChildPath -Path $Path
    if (Test-Path -LiteralPath $checkedPath) {
        if (-not (Test-Path -LiteralPath $checkedPath -PathType Leaf)) {
            throw "Expected a release file but found another item: $checkedPath"
        }
        [void](Assert-NoReparsePath -Path $checkedPath `
            -Description 'Release file cleanup')
        Remove-Item -LiteralPath $checkedPath -Force
    }
}

function Read-StrictKeyValueFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedKeys,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $resolvedPath = Get-ResolvedLeafPath -Path $Path -Description $Description
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $values = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines($resolvedPath, $encoding)) {
        if ($line -cnotmatch '^(?<key>[A-Z0-9_]+)=(?<value>[^\r\n]+)$') {
            throw "Malformed $Description line: $line"
        }
        if ($values.ContainsKey($Matches['key'])) {
            throw "Duplicate $Description key: $($Matches['key'])"
        }
        $values.Add($Matches['key'], $Matches['value'])
    }
    if ($values.Count -ne $ExpectedKeys.Count) {
        throw "$Description must contain exactly $($ExpectedKeys.Count) keys."
    }
    foreach ($key in $ExpectedKeys) {
        if (-not $values.ContainsKey($key)) {
            throw "$Description omits $key."
        }
    }
    return $values
}

function Get-ProcessStartTicks {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId
    )

    try {
        $process = [Diagnostics.Process]::GetProcessById($ProcessId)
        try {
            return $process.StartTime.ToUniversalTime().Ticks
        } finally {
            $process.Dispose()
        }
    } catch [ArgumentException] {
        return $null
    } catch [InvalidOperationException] {
        return $null
    }
}

function Resolve-BuildLockOwnerPid {
    param(
        [ValidateRange(0, [int]::MaxValue)]
        [int]$ExplicitOwnerPid
    )

    if ($ExplicitOwnerPid -gt 0) {
        return $ExplicitOwnerPid
    }

    # MSBuild Exec starts powershell.exe through a short-lived cmd.exe. Walk
    # the observed process ancestry instead of treating the immediate parent
    # as the build owner; that parent exits as soon as this Exec completes.
    $candidatePid = $PID
    for ($depth = 0; $depth -lt 32; ++$depth) {
        $record = Get-CimInstance -ClassName Win32_Process `
            -Filter "ProcessId = $candidatePid"
        if ($null -eq $record -or [int]$record.ParentProcessId -le 0) {
            break
        }
        $candidatePid = [int]$record.ParentProcessId
        $parent = Get-CimInstance -ClassName Win32_Process `
            -Filter "ProcessId = $candidatePid"
        if ($null -eq $parent) {
            break
        }
        if ([string]$parent.Name -ieq 'MSBuild.exe') {
            return $candidatePid
        }
    }
    throw 'Could not resolve the owning MSBuild process for the final-build lock.'
}

function Read-BuildLock {
    $values = Read-StrictKeyValueFile -Path $buildLockPath `
        -ExpectedKeys @(
            'TOKEN', 'OWNER_PID', 'OWNER_START_UTC_TICKS', 'REPO_KEY') `
        -Description 'Final-build lock'
    if ($values['TOKEN'] -cnotmatch '^[0-9a-f]{32}$' -or
        $values['OWNER_PID'] -cnotmatch '^[1-9][0-9]{0,9}$' -or
        $values['OWNER_START_UTC_TICKS'] -cnotmatch '^[1-9][0-9]{0,18}$' -or
        $values['REPO_KEY'] -cne $repoKey) {
        throw 'Final-build lock has an invalid contract.'
    }
    return $values
}

function Test-BuildLockOwnerAlive {
    param(
        [Parameter(Mandatory = $true)]
        [Collections.Generic.Dictionary[string, string]]$Values
    )

    $actualTicks = Get-ProcessStartTicks -ProcessId ([int]$Values['OWNER_PID'])
    return $null -ne $actualTicks -and
        [string]$actualTicks -ceq $Values['OWNER_START_UTC_TICKS']
}

function Invoke-BuildLockAction {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Acquire', 'Release')]
        [string]$Action,

        [Parameter(Mandatory = $true)]
        [string]$Token,

        [Parameter(Mandatory = $true)]
        [int]$OwnerPid
    )

    [IO.Directory]::CreateDirectory($releaseRoot) | Out-Null
    [void](Assert-NoReparsePath -Path $releaseRoot `
        -Description 'Release root')
    [void](Assert-NoReparsePath -Path $buildLockPath `
        -Description 'Final-build lock path')

    if ($Action -ceq 'Release') {
        $values = Read-BuildLock
        if ($values['TOKEN'] -cne $Token -or
            $values['OWNER_PID'] -cne [string]$OwnerPid) {
            throw 'Refusing to release a final-build lock owned by another invocation.'
        }
        Remove-ReleaseFile -Path $buildLockPath
        return
    }

    $ownerStartTicks = Get-ProcessStartTicks -ProcessId $OwnerPid
    if ($null -eq $ownerStartTicks) {
        throw "Final-build lock owner process is not running: $OwnerPid"
    }
    if (Test-Path -LiteralPath $buildLockPath) {
        $existing = Read-BuildLock
        if (Test-BuildLockOwnerAlive -Values $existing) {
            throw 'Another SPatch final build already owns the canonical outputs.'
        }
        Remove-ReleaseFile -Path $buildLockPath
    }

    $lockText = @(
        "TOKEN=$Token"
        "OWNER_PID=$OwnerPid"
        "OWNER_START_UTC_TICKS=$ownerStartTicks"
        "REPO_KEY=$repoKey"
    ) -join [Environment]::NewLine
    $lockText += [Environment]::NewLine
    try {
        $stream = [IO.File]::Open(
            $buildLockPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try {
            $bytes = [Text.UTF8Encoding]::new($false).GetBytes($lockText)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        } finally {
            $stream.Dispose()
        }
    } catch [IO.IOException] {
        throw 'Another SPatch final build acquired the canonical outputs concurrently.'
    }
}

function Assert-BuildLockHeld {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Token
    )

    $values = Read-BuildLock
    if ($values['TOKEN'] -cne $Token -or
        -not (Test-BuildLockOwnerAlive -Values $values)) {
        throw 'The current MSBuild invocation does not own the final-build lock.'
    }
}

function Test-ByteIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Left,

        [Parameter(Mandatory = $true)]
        [string]$Right
    )

    $leftBytes = [IO.File]::ReadAllBytes($Left)
    $rightBytes = [IO.File]::ReadAllBytes($Right)
    if ($leftBytes.Length -ne $rightBytes.Length) {
        return $false
    }
    for ($index = 0; $index -lt $leftBytes.Length; ++$index) {
        if ($leftBytes[$index] -ne $rightBytes[$index]) {
            return $false
        }
    }
    return $true
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

function Copy-FileSnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $sourceStream = [IO.File]::Open(
        $Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $sourceLastWriteTimeUtc = [IO.File]::GetLastWriteTimeUtc($Source)
        $destinationStream = [IO.File]::Open(
            $Destination,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try {
            $sourceStream.CopyTo($destinationStream)
            $destinationStream.Flush($true)
        } finally {
            $destinationStream.Dispose()
        }
    } finally {
        $sourceStream.Dispose()
    }
    [IO.File]::SetLastWriteTimeUtc($Destination, $sourceLastWriteTimeUtc)
}

function Assert-PinnedPayloadHashes {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$ExpectedHashes
    )

    foreach ($entry in $ExpectedHashes.GetEnumerator()) {
        $path = Join-Path $Root $entry.Key.Replace('/', '\')
        if ((Get-Sha256 -Path $path) -cne $entry.Value) {
            throw "Release notice or license drifted after staging: $($entry.Key)"
        }
    }
}

function Assert-SnapshotHashes {
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$ExpectedHashes
    )

    foreach ($entry in $ExpectedHashes.GetEnumerator()) {
        if ((Get-Sha256 -Path $entry.Key) -cne $entry.Value) {
            throw "A snapshotted release input changed: $($entry.Key)"
        }
    }
}

function Remove-ArtifactSiblingFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $checkedPath = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($checkedPath)
    if (-not $parent.Equals(
            $artifactDirectory, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside the artifact directory: $checkedPath"
    }
    if (Test-Path -LiteralPath $checkedPath) {
        if (-not (Test-Path -LiteralPath $checkedPath -PathType Leaf)) {
            throw "Expected an artifact-directory file but found another item: $checkedPath"
        }
        [void](Assert-NoReparsePath -Path $checkedPath `
            -Description 'Artifact-directory cleanup')
        Remove-Item -LiteralPath $checkedPath -Force
    }
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
        throw "$Description must be a native x64 PE32+ image: $Path"
    }
}

function Assert-TestPolicyAttestation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$NormalPath,

        [Parameter(Mandatory = $true)]
        [string]$FinalPath
    )

    $normalMarker =
        'normal tests should prove current developer controls remain readable'
    $finalMarker =
        'SPATCH_FINAL_RELEASE should force task-dispatch and SMAA debug controls off'
    $normalText = [Text.Encoding]::ASCII.GetString(
        [IO.File]::ReadAllBytes($NormalPath))
    $finalText = [Text.Encoding]::ASCII.GetString(
        [IO.File]::ReadAllBytes($FinalPath))
    if ($normalText.IndexOf($normalMarker, [StringComparison]::Ordinal) -lt 0 -or
        $normalText.IndexOf($finalMarker, [StringComparison]::Ordinal) -ge 0) {
        throw 'Normal tests do not positively attest the normal compile-time policy.'
    }
    if ($finalText.IndexOf($finalMarker, [StringComparison]::Ordinal) -lt 0 -or
        $finalText.IndexOf($normalMarker, [StringComparison]::Ordinal) -ge 0) {
        throw ('Final-policy tests do not positively attest ' +
               'SPATCH_FINAL_RELEASE compile-time policy.')
    }
}

function Test-ModIniDesignContract {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Primary,

        [Parameter(Mandatory = $true)]
        [string]$ValidatorPath,

        [Parameter(Mandatory = $true)]
        [string]$DisplayPath
    )

    # Keep the shipped INI validator repository-owned so clean developers and
    # CI do not depend on a private Codex skill installation or ambient Python.
    if (-not (Test-Path -LiteralPath $ValidatorPath -PathType Leaf)) {
        throw "Repository INI-design validator is missing: $ValidatorPath"
    }

    $allowedSections = @(
        'Cutscenes'
        'Input'
        'Graphics'
        'TextureFiltering'
        'Shadows'
        'Display'
        'Gameplay'
        'Stability'
        'AntiAliasing'
    )
    $output = & $ValidatorPath `
        -Path $Path `
        -Primary $Primary `
        -AllowedSection $allowedSections
    $displayOutput = @($output | ForEach-Object {
        $_.Replace($Path, $DisplayPath)
    })
    Write-Host ($displayOutput -join [Environment]::NewLine)
}
function Get-PackageRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/') +
                  [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package file escaped its staging root: $fullPath"
    }
    return $fullPath.Substring($rootPrefix.Length).Replace('\', '/')
}

function Get-OrdinalSortedStrings([string[]] $Values) {
    $copy = [string[]]@($Values)
    [Array]::Sort($copy, [StringComparer]::Ordinal)
    return $copy
}

function Write-DeterministicArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $destinationFull = [IO.Path]::GetFullPath($Destination)
    Assert-NoReparseTree -Root $rootFull -Description 'Archive input tree'
    $rootPrefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if ($destinationFull.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Deterministic archive output must be outside its input tree.'
    }
    if (Test-Path -LiteralPath $destinationFull) {
        throw "Deterministic archive output already exists: $destinationFull"
    }
    $destinationParent = [IO.Path]::GetDirectoryName($destinationFull)
    if (-not (Test-Path -LiteralPath $destinationParent -PathType Container)) {
        throw "Deterministic archive output directory is missing: $destinationParent"
    }
    [void](Assert-NoReparsePath -Path $destinationParent `
        -Description 'Archive output directory')

    Add-Type -AssemblyName System.IO.Compression
    $filesByRelativePath =
        [Collections.Generic.Dictionary[string, string]]::new(
            [StringComparer]::Ordinal)
    foreach ($file in Get-ChildItem -LiteralPath $rootFull -File -Recurse -Force) {
        $relativePath = Get-PackageRelativePath `
            -Root $rootFull -Path $file.FullName
        if ($filesByRelativePath.ContainsKey($relativePath)) {
            throw "Archive input contains a duplicate path: $relativePath"
        }
        $filesByRelativePath.Add($relativePath, $file.FullName)
    }
    $relativePaths = @(Get-OrdinalSortedStrings @($filesByRelativePath.Keys))
    $fixedTimestamp = [DateTimeOffset]::new(
        1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
    $outputStream = [IO.File]::Open(
        $destinationFull,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $outputStream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($relativePath in $relativePaths) {
                $entry = $archive.CreateEntry(
                    $relativePath,
                    [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $fixedTimestamp
                $entry.ExternalAttributes = 0
                $sourceStream = [IO.File]::Open(
                    $filesByRelativePath[$relativePath],
                    [IO.FileMode]::Open,
                    [IO.FileAccess]::Read,
                    [IO.FileShare]::Read)
                try {
                    $entryStream = $entry.Open()
                    try {
                        $sourceStream.CopyTo($entryStream)
                    } finally {
                        $entryStream.Dispose()
                    }
                } finally {
                    $sourceStream.Dispose()
                }
            }
        } finally {
            $archive.Dispose()
        }
        $outputStream.Flush($true)
    } finally {
        $outputStream.Dispose()
    }
}

if ($PSCmdlet.ParameterSetName -ceq 'BuildLock') {
    $resolvedLockOwnerPid = Resolve-BuildLockOwnerPid `
        -ExplicitOwnerPid $LockOwnerPid
    Invoke-BuildLockAction -Action $BuildLockAction -Token $LockToken `
        -OwnerPid $resolvedLockOwnerPid
    return
}

if ($PSCmdlet.ParameterSetName -ceq 'ArchiveFixture') {
    $fixtureRoot = [IO.Path]::GetFullPath($ArchiveFixtureRoot)
    $fixtureOutput = [IO.Path]::GetFullPath($ArchiveFixtureOutputPath)
    Write-DeterministicArchive -Root $fixtureRoot -Destination $fixtureOutput
    Write-Output (Get-Sha256 -Path $fixtureOutput)
    return
}

$resolvedArtifact =
    Get-ResolvedLeafPath -Path $ArtifactPath -Description 'Final SPatch artifact'
$resolvedNormalTests =
    Get-ResolvedLeafPath -Path $NormalTestsPath -Description 'Normal test executable'
$resolvedFinalTests =
    Get-ResolvedLeafPath -Path $FinalTestsPath -Description 'Final-policy test executable'
$readmePath = Get-ResolvedLeafPath -Path (Join-Path $repoRoot 'README.md') `
    -Description 'SPatch README'
$configHeaderPath = Get-ResolvedLeafPath `
    -Path (Join-Path $repoRoot 'src\Config.h') `
    -Description 'SPatch configuration-version declaration'
$modIniDesignValidatorPath = Get-ResolvedLeafPath `
    -Path (Join-Path $scriptRoot 'Test-ModIniDesign.ps1') `
    -Description 'Repository INI-design validator'
$noticePath = Get-ResolvedLeafPath `
    -Path (Join-Path $noticeRoot 'THIRD_PARTY_NOTICES.md') `
    -Description 'Third-party notices'
$minHookLicense = Get-ResolvedLeafPath `
    -Path (Join-Path $noticeRoot 'licenses\MinHook-BSD-2-Clause.txt') `
    -Description 'MinHook license'
$smaaLicense = Get-ResolvedLeafPath `
    -Path (Join-Path $noticeRoot 'licenses\SMAA-MIT.txt') `
    -Description 'SMAA license'

$expectedArtifactPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\Release\SPatch.asi'))
if (-not $resolvedArtifact.Equals(
        $expectedArtifactPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The final artifact must be the canonical Release output: $resolvedArtifact"
}
$artifactDirectory = [IO.Path]::GetDirectoryName($resolvedArtifact)
$expectedNormalTests = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\tests\Release\false\SPatchTests.exe'))
$expectedFinalTests = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\tests\Release\true\SPatchTests.exe'))
if (-not $resolvedNormalTests.Equals(
        $expectedNormalTests, [StringComparison]::OrdinalIgnoreCase) -or
    -not $resolvedFinalTests.Equals(
        $expectedFinalTests, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Final publication requires the canonical isolated Release test outputs.'
}
$attestationRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\obj\Release\true'))
$attestationFull = [IO.Path]::GetFullPath($BuildAttestationPath)
$attestationPrefix = $attestationRoot.TrimEnd([char[]]'\/') +
                     [IO.Path]::DirectorySeparatorChar
$expectedAttestationName =
    "SPatch.final-build.$BuildInvocationId.attestation"
if (-not $attestationFull.StartsWith(
        $attestationPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($attestationFull) -cne $expectedAttestationName) {
    throw 'Final-build attestation is not the current invocation-owned path.'
}
Assert-BuildLockHeld -Token $BuildLockToken
$buildAttestation = Read-StrictKeyValueFile -Path $attestationFull `
    -ExpectedKeys @(
        'SPATCH_FINAL_BUILD', 'INVOCATION_ID', 'CONFIGURATION', 'PLATFORM',
        'FINAL_RELEASE', 'PROJECT', 'ARTIFACT', 'BUILD_LOCK_TOKEN') `
    -Description 'Final-build attestation'
if ($buildAttestation['SPATCH_FINAL_BUILD'] -cne '1' -or
    $buildAttestation['INVOCATION_ID'] -cne $BuildInvocationId -or
    $buildAttestation['CONFIGURATION'] -cne 'Release' -or
    $buildAttestation['PLATFORM'] -cne 'x64' -or
    $buildAttestation['FINAL_RELEASE'] -cne 'true' -or
    $buildAttestation['PROJECT'] -cne 'SPatch.vcxproj' -or
    $buildAttestation['ARTIFACT'] -cne 'build/Release/SPatch.asi' -or
    $buildAttestation['BUILD_LOCK_TOKEN'] -cne $BuildLockToken) {
    throw 'Final-build attestation does not describe this Release|x64 build.'
}
$buildAttestationHash = Get-Sha256 -Path $attestationFull
$receiptPath = Join-Path $artifactDirectory 'SPatch.final-release.sha256'
$temporaryReceipt =
    Join-Path $artifactDirectory ".SPatch.final-release-$serial.tmp"
$receiptBackup =
    Join-Path $artifactDirectory ".SPatch.final-release.sha256.previous-$serial"
if ($resolvedNormalTests -ieq $resolvedFinalTests) {
    throw 'Normal and SPATCH_FINAL_RELEASE tests must use isolated executables.'
}

$pdbPath = [IO.Path]::ChangeExtension($resolvedArtifact, '.pdb')
if (Test-Path -LiteralPath $pdbPath -PathType Leaf) {
    throw "Final release output still contains a PDB: $pdbPath"
}
$canonicalArtifactHash = Get-Sha256 -Path $resolvedArtifact

$pinnedPayloadHashes = [ordered]@{
    'THIRD_PARTY_NOTICES.md' =
        '9DEF04137AF8AC17BF3EA811594D3E9E9E98A5B88C96AAF09E750AB182F38386'
    'licenses/MinHook-BSD-2-Clause.txt' =
        '2EC244F9AC8ECD4E07E2121068011AB7FDA7B31338167DB7D0B6EFBE7A4994CB'
    'licenses/SMAA-MIT.txt' =
        '966E45E99329D827CB3C0D5C6040BE4930908A045CF6FB0C1432172D51DDFEB6'
}

[IO.Directory]::CreateDirectory($releaseRoot) | Out-Null
[void](Assert-NoReparsePath -Path $repoRoot -Description 'Repository root')
[void](Assert-NoReparsePath -Path $releaseRoot -Description 'Release root')
$publicationMutex = [Threading.Mutex]::new(
    $false, $publicationMutexName)
$ownsPublicationMutex = $false
$primaryError = $null
$cleanupErrors = [Collections.Generic.List[Exception]]::new()

try {
    try {
        $ownsPublicationMutex = $publicationMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $ownsPublicationMutex = $true
    }
    if (-not $ownsPublicationMutex) {
        throw 'Another SPatch base-release publication is already in progress.'
    }

    Remove-ReleaseDirectory -Path $stagingPackage
    Remove-ReleaseDirectory -Path $validationRoot
    Remove-ReleaseDirectory -Path $archiveValidationRoot
    Remove-ReleaseFile -Path $temporaryArchive
    Remove-ReleaseFile -Path $repeatArchive
    Remove-ArtifactSiblingFile -Path $temporaryReceipt
    if ((Test-Path -LiteralPath $packageBackup) -or
        (Test-Path -LiteralPath $archiveBackup) -or
        (Test-Path -LiteralPath $receiptBackup)) {
        throw 'A unique release-publication backup path already exists.'
    }
    if ((Test-Path -LiteralPath $receiptPath) -and
        -not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw "FinalRelease identity path is not a file: $receiptPath"
    }
    if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
        [void](Assert-NoReparsePath -Path $receiptPath `
            -Description 'Existing FinalRelease identity')
    }
    if (Test-Path -LiteralPath $activePackage -PathType Container) {
        Assert-NoReparseTree -Root $activePackage `
            -Description 'Existing active release package'
    }
    if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
        [void](Assert-NoReparsePath -Path $archivePath `
            -Description 'Existing active release archive')
    }

    [IO.Directory]::CreateDirectory($stagingPackage) | Out-Null
    [IO.Directory]::CreateDirectory(
        (Join-Path $stagingPackage 'licenses')) | Out-Null
    [IO.Directory]::CreateDirectory($validationRoot) | Out-Null

    $stagedArtifact = Join-Path $stagingPackage 'SPatch.asi'
    $stagedIni = Join-Path $stagingPackage 'SPatch.ini'
    $stagedReadme = Join-Path $stagingPackage 'README.md'
    $stagedNotice = Join-Path $stagingPackage 'THIRD_PARTY_NOTICES.md'
    $stagedMinHookLicense =
        Join-Path $stagingPackage 'licenses\MinHook-BSD-2-Clause.txt'
    $stagedSmaaLicense = Join-Path $stagingPackage 'licenses\SMAA-MIT.txt'
    $snapshottedConfigHeader = Join-Path $validationRoot 'Config.h'
    $snapshottedModIniDesignValidator =
        Join-Path $validationRoot 'Test-ModIniDesign.ps1'
    $snapshottedNormalTests = Join-Path $validationRoot 'SPatchTests.normal.exe'
    $snapshottedFinalTests = Join-Path $validationRoot 'SPatchTests.final.exe'
    Copy-FileSnapshot -Source $resolvedArtifact -Destination $stagedArtifact
    Copy-FileSnapshot -Source $readmePath -Destination $stagedReadme
    Copy-FileSnapshot -Source $noticePath -Destination $stagedNotice
    Copy-FileSnapshot -Source $minHookLicense -Destination $stagedMinHookLicense
    Copy-FileSnapshot -Source $smaaLicense -Destination $stagedSmaaLicense
    Copy-FileSnapshot `
        -Source $configHeaderPath -Destination $snapshottedConfigHeader
    Copy-FileSnapshot `
        -Source $modIniDesignValidatorPath `
        -Destination $snapshottedModIniDesignValidator
    Copy-FileSnapshot `
        -Source $resolvedNormalTests -Destination $snapshottedNormalTests
    Copy-FileSnapshot `
        -Source $resolvedFinalTests -Destination $snapshottedFinalTests

    $declaredConfigVersion =
        Get-DeclaredConfigVersion -Path $snapshottedConfigHeader
    $stagedDefaultFileName = "SPatch-default-v$declaredConfigVersion.ini"
    $stagedDefaultSource = Get-ResolvedLeafPath `
        -Path (Join-Path $repoRoot `
            (Join-Path 'artifacts\release-staging' $stagedDefaultFileName)) `
        -Description (
            "Staged configuration-v$declaredConfigVersion default INI contract")
    Copy-FileSnapshot -Source $stagedDefaultSource -Destination $stagedIni

    Assert-X64PeImage -Path $stagedArtifact -Description 'Staged SPatch artifact'
    Assert-X64PeImage `
        -Path $snapshottedNormalTests -Description 'Snapshotted normal test executable'
    Assert-X64PeImage `
        -Path $snapshottedFinalTests -Description 'Snapshotted final-policy test executable'
    Assert-TestPolicyAttestation `
        -NormalPath $snapshottedNormalTests -FinalPath $snapshottedFinalTests
    $stagedDefaultName = [IO.Path]::GetFileName($stagedDefaultSource)
    $stagedDefaultText = [IO.File]::ReadAllText($stagedIni)
    if ($stagedDefaultName -cne $stagedDefaultFileName -or
        $stagedDefaultText -notmatch (
            '(?m)^ConfigVersion=' +
            [regex]::Escape([string]$declaredConfigVersion) + '\r?$')) {
        throw ("Staged default INI filename and ConfigVersion content must " +
               "both be v$declaredConfigVersion.")
    }

    $snapshottedInputHashes = [ordered]@{}
    foreach ($path in @(
            $stagedArtifact,
            $stagedIni,
            $stagedReadme,
            $stagedNotice,
            $stagedMinHookLicense,
            $stagedSmaaLicense,
            $snapshottedConfigHeader,
            $snapshottedModIniDesignValidator,
            $snapshottedNormalTests,
            $snapshottedFinalTests)) {
        $snapshottedInputHashes[$path] = Get-Sha256 -Path $path
    }
    $snapshottedArtifactHash = $snapshottedInputHashes[$stagedArtifact]
    if ($snapshottedArtifactHash -cne $canonicalArtifactHash) {
        throw 'The canonical FinalRelease artifact changed while it was snapshotted.'
    }
    Assert-PinnedPayloadHashes `
        -Root $stagingPackage -ExpectedHashes $pinnedPayloadHashes
    Test-ModIniDesignContract `
        -Path $stagedIni `
        -Primary 'SPatch' `
        -ValidatorPath $snapshottedModIniDesignValidator `
        -DisplayPath $stagedDefaultSource

    $normalDefault = Join-Path $validationRoot 'SPatch.normal.ini'
    $finalDefault = Join-Path $validationRoot 'SPatch.final.ini'
    & $snapshottedNormalTests '--write-default-config' $normalDefault
    if ($LASTEXITCODE -ne 0) {
        throw "Normal tests failed to generate the default INI (exit $LASTEXITCODE)."
    }
    & $snapshottedFinalTests '--write-default-config' $finalDefault
    if ($LASTEXITCODE -ne 0) {
        throw "Final-policy tests failed to generate the default INI (exit $LASTEXITCODE)."
    }
    Assert-SnapshotHashes -ExpectedHashes $snapshottedInputHashes
    if (-not (Test-ByteIdentity -Left $normalDefault -Right $finalDefault)) {
        throw 'Normal and SPATCH_FINAL_RELEASE default INI bytes differ.'
    }
    $defaultText = [IO.File]::ReadAllText($finalDefault)
    $versionMatches = @([regex]::Matches(
        $defaultText,
        '(?m)^ConfigVersion=(?<version>[1-9][0-9]*)\r?$'))
    if ($versionMatches.Count -ne 1 -or
        $defaultText -notmatch '(?m)^WriteCrashDumps=1\r?$' -or
        $defaultText -notmatch 'WriteCrashDumps=1\z') {
        throw ('Generated release INI must contain exactly one positive ' +
               'ConfigVersion and end with the default WriteCrashDumps=1 contract.')
    }
    [int]$configVersion = 0
    if (-not [int]::TryParse(
            $versionMatches[0].Groups['version'].Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$configVersion)) {
        throw 'Generated release INI has an unsupported ConfigVersion.'
    }
    if ($configVersion -ne $declaredConfigVersion) {
        throw ("Generated configuration v$configVersion does not match " +
               "Config.h v$declaredConfigVersion; rebuild both test executables.")
    }
    if (-not (Test-ByteIdentity `
            -Left $finalDefault -Right $stagedIni)) {
        throw ("artifacts\release-staging\$stagedDefaultFileName drifted " +
               'from the freshly generated default INI bytes.')
    }

    Assert-X64PeImage -Path $stagedArtifact -Description 'Staged SPatch artifact'
    Assert-SnapshotHashes -ExpectedHashes $snapshottedInputHashes
    if ((Get-Sha256 -Path $stagedArtifact) -cne $snapshottedArtifactHash -or
        -not (Test-ByteIdentity -Left $finalDefault -Right $stagedIni)) {
        throw 'A staged first-party release file changed after validation.'
    }
    Assert-PinnedPayloadHashes `
        -Root $stagingPackage -ExpectedHashes $pinnedPayloadHashes

    $expectedPayload = @(Get-OrdinalSortedStrings @(
        'licenses/MinHook-BSD-2-Clause.txt'
        'licenses/SMAA-MIT.txt'
        'README.md'
        'SPatch.asi'
        'SPatch.ini'
        'THIRD_PARTY_NOTICES.md'
    ))
    $payloadFiles = @(Get-ChildItem -LiteralPath $stagingPackage -File -Recurse)
    $payloadPaths = @($payloadFiles | ForEach-Object {
        Get-PackageRelativePath -Root $stagingPackage -Path $_.FullName
    } | Sort-Object)
    if ($payloadPaths.Count -ne $expectedPayload.Count -or
        @(Compare-Object -CaseSensitive -ReferenceObject $expectedPayload `
            -DifferenceObject $payloadPaths).Count -ne 0) {
        throw 'Base release payload contains a missing, stale, or unexpected file.'
    }

    $manifestByPath =
        [Collections.Generic.Dictionary[string, string]]::new(
            [StringComparer]::Ordinal)
    foreach ($payloadFile in $payloadFiles) {
        $relativePath =
            Get-PackageRelativePath -Root $stagingPackage `
                -Path $payloadFile.FullName
        $hash = Get-Sha256 -Path $payloadFile.FullName
        $manifestByPath.Add($relativePath, "$hash *$relativePath")
    }
    $manifestLines = @(Get-OrdinalSortedStrings @($manifestByPath.Keys) |
        ForEach-Object { $manifestByPath[$_] })
    $manifestPath = Join-Path $stagingPackage 'SHA256SUMS.txt'
    [IO.File]::WriteAllLines(
        $manifestPath, $manifestLines, [Text.UTF8Encoding]::new($false))

    $manifestEntries = [ordered]@{}
    foreach ($line in [IO.File]::ReadAllLines($manifestPath)) {
        if ($line -notmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>.+)$') {
            throw "Malformed release manifest line: $line"
        }
        if ($manifestEntries.Contains($Matches['path'])) {
            throw "Duplicate release manifest path: $($Matches['path'])"
        }
        $manifestEntries[$Matches['path']] = $Matches['hash']
    }
    if ($manifestEntries.Count -ne $expectedPayload.Count) {
        throw 'Release manifest does not cover every non-manifest package file.'
    }
    foreach ($relativePath in $expectedPayload) {
        if (-not $manifestEntries.Contains($relativePath)) {
            throw "Release manifest is missing $relativePath."
        }
        $manifestedPath = Join-Path $stagingPackage $relativePath.Replace('/', '\')
        $actualHash = Get-Sha256 -Path $manifestedPath
        if ($actualHash -cne $manifestEntries[$relativePath]) {
            throw "Release manifest hash mismatch for $relativePath."
        }
    }

    $expectedPackage = @($expectedPayload + 'SHA256SUMS.txt') | Sort-Object
    $packagePaths = @(Get-ChildItem -LiteralPath $stagingPackage -File -Recurse |
        ForEach-Object {
            Get-PackageRelativePath -Root $stagingPackage -Path $_.FullName
        } | Sort-Object)
    if ($packagePaths.Count -ne $expectedPackage.Count -or
        @(Compare-Object -CaseSensitive -ReferenceObject $expectedPackage `
            -DifferenceObject $packagePaths).Count -ne 0) {
        throw 'Validated base package whitelist drifted.'
    }

    Write-DeterministicArchive `
        -Root $stagingPackage -Destination $temporaryArchive
    Write-DeterministicArchive `
        -Root $stagingPackage -Destination $repeatArchive
    if ((Get-Sha256 -Path $temporaryArchive) -cne
            (Get-Sha256 -Path $repeatArchive) -or
        -not (Test-ByteIdentity `
            -Left $temporaryArchive -Right $repeatArchive)) {
        throw ('Deterministic ZIP proof failed: identical staged inputs ' +
               'produced different archive bytes.')
    }
    Remove-ReleaseFile -Path $repeatArchive
    [IO.Directory]::CreateDirectory($archiveValidationRoot) | Out-Null
    Expand-Archive -LiteralPath $temporaryArchive `
        -DestinationPath $archiveValidationRoot
    $archivedPaths = @(Get-ChildItem -LiteralPath $archiveValidationRoot `
        -File -Recurse | ForEach-Object {
            Get-PackageRelativePath -Root $archiveValidationRoot -Path $_.FullName
        } | Sort-Object)
    if ($archivedPaths.Count -ne $expectedPackage.Count -or
        @(Compare-Object -CaseSensitive -ReferenceObject $expectedPackage `
            -DifferenceObject $archivedPaths).Count -ne 0) {
        throw 'Release archive whitelist differs from the validated package.'
    }
    foreach ($relativePath in $expectedPackage) {
        $stagedPath = Join-Path $stagingPackage $relativePath.Replace('/', '\')
        $archivedPath =
            Join-Path $archiveValidationRoot $relativePath.Replace('/', '\')
        if (-not (Test-ByteIdentity -Left $stagedPath -Right $archivedPath)) {
            throw "Release archive changed $relativePath."
        }
    }

    $expectedArchiveHash = Get-Sha256 -Path $temporaryArchive
    $expectedIniHash =
        Get-Sha256 -Path (Join-Path $stagingPackage 'SPatch.ini')
    $expectedArtifactHash =
        Get-Sha256 -Path (Join-Path $stagingPackage 'SPatch.asi')
    $expectedManifestHash =
        Get-Sha256 -Path (Join-Path $stagingPackage 'SHA256SUMS.txt')
    if ($expectedArtifactHash -cne $snapshottedArtifactHash) {
        throw 'The staged SPatch artifact changed after it was snapshotted.'
    }
    Assert-BuildLockHeld -Token $BuildLockToken
    if ((Get-Sha256 -Path $resolvedArtifact) -cne $canonicalArtifactHash -or
        (Get-Sha256 -Path $attestationFull) -cne $buildAttestationHash) {
        throw 'The current final build or its attestation changed before publication.'
    }

    $packageBackedUp = $false
    $archiveBackedUp = $false
    $receiptBackedUp = $false
    $receiptPromoted = $false
    $promotionStarted = $false
    try {
        if (Test-Path -LiteralPath $activePackage -PathType Container) {
            Assert-NoReparseTree -Root $activePackage `
                -Description 'Active release package before promotion'
            Move-Item -LiteralPath $activePackage -Destination $packageBackup
            $packageBackedUp = $true
        } elseif (Test-Path -LiteralPath $activePackage) {
            throw "Active package path is not a directory: $activePackage"
        }
        if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
            [void](Assert-NoReparsePath -Path $archivePath `
                -Description 'Active release archive before promotion')
            Move-Item -LiteralPath $archivePath -Destination $archiveBackup
            $archiveBackedUp = $true
        } elseif (Test-Path -LiteralPath $archivePath) {
            throw "Active archive path is not a file: $archivePath"
        }

        $promotionStarted = $true
        Move-Item -LiteralPath $stagingPackage -Destination $activePackage
        Move-Item -LiteralPath $temporaryArchive -Destination $archivePath
        if (-not (Test-Path -LiteralPath $activePackage -PathType Container) -or
            -not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
            throw 'Release publication did not create both active outputs.'
        }

        $promotedPaths = @(Get-ChildItem -LiteralPath $activePackage `
            -File -Recurse | ForEach-Object {
                Get-PackageRelativePath -Root $activePackage -Path $_.FullName
            } | Sort-Object)
        if ($promotedPaths.Count -ne $expectedPackage.Count -or
            @(Compare-Object -CaseSensitive -ReferenceObject $expectedPackage `
                -DifferenceObject $promotedPaths).Count -ne 0) {
            throw 'Promoted base package differs from the validated whitelist.'
        }

        $archiveHash = Get-Sha256 -Path $archivePath
        $iniHash = Get-Sha256 -Path (Join-Path $activePackage 'SPatch.ini')
        $artifactHash = Get-Sha256 -Path (Join-Path $activePackage 'SPatch.asi')
        $manifestHash =
            Get-Sha256 -Path (Join-Path $activePackage 'SHA256SUMS.txt')
        if ($archiveHash -cne $expectedArchiveHash -or
            $iniHash -cne $expectedIniHash -or
            $artifactHash -cne $expectedArtifactHash -or
            $manifestHash -cne $expectedManifestHash) {
            throw 'Promoted release outputs changed after validation.'
        }

        Assert-BuildLockHeld -Token $BuildLockToken
        if ((Get-Sha256 -Path $resolvedArtifact) -cne $artifactHash) {
            throw 'The canonical FinalRelease artifact changed during publication.'
        }
        $receiptLines = @(
            'SPATCH_FINAL_RELEASE=1'
            'CONFIGURATION=Release'
            'PLATFORM=x64'
            'PE_MACHINE=8664'
            'PE_OPTIONAL_MAGIC=020B'
            'FINAL_POLICY_ATTESTED=1'
            "SHA256=$artifactHash"
            'FILE=SPatch.asi'
            'TEST_MODES=normal,SPATCH_FINAL_RELEASE'
            'PACKAGE_DIR=SPatch-Base'
            'PACKAGE=SPatch-Base.zip'
            "PACKAGE_SHA256=$archiveHash"
            'PACKAGE_ASI=SPatch-Base/SPatch.asi'
            "PACKAGE_ASI_SHA256=$artifactHash"
            'DEFAULT_INI=SPatch-Base/SPatch.ini'
            "DEFAULT_INI_SHA256=$iniHash"
            'MANIFEST=SPatch-Base/SHA256SUMS.txt'
            "MANIFEST_SHA256=$manifestHash"
        )
        $receiptText = ($receiptLines -join [Environment]::NewLine) +
                       [Environment]::NewLine
        $asciiEncoding = [Text.ASCIIEncoding]::new()
        [IO.File]::WriteAllText($temporaryReceipt, $receiptText, $asciiEncoding)
        if ([IO.File]::ReadAllText($temporaryReceipt, $asciiEncoding) -cne
            $receiptText) {
            throw 'Staged FinalRelease identity bytes changed after writing.'
        }
        $expectedReceiptHash = Get-Sha256 -Path $temporaryReceipt

        if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
            [IO.File]::Replace(
                $temporaryReceipt, $receiptPath, $receiptBackup, $true)
            $receiptBackedUp = $true
        } elseif (Test-Path -LiteralPath $receiptPath) {
            throw "FinalRelease identity path is not a file: $receiptPath"
        } else {
            [IO.File]::Move($temporaryReceipt, $receiptPath)
        }
        $receiptPromoted = $true
        $promotedReceiptLines =
            [IO.File]::ReadAllLines($receiptPath, $asciiEncoding)
        if ($promotedReceiptLines.Count -ne $receiptLines.Count) {
            throw ('Promoted FinalRelease identity does not contain exactly ' +
                   "$($receiptLines.Count) lines.")
        }
        for ($index = 0; $index -lt $receiptLines.Count; ++$index) {
            if ($promotedReceiptLines[$index] -cne $receiptLines[$index]) {
                throw "Promoted FinalRelease identity line $($index + 1) changed."
            }
        }
        if ([IO.File]::ReadAllText($receiptPath, $asciiEncoding) -cne
                $receiptText -or
            (Get-Sha256 -Path $receiptPath) -cne $expectedReceiptHash) {
            throw 'Promoted FinalRelease identity changed after validation.'
        }
    } catch {
        $publicationError = $_
        $rollbackErrors = [Collections.Generic.List[string]]::new()
        if ($receiptPromoted) {
            try {
                if (Test-Path -LiteralPath $receiptPath) {
                    Remove-ArtifactSiblingFile -Path $receiptPath
                }
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($promotionStarted) {
            try {
                if (Test-Path -LiteralPath $activePackage) {
                    Remove-ReleaseDirectory -Path $activePackage
                }
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
            try {
                if (Test-Path -LiteralPath $archivePath) {
                    Remove-ReleaseFile -Path $archivePath
                }
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($packageBackedUp -and
            (Test-Path -LiteralPath $packageBackup -PathType Container)) {
            try {
                Move-Item -LiteralPath $packageBackup -Destination $activePackage
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($archiveBackedUp -and
            (Test-Path -LiteralPath $archiveBackup -PathType Leaf)) {
            try {
                Move-Item -LiteralPath $archiveBackup -Destination $archivePath
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($receiptBackedUp -and
            (Test-Path -LiteralPath $receiptBackup -PathType Leaf)) {
            try {
                Move-Item -LiteralPath $receiptBackup -Destination $receiptPath
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($rollbackErrors.Count -ne 0) {
            throw ("$($publicationError.Exception.Message) Rollback also failed: " +
                   ($rollbackErrors -join ' | '))
        }
        throw $publicationError
    }

    if ($packageBackedUp) {
        Remove-ReleaseDirectory -Path $packageBackup
    }
    if ($archiveBackedUp) {
        Remove-ReleaseFile -Path $archiveBackup
    }
    if ($receiptBackedUp) {
        Remove-ArtifactSiblingFile -Path $receiptBackup
    }
    if ((Test-Path -LiteralPath $packageBackup) -or
        (Test-Path -LiteralPath $archiveBackup) -or
        (Test-Path -LiteralPath $receiptBackup)) {
        throw 'Release publication left a previous-output backup behind.'
    }
} catch {
    $primaryError = $_
} finally {
    $cleanupActions = @(
        @{ Name = 'staging package'; Action = {
                if (Test-Path -LiteralPath $stagingPackage) {
                    Remove-ReleaseDirectory -Path $stagingPackage
                }
            } }
        @{ Name = 'validation directory'; Action = {
                if (Test-Path -LiteralPath $validationRoot) {
                    Remove-ReleaseDirectory -Path $validationRoot
                }
            } }
        @{ Name = 'archive-validation directory'; Action = {
                if (Test-Path -LiteralPath $archiveValidationRoot) {
                    Remove-ReleaseDirectory -Path $archiveValidationRoot
                }
            } }
        @{ Name = 'temporary archive'; Action = {
                if (Test-Path -LiteralPath $temporaryArchive) {
                    Remove-ReleaseFile -Path $temporaryArchive
                }
            } }
        @{ Name = 'repeat archive'; Action = {
                if (Test-Path -LiteralPath $repeatArchive) {
                    Remove-ReleaseFile -Path $repeatArchive
                }
            } }
        @{ Name = 'temporary receipt'; Action = {
                if (Test-Path -LiteralPath $temporaryReceipt) {
                    Remove-ArtifactSiblingFile -Path $temporaryReceipt
                }
            } }
        @{ Name = 'one-time build attestation'; Action = {
                if (Test-Path -LiteralPath $attestationFull) {
                    [void](Assert-NoReparsePath -Path $attestationFull `
                        -Description 'Final-build attestation cleanup')
                    Remove-Item -LiteralPath $attestationFull -Force
                }
            } }
    )
    foreach ($cleanup in $cleanupActions) {
        try {
            & $cleanup.Action
        } catch {
            $cleanupErrors.Add([InvalidOperationException]::new(
                "Cleanup of $($cleanup.Name) failed: $($_.Exception.Message)",
                $_.Exception))
        }
    }
    try {
        $leftovers = @(Get-ChildItem -LiteralPath $releaseRoot -Force |
            Where-Object {
                $_.Name -match '^\.SPatch-Base\.(staging|validation|archive-validation|previous)-' -or
                $_.Name -match '^\.SPatch-Base\.zip\.previous-' -or
                $_.Name -match '^\.SPatch-Base-[0-9]+-.+\.zip$'
            })
        if ($leftovers.Count -ne 0) {
            throw ('Release publication left transient paths behind: ' +
                   (($leftovers | Select-Object -ExpandProperty Name) -join ', '))
        }
        $receiptLeftovers = @(
            Get-ChildItem -LiteralPath $artifactDirectory -Force |
                Where-Object {
                    $_.Name -match '^\.SPatch\.final-release-[0-9]+-[0-9a-f]+\.tmp$' -or
                    $_.Name -match '^\.SPatch\.final-release\.sha256\.previous-[0-9]+-[0-9a-f]+$'
                })
        if ($receiptLeftovers.Count -ne 0) {
            throw ('Release publication left transient receipt paths behind: ' +
                   (($receiptLeftovers |
                        Select-Object -ExpandProperty Name) -join ', '))
        }
    } catch {
        $cleanupErrors.Add($_.Exception)
    }
    if ($ownsPublicationMutex) {
        try {
            $publicationMutex.ReleaseMutex()
        } catch {
            $cleanupErrors.Add($_.Exception)
        }
    }
    try {
        $publicationMutex.Dispose()
    } catch {
        $cleanupErrors.Add($_.Exception)
    }
}

if ($null -ne $primaryError) {
    if ($cleanupErrors.Count -eq 0) {
        throw $primaryError
    }
    $failures = [Collections.Generic.List[Exception]]::new()
    $failures.Add($primaryError.Exception)
    foreach ($cleanupError in $cleanupErrors) {
        $failures.Add($cleanupError)
    }
    throw [AggregateException]::new(
        "Release publication failed: $($primaryError.Exception.Message)",
        $failures)
}
if ($cleanupErrors.Count -ne 0) {
    throw [AggregateException]::new(
        'Release publication succeeded but cleanup failed.', $cleanupErrors)
}

Write-Host "Validated base release package: $activePackage"
Write-Host "Validated configuration-v$configVersion default INI SHA-256: $iniHash"
Write-Host "Validated base release archive SHA-256: $archiveHash"
