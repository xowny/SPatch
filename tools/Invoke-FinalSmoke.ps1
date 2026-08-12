[CmdletBinding()]
param(
    [string] $RepoRoot = '',
    [string] $GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\SleepingDogsDefinitiveEdition',
    [ValidateRange(640, 16384)]
    [int] $ExpectedWidth = 3840,
    [ValidateRange(480, 16384)]
    [int] $ExpectedHeight = 2160,
    [ValidatePattern('^[^\\/:*?"<>|]+\.dll$')]
    [string] $AsiLoaderName = 'dinput8.dll',
    [switch] $ValidateOnly
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
$builtAsi = Join-Path $RepoRoot 'build\Release\SPatch.asi'
$finalReleaseIdentity = Join-Path $RepoRoot 'build\Release\SPatch.final-release.sha256'
$sourceConfigHeader = Join-Path $RepoRoot 'src\Config.h'
$basePackageRoot = Join-Path $RepoRoot 'artifacts\release\SPatch-Base'
$basePackageIni = Join-Path $basePackageRoot 'SPatch.ini'
$basePackageManifest = Join-Path $basePackageRoot 'SHA256SUMS.txt'
$basePackageArchive = Join-Path $RepoRoot 'artifacts\release\SPatch-Base.zip'
$packageRoot = Join-Path $RepoRoot `
    'artifacts\shenlong\Publishing-Release\ShenLong-Package'
$packageManifest = Join-Path $packageRoot 'SHA256SUMS.txt'
$packageArchive = Join-Path $RepoRoot 'artifacts\shenlong\ShenLong.zip'
$installedAsi = Join-Path $GameRoot 'SPatch.asi'
$installedAddon = Join-Path $GameRoot 'ShenLong.asi'
$installedDxgi = Join-Path $GameRoot 'dxgi.dll'
$installedManifest = Join-Path $GameRoot 'ShenLong-SHA256SUMS.txt'
$baseIni = Join-Path $GameRoot 'SPatch.ini'
$ini = Join-Path $GameRoot 'ShenLong.ini'
$previousIni = Join-Path $GameRoot 'SPatch.ini.previous.bak'
$reshadeIni = Join-Path $GameRoot 'ReShade.ini'
$spatchLog = Join-Path $GameRoot 'SPatch.log'
$reshadeLog = Join-Path $GameRoot 'ReShade.log'
$gameExe = Join-Path $GameRoot 'sdhdship.exe'
$benchmarkShortcut = $script:SPatchBenchmarkShortcutPath
$asiLoader = Join-Path $GameRoot $AsiLoaderName
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$writeUtf8 = [Text.UTF8Encoding]::new($false)
$maximumLogBytes = 64MB
$script:ownedSmokeProcesses = [Collections.Generic.Dictionary[int, datetime]]::new()

function Get-Hash([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Assert-FileByteIdentity(
    [string] $Source,
    [string] $Published,
    [string] $Label) {
    foreach ($path in @($Source, $Published)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "$Label identity prerequisite is missing: $path"
        }
    }
    $sourceInfo = Get-Item -LiteralPath $Source
    $publishedInfo = Get-Item -LiteralPath $Published
    if ($sourceInfo.Length -ne $publishedInfo.Length -or
        (Get-Hash $Source) -cne (Get-Hash $Published)) {
        throw "$Label changed after its release package was published."
    }
}

function Get-ByteHash(
    [byte[]] $Bytes,
    [int] $Offset = 0,
    [int] $Count = -1) {
    if ($Count -lt 0) {
        $Count = $Bytes.Length - $Offset
    }
    if ($Offset -lt 0 -or $Count -lt 0 -or $Offset + $Count -gt $Bytes.Length) {
        throw 'Invalid byte range requested for hashing.'
    }
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha256.ComputeHash($Bytes, $Offset, $Count)
        return ([BitConverter]::ToString($hash)).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
}

function Read-SharedFileBytes([string] $Path) {
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $length = $stream.Length
        if ($length -lt 0 -or $length -gt $maximumLogBytes) {
            throw "Refusing to read an unexpectedly large smoke log: $Path ($length bytes)"
        }
        $bytes = [byte[]]::new([int] $length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $read = $stream.Read($bytes, $offset, $bytes.Length - $offset)
            if ($read -le 0) {
                throw "Smoke log was truncated while being read: $Path"
            }
            $offset += $read
        }
        return ,$bytes
    } finally {
        $stream.Dispose()
    }
}

function New-LogSnapshot([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{
            Exists = $false
            Bytes = [byte[]]::new(0)
            Hash = ''
        }
    }
    [byte[]] $bytes = Read-SharedFileBytes $Path
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
        throw "$Label was not created by the current smoke arm: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    if ($item.LastWriteTimeUtc -lt $ProcessStartUtc.AddSeconds(-2)) {
        throw "$Label was not written by the current process session."
    }

    [byte[]] $afterBytes = Read-SharedFileBytes $Path
    if ($afterBytes.Length -eq 0) {
        throw "$Label is empty after the current process session."
    }

    $oldIsExactPrefix = $false
    if ($Before.Exists -and $afterBytes.Length -ge $Before.Bytes.Length) {
        $afterPrefixHash = Get-ByteHash $afterBytes 0 $Before.Bytes.Length
        $oldIsExactPrefix = $afterPrefixHash -ceq $Before.Hash
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
        # ReShade normally truncates its log on startup, while SPatch may rotate
        # at its size limit. In either case the replacement file is the complete
        # current session rather than an append to the previous bytes.
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

function Test-PathsEqual([string] $Left, [string] $Right) {
    if ([string]::IsNullOrWhiteSpace($Left) -or
        [string]::IsNullOrWhiteSpace($Right)) {
        return $false
    }
    try {
        return [IO.Path]::GetFullPath($Left).TrimEnd([char[]]'\/').Equals(
            [IO.Path]::GetFullPath($Right).TrimEnd([char[]]'\/'),
            [StringComparison]::OrdinalIgnoreCase)
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

function Stop-TaskStartedSteamProcesses([datetime] $RunStart) {
    $success = $true
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
                Write-Warning (
                    "Task-started Steam cleanup failed for PID $($steamProcess.Id): $($_.Exception.Message)")
                $success = $false
            }
        }
    }
    return $success
}

function Test-TaskOwnedSmokeProcess(
    [Diagnostics.Process] $Process,
    [datetime] $SmokeStart,
    [string] $ExpectedPath) {
    try {
        if (-not $script:ownedSmokeProcesses.ContainsKey($Process.Id)) {
            return $false
        }
        return -not $Process.HasExited -and
            $Process.StartTime -eq $script:ownedSmokeProcesses[$Process.Id] -and
            $Process.StartTime -ge $SmokeStart -and
            (Test-PathsEqual $Process.Path $ExpectedPath)
    } catch {
        # A process that exited while it was being enumerated needs no cleanup.
        return $false
    }
}

function Register-TaskOwnedSmokeProcess(
    [Diagnostics.Process] $Process,
    [datetime] $SmokeStart,
    [string] $ExpectedPath) {
    $Process.Refresh()
    if ($Process.HasExited -or
        $Process.StartTime -lt $SmokeStart.AddSeconds(-2) -or
        -not (Test-PathsEqual $Process.Path $ExpectedPath)) {
        throw 'The smoke launcher did not return the exact task-owned game process.'
    }
    if ($script:ownedSmokeProcesses.ContainsKey($Process.Id) -and
        $script:ownedSmokeProcesses[$Process.Id] -ne $Process.StartTime) {
        throw "The smoke process PID was reused before registration: $($Process.Id)"
    }
    $script:ownedSmokeProcesses[$Process.Id] = $Process.StartTime
}

function Stop-TaskOwnedSmokeProcess(
    [Diagnostics.Process] $Process,
    [datetime] $SmokeStart,
    [string] $ExpectedPath) {
    if (-not $Process) {
        return $true
    }

    $processId = $Process.Id
    if (-not $script:ownedSmokeProcesses.ContainsKey($processId)) {
        return $false
    }
    try {
        $originalStartTime = $Process.StartTime
    } catch {
        return $true
    }
    if ($originalStartTime -ne $script:ownedSmokeProcesses[$processId]) {
        return $false
    }

    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        $live = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if (-not $live) {
            return $true
        }
        try {
            # Never terminate a different process if Windows reuses the PID
            # while cleanup is polling.
            if ($live.StartTime -ne $originalStartTime -or
                $live.StartTime -lt $SmokeStart -or
                -not (Test-PathsEqual $live.Path $ExpectedPath)) {
                return $false
            }
            Stop-Process -InputObject $live -Force -ErrorAction SilentlyContinue
            [void] $live.WaitForExit(2000)
        } catch {
            if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) {
                return $true
            }
        }
        Start-Sleep -Milliseconds 100
    }

    $remaining = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $remaining) {
        return $true
    }
    try {
        if ($remaining.StartTime -ne $originalStartTime -or
            -not (Test-PathsEqual $remaining.Path $ExpectedPath)) {
            return $false
        }
        return $remaining.HasExited
    } catch {
        return $true
    }
}

function Stop-TaskOwnedSmokeProcesses(
    [datetime] $SmokeStart,
    [string] $ExpectedPath) {
    $deadline = (Get-Date).AddSeconds(15)
    $quietSince = $null
    while ((Get-Date) -lt $deadline) {
        $processes = @(Get-Process -Name sdhdship -ErrorAction SilentlyContinue)
        foreach ($process in $processes) {
            if (-not (Test-TaskOwnedSmokeProcess $process $SmokeStart $ExpectedPath)) {
                $live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
                if (-not $live) {
                    continue
                }
                # A live process not explicitly registered from this task's
                # launcher is never terminated, even if it uses the same path.
                return $false
            }
            [void] (Stop-TaskOwnedSmokeProcess $process $SmokeStart $ExpectedPath)
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

function Get-CheckedChildPath(
    [string] $Root,
    [string] $RelativePath,
    [string] $Label) {
    $relative = $RelativePath.Replace('/', '\')
    if ([string]::IsNullOrWhiteSpace($relative) -or
        [IO.Path]::IsPathRooted($relative) -or
        @($relative -split '\\' | Where-Object {
                $_ -ceq '..' -or $_ -ceq '.' -or
                [string]::IsNullOrWhiteSpace($_)
            }).Count -ne 0) {
        throw "$Label contains an unsafe relative path: $RelativePath"
    }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $rootPrefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath((Join-Path $rootFull $relative))
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label path escapes its root: $RelativePath"
    }
    return $fullPath
}

function Assert-X64PeFile([string] $Path, [string] $Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($stream.Length -lt 0x40) {
            throw "$Label is too small to be a PE file: $Path"
        }
        $reader = [IO.BinaryReader]::new($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw "$Label has no DOS MZ header: $Path"
            }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            if ($peOffset -lt 0x40 -or $peOffset + 6 -gt $stream.Length) {
                throw "$Label has an invalid PE header offset: $Path"
            }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) {
                throw "$Label has no PE signature: $Path"
            }
            $machine = $reader.ReadUInt16()
            if ($machine -ne 0x8664) {
                throw ("$Label is not x64 (machine=0x{0:X4}): {1}" -f $machine, $Path)
            }
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-GraphicsManifestEntries {
    if (-not (Test-Path -LiteralPath $packageManifest -PathType Leaf)) {
        throw 'Publishing-Release SHA256SUMS.txt is missing.'
    }
    $entries = @()
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($packageManifest)) {
        $match = [regex]::Match($line, '^([0-9A-F]{64}) \*(.+)$')
        if (-not $match.Success) {
            throw "Malformed graphics manifest line: $line"
        }
        $relative = $match.Groups[2].Value.Replace('/', '\')
        if (-not $seen.Add($relative)) {
            throw "Duplicate graphics manifest path: $relative"
        }
        if ($relative -ieq 'SHA256SUMS.txt') {
            throw 'The graphics manifest may not contain itself.'
        }
        [void] (Get-CheckedChildPath $packageRoot $relative 'Graphics manifest')
        $entries += [pscustomobject]@{
            ExpectedHash = $match.Groups[1].Value
            RelativePath = $relative
        }
    }
    if ($entries.Count -eq 0) {
        throw 'Publishing-Release graphics manifest is empty.'
    }

    foreach ($required in @(
            'ShenLong.asi',
            'ShenLong.ini',
            'dxgi.dll',
            'ReShade.ini',
            'SHENLONG-README.md',
            'Install-ShenLong.ps1',
            'THIRD_PARTY_NOTICES.md',
            'ShenLong\ShaderCache\v1\manifest.tsv')) {
        if (-not $seen.Contains($required)) {
            throw "Publishing-Release manifest omits required file: $required"
        }
    }
    return $entries
}

function Get-TreeRelativeFiles([string] $Root) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    return @(
        Get-ChildItem -LiteralPath $rootFull -File -Recurse | ForEach-Object {
            $fullPath = [IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Enumerated file escaped package root: $fullPath"
            }
            $fullPath.Substring($prefix.Length).Replace('/', '\')
        })
}

function Assert-PackageTree([object[]] $Entries) {
    $expected = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $Entries) {
        [void] $expected.Add($entry.RelativePath)
    }
    [void] $expected.Add('SHA256SUMS.txt')

    $actual = @(Get-TreeRelativeFiles $packageRoot)
    if ($actual.Count -ne $expected.Count) {
        throw "Publishing-Release tree is not exact: expected $($expected.Count) files, found $($actual.Count)."
    }
    foreach ($relative in $actual) {
        if (-not $expected.Contains($relative)) {
            throw "Publishing-Release contains an unmanifested file: $relative"
        }
    }

    foreach ($entry in $Entries) {
        $path = Get-CheckedChildPath $packageRoot $entry.RelativePath 'Packaged graphics'
        if ((Get-Hash $path) -cne $entry.ExpectedHash) {
            throw "Packaged graphics hash mismatch: $($entry.RelativePath)"
        }
    }
}

function Assert-ShenLongPackageArchive {
    if (-not (Test-Path -LiteralPath $packageArchive -PathType Leaf)) {
        throw "ShenLong package archive is missing: $packageArchive"
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $expectedFiles = @(Get-TreeRelativeFiles $packageRoot)
    $expected = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($expectedFile in $expectedFiles) {
        [void] $expected.Add([string] $expectedFile)
    }
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $archive = [IO.Compression.ZipFile]::OpenRead($packageArchive)
    try {
        foreach ($entry in $archive.Entries) {
            $entryName = $entry.FullName
            if ([string]::IsNullOrWhiteSpace($entry.Name) -or
                -not $entryName.StartsWith(
                    'ShenLong-Package/', [StringComparison]::Ordinal) -or
                $entryName.Contains('\')) {
                throw "ShenLong archive entry is outside the required file-only envelope: $entryName"
            }
            $relativeArchivePath = $entryName.Substring(
                'ShenLong-Package/'.Length)
            $components = @($relativeArchivePath -split '/')
            if ($components.Count -eq 0 -or
                @($components | Where-Object {
                        [string]::IsNullOrWhiteSpace($_) -or
                        $_ -ceq '.' -or $_ -ceq '..' -or $_.Contains(':')
                    }).Count -ne 0) {
                throw "ShenLong archive entry has an unsafe path: $entryName"
            }
            $relative = $relativeArchivePath.Replace('/', '\')
            if (-not $seen.Add($relative)) {
                throw "ShenLong archive contains a duplicate Windows path: $entryName"
            }
            if (-not $expected.Contains($relative)) {
                throw "ShenLong archive contains a file absent from the published package: $entryName"
            }
            $publishedPath = Get-CheckedChildPath `
                $packageRoot $relative 'Published ShenLong archive identity'
            if ($entry.Length -ne (Get-Item -LiteralPath $publishedPath).Length) {
                throw "ShenLong archive entry length mismatch: $entryName"
            }
            $entryStream = $entry.Open()
            $sha256 = [Security.Cryptography.SHA256]::Create()
            try {
                $entryHash = ([BitConverter]::ToString(
                    $sha256.ComputeHash($entryStream))).Replace('-', '')
            } finally {
                $sha256.Dispose()
                $entryStream.Dispose()
            }
            if ($entryHash -cne (Get-Hash $publishedPath)) {
                throw "ShenLong archive entry bytes differ from the published package: $entryName"
            }
        }
    } finally {
        $archive.Dispose()
    }
    if ($seen.Count -ne $expected.Count) {
        throw ("ShenLong archive content is incomplete: expected " +
               "$($expected.Count) files, found $($seen.Count).")
    }
    foreach ($relative in $expected) {
        if (-not $seen.Contains($relative)) {
            throw "ShenLong archive omits published package file: $relative"
        }
    }
}

function Assert-BaseReleasePackage {
    foreach ($path in @(
            $basePackageRoot,
            $basePackageIni,
            $basePackageManifest,
            $basePackageArchive)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Base-release prerequisite is missing: $path"
        }
    }
    if (-not (Test-Path -LiteralPath $basePackageRoot -PathType Container)) {
        throw "Base-release package root is not a directory: $basePackageRoot"
    }
    foreach ($path in @(
            $basePackageIni,
            $basePackageManifest,
            $basePackageArchive)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Base-release prerequisite is not a file: $path"
        }
    }

    $expectedFiles = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in @(
            'SPatch.asi',
            'SPatch.ini',
            'README.md',
            'THIRD_PARTY_NOTICES.md',
            'licenses\MinHook-BSD-2-Clause.txt',
            'licenses\SMAA-MIT.txt')) {
        [void] $expectedFiles.Add($relative)
    }

    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines(
            $basePackageManifest, $strictUtf8)) {
        $match = [regex]::Match($line, '^([0-9A-F]{64}) \*(.+)$')
        if (-not $match.Success) {
            throw "Malformed base-release manifest line: $line"
        }
        $relative = $match.Groups[2].Value.Replace('/', '\')
        if (-not $seen.Add($relative)) {
            throw "Duplicate base-release manifest path: $relative"
        }
        if (-not $expectedFiles.Contains($relative)) {
            throw "Base-release manifest contains an unexpected path: $relative"
        }
        $path = Get-CheckedChildPath `
            $basePackageRoot $relative 'Base-release manifest'
        if ((Get-Hash $path) -cne $match.Groups[1].Value) {
            throw "Base-release hash mismatch: $relative"
        }
    }
    if ($seen.Count -ne $expectedFiles.Count) {
        throw ("Base-release manifest is incomplete: expected " +
               "$($expectedFiles.Count) files, found $($seen.Count).")
    }
    foreach ($relative in $expectedFiles) {
        if (-not $seen.Contains($relative)) {
            throw "Base-release manifest omits required file: $relative"
        }
    }

    $expectedTree = [Collections.Generic.HashSet[string]]::new(
        $expectedFiles, [StringComparer]::OrdinalIgnoreCase)
    [void] $expectedTree.Add('SHA256SUMS.txt')
    $actualTree = @(Get-TreeRelativeFiles $basePackageRoot)
    if ($actualTree.Count -ne $expectedTree.Count) {
        throw ("Base-release tree is not exact: expected $($expectedTree.Count) " +
               "files, found $($actualTree.Count).")
    }
    foreach ($relative in $actualTree) {
        if (-not $expectedTree.Contains($relative)) {
            throw "Base-release tree contains an unexpected file: $relative"
        }
    }
    if ((Get-Hash (Join-Path $basePackageRoot 'SPatch.asi')) -cne
        (Get-Hash $builtAsi)) {
        throw 'The base-release SPatch.asi does not match the canonical build.'
    }
    foreach ($identity in @(
            @((Join-Path $RepoRoot 'README.md'),
              (Join-Path $basePackageRoot 'README.md'), 'Base README'),
            @((Join-Path $RepoRoot 'artifacts\release-notices\THIRD_PARTY_NOTICES.md'),
              (Join-Path $basePackageRoot 'THIRD_PARTY_NOTICES.md'),
              'Base third-party notices'),
            @((Join-Path $RepoRoot 'artifacts\release-notices\licenses\MinHook-BSD-2-Clause.txt'),
              (Join-Path $basePackageRoot 'licenses\MinHook-BSD-2-Clause.txt'),
              'Base MinHook license'),
            @((Join-Path $RepoRoot 'artifacts\release-notices\licenses\SMAA-MIT.txt'),
              (Join-Path $basePackageRoot 'licenses\SMAA-MIT.txt'),
              'Base SMAA license'))) {
        Assert-FileByteIdentity $identity[0] $identity[1] $identity[2]
    }

    $archiveValidationRoot = Join-Path `
        (Split-Path -Parent $basePackageArchive) `
        ('.final-smoke-archive-{0}-{1}' -f
            $PID, [Guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($archiveValidationRoot) | Out-Null
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($basePackageArchive)
        try {
            $archivePaths = [Collections.Generic.HashSet[string]]::new(
                [StringComparer]::OrdinalIgnoreCase)
            foreach ($entry in $archive.Entries) {
                $relative = $entry.FullName.Replace('/', '\')
                if ([string]::IsNullOrWhiteSpace($entry.Name) -or
                    -not $archivePaths.Add($relative)) {
                    throw "Base archive contains a directory or duplicate path: $($entry.FullName)"
                }
                $destination = Get-CheckedChildPath `
                    $archiveValidationRoot $relative 'Base archive entry'
                [IO.Directory]::CreateDirectory(
                    [IO.Path]::GetDirectoryName($destination)) | Out-Null
                $inputStream = $entry.Open()
                $outputStream = [IO.File]::Open(
                    $destination,
                    [IO.FileMode]::CreateNew,
                    [IO.FileAccess]::Write,
                    [IO.FileShare]::None)
                try {
                    $inputStream.CopyTo($outputStream)
                } finally {
                    $outputStream.Dispose()
                    $inputStream.Dispose()
                }
            }
        } finally {
            $archive.Dispose()
        }

        $archiveTree = @(Get-TreeRelativeFiles $archiveValidationRoot)
        if ($archiveTree.Count -ne $actualTree.Count) {
            throw ("Base archive tree differs from the active package: expected " +
                   "$($actualTree.Count) files, found $($archiveTree.Count).")
        }
        foreach ($relative in $actualTree) {
            $archived = Get-CheckedChildPath `
                $archiveValidationRoot $relative 'Extracted base archive'
            $published = Get-CheckedChildPath `
                $basePackageRoot $relative 'Active base package'
            if (-not (Test-Path -LiteralPath $archived -PathType Leaf) -or
                (Get-Hash $archived) -cne (Get-Hash $published)) {
                throw "Base archive bytes differ from the active package: $relative"
            }
        }
    } finally {
        if (Test-Path -LiteralPath $archiveValidationRoot) {
            Remove-Item -LiteralPath $archiveValidationRoot -Recurse -Force
        }
    }
}

function Assert-PrecompiledShaderCachePackage([object[]] $Entries) {
    $packagedSources = @(
        $Entries | Where-Object {
            [IO.Path]::GetExtension($_.RelativePath) -in @(
                '.hlsl', '.hlsli', '.fx')
        })
    if ($packagedSources.Count -ne 0) {
        throw "Publishing-Release contains runtime shader source: $($packagedSources.RelativePath -join ', ')"
    }

    $cacheManifestRelative = 'ShenLong\ShaderCache\v1\manifest.tsv'
    $cacheManifestEntries = @(
        $Entries | Where-Object {
            $_.RelativePath -ieq $cacheManifestRelative
        })
    if ($cacheManifestEntries.Count -ne 1) {
        throw "Expected one precompiled shader-cache manifest, found $($cacheManifestEntries.Count)."
    }
    $cacheManifestPath = Get-CheckedChildPath `
        $packageRoot $cacheManifestRelative 'Shader-cache manifest'
    $lines = [IO.File]::ReadAllLines($cacheManifestPath)
    $expectedHeader = "Configuration`tSSSDevelopment`tSHA256`tBytes`tPath`tSource`tEntryPoint`tProfile`tDefines"
    if ($lines.Count -lt 2 -or $lines[0] -cne $expectedHeader) {
        throw 'The precompiled shader-cache manifest header is invalid.'
    }

    $outerEntries = @{}
    foreach ($entry in $Entries) {
        $outerEntries[$entry.RelativePath] = $entry
    }
    $seenCachePaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $featureCounts = @{
        PBR = 0
        GI = 0
        SDAO = 0
        SSS = 0
        Water = 0
    }
    for ($index = 1; $index -lt $lines.Count; ++$index) {
        $fields = $lines[$index].Split([char] 9)
        if ($fields.Count -ne 9 -or
            $fields[0] -cne 'Publishing-Release' -or
            $fields[1] -cne '0' -or
            $fields[2] -cnotmatch '^[0-9A-F]{64}$') {
            throw "Malformed precompiled shader-cache manifest row $($index + 1)."
        }
        [long] $byteLength = 0
        if (-not [long]::TryParse(
                $fields[3],
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref] $byteLength) -or
            $byteLength -le 0) {
            throw "Invalid shader byte length at cache-manifest row $($index + 1)."
        }

        $cacheRelative = $fields[4].Replace('/', '\')
        if (-not $seenCachePaths.Add($cacheRelative)) {
            throw "Duplicate precompiled shader-cache path: $cacheRelative"
        }
        $feature = $cacheRelative.Split([char] '\')[0]
        if (-not $featureCounts.ContainsKey($feature) -or
            [IO.Path]::GetExtension($cacheRelative) -cne '.cso') {
            throw "Unexpected precompiled shader-cache path: $cacheRelative"
        }
        ++$featureCounts[$feature]

        $outerRelative = "ShenLong\ShaderCache\v1\$cacheRelative"
        if (-not $outerEntries.ContainsKey($outerRelative)) {
            throw "Outer Publishing manifest omits cached shader: $outerRelative"
        }
        $outerEntry = $outerEntries[$outerRelative]
        if ($outerEntry.ExpectedHash -cne $fields[2]) {
            throw "Nested/outer shader-cache hash mismatch: $cacheRelative"
        }
        $cachedShaderPath = Get-CheckedChildPath `
            $packageRoot $outerRelative 'Precompiled shader cache'
        if ((Get-Item -LiteralPath $cachedShaderPath).Length -ne $byteLength) {
            throw "Precompiled shader-cache byte length mismatch: $cacheRelative"
        }
        if ((Get-Hash $cachedShaderPath) -cne $fields[2]) {
            throw "Precompiled shader-cache file hash mismatch: $cacheRelative"
        }
    }

    $expectedFeatureCounts = @{
        PBR = 18
        GI = 36
        SDAO = 26
        SSS = 11
        Water = 3
    }
    foreach ($feature in $expectedFeatureCounts.Keys) {
        if ($featureCounts[$feature] -ne $expectedFeatureCounts[$feature]) {
            throw ("Publishing-Release {0} cache is incomplete: expected {1}, found {2}." -f
                $feature,
                $expectedFeatureCounts[$feature],
                $featureCounts[$feature])
        }
    }
    foreach ($quality in 0..4) {
        $gtaoLiteCachePath =
            "SDAO\main_pass_cs.cs_5_0.q$quality.gtaolite1.cso"
        if (-not $seenCachePaths.Contains($gtaoLiteCachePath)) {
            throw "Publishing-Release cache omits GTAO Lite quality ${quality}: $gtaoLiteCachePath"
        }
    }
    if ($seenCachePaths.Count -ne 94) {
        throw "Publishing-Release cache is incomplete: expected 94 packaged shaders, found $($seenCachePaths.Count)."
    }
    $outerCacheShaders = @(
        $Entries | Where-Object {
            $_.RelativePath.StartsWith(
                'ShenLong\ShaderCache\v1\',
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetExtension($_.RelativePath) -ieq '.cso'
        })
    if ($outerCacheShaders.Count -ne $seenCachePaths.Count) {
        throw 'The outer Publishing manifest and nested shader-cache manifest do not cover the same CSO set.'
    }
}

function Test-GraphicsDeploymentPath([string] $RelativePath) {
    return $RelativePath -ieq 'ShenLong.asi' -or
        $RelativePath -ieq 'dxgi.dll' -or
        $RelativePath -ieq 'ReShade.ini' -or
        $RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
}

function Get-InstalledGraphicsManifestEntries([object[]] $PackageEntries) {
    if (-not (Test-Path -LiteralPath $installedManifest -PathType Leaf)) {
        throw "The installed graphics manifest is missing: $installedManifest"
    }
    $packageByPath = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $deploymentEntries = @($PackageEntries | Where-Object {
            Test-GraphicsDeploymentPath $_.RelativePath
        })
    foreach ($entry in $deploymentEntries) {
        $packageByPath.Add($entry.RelativePath, $entry)
    }

    $installedEntries = [Collections.Generic.List[object]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($installedManifest, $strictUtf8)) {
        $match = [regex]::Match($line, '^([0-9A-F]{64}) \*(.+)$')
        if (-not $match.Success) {
            throw "Malformed installed graphics manifest line: $line"
        }
        $relative = $match.Groups[2].Value.Replace('/', '\')
        if (-not $seen.Add($relative)) {
            throw "Duplicate installed graphics manifest path: $relative"
        }
        [void] (Get-CheckedChildPath $GameRoot $relative `
            'Installed graphics manifest')
        if (-not $packageByPath.ContainsKey($relative)) {
            throw "Installed graphics manifest contains an unexpected path: $relative"
        }
        $packageEntry = $packageByPath[$relative]
        if ($match.Groups[1].Value -cne $packageEntry.ExpectedHash) {
            throw "Installed graphics manifest hash differs from the package: $relative"
        }
        $installedEntries.Add($packageEntry)
    }
    if ($installedEntries.Count -gt $deploymentEntries.Count) {
        throw 'Installed graphics manifest does not match the managed package set.'
    }
    return $installedEntries
}

function Assert-InstalledPackage([object[]] $Entries) {
    $installedEntries = @(Get-InstalledGraphicsManifestEntries $Entries)
    foreach ($entry in $installedEntries) {
        $path = Get-CheckedChildPath $GameRoot $entry.RelativePath 'Installed graphics'
        if ((Get-Hash $path) -cne $entry.ExpectedHash) {
            throw "Installed graphics hash mismatch: $($entry.RelativePath)"
        }
    }

    foreach ($entry in @($Entries | Where-Object {
                (Test-GraphicsDeploymentPath $_.RelativePath) -and
                $_.RelativePath -ine 'ReShade.ini'
            })) {
        $path = Get-CheckedChildPath `
            $GameRoot $entry.RelativePath 'Installed graphics payload'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Hash $path) -cne $entry.ExpectedHash) {
            throw "Installed graphics payload hash mismatch: $($entry.RelativePath)"
        }
    }

    $expectedRootAddons = @(
        $Entries | Where-Object {
            [IO.Path]::GetDirectoryName($_.RelativePath) -eq '' -and
            [IO.Path]::GetExtension($_.RelativePath) -in @(
                '.addon', '.addon64')
        } | ForEach-Object { [IO.Path]::GetFileName($_.RelativePath) })
    $installedRootAddons = @(
        Get-ChildItem -LiteralPath $GameRoot -File |
            Where-Object {
                [IO.Path]::GetExtension($_.Name) -in @('.addon', '.addon64')
            } |
            Select-Object -ExpandProperty Name)
    if ($installedRootAddons.Count -ne $expectedRootAddons.Count -or
        @(Compare-Object -ReferenceObject $expectedRootAddons `
            -DifferenceObject $installedRootAddons).Count -ne 0) {
        throw "Unexpected root add-on set is installed: $($installedRootAddons -join ', ')"
    }

    foreach ($forbiddenFile in @(
            'SPatchGI.addon',
            'SPatchGTAO.addon',
            'SPatchSDAO.addon',
            'SPatchSSS.addon',
            'SPatchPBR.addon',
            'SPatchPCSS.addon',
            'Luma-Sleeping Dogs Definitive Edition.addon')) {
        if (Test-Path -LiteralPath (Join-Path $GameRoot $forbiddenFile)) {
            throw "A forbidden legacy/experimental add-on is installed: $forbiddenFile"
        }
    }
    foreach ($forbiddenDirectory in @('Luma', 'SPatch\PCSS')) {
        if (Test-Path -LiteralPath (Join-Path $GameRoot $forbiddenDirectory)) {
            throw "A forbidden legacy/experimental graphics directory is installed: $forbiddenDirectory"
        }
    }

    $shenLongRoot = Join-Path $GameRoot 'ShenLong'
    if (-not (Test-Path -LiteralPath $shenLongRoot -PathType Container)) {
        throw 'Installed managed ShenLong graphics directory is missing.'
    }
    $expectedShenLongFiles = @(
        $Entries | Where-Object {
            $_.RelativePath.StartsWith(
                'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
        } | ForEach-Object { $_.RelativePath.Substring(9) })
    $manifestOwnsReShadeCache = @(
        $expectedShenLongFiles | Where-Object {
            $_.StartsWith(
                'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase)
        }).Count -ne 0
    $actualShenLongFiles = @(
        Get-TreeRelativeFiles $shenLongRoot | Where-Object {
            $manifestOwnsReShadeCache -or
            -not $_.StartsWith(
                'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase)
        })
    if ($actualShenLongFiles.Count -ne $expectedShenLongFiles.Count -or
        @(Compare-Object -ReferenceObject $expectedShenLongFiles `
            -DifferenceObject $actualShenLongFiles).Count -ne 0) {
        throw 'Installed ShenLong graphics tree is not the exact manifest-owned Publishing-Release tree.'
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
            ForEach-Object { $_.FullName.Substring($shenLongRoot.Length + 1) } |
            Where-Object {
                $manifestOwnsReShadeCache -or
                ($_ -ine 'ReShadeCache' -and
                 -not $_.StartsWith(
                    'ReShadeCache\', [StringComparison]::OrdinalIgnoreCase))
            })
    if ($actualDirectories.Count -ne $expectedDirectories.Count) {
        throw 'Installed ShenLong graphics tree contains unexpected directories.'
    }
    foreach ($relativeDirectory in $actualDirectories) {
        if (-not $expectedDirectories.Contains($relativeDirectory)) {
            throw "Installed ShenLong graphics tree contains an unexpected directory: $relativeDirectory"
        }
    }
}

function Get-DeclaredConfigVersion {
    if (-not (Test-Path -LiteralPath $sourceConfigHeader -PathType Leaf)) {
        throw "Configuration header is missing: $sourceConfigHeader"
    }
    $header = [IO.File]::ReadAllText($sourceConfigHeader, $strictUtf8)
    $matches = @([regex]::Matches(
        $header,
        '(?m)^\s*inline\s+constexpr\s+int\s+kConfigVersion\s*=\s*(?<version>[1-9][0-9]*)\s*;\s*$'))
    if ($matches.Count -ne 1) {
        throw ("Config.h must declare exactly one positive kConfigVersion; " +
               "found $($matches.Count).")
    }
    return [int] $matches[0].Groups['version'].Value
}

function Assert-FinalReleaseIdentity {
    if (-not (Test-Path -LiteralPath $finalReleaseIdentity -PathType Leaf)) {
        throw 'The FinalRelease identity receipt is missing.'
    }
    $expectedKeys = @(
        'SPATCH_FINAL_RELEASE',
        'CONFIGURATION',
        'PLATFORM',
        'PE_MACHINE',
        'PE_OPTIONAL_MAGIC',
        'FINAL_POLICY_ATTESTED',
        'SHA256',
        'FILE',
        'TEST_MODES',
        'PACKAGE_DIR',
        'PACKAGE',
        'PACKAGE_SHA256',
        'PACKAGE_ASI',
        'PACKAGE_ASI_SHA256',
        'DEFAULT_INI',
        'DEFAULT_INI_SHA256',
        'MANIFEST',
        'MANIFEST_SHA256')
    $receipt = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines(
            $finalReleaseIdentity, $strictUtf8)) {
        $match = [regex]::Match($line, '^([A-Z0-9_]+)=(.*)$')
        if (-not $match.Success) {
            throw "Malformed FinalRelease identity line: $line"
        }
        $key = $match.Groups[1].Value
        if ($key -notin $expectedKeys -or $receipt.ContainsKey($key)) {
            throw "Unexpected or duplicate FinalRelease identity key: $key"
        }
        $receipt.Add($key, $match.Groups[2].Value)
    }
    if ($receipt.Count -ne $expectedKeys.Count) {
        throw ("The FinalRelease identity receipt is incomplete: expected " +
               "$($expectedKeys.Count) keys, found $($receipt.Count).")
    }
    foreach ($key in $expectedKeys) {
        if (-not $receipt.ContainsKey($key)) {
            throw "The FinalRelease identity receipt omits $key."
        }
    }
    if ($receipt['SPATCH_FINAL_RELEASE'] -cne '1' -or
        $receipt['CONFIGURATION'] -cne 'Release' -or
        $receipt['PLATFORM'] -cne 'x64' -or
        $receipt['PE_MACHINE'] -cne '8664' -or
        $receipt['PE_OPTIONAL_MAGIC'] -cne '020B' -or
        $receipt['FINAL_POLICY_ATTESTED'] -cne '1' -or
        $receipt['FILE'] -cne 'SPatch.asi' -or
        $receipt['TEST_MODES'] -cne 'normal,SPATCH_FINAL_RELEASE' -or
        $receipt['PACKAGE_DIR'] -cne 'SPatch-Base' -or
        $receipt['PACKAGE'] -cne 'SPatch-Base.zip' -or
        $receipt['PACKAGE_ASI'] -cne 'SPatch-Base/SPatch.asi' -or
        $receipt['DEFAULT_INI'] -cne 'SPatch-Base/SPatch.ini' -or
        $receipt['MANIFEST'] -cne 'SPatch-Base/SHA256SUMS.txt') {
        throw 'The FinalRelease identity receipt has an invalid build contract.'
    }
    foreach ($key in @(
            'SHA256',
            'PACKAGE_SHA256',
            'PACKAGE_ASI_SHA256',
            'DEFAULT_INI_SHA256',
            'MANIFEST_SHA256')) {
        if ($receipt[$key] -cnotmatch '^[0-9A-F]{64}$') {
            throw "The FinalRelease identity receipt has an invalid $key value."
        }
    }
    if ($receipt['SHA256'] -cne (Get-Hash $builtAsi)) {
        throw 'The FinalRelease identity receipt does not match SPatch.asi.'
    }
    if ($receipt['DEFAULT_INI_SHA256'] -cne (Get-Hash $basePackageIni)) {
        throw 'The FinalRelease identity receipt does not match the packaged default INI.'
    }
    if ($receipt['PACKAGE_SHA256'] -cne (Get-Hash $basePackageArchive)) {
        throw 'The FinalRelease identity receipt does not match SPatch-Base.zip.'
    }
    if ($receipt['PACKAGE_ASI_SHA256'] -cne
        (Get-Hash (Join-Path $basePackageRoot 'SPatch.asi'))) {
        throw 'The FinalRelease identity receipt does not match the packaged ASI.'
    }
    if ($receipt['MANIFEST_SHA256'] -cne (Get-Hash $basePackageManifest)) {
        throw 'The FinalRelease identity receipt does not match the package manifest.'
    }
}

function Resolve-DisplaySettingsPath {
    $gogPath = Join-Path $GameRoot 'Save\DisplaySettings.xml'
    $steamPath = Join-Path $GameRoot 'data\DisplaySettings.xml'
    if (Test-Path -LiteralPath $gogPath -PathType Leaf) {
        return [IO.Path]::GetFullPath($gogPath)
    }
    $gogMarkers = @(
        Get-ChildItem -LiteralPath $GameRoot -Filter 'goggame-*.info' -File `
            -ErrorAction SilentlyContinue)
    if ($gogMarkers.Count -ne 0) {
        return [IO.Path]::GetFullPath($gogPath)
    }
    if (Test-Path -LiteralPath $steamPath -PathType Leaf) {
        return [IO.Path]::GetFullPath($steamPath)
    }
    return [IO.Path]::GetFullPath($steamPath)
}

function Get-IniSectionMatch(
    [string] $Text,
    [string] $Section) {
    $escapedSection = [regex]::Escape($Section)
    $pattern = "(?ms)^[ \t]*\[$escapedSection\][ \t]*(?:[;#][^\r\n]*)?\r?\n(?<body>.*?)(?=^[ \t]*\[[^\]\r\n]+\][ \t]*(?:[;#][^\r\n]*)?\r?\n|\z)"
    $matches = @([regex]::Matches($Text, $pattern))
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
    $keyPattern = "(?m)^[ \t]*$escapedKey[ \t]*=[ \t]*(?<value>[^\r\n]*?)[ \t]*(?:\r?\n|$)"
    $matches = @([regex]::Matches($body, $keyPattern))
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
    $body = $bodyGroup.Value
    $escapedKey = [regex]::Escape($Key)
    $keyRegex = [regex]::new(
        "(?m)^(?<prefix>[ \t]*$escapedKey[ \t]*=)[^\r\n]*(?<ending>\r?\n|$)")
    $matches = @($keyRegex.Matches($body))
    if ($matches.Count -ne 1) {
        throw "INI [$Section] must contain exactly one $Key key; found $($matches.Count)."
    }
    $newBody = $keyRegex.Replace(
        $body,
        [Text.RegularExpressions.MatchEvaluator] {
            param($match)
            return $match.Groups['prefix'].Value + $Value +
                $match.Groups['ending'].Value
        },
        1)
    return $Text.Substring(0, $bodyGroup.Index) + $newBody +
        $Text.Substring($bodyGroup.Index + $bodyGroup.Length)
}

function Set-OrAddIniValueStrict(
    [string] $Text,
    [string] $Section,
    [string] $Key,
    [string] $Value) {
    $escapedSection = [regex]::Escape($Section)
    $sectionPattern = "(?ms)^[ \t]*\[$escapedSection\][ \t]*(?:[;#][^\r\n]*)?\r?\n(?<body>.*?)(?=^[ \t]*\[[^\]\r\n]+\][ \t]*(?:[;#][^\r\n]*)?\r?\n|\z)"
    $sectionMatches = @([regex]::Matches($Text, $sectionPattern))
    if ($sectionMatches.Count -gt 1) {
        throw "INI must contain at most one [$Section] section; found $($sectionMatches.Count)."
    }
    $newline = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    if ($sectionMatches.Count -eq 0) {
        $separator = if ($Text.EndsWith($newline)) {
            $newline
        } else {
            $newline + $newline
        }
        return $Text + $separator + "[$Section]$newline$Key=$Value$newline"
    }

    $sectionMatch = $sectionMatches[0]
    $bodyGroup = $sectionMatch.Groups['body']
    $body = $bodyGroup.Value
    $escapedKey = [regex]::Escape($Key)
    $keyMatches = @([regex]::Matches(
        $body,
        "(?m)^[ \t]*$escapedKey[ \t]*=[^\r\n]*(?:\r?\n|$)"))
    if ($keyMatches.Count -gt 1) {
        throw "INI [$Section] must contain at most one $Key key; found $($keyMatches.Count)."
    }
    if ($keyMatches.Count -eq 1) {
        return Set-IniValueStrict $Text $Section $Key $Value
    }

    $prefix = if ($body.Length -eq 0 -or
        $body.EndsWith("`n")) { '' } else { $newline }
    $newBody = $body + $prefix + "$Key=$Value$newline"
    return $Text.Substring(0, $bodyGroup.Index) + $newBody +
        $Text.Substring($bodyGroup.Index + $bodyGroup.Length)
}

function Assert-IniValue(
    [string] $Text,
    [string] $Section,
    [string] $Key,
    [string] $Expected) {
    $actual = Get-IniValueStrict $Text $Section $Key
    if ($actual -cne $Expected) {
        throw "Smoke INI [$Section] $Key expected '$Expected', found '$actual'."
    }
}

function Assert-PackagedReShadeConfiguration([string] $Text) {
    [void](Assert-ReShadeRootAddonPolicy $Text 'Packaged ReShade.ini' `
        -RequireAddonSection -RequireAddonPath)
}

function Set-SmokeArmIni(
    [string] $OriginalText,
    [object] $Arm) {
    $settings = @(
        [pscustomobject]@{ Section = 'ShenLong'; Key = 'Enabled'; Value = '1' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgX'; Value = '1' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgXLook'; Value = 'MediumHigh' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgXStrength'; Value = '100' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgXExposure'; Value = '100' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'SubsurfaceScattering'; Value = '1' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'StockHairBlur'; Value = '0' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'SSSQuality'; Value = '2' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'SSSStrength'; Value = '100' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'SSSRadius'; Value = '100' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'EyeScattering'; Value = '1' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'HairScattering'; Value = '1' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'TeethScattering'; Value = '1' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'FoliageTransmission'; Value = '1' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'WaterScattering'; Value = '1' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'SDAOQuality'; Value = '2' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'SDAORadius'; Value = '0.5' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'SDAOStrength'; Value = '100' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'GTAOLiteQuality'; Value = '2' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'GTAOLiteRadius'; Value = '0.5' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'GTAOLiteStrength'; Value = '100' },
        [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GIQuality'; Value = '2' },
        [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GIStrength'; Value = '100' },
        [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GIRadius'; Value = '4' },
        [pscustomobject]@{ Section = 'Shadows'; Key = 'ShadowResolution'; Value = ([string] $Arm.ShadowResolution) }
    ) + $Arm.Settings

    $text = $OriginalText
    foreach ($setting in $settings) {
        $text = Set-IniValueStrict `
            $text $setting.Section $setting.Key $setting.Value
    }
    $censusValue = if ($Arm.ShadowResolution -ne 0) { '1' } else { '0' }
    $text = Set-OrAddIniValueStrict `
        $text 'Debug' 'CensusShadowConsumers' $censusValue
    foreach ($setting in $settings) {
        Assert-IniValue $text $setting.Section $setting.Key $setting.Value
    }
    Assert-IniValue $text 'Debug' 'CensusShadowConsumers' $censusValue
    return $text
}

function Set-BaseSmokeIni([string] $OriginalText) {
    $settings = @(
        [pscustomobject]@{ Section = 'SPatch'; Key = 'Enabled'; Value = '1' },
        [pscustomobject]@{ Section = 'Debug'; Key = 'Logging'; Value = '1' },
        [pscustomobject]@{ Section = 'Debug'; Key = 'WriteCrashDumps'; Value = '1' },
        [pscustomobject]@{ Section = 'Input'; Key = 'GTAIVCarCamera'; Value = '1' },
        [pscustomobject]@{ Section = 'Input'; Key = 'GTAIVBikeCamera'; Value = '1' },
        [pscustomobject]@{ Section = 'TextureFiltering'; Key = 'AnisotropicFiltering'; Value = '16' },
        [pscustomobject]@{ Section = 'TextureFiltering'; Key = 'ForceAnisotropicFiltering'; Value = '1' },
        [pscustomobject]@{ Section = 'AntiAliasing'; Key = 'SMAA'; Value = '1' },
        [pscustomobject]@{ Section = 'AntiAliasing'; Key = 'SMAAPreset'; Value = '3' })
    $text = $OriginalText
    foreach ($setting in $settings) {
        $text = Set-IniValueStrict `
            $text $setting.Section $setting.Key $setting.Value
        Assert-IniValue $text `
            $setting.Section $setting.Key $setting.Value
    }
    if (-not $text.EndsWith('WriteCrashDumps=1',
            [StringComparison]::Ordinal)) {
        throw 'The SPatch smoke INI no longer ends with WriteCrashDumps=1.'
    }
    return $text
}

function Assert-ContainsLiteral(
    [string] $Text,
    [string] $Expected,
    [string] $Label) {
    if ($Text.IndexOf($Expected, [StringComparison]::Ordinal) -lt 0) {
        throw "$Label is missing current-session evidence: $Expected"
    }
}

function Assert-Matches(
    [string] $Text,
    [string] $Pattern,
    [string] $Label) {
    $match = [regex]::Match($Text, $Pattern)
    if (-not $match.Success) {
        throw "$Label is missing current-session pattern: $Pattern"
    }
    return $match
}

function Assert-VehicleCameraRuntimeEvidence(
    [string] $SpatchText,
    [bool] $CarExpected,
    [bool] $BikeExpected) {
    $carValue = if ($CarExpected) { 1 } else { 0 }
    $bikeValue = if ($BikeExpected) { 1 } else { 0 }
    Assert-ContainsLiteral $SpatchText `
        "requested_config input gta_iv_car_camera=$carValue gta_iv_bike_camera=$bikeValue" `
        'SPatch.log'

    foreach ($staleField in @(
            'gtaiv_car_camera ',
            'gtaiv_car_camera_probe ',
            'effective_offset_m=',
            'dynamic_probe_requested=',
            'dynamic_probe_installed=',
            'update_probe_installed=',
            'desired_pose_probe_installed=')) {
        if ($SpatchText.IndexOf(
                $staleField, [StringComparison]::Ordinal) -ge 0) {
            throw "SPatch.log contains stale vehicle-camera diagnostics: $staleField"
        }
    }

    $installPattern =
        "(?m)^[^\r\n]*gtaiv_vehicle_camera car_requested=$carValue " +
        "bike_requested=$bikeValue[^\r\n]*"
    $installMatches = @([regex]::Matches($SpatchText, $installPattern))
    $probePattern =
        "(?m)gtaiv_vehicle_camera_probe event=state_change mode=active " +
        "mutation=[01] car_enabled=$carValue bike_enabled=$bikeValue " +
        "class_enabled=[01] policy_evaluated=[01] " +
        "base_drive_branch_readable=[01] " +
        "base_drive_branch_selected=[01] " +
        "target_profile=(?:none|road_drive|road_flee|motorcycle_drive_block) " +
        "target_slot_match_mask=0x[0-9A-F]{4} " +
        "selected_slot_match_mask=0x[0-9A-F]{4} readable=[01] " +
        "blend_factor=[^ \r\n]+ base_offset_m=[^ \r\n]+ " +
        "applied_delta_m=[^ \r\n]+[^\r\n]*"
    $probeMatches = @([regex]::Matches($SpatchText, $probePattern))
    $featureRequested = $CarExpected -or $BikeExpected
    if ($featureRequested) {
        if ($installMatches.Count -ne 1) {
            throw ("Expected exactly one vehicle-camera installation record for " +
                   "car=$carValue bike=$bikeValue; found $($installMatches.Count).")
        }
        $installLine = $installMatches[0].Value
        foreach ($requiredField in @(
                'installed=1',
                'update_installed=1',
                'desired_pose_installed=1',
                'angular_approach_installed=1',
                'dynamics_mutation=1',
                'layout=legacy_researched')) {
            $requiredPattern = '(?:^|\s)' +
                [Regex]::Escape($requiredField) + '(?:\s|$)'
            if (-not [Regex]::IsMatch($installLine, $requiredPattern)) {
                throw ("Vehicle-camera installation record is missing " +
                       "required behavior evidence: $requiredField")
            }
        }
        if ($probeMatches.Count -eq 0) {
            throw ("SPatch.log has no vehicle-camera state record with Drive " +
                   "branch fields and applied_delta_m for car=$carValue " +
                   "bike=$bikeValue.")
        }
        foreach ($probeMatch in $probeMatches) {
            $probeLine = $probeMatch.Value
            foreach ($validityField in @(
                    'active_fields_readable=[01]',
                    'source_weight_valid=[01]')) {
                if ($probeLine -notmatch "(?:^|\s)$validityField(?:\s|$)") {
                    throw ("Vehicle-camera state record is missing current " +
                           "validity evidence: $validityField")
                }
            }
            if ($probeLine -notmatch
                '(?:^|\s)active_fields_readable=0(?:\s|$)') {
                continue
            }
            foreach ($inactiveField in @(
                    'source_weight_valid=0',
                    'target_parameters=0x0+',
                    'source_weight=0\.000000',
                    'update_eye=0',
                    'looking_back=0',
                    'state_b5a=0',
                    'state_b5b=0',
                    'state_b5c=0')) {
                if ($probeLine -notmatch "(?:^|\s)$inactiveField(?:\s|$)") {
                    throw ("Inactive vehicle-camera state was not sanitized: " +
                           $inactiveField)
                }
            }
        }
        return
    }

    if ($installMatches.Count -ne 0 -or
        $SpatchText.IndexOf(
            'gtaiv_vehicle_camera ', [StringComparison]::Ordinal) -ge 0 -or
        $SpatchText.IndexOf(
            'gtaiv_vehicle_camera_probe ', [StringComparison]::Ordinal) -ge 0 -or
        $SpatchText.IndexOf(
            'gtaiv_vehicle_camera_dynamic ', [StringComparison]::Ordinal) -ge 0) {
        throw 'Vehicle-camera hooks or diagnostics ran while both camera toggles were disabled.'
    }
}

function Assert-NoSmokeFailures(
    [string] $SpatchText,
    [string] $ReshadeText) {
    if ($SpatchText -match '(?im)\[(?:ERROR|WARN)\]') {
        throw 'The current SPatch session contains an error or warning.'
    }
    foreach ($pattern in @(
            '(?im)\|[ \t]*ERROR[ \t]*\|',
            '(?i)assert(?:ion)?[ \t_-]*failed',
            '(?i)DXGI_ERROR_DEVICE_(?:REMOVED|HUNG|RESET)',
            '(?i)device[ \t_-]*(?:was[ \t_-]*)?(?:removed|hung|lost)',
            '(?i)Required precompiled shader cache entry is missing',
            '(?i)Precompiled shader cache unavailable',
            '(?i)(?:Development[ \t]+)?source[ \t_-]*fallback',
            '(?im)\[ShenLong(?:-[^\]]+)?\].*(?:\bfailed\b|\bfailure\b|Could not|unavailable for 120 consecutive|unsupported graphics API|disabled\.)')) {
        if ($ReshadeText -match $pattern) {
            throw "The current ReShade session contains failure evidence: $pattern"
        }
    }
}

function Get-LoadedModuleEvidence(
    [Diagnostics.Process] $Process,
    [string] $ExpectedSpatchPath,
    [string] $ExpectedSpatchHash,
    [string] $ExpectedLoaderPath,
    [string] $LoaderName,
    [string] $ExpectedLoaderHash,
    [string] $ExpectedAddonPath,
    [string] $ExpectedAddonHash,
    [string] $ExpectedDxgiPath,
    [string] $ExpectedDxgiHash) {
    $modules = @($Process.Modules)
    $spatchModules = @($modules | Where-Object { $_.ModuleName -ieq 'SPatch.asi' })
    if ($spatchModules.Count -ne 1) {
        throw "Expected exactly one loaded SPatch.asi, found $($spatchModules.Count)."
    }
    $spatchPath = [IO.Path]::GetFullPath($spatchModules[0].FileName)
    if (-not (Test-PathsEqual $spatchPath $ExpectedSpatchPath)) {
        throw "Loaded SPatch.asi came from the wrong path: $spatchPath"
    }
    if ((Get-Hash $spatchPath) -cne $ExpectedSpatchHash) {
        throw 'Loaded SPatch.asi path no longer has the expected FinalRelease hash.'
    }

    $loaderNameModules = @(
        $modules | Where-Object { $_.ModuleName -ieq $LoaderName })
    $loaderModules = @(
        $loaderNameModules | Where-Object {
            Test-PathsEqual $_.FileName $ExpectedLoaderPath
        })
    if ($loaderModules.Count -ne 1) {
        $loadedPaths = @(
            $loaderNameModules | ForEach-Object { $_.FileName }) -join '; '
        throw ("Expected exactly one loaded {0} ASI loader at {1}, found {2}. " +
            "Same-name module paths: {3}" -f
                $LoaderName,
                $ExpectedLoaderPath,
                $loaderModules.Count,
                $loadedPaths)
    }
    $loaderPath = [IO.Path]::GetFullPath($loaderModules[0].FileName)
    if (-not (Test-PathsEqual $loaderPath $ExpectedLoaderPath)) {
        throw "Loaded ASI loader came from the wrong path: $loaderPath"
    }
    $loadedLoaderHash = Get-Hash $loaderPath
    if ($loadedLoaderHash -cne $ExpectedLoaderHash) {
        throw 'The ASI loader changed between preflight and module validation.'
    }

    $addonModules = @(
        $modules | Where-Object { $_.ModuleName -ieq 'ShenLong.asi' })
    if ($addonModules.Count -ne 1) {
        throw "Expected exactly one loaded ShenLong.asi, found $($addonModules.Count)."
    }
    $addonPath = [IO.Path]::GetFullPath($addonModules[0].FileName)
    if (-not (Test-PathsEqual $addonPath $ExpectedAddonPath)) {
        throw "Loaded ShenLong.asi came from the wrong path: $addonPath"
    }
    $loadedAddonHash = Get-Hash $addonPath
    if ($loadedAddonHash -cne $ExpectedAddonHash) {
        throw 'Loaded ShenLong.asi does not match the Publishing-Release manifest.'
    }

    $dxgiNameModules = @(
        $modules | Where-Object { $_.ModuleName -ieq 'dxgi.dll' })
    $dxgiModules = @(
        $dxgiNameModules | Where-Object {
            Test-PathsEqual $_.FileName $ExpectedDxgiPath
        })
    if ($dxgiModules.Count -ne 1) {
        $loadedPaths = @(
            $dxgiNameModules | ForEach-Object { $_.FileName }) -join '; '
        throw ("Expected exactly one loaded packaged dxgi.dll at " +
               "$ExpectedDxgiPath; found $($dxgiModules.Count). " +
               "Same-name module paths: $loadedPaths")
    }
    $systemDxgi = Join-Path $env:SystemRoot 'System32\dxgi.dll'
    $unexpectedDxgiHosts = @(
        $dxgiNameModules | Where-Object {
            -not (Test-PathsEqual $_.FileName $ExpectedDxgiPath) -and
            -not (Test-PathsEqual $_.FileName $systemDxgi)
        })
    if ($unexpectedDxgiHosts.Count -ne 0) {
        throw ("An alternate dxgi.dll host is loaded: " +
               (($unexpectedDxgiHosts | ForEach-Object {
                           $_.FileName
                       }) -join '; '))
    }
    $dxgiPath = [IO.Path]::GetFullPath($dxgiModules[0].FileName)
    $loadedDxgiHash = Get-Hash $dxgiPath
    if ($loadedDxgiHash -cne $ExpectedDxgiHash) {
        throw 'Loaded dxgi.dll does not match the Publishing-Release manifest.'
    }

    $loadedForbidden = @(
        $modules | Where-Object {
            $_.ModuleName -ilike 'dxvk*.dll' -or
            $_.ModuleName -ieq 'd3d11on12.dll'
        })
    if ($loadedForbidden.Count -ne 0) {
        throw ('A non-native renderer module is loaded: ' +
            (($loadedForbidden | ForEach-Object { $_.FileName }) -join '; '))
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
        throw ("Stock Native D3D11 requires exactly one system d3d11.dll; " +
               "loaded paths: " +
               (($d3d11NameModules | ForEach-Object {
                           $_.FileName
                       }) -join '; '))
    }
    $nativeD3d11Path = [IO.Path]::GetFullPath(
        $systemD3d11Modules[0].FileName)
    $nativeD3d11Hash = Get-Hash $nativeD3d11Path

    $optionalSystemModules = @{}
    foreach ($systemModuleName in @('dbghelp.dll', 'D3DCompiler_47.dll')) {
        $expectedSystemPath = Join-Path `
            $env:SystemRoot "System32\$systemModuleName"
        $sameNameModules = @(
            $modules | Where-Object {
                $_.ModuleName -ieq $systemModuleName
            })
        if ($sameNameModules.Count -eq 0) {
            $optionalSystemModules[$systemModuleName] = $null
            continue
        }
        $unexpectedPaths = @(
            $sameNameModules | Where-Object {
                -not (Test-PathsEqual $_.FileName $expectedSystemPath)
            })
        if ($sameNameModules.Count -ne 1 -or
            $unexpectedPaths.Count -ne 0) {
            throw ("Loaded $systemModuleName must come only from " +
                   "$expectedSystemPath; loaded paths: " +
                   (($sameNameModules | ForEach-Object {
                               $_.FileName
                           }) -join '; '))
        }
        $systemModulePath = [IO.Path]::GetFullPath(
            $sameNameModules[0].FileName)
        $optionalSystemModules[$systemModuleName] = [pscustomobject]@{
            Path = $systemModulePath
            Hash = Get-Hash $systemModulePath
        }
    }

    $dbghelpEvidence = $optionalSystemModules['dbghelp.dll']
    $d3dCompiler47Evidence = $optionalSystemModules['D3DCompiler_47.dll']

    return [pscustomobject]@{
        SpatchPath = $spatchPath
        SpatchHash = $ExpectedSpatchHash
        LoaderPath = $loaderPath
        LoaderHash = $loadedLoaderHash
        AddonPath = $addonPath
        AddonHash = $loadedAddonHash
        DxgiPath = $dxgiPath
        DxgiHash = $loadedDxgiHash
        NativeD3d11Path = $nativeD3d11Path
        NativeD3d11Hash = $nativeD3d11Hash
        SystemDbghelpPath = if ($dbghelpEvidence) {
            $dbghelpEvidence.Path
        } else {
            $null
        }
        SystemDbghelpHash = if ($dbghelpEvidence) {
            $dbghelpEvidence.Hash
        } else {
            $null
        }
        SystemD3dCompiler47Path = if ($d3dCompiler47Evidence) {
            $d3dCompiler47Evidence.Path
        } else {
            $null
        }
        SystemD3dCompiler47Hash = if ($d3dCompiler47Evidence) {
            $d3dCompiler47Evidence.Hash
        } else {
            $null
        }
    }
}

function Start-SmokeGame([datetime] $RequestedUtc) {
    if (-not (Test-Path -LiteralPath $benchmarkShortcut -PathType Leaf)) {
        throw "The unattended benchmark shortcut is missing: $benchmarkShortcut"
    }
    $latestResult = @(Get-ChildItem -LiteralPath $GameRoot `
            -Filter 'BenchmarkResult-*.xml' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1)
    if ($latestResult.Count -eq 1) {
        $releaseDelayMilliseconds =
            Get-SPatchBenchmarkReleaseDelayMilliseconds $latestResult[0]
        if ($releaseDelayMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $releaseDelayMilliseconds
        }
    }
    $launch = Invoke-SPatchBenchmarkShortcut $gameExe $RequestedUtc 120
    Register-TaskOwnedSmokeProcess `
        $launch.Process $RequestedUtc.ToLocalTime() $gameExe
    Register-ShortcutSteamOwnership $launch.Receipt
    return $launch
}

function Wait-ForRuntimeReady(
    [Diagnostics.Process] $Process,
    [string] $ExpectedSpatchHash,
    [string] $ExpectedLoaderHash,
    [string] $ExpectedAddonHash,
    [string] $ExpectedDxgiHash) {
    $deadline = (Get-Date).AddSeconds(60)
    $lastError = ''
    $shell = New-Object -ComObject WScript.Shell
    while ((Get-Date) -lt $deadline -and -not $Process.HasExited) {
        try {
            $Process.Refresh()
            if ($Process.MainWindowHandle -ne 0) {
                [void] $shell.AppActivate($Process.Id)
            }
            if ($Process.Responding) {
                return Get-LoadedModuleEvidence `
                    $Process $installedAsi $ExpectedSpatchHash $asiLoader `
                    $AsiLoaderName $ExpectedLoaderHash $installedAddon `
                    $ExpectedAddonHash $installedDxgi $ExpectedDxgiHash
            }
        } catch {
            $lastError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 250
    }
    if ($Process.HasExited) {
        throw 'The game exited before the SPatch runtime became ready.'
    }
    throw "The game did not expose the exact responsive SPatch/ASI-loader module set: $lastError"
}

function Get-BenchmarkResultSnapshot {
    return Get-SPatchFileIdentitySnapshot $GameRoot 'BenchmarkResult-*.xml'
}

function Wait-ForBenchmarkResult(
    [Diagnostics.Process] $Process,
    [Collections.Generic.Dictionary[string, object]] $BeforeSnapshot,
    [datetime] $RequestedUtc) {
    $deadline = (Get-Date).AddMinutes(4)
    $shell = New-Object -ComObject WScript.Shell
    $lastParseError = ''
    $exitRetryPerformed = $false
    while ((Get-Date) -lt $deadline) {
        $Process.Refresh()
        if (-not $Process.HasExited -and $Process.MainWindowHandle -ne 0) {
            [void] $shell.AppActivate($Process.Id)
        }
        $nowUtc = [DateTime]::UtcNow
        $results = @(Get-ChildItem -LiteralPath $GameRoot `
            -Filter 'BenchmarkResult-*.xml' -File |
            Where-Object {
                $_.LastWriteTimeUtc -ge $RequestedUtc.AddSeconds(-3) -and
                $_.LastWriteTimeUtc -le $nowUtc.AddSeconds(5)
            } |
            Sort-Object LastWriteTimeUtc -Descending)
        $eligibleResults = [Collections.Generic.List[IO.FileInfo]]::new()
        foreach ($result in $results) {
            try {
                $identity = [pscustomobject]@{
                    Name = $result.Name
                    Length = [int64] $result.Length
                    Sha256 = Get-Hash $result.FullName
                    LastWriteTimeUtc = $result.LastWriteTimeUtc
                    FullName = $result.FullName
                }
                if ($BeforeSnapshot.ContainsKey($result.Name)) {
                    $previous = $BeforeSnapshot[$result.Name]
                    if ($previous.Length -eq $identity.Length -and
                        $previous.Sha256 -ceq $identity.Sha256) {
                        continue
                    }
                }
                $eligibleResults.Add($result)
                [void] (Get-BenchmarkSummary $result)
                return [pscustomobject]@{
                    File = $result
                    Identity = $identity
                }
            } catch {
                $lastParseError = $_.Exception.Message
            }
        }
        if ($Process.HasExited) {
            if (-not $exitRetryPerformed) {
                $exitRetryPerformed = $true
                Start-Sleep -Milliseconds 500
                continue
            }
            if ($eligibleResults.Count -eq 0) {
                throw 'The game exited without producing a current benchmark result.'
            }
            throw "The game exited without a complete valid benchmark result: $lastParseError"
        }
        Start-Sleep -Milliseconds 500
    }
    if ([string]::IsNullOrWhiteSpace($lastParseError)) {
        throw 'The final smoke did not produce a benchmark result.'
    }
    throw "The final smoke did not produce a complete valid benchmark result: $lastParseError"
}

function Get-BenchmarkSummary([IO.FileInfo] $Result) {
    try {
        [xml] $document = [IO.File]::ReadAllText($Result.FullName)
        $fps = $document.Benchmark.FPS
        $values = @{}
        foreach ($metric in @('Average', 'Min', 'Max')) {
            $raw = [string] $fps.$metric.value
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
        if ($values['Average'] -le 0 -or $values['Max'] -le 0 -or
            $values['Min'] -lt 0 -or
            $values['Min'] -gt $values['Average'] -or
            $values['Average'] -gt $values['Max']) {
            throw 'FPS values do not satisfy 0 <= min <= average <= max with positive average and max'
        }
    } catch {
        throw "Benchmark XML is malformed: $($Result.FullName): $($_.Exception.Message)"
    }
    return [pscustomobject]@{
        Path = $Result.FullName
        AverageFps = $values['Average']
        MinimumFps = $values['Min']
        MaximumFps = $values['Max']
    }
}

function Assert-SelectedDisplayPath(
    [string] $SpatchText,
    [string] $ExpectedPath) {
    $matches = @([regex]::Matches(
        $SpatchText,
        '(?m)display_settings selected_path=(?<path>[^\r\n]+)\r?$'))
    if ($matches.Count -ne 1) {
        throw "Expected one current-session display_settings selected_path log, found $($matches.Count)."
    }
    $selected = $matches[0].Groups['path'].Value.Trim()
    if (-not (Test-PathsEqual $selected $ExpectedPath)) {
        throw "SPatch selected the wrong DisplaySettings.xml: $selected"
    }
}

function Assert-VerifiedRenderExtent(
    [int] $Width,
    [int] $Height,
    [string[]] $VerifiedExtents,
    [string] $Label) {
    $key = "${Width}x${Height}"
    if (-not ($VerifiedExtents -ccontains $key)) {
        throw "$Label reported an extent absent from both SMAA and ResizeBuffers: $key"
    }
}

function Assert-CommonRuntimeEvidence(
    [string] $SpatchText,
    [string] $ReshadeText,
    [bool] $CarCameraExpected,
    [bool] $BikeCameraExpected) {
    foreach ($evidence in @(
            'SPatch bootstrap starting',
            'supported=yes',
            'smaa_shader_precompile_ready preset=3',
            'aa_owner_hook requested=1 installed=1 probe=0 stock_suppression=1',
            'aa_fx_hook requested=1 installed=1 probe=0 stock_suppression=1',
            'requested_config texture_filtering anisotropy=16 force_verified_trilinear=1',
            'texture_filtering anisotropy_writer requested=16 installed=1 layout=legacy_researched',
            'texture_filtering sampler_builder requested=16 installed=1 layout=legacy_researched',
            'texture_filtering sampler_builder_first_verified_invocation=1',
            'engine_patch name=force_anisotropic_filtering result=applied',
            'SPatch initialized')) {
        Assert-ContainsLiteral $SpatchText $evidence 'SPatch.log'
    }
    Assert-VehicleCameraRuntimeEvidence `
        $SpatchText $CarCameraExpected $BikeCameraExpected
    $smaaExtents = @([regex]::Matches(
        $SpatchText,
        '(?m)smaa_resources_ready width=([1-9][0-9]*) height=([1-9][0-9]*) format=28 output=(?:direct_srgb|srgb_copy_fallback) stencil=1'))
    if ($smaaExtents.Count -eq 0) {
        throw 'SPatch.log has no current-session ready SMAA extent.'
    }
    $smaaExtent = $smaaExtents[$smaaExtents.Count - 1]
    $activeWidth = [int] $smaaExtent.Groups[1].Value
    $activeHeight = [int] $smaaExtent.Groups[2].Value
    # The verified shortcut requests the nominal 4K extent, but the game may
    # finish its fullscreen transition at the nominal extent or a
    # Windows-managed client area (both width and height have been observed to
    # change). Prove the requested extent and the final ResizeBuffers extent,
    # then require every renderer below to agree with the latter rather than
    # guessing a window-chrome tolerance.
    $resizeMatches = @([regex]::Matches(
        $ReshadeText,
        'IDXGISwapChain::ResizeBuffers\([^\r\n]*Width = (?<width>[1-9][0-9]*), Height = (?<height>[1-9][0-9]*)'))
    if ($resizeMatches.Count -eq 0) {
        throw 'ReShade.log has no current-session ResizeBuffers extent.'
    }
    $nominalResize = @($resizeMatches | Where-Object {
            [int] $_.Groups['width'].Value -eq $ExpectedWidth -and
            [int] $_.Groups['height'].Value -eq $ExpectedHeight
        })
    if ($nominalResize.Count -eq 0) {
        throw "ReShade.log never requested ${ExpectedWidth}x${ExpectedHeight}."
    }
    $smaaExtentKeys = @($smaaExtents | ForEach-Object {
            '{0}x{1}' -f $_.Groups[1].Value, $_.Groups[2].Value
        } | Sort-Object -Unique)
    $resizeExtentKeys = @($resizeMatches | ForEach-Object {
            '{0}x{1}' -f $_.Groups['width'].Value,
                $_.Groups['height'].Value
        } | Sort-Object -Unique)
    $verifiedExtentKeys = @($smaaExtentKeys | Where-Object {
            $resizeExtentKeys -ccontains $_
        })
    if ($verifiedExtentKeys.Count -eq 0) {
        throw 'SMAA and ResizeBuffers reported no common active render extent.'
    }
    $lastResize = $resizeMatches[$resizeMatches.Count - 1]
    if ([int] $lastResize.Groups['width'].Value -ne $activeWidth -or
        [int] $lastResize.Groups['height'].Value -ne $activeHeight) {
        throw ('The final ResizeBuffers extent does not match active SMAA: ' +
               ('resize={0}x{1} smaa={2}x{3}.' -f
                   $lastResize.Groups['width'].Value,
                   $lastResize.Groups['height'].Value,
                   $activeWidth, $activeHeight))
    }

    foreach ($evidence in @(
            '[ShenLong-Native] HairBlur hook profile=legacy_researched rva=0x3E7C0 installed=1 minhook=0.',
            '[ShenLong-Native] StockHairBlur=0 skipped the exact named HairBlur submission.',
            '[ShenLong-AgX] configured enabled=1 look=MediumHigh strength=100 exposure=100 boundary=0x67843125 pipeline_replace=1.',
            '[ShenLong-AgX] Full-RGB AgX pipeline replacement confirmed for exact shader 0x67843125',
            '[ShenLong-SSS] config: enabled=1 skin=1 eye=1 hair=1 teeth=1 foliage=1 quality=2 strength=100% radius=100%',
            '[ShenLong-SSS] Precompiled shader cache v1 loaded',
            '[ShenLong-SSS] exact material-profile hooks active.',
            '[ShenLong-Water] precompiled shader cache v1 loaded for all three exact water variants.',
            '[ShenLong-Water] enabled=1 ready=1 isotropic_strength=100%.')) {
        Assert-ContainsLiteral $ReshadeText $evidence 'ReShade.log'
    }
    $sssDispatch = Assert-Matches $ReshadeText `
        '(?m)\[ShenLong-SSS\] dispatched at (?<width>[1-9][0-9]*)x(?<height>[1-9][0-9]*); quality=2, replayed_draws=\{skin:(?<skin>[1-9][0-9]*) eye:[0-9]+ hair:[0-9]+ teeth:[0-9]+ foliage:[0-9]+\}\.' `
        'ReShade.log'
    Assert-VerifiedRenderExtent `
        ([int] $sssDispatch.Groups['width'].Value) `
        ([int] $sssDispatch.Groups['height'].Value) `
        $verifiedExtentKeys `
        'ShenLong SSS'
    return [pscustomobject]@{
        ActiveWidth = $activeWidth
        ActiveHeight = $activeHeight
        SssDispatch = $sssDispatch
        VerifiedExtents = [string[]] $verifiedExtentKeys
    }
}

function Assert-ArmEvidence(
    [string] $AoMode,
    [bool] $GiExpected,
    [bool] $PbrExpected,
    [int] $ShadowResolution,
    [bool] $CarCameraExpected,
    [bool] $BikeCameraExpected,
    [string] $SpatchText,
    [string] $ReshadeText) {
    $commonEvidence = Assert-CommonRuntimeEvidence `
        $SpatchText $ReshadeText $CarCameraExpected $BikeCameraExpected
    $activeWidth = [int] $commonEvidence.ActiveWidth
    $activeHeight = [int] $commonEvidence.ActiveHeight
    $verifiedExtents = [string[]] $commonEvidence.VerifiedExtents
    if ($AoMode -eq 'Original') {
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-Native] verified pre-device transaction committed profile=legacy_researched custom_ao=0 stock_hair_blur=0.' `
            'ReShade.log'
    } else {
        foreach ($nativeAoEvidence in @(
                '[ShenLong-Native] AO-stage hook profile=legacy_researched rva=0x35370 installed=1 minhook=0.',
                '[ShenLong-Native] verified pre-device transaction committed profile=legacy_researched custom_ao=1 stock_hair_blur=0.',
                '[ShenLong-Native] custom AO observed the verified high-quality live scheduler path')) {
            Assert-ContainsLiteral $ReshadeText $nativeAoEvidence 'ReShade.log'
        }
    }
    foreach ($failureMarker in @(
            'PARTIAL FAILURE',
            'Native hook installation failed',
            'Hook install failed',
            'No immediate context',
            'CreateDeferredContext failed',
            'Driver rejected the patched shadow shader',
            'matched but produced no patchable constants',
            'Could not resolve the add-on directory',
            'Unsupported graphics API')) {
        if ($ReshadeText.IndexOf(
                $failureMarker,
                [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Shadow runtime reported failure evidence: $failureMarker"
        }
    }
    switch ($ShadowResolution) {
        2048 {
            foreach ($required in @(
                    'ShadowResolution=2048 texture_scale=1.000 filter_atlas=2048 shaders=12 census=1.',
                    'Native shadow-map detours armed (device + immediate/deferred context hooks).',
                    'Immediate-context hooks: ok.',
                    'Deferred-context hooks: ok.')) {
                Assert-ContainsLiteral $ReshadeText $required 'ReShade.log'
            }
            foreach ($pattern in @(
                    'Scaled shadow map (?<native>512|1024|1408)x\k<native> -> 2048x2048\.',
                    'Census snapshot: shaders patched=[0-9]+ maps scaled=[1-9][0-9]* viewport corrections=[1-9][0-9]* \(srv-blocked=[0-9]+, cmdlist-exec=[0-9]+\)\.')) {
                [void] (Assert-Matches $ReshadeText $pattern 'ReShade.log')
            }
            $consumerMatch = [regex]::Match(
                $ReshadeText,
                'Consumer census: exact identities observed=(?<count>[1-9][0-9]*)\.')
            if (-not $consumerMatch.Success) {
                throw 'ReShade.log has no exact shadow consumer identity observed during create_pipeline.'
            }
            $classification = [regex]::Match(
                $ReshadeText,
                'Confirmed (?<total>[1-9][0-9]*) map-aware shadow pipeline\(s\) at 2048 texels \(patched=(?<patched>[0-9]+), verified-native-2048=(?<native>[0-9]+)\)\.')
            if (-not $classification.Success) {
                throw 'ReShade.log has no explicit patched/verified-native-2048 pipeline classification.'
            }
            $classified = [int64] $classification.Groups['patched'].Value +
                [int64] $classification.Groups['native'].Value
            $classifiedTotal =
                [int64] $classification.Groups['total'].Value
            $observedConsumers =
                [int64] $consumerMatch.Groups['count'].Value
            if ($classified -ne $classifiedTotal -or
                $observedConsumers -ne $classifiedTotal -or
                $classified -le 0) {
                throw 'The 2048 consumer census and map-aware pipeline classification counts are inconsistent.'
            }
        }
        0 {
            Assert-ContainsLiteral $ReshadeText `
                'ShadowResolution=0 -> native shadow maps and filters retained.' `
                'ReShade.log'
            foreach ($pattern in @(
                    'Native shadow-map detours armed',
                    'Immediate-context hooks:',
                    'Deferred-context hooks:',
                    'Scaled shadow map [0-9]+x[0-9]+ -> [0-9]+x[0-9]+\.',
                    'Patched shadow shader',
                    'Confirmed [1-9][0-9]* map-aware shadow pipeline\(s\)',
                    'Census snapshot:')) {
                if ($ReshadeText -match $pattern) {
                    throw "Native shadow control emitted scaled-shadow evidence: $pattern"
                }
            }
        }
        default {
            throw "Unsupported shadow-resolution smoke value: $ShadowResolution"
        }
    }
    if ($PbrExpected) {
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-PBR] configured enabled=1 replaceable_variants=18 native_passthrough_variants=2 strength=100 cache=PBR pipeline_replace=1 bind_telemetry=first-success-only draw_replay=0.' `
            'ReShade.log'
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-PBR] ready=1 validated=18/18 unique=18 driver_accepted=18 replacement_mask=0xFDFFE atomic_all_or_nothing=1.' `
            'ReShade.log'
        $pbrPolicy = Assert-Matches $ReshadeText `
            '(?im)\[ShenLong-PBR\] runtime replacement policy active=18/20 replacement_target_mask=0x([0-9a-f]+) native_ambient_mask=0x([0-9a-f]+) native_compatibility_mask=0x([0-9a-f]+) direct_specular_aa=opaque-normal-derivative,vehicle-glass=none\.\r?$' `
            'ReShade.log'
        $pbrReplacementTargetMask = [Convert]::ToUInt32(
            $pbrPolicy.Groups[1].Value, 16)
        $pbrNativeAmbientMask = [Convert]::ToUInt32(
            $pbrPolicy.Groups[2].Value, 16)
        $pbrNativeCompatibilityMask = [Convert]::ToUInt32(
            $pbrPolicy.Groups[3].Value, 16)
        if ($pbrReplacementTargetMask -ne [uint32] 0xFDFFE -or
            $pbrNativeAmbientMask -ne [uint32] 0x00001 -or
            $pbrNativeCompatibilityMask -ne [uint32] 0x02000) {
            throw ('PBR runtime replacement policy masks are inconsistent: ' +
                   ('replacement_target=0x{0:X} native_ambient=0x{1:X} native_compatibility=0x{2:X}' -f
                       $pbrReplacementTargetMask, $pbrNativeAmbientMask,
                       $pbrNativeCompatibilityMask))
        }
        $pbrEvidence = Assert-Matches $ReshadeText `
            '(?m)\[ShenLong-PBR\] present=(?:300|1800) enabled=1 ready=1 strength=100 validated=18/18 native_passthrough=2 discovery_mask=0x([0-9A-F]+) replacement_mask=0x([0-9A-F]+) first_bound_mask=0x([0-9A-F]+) requested=([1-9][0-9]*) confirmed=([1-9][0-9]*) first_bind_samples=1 tag_failures=0;' `
            'ReShade.log'
        $pbrDiscovered = [Convert]::ToUInt32(
            $pbrEvidence.Groups[1].Value, 16)
        $pbrReplaced = [Convert]::ToUInt32(
            $pbrEvidence.Groups[2].Value, 16)
        $pbrBound = [Convert]::ToUInt32(
            $pbrEvidence.Groups[3].Value, 16)
        if ($pbrDiscovered -ne $pbrReplacementTargetMask -or
            $pbrReplaced -ne $pbrReplacementTargetMask -or
            ($pbrDiscovered -band [uint32] 0x02001) -ne 0 -or
            $pbrBound -eq 0 -or ($pbrBound -band $pbrReplaced) -ne $pbrBound) {
            throw ('PBR discovery/replacement/bind coverage is inconsistent: ' +
                   ('discovered=0x{0:X} replaced=0x{1:X} bound=0x{2:X}' -f
                       $pbrDiscovered, $pbrReplaced, $pbrBound))
        }
    } else {
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-PBR] configured enabled=0 replaceable_variants=18 native_passthrough_variants=2 strength=100 cache=PBR pipeline_replace=1 bind_telemetry=first-success-only draw_replay=0.' `
            'ReShade.log'
        if ($ReshadeText -match '\[ShenLong-PBR\] (?:ready=1|present=)') {
            throw 'PBR initialized or replaced a pipeline during the native PBR control arm.'
        }
    }
    if ($GiExpected) {
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-GI] config: enabled=1 quality=2' 'ReShade.log'
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-GI] Precompiled shader cache v1 loaded' 'ReShade.log'
        $giExtent = Assert-Matches $ReshadeText `
            '(?m)\[ShenLong-GI\] active at (?<width>[1-9][0-9]*)x(?<height>[1-9][0-9]*)' `
            'ReShade.log'
        Assert-VerifiedRenderExtent `
            ([int] $giExtent.Groups['width'].Value) `
            ([int] $giExtent.Groups['height'].Value) `
            $verifiedExtents `
            'ShenLong GI'
        Assert-ContainsLiteral $ReshadeText `
            'normal-cache + trace + 2 filter + direct-additive composite' `
            'ReShade.log'
        Assert-ContainsLiteral $ReshadeText `
            'AO is independently selected and only overridden by the AO coordinator.' `
            'ReShade.log'
    } else {
        Assert-ContainsLiteral $ReshadeText `
            '[ShenLong-GI] config: enabled=0 quality=2' 'ReShade.log'
        if ($ReshadeText -match '\[ShenLong-GI\] active at') {
            throw 'GI ran while the GI-disabled smoke arm was active.'
        }
    }

    switch ($AoMode) {
        'SDAO' {
            Assert-ContainsLiteral $ReshadeText `
                '[ShenLong-SDAO] config: mode=SDAO quality=2 radius=0.500m strength=100%' `
                'ReShade.log'
            Assert-ContainsLiteral $ReshadeText `
                '[ShenLong-SDAO] Precompiled shader cache v1 loaded (mode=SDAO quality=2).' `
                'ReShade.log'
            $captureEvidence = Assert-Matches $ReshadeText `
                '(?m)\[ShenLong-SDAO\] stochastic capture active: frame=[1-9][0-9]* draws=(?<draws>[1-9][0-9]*) misses=0 layers=2 resolution=(?<width>[1-9][0-9]*)x(?<height>[1-9][0-9]*) alpha=0\.2 path=full-resolution-min-blend-replay capture-shaders=[1-9][0-9]* fallback-shaders=[0-9]+ rejected-shaders=[0-9]+\.' `
                'ReShade.log'
            $activeEvidence = Assert-Matches $ReshadeText `
                '(?m)\[ShenLong-SDAO\] full-resolution SDAO active at (?<width>[1-9][0-9]*)x(?<height>[1-9][0-9]*); quality=2 layers=2 capture-draws=(?<draws>[1-9][0-9]*), full-resolution alpha-only MIN-blend replay preserves native early depth/MRT/depth without UAV atomics, DiligentFX-derived horizon evaluation and cross-bilateral reconstruction enabled\.' `
                'ReShade.log'
            Assert-VerifiedRenderExtent `
                ([int] $captureEvidence.Groups['width'].Value) `
                ([int] $captureEvidence.Groups['height'].Value) `
                $verifiedExtents `
                'ShenLong SDAO capture'
            Assert-VerifiedRenderExtent `
                ([int] $activeEvidence.Groups['width'].Value) `
                ([int] $activeEvidence.Groups['height'].Value) `
                $verifiedExtents `
                'ShenLong SDAO composition'
            if ($captureEvidence.Groups['draws'].Value -cne
                    $activeEvidence.Groups['draws'].Value -or
                $captureEvidence.Groups['width'].Value -cne
                    $activeEvidence.Groups['width'].Value -or
                $captureEvidence.Groups['height'].Value -cne
                    $activeEvidence.Groups['height'].Value) {
                throw 'SDAO capture and composition reported different draw counts or extents.'
            }
        }
        'GTAOLite' {
            Assert-ContainsLiteral $ReshadeText `
                '[ShenLong-SDAO] config: mode=GTAOLite quality=2 radius=0.500m strength=100%' `
                'ReShade.log'
            Assert-ContainsLiteral $ReshadeText `
                '[ShenLong-SDAO] Precompiled shader cache v1 loaded (mode=GTAOLite quality=2).' `
                'ReShade.log'
            $gtaoExtent = Assert-Matches $ReshadeText `
                '(?m)\[ShenLong-AO\] GTAO Lite active at (?<width>[1-9][0-9]*)x(?<height>[1-9][0-9]*); quality=2' `
                'ReShade.log'
            Assert-VerifiedRenderExtent `
                ([int] $gtaoExtent.Groups['width'].Value) `
                ([int] $gtaoExtent.Groups['height'].Value) `
                $verifiedExtents `
                'ShenLong GTAO Lite'
            Assert-ContainsLiteral $ReshadeText `
                'no stochastic capture overhead.' 'ReShade.log'
            if ($ReshadeText -match '\[ShenLong-SDAO\] stochastic capture active:' -or
                $ReshadeText -match '\[ShenLong-SDAO\] full-resolution SDAO active') {
                throw 'SDAO stochastic capture or composition ran during the GTAO Lite arm.'
            }
        }
        'Original' {
            Assert-ContainsLiteral $ReshadeText `
                '[ShenLong-SDAO] config: mode=Original quality=2 radius=0.500m strength=100%' `
                'ReShade.log'
            if ($ReshadeText -match '\[ShenLong-SDAO\] stochastic capture active:' -or
                $ReshadeText -match '\[ShenLong-SDAO\] full-resolution SDAO active' -or
                $ReshadeText -match '\[ShenLong-AO\] GTAO Lite active') {
                throw 'A replacement AO renderer ran during the Original AO arm.'
            }
            if ($ReshadeText -match
                    '\[ShenLong-Native\] AO-stage hook' -or
                $ReshadeText -match
                    '\[ShenLong-Native\] custom AO observed the verified high-quality live scheduler path') {
                throw 'The custom-AO scheduler was enabled during the Original AO arm.'
            }
        }
        default {
            throw "Unknown ambient-occlusion smoke mode: $AoMode"
        }
    }
    return [pscustomobject]@{
        ActiveWidth = $activeWidth
        ActiveHeight = $activeHeight
        SssReplayedSkinDraws =
            [int] $commonEvidence.SssDispatch.Groups['skin'].Value
    }
}

function Invoke-SmokeArm(
    [object] $Arm,
    [string] $EvidenceRoot,
    [datetime] $RunStart,
    [string] $BaseArmIniText,
    [string] $OriginalBaseIniHash,
    [string] $OriginalShenLongIniText,
    [string] $OriginalShenLongIniHash,
    [byte[]] $OriginalReShadeIniBytes,
    [string] $OriginalReShadeIniHash,
    [byte[]] $PackagedReShadeIniBytes,
    [string] $PackagedReShadeIniHash,
    [string] $DisplaySettingsPath,
    [string] $DisplaySettingsHash,
    [object[]] $GraphicsEntries,
    [string] $ExpectedSpatchHash,
    [string] $ExpectedLoaderHash,
    [string] $ExpectedAddonHash,
    [string] $ExpectedDxgiHash,
    [Collections.Generic.List[string]] $CleanupFailures) {
    $cleanupFailureCount = $CleanupFailures.Count
    if (-not (Stop-TaskOwnedSmokeProcesses $RunStart $gameExe)) {
        throw "A game process was active before the $($Arm.Name) smoke arm."
    }
    if ((Get-Hash $baseIni) -cne $OriginalBaseIniHash) {
        throw "SPatch.ini was not restored before the $($Arm.Name) smoke arm."
    }
    if ((Get-Hash $ini) -cne $OriginalShenLongIniHash) {
        throw "ShenLong.ini was not restored before the $($Arm.Name) smoke arm."
    }
    if ($originalPreviousIniExists) {
        if (-not (Test-Path -LiteralPath $previousIni -PathType Leaf) -or
            (Get-Hash $previousIni) -cne $originalPreviousIniHash) {
            throw "SPatch.ini.previous.bak was not restored before the $($Arm.Name) smoke arm."
        }
    } elseif (Test-Path -LiteralPath $previousIni) {
        throw "SPatch.ini.previous.bak should be absent before the $($Arm.Name) smoke arm."
    }
    if ((Get-Hash $reshadeIni) -cne $OriginalReShadeIniHash) {
        throw "ReShade.ini was not restored before the $($Arm.Name) smoke arm."
    }
    if ((Get-Hash $DisplaySettingsPath) -cne $DisplaySettingsHash) {
        throw "DisplaySettings.xml changed before the $($Arm.Name) smoke arm."
    }

    $armIniText = Set-SmokeArmIni $OriginalShenLongIniText $Arm
    $carCameraValue = if ($Arm.CarCameraEnabled) { '1' } else { '0' }
    $bikeCameraValue = if ($Arm.BikeCameraEnabled) { '1' } else { '0' }
    $armBaseIniText = Set-IniValueStrict `
        $BaseArmIniText 'Input' 'GTAIVCarCamera' $carCameraValue
    $armBaseIniText = Set-IniValueStrict `
        $armBaseIniText 'Input' 'GTAIVBikeCamera' $bikeCameraValue
    Assert-IniValue $armBaseIniText `
        'Input' 'GTAIVCarCamera' $carCameraValue
    Assert-IniValue $armBaseIniText `
        'Input' 'GTAIVBikeCamera' $bikeCameraValue
    $process = $null
    $launch = $null
    $armFailure = $null
    $armResult = $null
    $attemptDumpBefore = $null
    $dumpBeforeEvidence = $null
    $dumpAfterEvidence = $null
    $dumpFailureRecorded = $false
    $gameExitCode = $null
    try {
        Write-SPatchAtomicText `
            $GameRoot $baseIni $armBaseIniText $writeUtf8 `
            'SPatch.ini smoke diagnostics'
        Write-SPatchAtomicText `
            $GameRoot $ini $armIniText $writeUtf8 'ShenLong.ini smoke arm'
        Write-SPatchAtomicBytes `
            $GameRoot $reshadeIni $PackagedReShadeIniBytes `
            'ReShade.ini smoke arm'
        if ((Get-Hash $reshadeIni) -cne $PackagedReShadeIniHash) {
            throw 'Failed to install the packaged ReShade.ini for the smoke arm.'
        }
        foreach ($setting in $Arm.Settings) {
            Assert-IniValue $armIniText `
                $setting.Section $setting.Key $setting.Value
        }
        Assert-IniValue $armIniText `
            'AmbientOcclusion' 'AmbientOcclusion' $Arm.AoMode
        $expectedGiValue = if ($Arm.GiEnabled) { '1' } else { '0' }
        Assert-IniValue $armIniText `
            'GlobalIllumination' 'GlobalIllumination' $expectedGiValue

        $benchmarkAttempt = 0
        while ($true) {
            ++$benchmarkAttempt
            $spatchBefore = New-LogSnapshot $spatchLog
            $reshadeBefore = New-LogSnapshot $reshadeLog
            $benchmarkBefore = Get-BenchmarkResultSnapshot
            $attemptDumpBefore = Get-SPatchFileIdentitySnapshot `
                $GameRoot 'SPatch-*.dmp'
            $requestedUtc = [DateTime]::UtcNow
            $launch = Start-SmokeGame $requestedUtc
            $process = $launch.Process
            $processStartUtc = $process.StartTime.ToUniversalTime()
            $moduleEvidence = Wait-ForRuntimeReady `
                $process $ExpectedSpatchHash $ExpectedLoaderHash `
                $ExpectedAddonHash $ExpectedDxgiHash
            try {
                $result = Wait-ForBenchmarkResult `
                    $process $benchmarkBefore $requestedUtc
                $naturalExit = Wait-SPatchNaturalProcessExit `
                    $process $launch.ProcessIdentity 30
                if (-not $naturalExit.Exited) {
                    throw 'The task-owned final-smoke benchmark did not exit naturally within 30 seconds of its result.'
                }
                $gameExitCode = [int] $naturalExit.ExitCode
                if ($gameExitCode -notin @(0, 1)) {
                    throw "The final-smoke benchmark exited naturally with unexpected code $gameExitCode."
                }
                $resultFile = Get-Item -LiteralPath $result.File.FullName -Force
                $result.Identity = [pscustomobject]@{
                    Name = $resultFile.Name
                    Length = [int64] $resultFile.Length
                    Sha256 = Get-Hash $resultFile.FullName
                    LastWriteTimeUtc = $resultFile.LastWriteTimeUtc
                    FullName = $resultFile.FullName
                }
                if ($result.Identity.LastWriteTimeUtc -gt
                    [DateTime]::UtcNow.AddSeconds(5)) {
                    throw 'The completed final-smoke result timestamp is too far in the future.'
                }
                if ($benchmarkBefore.ContainsKey($result.Identity.Name)) {
                    $oldResultIdentity = $benchmarkBefore[$result.Identity.Name]
                    if ($oldResultIdentity.Length -eq $result.Identity.Length -and
                        $oldResultIdentity.Sha256 -ceq $result.Identity.Sha256) {
                        throw 'The completed final-smoke result reverted to its pre-attempt identity.'
                    }
                }
                $dumpBeforeEvidence = $attemptDumpBefore
                $dumpAfterEvidence = Get-SPatchFileIdentitySnapshot `
                    $GameRoot 'SPatch-*.dmp'
                $dumpChanges = @(Compare-SPatchFileIdentitySnapshot `
                    $dumpBeforeEvidence $dumpAfterEvidence)
                if ($dumpChanges.Count -ne 0) {
                    $dumpFailureRecorded = $true
                    throw ('The final smoke created or changed SPatch crash dumps: ' +
                        (($dumpChanges | ForEach-Object {
                                    "$($_.Kind):$($_.Name):$($_.After.Length):$($_.After.Sha256)"
                                }) -join '; '))
                }
                break
            } catch {
                $missingResult =
                    $_.Exception.Message -ceq `
                    'The game exited without producing a current benchmark result.'
                if (-not $missingResult -or $benchmarkAttempt -ge 2) {
                    throw
                }
                if (-not (Stop-TaskOwnedSmokeProcess `
                        $process $requestedUtc.ToLocalTime() $gameExe)) {
                    throw "Failed to clean up the result-less $($Arm.Name) benchmark attempt."
                }
                $attemptDumpAfter = Get-SPatchFileIdentitySnapshot `
                    $GameRoot 'SPatch-*.dmp'
                $attemptDumpChanges = @(Compare-SPatchFileIdentitySnapshot `
                    $attemptDumpBefore $attemptDumpAfter)
                if ($attemptDumpChanges.Count -ne 0) {
                    $dumpFailureRecorded = $true
                    throw ('The result-less final smoke created or changed SPatch crash dumps: ' +
                        (($attemptDumpChanges | ForEach-Object {
                                    "$($_.Kind):$($_.Name):$($_.After.Length):$($_.After.Sha256)"
                                }) -join '; '))
                }
                $process = $null
                Start-Sleep -Seconds 1
            }
        }
        if (-not (Stop-TaskOwnedSmokeProcesses $RunStart $gameExe)) {
            throw "A delayed task-owned game process remained after the $($Arm.Name) benchmark."
        }

        $armEvidenceRoot = Join-Path $EvidenceRoot $Arm.Name
        [void] (New-Item -ItemType Directory -Force -Path $armEvidenceRoot)
        $spatchEvidencePath = Join-Path $armEvidenceRoot 'SPatch.log'
        $reshadeEvidencePath = Join-Path $armEvidenceRoot 'ReShade.log'
        $benchmarkEvidencePath = Join-Path $armEvidenceRoot 'BenchmarkResult.xml'
        $benchmarkSummaryPath = Join-Path $armEvidenceRoot 'benchmark-summary.json'
        Copy-Item -LiteralPath $result.Identity.FullName `
            -Destination $benchmarkEvidencePath -Force
        $benchmark = Get-BenchmarkSummary (
            Get-Item -LiteralPath $benchmarkEvidencePath)
        $spatchText = Get-FreshLogSessionText `
            $spatchLog $spatchBefore $processStartUtc 'SPatch.log'
        $reshadeText = Get-FreshLogSessionText `
            $reshadeLog $reshadeBefore $processStartUtc 'ReShade.log'
        [IO.File]::WriteAllText($spatchEvidencePath, $spatchText, $writeUtf8)
        [IO.File]::WriteAllText($reshadeEvidencePath, $reshadeText, $writeUtf8)
        [IO.File]::WriteAllText(
            $benchmarkSummaryPath,
            ([ordered]@{
                    arm = $Arm.Name
                    renderer = $Arm.AoMode
                    ambient_occlusion = $Arm.AoMode
                    gi_enabled = $Arm.GiEnabled
                    pbr_enabled = $Arm.PbrEnabled
                    shadow_resolution = $Arm.ShadowResolution
                    gta_iv_car_camera = $Arm.CarCameraEnabled
                    gta_iv_bike_camera = $Arm.BikeCameraEnabled
                    average_fps = $benchmark.AverageFps
                    minimum_fps = $benchmark.MinimumFps
                    maximum_fps = $benchmark.MaximumFps
                } | ConvertTo-Json),
            $writeUtf8)
        Assert-NoSmokeFailures $spatchText $reshadeText
        Assert-SelectedDisplayPath $spatchText $DisplaySettingsPath
        $runtimeEvidence = Assert-ArmEvidence `
            $Arm.AoMode `
            $Arm.GiEnabled `
            $Arm.PbrEnabled `
            $Arm.ShadowResolution `
            $Arm.CarCameraEnabled `
            $Arm.BikeCameraEnabled `
            $spatchText `
            $reshadeText

        if ((Get-Hash $DisplaySettingsPath) -cne $DisplaySettingsHash) {
            throw "$($Arm.Name) changed DisplaySettings.xml during the benchmark."
        }
        Assert-FinalReleaseIdentity
        Assert-PackageTree $GraphicsEntries
        Assert-InstalledPackage $GraphicsEntries

        $armResult = [ordered]@{
            arm = $Arm.Name
            renderer = $Arm.AoMode
            ambient_occlusion = $Arm.AoMode
            gi_enabled = $Arm.GiEnabled
            pbr_enabled = $Arm.PbrEnabled
            shadow_resolution = $Arm.ShadowResolution
            gta_iv_car_camera = $Arm.CarCameraEnabled
            gta_iv_bike_camera = $Arm.BikeCameraEnabled
            process_id = $process.Id
            process_start_utc_ticks = $launch.ProcessIdentity.StartTimeUtcTicks
            launcher = $benchmarkShortcut
            game_exit_code = $gameExitCode
            loaded_spatch_path = $moduleEvidence.SpatchPath
            loaded_spatch_sha256 = $moduleEvidence.SpatchHash
            asi_loader_path = $moduleEvidence.LoaderPath
            asi_loader_sha256 = $moduleEvidence.LoaderHash
            loaded_graphics_addon_path = $moduleEvidence.AddonPath
            loaded_graphics_addon_sha256 = $moduleEvidence.AddonHash
            loaded_reshade_host_path = $moduleEvidence.DxgiPath
            loaded_reshade_host_sha256 = $moduleEvidence.DxgiHash
            loaded_native_d3d11_path = $moduleEvidence.NativeD3d11Path
            loaded_native_d3d11_sha256 = $moduleEvidence.NativeD3d11Hash
            loaded_system_dbghelp_path = $moduleEvidence.SystemDbghelpPath
            loaded_system_dbghelp_sha256 = $moduleEvidence.SystemDbghelpHash
            loaded_system_d3dcompiler_47_path = `
                $moduleEvidence.SystemD3dCompiler47Path
            loaded_system_d3dcompiler_47_sha256 = `
                $moduleEvidence.SystemD3dCompiler47Hash
            display_settings_path = $DisplaySettingsPath
            display_settings_sha256 = $DisplaySettingsHash
            benchmark_result = $benchmarkEvidencePath
            benchmark_result_name = $result.Identity.Name
            benchmark_result_length = $result.Identity.Length
            benchmark_result_sha256 = $result.Identity.Sha256
            evidence_directory = $armEvidenceRoot
            spatch_log_sha256 = Get-Hash $spatchEvidencePath
            reshade_log_sha256 = Get-Hash $reshadeEvidencePath
            average_fps = $benchmark.AverageFps
            minimum_fps = $benchmark.MinimumFps
            maximum_fps = $benchmark.MaximumFps
            smaa_preset = 3
            render_width = [int] $runtimeEvidence.ActiveWidth
            render_height = [int] $runtimeEvidence.ActiveHeight
            render_resolution =
                "$($runtimeEvidence.ActiveWidth)x$($runtimeEvidence.ActiveHeight)"
            agx_look = 'MediumHigh'
            sss_quality = 2
            sss_replayed_skin_draws =
                [int] $runtimeEvidence.SssReplayedSkinDraws
            precompiled_shader_cache = @(
                'SSS'
                'Water'
                if ($Arm.PbrEnabled) { 'PBR' }
                if ($Arm.GiEnabled) { 'GI' }
                if ($Arm.AoMode -ne 'Original') { 'SDAO' })
            benchmark_complete = $true
            benchmark_attempts = $benchmarkAttempt
            process_shutdown = 'natural_before_evidence'
            crash_dumps_before = @(
                Convert-SPatchSnapshotToEvidence $dumpBeforeEvidence)
            crash_dumps_after = @(
                Convert-SPatchSnapshotToEvidence $dumpAfterEvidence)
            new_or_changed_crash_dumps = 0
        }
    } catch {
        $armFailure = $_
    } finally {
        $cleanupFailed = $false
        if ($process) {
            try {
                $cleanupFailed = -not (Stop-TaskOwnedSmokeProcess `
                    $process $requestedUtc.ToLocalTime() $gameExe)
            } catch {
                $cleanupFailed = $true
                $CleanupFailures.Add(
                    "$($Arm.Name) owned-process cleanup failed: $($_.Exception.Message)")
            }
        }
        try {
            if (-not (Stop-TaskOwnedSmokeProcesses $RunStart $gameExe)) {
                $cleanupFailed = $true
            }
        } catch {
            $cleanupFailed = $true
            $CleanupFailures.Add(
                "$($Arm.Name) task-process cleanup failed: $($_.Exception.Message)")
        }
        if (@(Get-SPatchLiveGameProcesses).Count -ne 0) {
            $CleanupFailures.Add(
                "$($Arm.Name) configuration restoration refused while an unowned or resistant game process is live")
        } else {
            try {
                [void] (Restore-SPatchRecoveryBackup `
                    $GameRoot $userStateTargets -KeepBackup)
            } catch {
                $CleanupFailures.Add(
                    "$($Arm.Name) recovery-backed restoration failed: $($_.Exception.Message)")
            }
        }
        if ($cleanupFailed) {
            $CleanupFailures.Add(
                "$($Arm.Name) did not stop every task-owned game process")
        }
        if ($null -ne $attemptDumpBefore) {
            try {
                $finalDumpSnapshot = Get-SPatchFileIdentitySnapshot `
                    $GameRoot 'SPatch-*.dmp'
                $finalDumpChanges = @(Compare-SPatchFileIdentitySnapshot `
                    $attemptDumpBefore $finalDumpSnapshot)
                if ($finalDumpChanges.Count -ne 0 -and -not $dumpFailureRecorded) {
                    $CleanupFailures.Add(
                        "$($Arm.Name) created or changed crash dumps: " +
                        (($finalDumpChanges | ForEach-Object {
                                    "$($_.Kind):$($_.Name):$($_.After.Length):$($_.After.Sha256)"
                                }) -join '; '))
                }
            } catch {
                $CleanupFailures.Add(
                    "$($Arm.Name) crash-dump postflight failed: $($_.Exception.Message)")
            }
        }
    }
    if ($armFailure) {
        throw $armFailure
    }
    if ($CleanupFailures.Count -gt $cleanupFailureCount) {
        throw "The $($Arm.Name) smoke arm failed its cleanup barrier."
    }
    return $armResult
}

foreach ($path in @(
        $builtAsi,
        $finalReleaseIdentity,
        $sourceConfigHeader,
        $basePackageIni,
        $basePackageManifest,
        $basePackageArchive,
        $packageManifest)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Final-release validation prerequisite is missing: $path"
    }
}
Assert-X64PeFile $builtAsi 'FinalRelease SPatch.asi'
Assert-BaseReleasePackage
Assert-FinalReleaseIdentity
$declaredConfigVersion = Get-DeclaredConfigVersion
$stagedDefaultIni = Join-Path $RepoRoot `
    "artifacts\release-staging\SPatch-default-v$declaredConfigVersion.ini"
Assert-FileByteIdentity $stagedDefaultIni $basePackageIni `
    'Staged default SPatch.ini'
$baseDefaultText = [IO.File]::ReadAllText($basePackageIni, $strictUtf8)
if ($baseDefaultText.Length -gt 0 -and
    $baseDefaultText[0] -eq [char] 0xFEFF) {
    $baseDefaultText = $baseDefaultText.Substring(1)
}
Assert-IniValue $baseDefaultText 'SPatch' 'ConfigVersion' `
    ([string] $declaredConfigVersion)
Assert-IniValue $baseDefaultText 'Debug' 'WriteCrashDumps' '1'
Assert-IniValue $baseDefaultText 'Input' 'GTAIVCarCamera' '0'
Assert-IniValue $baseDefaultText 'Input' 'GTAIVBikeCamera' '0'
Assert-IniValue $baseDefaultText 'TextureFiltering' 'AnisotropicFiltering' '16'
Assert-IniValue $baseDefaultText 'TextureFiltering' 'ForceAnisotropicFiltering' '1'
if (-not $baseDefaultText.EndsWith('WriteCrashDumps=1',
        [StringComparison]::Ordinal)) {
    throw 'The packaged SPatch.ini must end with the default WriteCrashDumps=1 key.'
}

$graphicsEntries = @(Get-GraphicsManifestEntries)
$packageManifestHash = Get-Hash $packageManifest
Assert-PackageTree $graphicsEntries
Assert-ShenLongPackageArchive
Assert-PrecompiledShaderCachePackage $graphicsEntries
foreach ($identity in @(
        @((Join-Path $RepoRoot 'luma\README.md'),
          (Join-Path $packageRoot 'SHENLONG-README.md'), 'ShenLong README'),
        @((Join-Path $RepoRoot 'tools\Install-ShenLong.ps1'),
          (Join-Path $packageRoot 'Install-ShenLong.ps1'),
          'ShenLong package installer'),
        @((Join-Path $RepoRoot 'luma\THIRD_PARTY_NOTICES.md'),
          (Join-Path $packageRoot 'THIRD_PARTY_NOTICES.md'),
          'Graphics third-party notices'))) {
    Assert-FileByteIdentity $identity[0] $identity[1] $identity[2]
}
foreach ($licenseName in @(
        'DiligentFX-Apache-2.0.txt',
        'MinHook-BSD-2-Clause.txt',
        'ThreeJS-MIT.txt',
        'XeGTAO-MIT.txt')) {
    Assert-FileByteIdentity `
        (Join-Path $RepoRoot "luma\licenses\$licenseName") `
        (Join-Path $packageRoot "licenses\$licenseName") `
        "Graphics license $licenseName"
}
$packagedShenLongIniText = [IO.File]::ReadAllText(
    (Join-Path $packageRoot 'ShenLong.ini'), $strictUtf8)
if ($packagedShenLongIniText.Length -gt 0 -and
    $packagedShenLongIniText[0] -eq [char]0xFEFF) {
    $packagedShenLongIniText = $packagedShenLongIniText.Substring(1)
}
Assert-IniValue $packagedShenLongIniText 'ShenLong' 'ConfigVersion' '1'
$addonEntries = @(
    $graphicsEntries | Where-Object {
        $_.RelativePath -ieq 'ShenLong.asi'
    })
if ($addonEntries.Count -ne 1) {
    throw "Expected one ShenLong.asi manifest entry, found $($addonEntries.Count)."
}
$expectedAddonHash = $addonEntries[0].ExpectedHash
$dxgiEntries = @(
    $graphicsEntries | Where-Object {
        $_.RelativePath -ieq 'dxgi.dll'
    })
if ($dxgiEntries.Count -ne 1) {
    throw "Expected one dxgi.dll manifest entry, found $($dxgiEntries.Count)."
}
$expectedDxgiHash = $dxgiEntries[0].ExpectedHash
$reShadeEntries = @(
    $graphicsEntries | Where-Object {
        $_.RelativePath -ieq 'ReShade.ini'
    })
if ($reShadeEntries.Count -ne 1) {
    throw "Expected one ReShade.ini manifest entry, found $($reShadeEntries.Count)."
}
$packagedReShadeIni = Get-CheckedChildPath `
    $packageRoot 'ReShade.ini' 'Packaged ReShade configuration'
$packagedReShadeIniBytes = [IO.File]::ReadAllBytes($packagedReShadeIni)
try {
    $packagedReShadeIniText = $strictUtf8.GetString($packagedReShadeIniBytes)
} catch {
    throw "Packaged ReShade.ini is not valid UTF-8: $($_.Exception.Message)"
}
if ($packagedReShadeIniText.Length -gt 0 -and
    $packagedReShadeIniText[0] -eq [char] 0xFEFF) {
    $packagedReShadeIniText = $packagedReShadeIniText.Substring(1)
}
Assert-PackagedReShadeConfiguration $packagedReShadeIniText
$packagedReShadeIniHash = Get-Hash $packagedReShadeIni
if ($packagedReShadeIniHash -cne $reShadeEntries[0].ExpectedHash) {
    throw 'Packaged ReShade.ini does not match the graphics manifest.'
}

if ($ValidateOnly) {
    [pscustomobject]@{
        ConfigVersion = $declaredConfigVersion
        FinalReleaseAsiSha256 = Get-Hash $builtAsi
        BasePackageArchiveSha256 = Get-Hash $basePackageArchive
        GraphicsPackageArchiveSha256 = Get-Hash $packageArchive
        GraphicsPackageManifestSha256 = $packageManifestHash
        GraphicsManifestedFiles = $graphicsEntries.Count
        PrecompiledShaders = @($graphicsEntries | Where-Object {
                [IO.Path]::GetExtension($_.RelativePath) -ieq '.cso'
            }).Count
        PackagedReShadeIniSha256 = $packagedReShadeIniHash
        MutationPerformed = $false
    }
    return
}

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
$gogDisplaySettings = Join-Path $GameRoot 'Save\DisplaySettings.xml'
$resolvedLiveDisplaySettings = Resolve-DisplaySettingsPath
if ((Test-PathsEqual $resolvedLiveDisplaySettings $gogDisplaySettings) -or
    $liveGogMarkers.Count -ne 0) {
    throw 'The live final smoke is Steamworks-only; no live GOG launch path has been validated.'
}
if (Get-Process -Name sdhdship -ErrorAction SilentlyContinue) {
    throw 'A game process was active before the final smoke.'
}
$displaySettingsPath = $resolvedLiveDisplaySettings
$userStateTargets = @(Get-SPatchUserStateTargets `
    $GameRoot $displaySettingsPath)
[void] (Restore-SPatchRecoveryBackup $GameRoot $userStateTargets)
if (Test-Path -LiteralPath $previousIni) {
    throw 'Legacy SPatch.ini.previous.bak remains in the game directory; migrate it externally before final smoke.'
}
foreach ($path in @(
        $benchmarkShortcut,
        $gameExe,
        $installedAsi,
        $installedAddon,
        $installedDxgi,
        $installedManifest,
        $baseIni,
        $ini,
        $reshadeIni,
        $asiLoader)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Final-smoke prerequisite is missing: $path"
    }
}
Assert-X64PeFile $gameExe 'Game executable'
Assert-X64PeFile $asiLoader 'ASI loader'
if (Test-Path -LiteralPath (Join-Path $GameRoot 'SPatch.pdb') -PathType Leaf) {
    throw 'A diagnostic SPatch.pdb is installed beside the final artifact.'
}
$expectedSpatchHash = Get-Hash $builtAsi
$expectedLoaderHash = Get-Hash $asiLoader
if ((Get-Hash $installedAsi) -cne $expectedSpatchHash) {
    throw 'The installed ASI does not match the canonical FinalRelease build.'
}
Assert-InstalledPackage $graphicsEntries

$displaySettingsPath = Resolve-DisplaySettingsPath
if (-not (Test-Path -LiteralPath $displaySettingsPath -PathType Leaf)) {
    throw "The live DisplaySettings.xml is missing: $displaySettingsPath"
}
$displaySettingsBytes = [IO.File]::ReadAllBytes($displaySettingsPath)
$displaySettingsHash = Get-Hash $displaySettingsPath

$originalBaseIniBytes = [IO.File]::ReadAllBytes($baseIni)
$originalBaseIniHash = Get-Hash $baseIni
$originalIniBytes = [IO.File]::ReadAllBytes($ini)
$originalIniHash = Get-Hash $ini
$originalPreviousIniExists = Test-Path -LiteralPath $previousIni -PathType Leaf
$originalPreviousIniBytes = if ($originalPreviousIniExists) {
    [IO.File]::ReadAllBytes($previousIni)
} else {
    [byte[]]::new(0)
}
$originalPreviousIniHash = if ($originalPreviousIniExists) {
    Get-Hash $previousIni
} else {
    ''
}
$originalReShadeIniBytes = [IO.File]::ReadAllBytes($reshadeIni)
$originalReShadeIniHash = Get-Hash $reshadeIni
try {
    $baseIniOffset = 0
    if ($originalBaseIniBytes.Length -ge 3 -and
        $originalBaseIniBytes[0] -eq 0xEF -and
        $originalBaseIniBytes[1] -eq 0xBB -and
        $originalBaseIniBytes[2] -eq 0xBF) {
        $baseIniOffset = 3
    }
    $originalBaseIniText = $strictUtf8.GetString(
        $originalBaseIniBytes,
        $baseIniOffset,
        $originalBaseIniBytes.Length - $baseIniOffset)
} catch {
    throw "SPatch.ini is not valid UTF-8: $($_.Exception.Message)"
}
try {
    $originalReShadeIniText = $strictUtf8.GetString($originalReShadeIniBytes)
} catch {
    throw "Existing ReShade.ini is not valid UTF-8: $($_.Exception.Message)"
}
[void](Assert-ReShadeRootAddonPolicy `
    $originalReShadeIniText 'Existing ReShade.ini')
try {
    $iniOffset = 0
    if ($originalIniBytes.Length -ge 3 -and
        $originalIniBytes[0] -eq 0xEF -and
        $originalIniBytes[1] -eq 0xBB -and
        $originalIniBytes[2] -eq 0xBF) {
        $iniOffset = 3
    }
    $originalIniText = $strictUtf8.GetString(
        $originalIniBytes,
        $iniOffset,
        $originalIniBytes.Length - $iniOffset)
} catch {
    throw "ShenLong.ini is not valid UTF-8: $($_.Exception.Message)"
}
Assert-IniValue $originalBaseIniText 'SPatch' 'ConfigVersion' `
    ([string] $declaredConfigVersion)
foreach ($requiredKey in @(
        [pscustomobject]@{ Section = 'SPatch'; Key = 'Enabled' },
        [pscustomobject]@{ Section = 'Debug'; Key = 'Logging' },
        [pscustomobject]@{ Section = 'Debug'; Key = 'WriteCrashDumps' },
        [pscustomobject]@{ Section = 'Input'; Key = 'GTAIVCarCamera' },
        [pscustomobject]@{ Section = 'Input'; Key = 'GTAIVBikeCamera' },
        [pscustomobject]@{ Section = 'TextureFiltering'; Key = 'AnisotropicFiltering' },
        [pscustomobject]@{ Section = 'TextureFiltering'; Key = 'ForceAnisotropicFiltering' },
        [pscustomobject]@{ Section = 'AntiAliasing'; Key = 'SMAA' },
        [pscustomobject]@{ Section = 'AntiAliasing'; Key = 'SMAAPreset' })) {
    [void] (Get-IniValueStrict `
        $originalBaseIniText $requiredKey.Section $requiredKey.Key)
}
foreach ($requiredKey in @(
        [pscustomobject]@{ Section = 'ShenLong'; Key = 'Enabled' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgX' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgXLook' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgXStrength' },
        [pscustomobject]@{ Section = 'Tonemapping'; Key = 'AgXExposure' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'SDAOQuality' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'SDAORadius' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'SDAOStrength' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'GTAOLiteQuality' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'GTAOLiteRadius' },
        [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'GTAOLiteStrength' },
        [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination' },
        [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GIQuality' },
        [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'SubsurfaceScattering' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'StockHairBlur' },
        [pscustomobject]@{ Section = 'SubsurfaceScattering'; Key = 'SSSQuality' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'EyeScattering' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'HairScattering' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'TeethScattering' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'FoliageTransmission' },
        [pscustomobject]@{ Section = 'MaterialScattering'; Key = 'WaterScattering' },
        [pscustomobject]@{ Section = 'Shadows'; Key = 'ShadowResolution' })) {
    [void] (Get-IniValueStrict `
        $originalIniText $requiredKey.Section $requiredKey.Key)
}
Assert-IniValue $originalIniText 'ShenLong' 'ConfigVersion' '1'
if ($originalIniText -match '(?im)^\s*\[TextureFiltering\]\s*$') {
    throw 'ShenLong.ini still contains the SPatch-owned TextureFiltering section.'
}

$arms = @(
    [pscustomobject]@{
        Name = 'original-ao-gi-off-shadow-2048'
        AoMode = 'Original'
        GiEnabled = $false
        PbrEnabled = $true
        ShadowResolution = 2048
        CarCameraEnabled = $true
        BikeCameraEnabled = $false
        Settings = @(
            [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering'; Value = '1' },
            [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination'; Value = '0' },
            [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion'; Value = 'Original' })
    },
    [pscustomobject]@{
        Name = 'original-ao-gi-on-pbr-native-control'
        AoMode = 'Original'
        GiEnabled = $true
        PbrEnabled = $false
        ShadowResolution = 0
        CarCameraEnabled = $false
        BikeCameraEnabled = $true
        Settings = @(
            [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering'; Value = '0' },
            [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination'; Value = '1' },
            [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion'; Value = 'Original' })
    },
    [pscustomobject]@{
        Name = 'sdao-gi-off'
        AoMode = 'SDAO'
        GiEnabled = $false
        PbrEnabled = $true
        ShadowResolution = 0
        CarCameraEnabled = $true
        BikeCameraEnabled = $true
        Settings = @(
            [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering'; Value = '1' },
            [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination'; Value = '0' },
            [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion'; Value = 'SDAO' })
    },
    [pscustomobject]@{
        Name = 'sdao-gi-on'
        AoMode = 'SDAO'
        GiEnabled = $true
        PbrEnabled = $true
        ShadowResolution = 0
        CarCameraEnabled = $false
        BikeCameraEnabled = $false
        Settings = @(
            [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering'; Value = '1' },
            [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination'; Value = '1' },
            [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion'; Value = 'SDAO' })
    },
    [pscustomobject]@{
        Name = 'gtao-lite-gi-off'
        AoMode = 'GTAOLite'
        GiEnabled = $false
        PbrEnabled = $true
        ShadowResolution = 0
        CarCameraEnabled = $true
        BikeCameraEnabled = $true
        Settings = @(
            [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering'; Value = '1' },
            [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination'; Value = '0' },
            [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion'; Value = 'GTAOLite' })
    },
    [pscustomobject]@{
        Name = 'gtao-lite-gi-on'
        AoMode = 'GTAOLite'
        GiEnabled = $true
        PbrEnabled = $true
        ShadowResolution = 0
        CarCameraEnabled = $true
        BikeCameraEnabled = $true
        Settings = @(
            [pscustomobject]@{ Section = 'PhysicallyBasedRendering'; Key = 'PhysicallyBasedRendering'; Value = '1' },
            [pscustomobject]@{ Section = 'GlobalIllumination'; Key = 'GlobalIllumination'; Value = '1' },
            [pscustomobject]@{ Section = 'AmbientOcclusion'; Key = 'AmbientOcclusion'; Value = 'GTAOLite' })
    })

$expectedAoGiMatrix = @(
    'GTAOLite|0',
    'GTAOLite|1',
    'Original|0',
    'Original|1',
    'SDAO|0',
    'SDAO|1')
$actualAoGiMatrix = @(
    $arms | ForEach-Object {
        $giMarker = if ($_.GiEnabled) { 1 } else { 0 }
        '{0}|{1}' -f $_.AoMode, $giMarker
    } | Sort-Object -Unique)
if ($arms.Count -ne 6 -or
    $actualAoGiMatrix.Count -ne $expectedAoGiMatrix.Count -or
    @(Compare-Object `
        -ReferenceObject $expectedAoGiMatrix `
        -DifferenceObject $actualAoGiMatrix `
        -CaseSensitive).Count -ne 0) {
    throw 'Final smoke must cover each Original/SDAO/GTAOLite and GI off/on combination exactly once.'
}
$expectedCameraMatrix = @('0|0', '0|1', '1|0', '1|1')
# The benchmark has no drivable vehicle scene. These arms prove installation of
# the setter, Update, desired-pose, and angular-approach hooks plus the startup
# setter/state path; gameplay remains the behavioral and visual gate.
$actualCameraMatrix = @(
    $arms | ForEach-Object {
        $carMarker = if ($_.CarCameraEnabled) { 1 } else { 0 }
        $bikeMarker = if ($_.BikeCameraEnabled) { 1 } else { 0 }
        '{0}|{1}' -f $carMarker, $bikeMarker
    } | Sort-Object -Unique)
if ($actualCameraMatrix.Count -ne $expectedCameraMatrix.Count -or
    @(Compare-Object `
        -ReferenceObject $expectedCameraMatrix `
        -DifferenceObject $actualCameraMatrix `
        -CaseSensitive).Count -ne 0) {
    throw 'Final smoke must cover all car-camera and bike-camera enablement combinations.'
}
$shadowProofArms = @($arms | Where-Object { $_.ShadowResolution -eq 2048 })
$shadowControlArms = @($arms | Where-Object { $_.ShadowResolution -eq 0 })
if ($shadowProofArms.Count -ne 1 -or
    $shadowControlArms.Count -ne $arms.Count - 1) {
    throw 'Final smoke must contain one 2048 shadow proof and stock-shadow controls for every other arm.'
}

$results = @()
$taskStartedSteamProcesses = [Collections.Generic.Dictionary[int, object]]::new()
$smokeStart = Get-Date
$smokeEvidenceRoot = Join-Path (
    Join-Path $RepoRoot 'artifacts\runtime-logs') (
        'final-smoke-{0}-{1}' -f $smokeStart.ToString('yyyyMMdd-HHmmss'), $PID)
[void] (New-Item -ItemType Directory -Force -Path $smokeEvidenceRoot)
$smokeFailure = $null
$cleanupFailures = [Collections.Generic.List[string]]::new()
$recovery = New-SPatchRecoveryBackup `
    $GameRoot $userStateTargets 'Invoke-FinalSmoke.ps1'
$baseArmIniText = Set-BaseSmokeIni $originalBaseIniText
try {
    foreach ($arm in $arms) {
        $results += Invoke-SmokeArm `
            $arm `
            $smokeEvidenceRoot `
            $smokeStart `
            $baseArmIniText `
            $originalBaseIniHash `
            $originalIniText `
            $originalIniHash `
            $originalReShadeIniBytes `
            $originalReShadeIniHash `
            $packagedReShadeIniBytes `
            $packagedReShadeIniHash `
            $displaySettingsPath `
            $displaySettingsHash `
            $graphicsEntries `
            $expectedSpatchHash `
            $expectedLoaderHash `
            $expectedAddonHash `
            $expectedDxgiHash `
            $cleanupFailures
    }
} catch {
    $smokeFailure = $_
} finally {
    try {
        $taskProcesses = @(
            Get-Process -Name sdhdship -ErrorAction SilentlyContinue |
                Where-Object {
                    Test-TaskOwnedSmokeProcess $_ $smokeStart $gameExe
                })
    } catch {
        $taskProcesses = @()
        $cleanupFailures.Add(
            "Top-level owned-process discovery failed: $($_.Exception.Message)")
    }
    foreach ($taskProcess in $taskProcesses) {
        try {
            if (-not (Stop-TaskOwnedSmokeProcess `
                    $taskProcess $smokeStart $gameExe)) {
                $cleanupFailures.Add(
                    "Top-level cleanup could not stop owned game PID $($taskProcess.Id).")
            }
        } catch {
            $cleanupFailures.Add(
                "Top-level cleanup failed for owned game PID $($taskProcess.Id): $($_.Exception.Message)")
        }
    }
    try {
        if (-not (Stop-TaskOwnedSmokeProcesses $smokeStart $gameExe)) {
            $cleanupFailures.Add(
                'Top-level cleanup found an unowned or resistant game process.')
        }
    } catch {
        $cleanupFailures.Add(
            "Top-level game cleanup failed: $($_.Exception.Message)")
    }
    try {
        if (-not (Stop-TaskStartedSteamProcesses $smokeStart)) {
            $cleanupFailures.Add(
                'Top-level cleanup could not stop every task-started Steam process.')
        }
    } catch {
        $cleanupFailures.Add(
            "Top-level Steam cleanup failed: $($_.Exception.Message)")
    }
    if (@(Get-SPatchLiveGameProcesses).Count -ne 0) {
        $cleanupFailures.Add(
            'Top-level configuration restoration refused while an unowned or resistant game process is live.')
    } else {
        try {
            [void] (Restore-SPatchRecoveryBackup `
                $GameRoot $userStateTargets -KeepBackup)
        } catch {
            $cleanupFailures.Add(
                "Top-level recovery-backed restoration failed: $($_.Exception.Message)")
        }
    }
    try {
        Complete-SPatchRecoveryBackup $GameRoot $userStateTargets
    } catch {
        $cleanupFailures.Add(
            "Top-level recovery completion failed: $($_.Exception.Message)")
    }
    try {
        Assert-PackageTree $graphicsEntries
        if ((Get-Hash $packageManifest) -cne $packageManifestHash) {
            throw 'Publishing-Release SHA256SUMS.txt changed during the smoke.'
        }
    } catch {
        $cleanupFailures.Add(
            "Top-level package postflight failed: $($_.Exception.Message)")
    }
}

if ($smokeFailure -or $cleanupFailures.Count -ne 0) {
    $messages = [Collections.Generic.List[string]]::new()
    if ($smokeFailure) {
        $messages.Add('Primary failure: ' + $smokeFailure.Exception.Message)
    }
    foreach ($cleanupFailure in $cleanupFailures) {
        $messages.Add('Cleanup failure: ' + $cleanupFailure)
    }
    throw [InvalidOperationException]::new(
        ('Final smoke failed after best-effort cleanup. ' +
            ($messages -join ' | ')),
        $(if ($smokeFailure) { $smokeFailure.Exception } else { $null }))
}

$finalSummaryPath = Join-Path $smokeEvidenceRoot 'summary.json'
$finalSummary = [ordered]@{
    aggregate_pass = $true
    final_release_asi_sha256 = $expectedSpatchHash
    final_release_identity_sha256 = Get-Hash $finalReleaseIdentity
    supported_game_path = $gameExe
    supported_game_sha256 = Get-Hash $gameExe
    asi_loader_path = $asiLoader
    asi_loader_sha256 = $expectedLoaderHash
    graphics_addon_sha256 = Get-Hash $installedAddon
    reshade_host_sha256 = Get-Hash $installedDxgi
    graphics_archive_sha256 = Get-Hash $packageArchive
    graphics_manifest_sha256 = $packageManifestHash
    original_spatch_config_sha256 = $originalBaseIniHash
    original_shenlong_config_sha256 = $originalIniHash
    original_previous_config_exists = $originalPreviousIniExists
    original_previous_config_sha256 = $originalPreviousIniHash
    original_reshade_config_sha256 = $originalReShadeIniHash
    smoke_reshade_config_sha256 = $packagedReShadeIniHash
    display_settings_path = $displaySettingsPath
    display_settings_sha256 = $displaySettingsHash
    expected_resolution = "${ExpectedWidth}x${ExpectedHeight}"
    evidence_directory = $smokeEvidenceRoot
    launcher = $benchmarkShortcut
    exact_shortcut_launch_enforced = $true
    write_crash_dumps_forced = $true
    summary_path = $finalSummaryPath
    arms = $results
    exact_user_files_restored = $true
    config_restored = $true
    reshade_config_restored = $true
    display_settings_restored = $true
    publishing_package_unchanged = $true
}
$finalSummaryJson = $finalSummary | ConvertTo-Json -Depth 7
[IO.File]::WriteAllText($finalSummaryPath, $finalSummaryJson, $writeUtf8)
$finalSummaryJson
} finally {
    if ($ownsLiveHarnessMutex) {
        $liveHarnessMutex.ReleaseMutex()
    }
    $liveHarnessMutex.Dispose()
}
