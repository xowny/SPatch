Set-StrictMode -Version Latest

$script:SPatchLiveHarnessSafetyVersion = '2026.08.10.6'
$script:SPatchLiveHarnessSafetyPath = [IO.Path]::GetFullPath($PSCommandPath)
$script:SPatchLiveHarnessMutexName = 'Local\SPatch.LiveGraphicsHarness'
$script:SPatchBenchmarkShortcutPath =
    'C:\Users\Admin\Desktop\Sleeping Dogs DE - Unattended Benchmark.lnk'
$script:SPatchRecoveryDirectoryName = '.spatch-live-harness-recovery'

function Test-SPatchPathEqual(
    [string] $Left,
    [string] $Right) {
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

function Get-SPatchSha256([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Assert-SPatchMutationPathSafe(
    [string] $Root,
    [string] $Path,
    [string] $Label = 'Mutation path') {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $pathFull = [IO.Path]::GetFullPath($Path)
    $rootPrefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $pathFull.Equals(
            $rootFull, [StringComparison]::OrdinalIgnoreCase) -and
        -not $pathFull.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its allowed root: $pathFull"
    }

    $relative = if ($pathFull.Equals(
            $rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        ''
    } else {
        $pathFull.Substring($rootPrefix.Length)
    }
    $components = [Collections.Generic.List[string]]::new()
    $components.Add($rootFull)
    $current = $rootFull
    foreach ($component in @($relative -split '[\\/]' | Where-Object {
                -not [string]::IsNullOrWhiteSpace($_)
            })) {
        $current = Join-Path $current $component
        $components.Add($current)
    }
    foreach ($componentPath in $components) {
        if (-not (Test-Path -LiteralPath $componentPath)) {
            continue
        }
        $item = Get-Item -LiteralPath $componentPath -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Label contains a reparse point and cannot be mutated: $componentPath"
        }
    }
    return $pathFull
}

function Write-SPatchAtomicBytes(
    [string] $Root,
    [string] $Path,
    [byte[]] $Bytes,
    [string] $Label = 'File') {
    $pathFull = Assert-SPatchMutationPathSafe $Root $Path $Label
    $parent = Split-Path -Parent $pathFull
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "$Label parent directory does not exist: $parent"
    }
    [void] (Assert-SPatchMutationPathSafe $Root $parent "$Label parent")
    $temporary = Join-Path $parent (
        '.{0}.spatch-{1}-{2}.tmp' -f
            [IO.Path]::GetFileName($pathFull), $PID,
            [Guid]::NewGuid().ToString('N'))
    $replacementBackup = Join-Path $parent (
        '.{0}.spatch-replaced-{1}-{2}.tmp' -f
            [IO.Path]::GetFileName($pathFull), $PID,
            [Guid]::NewGuid().ToString('N'))
    [void] (Assert-SPatchMutationPathSafe $Root $temporary "$Label temporary file")
    [void] (Assert-SPatchMutationPathSafe `
        $Root $replacementBackup "$Label replacement backup")
    $stream = $null
    try {
        $stream = [IO.FileStream]::new(
            $temporary,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None,
            4096,
            [IO.FileOptions]::WriteThrough)
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        if (Test-Path -LiteralPath $pathFull -PathType Leaf) {
            [IO.File]::Replace(
                $temporary, $pathFull, $replacementBackup, $true)
        } elseif (Test-Path -LiteralPath $pathFull) {
            throw "$Label target is not a regular file: $pathFull"
        } else {
            [IO.File]::Move($temporary, $pathFull)
        }
    } finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $replacementBackup -PathType Leaf) {
            Remove-Item -LiteralPath $replacementBackup -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Write-SPatchAtomicText(
    [string] $Root,
    [string] $Path,
    [string] $Text,
    [Text.Encoding] $Encoding = [Text.UTF8Encoding]::new($false),
    [string] $Label = 'File') {
    Write-SPatchAtomicBytes $Root $Path ($Encoding.GetBytes($Text)) $Label
}

function Get-SPatchUserStateTargets(
    [string] $GameRoot,
    [string] $DisplaySettingsPath) {
    return @(
        [pscustomobject]@{
            Label = 'SPatch.ini'
            RelativePath = 'SPatch.ini'
            Path = Join-Path $GameRoot 'SPatch.ini'
        }
        [pscustomobject]@{
            Label = 'SPatch.ini.previous.bak'
            RelativePath = 'SPatch.ini.previous.bak'
            Path = Join-Path $GameRoot 'SPatch.ini.previous.bak'
        }
        [pscustomobject]@{
            Label = 'ShenLong.ini'
            RelativePath = 'ShenLong.ini'
            Path = Join-Path $GameRoot 'ShenLong.ini'
        }
        [pscustomobject]@{
            Label = 'ReShade.ini'
            RelativePath = 'ReShade.ini'
            Path = Join-Path $GameRoot 'ReShade.ini'
        }
        [pscustomobject]@{
            Label = 'DisplaySettings.xml'
            RelativePath = ([IO.Path]::GetFullPath($DisplaySettingsPath)).Substring(
                [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/').Length + 1)
            Path = $DisplaySettingsPath
        }
    )
}

function Get-SPatchLiveGameProcesses {
    return @(Get-Process -Name sdhdship -ErrorAction SilentlyContinue)
}

function Assert-SPatchNoLiveGame([string] $Context) {
    $processes = @(Get-SPatchLiveGameProcesses)
    if ($processes.Count -ne 0) {
        $identities = @($processes | ForEach-Object {
                try {
                    'pid={0} start={1:o} path={2}' -f
                        $_.Id, $_.StartTime.ToUniversalTime(), $_.Path
                } catch {
                    'pid={0} identity=unavailable' -f $_.Id
                }
            })
        throw "$Context refused while a game process is live: $($identities -join '; ')"
    }
}

function Get-SPatchRecoveryRoot([string] $GameRoot) {
    return [IO.Path]::GetFullPath(
        (Join-Path $GameRoot $script:SPatchRecoveryDirectoryName))
}

function Remove-SPatchSafeTree(
    [string] $GameRoot,
    [string] $Path,
    [string] $Label) {
    $full = Assert-SPatchMutationPathSafe $GameRoot $Path $Label
    $rootFull = [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/')
    if ($full.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label cannot remove the game root."
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
    }
}

function Read-SPatchRecoveryManifest(
    [string] $GameRoot,
    [object[]] $Targets) {
    $recoveryRoot = Get-SPatchRecoveryRoot $GameRoot
    if (-not (Test-Path -LiteralPath $recoveryRoot -PathType Container)) {
        return $null
    }
    [void] (Assert-SPatchMutationPathSafe $GameRoot $recoveryRoot 'Recovery directory')
    $manifestPath = Join-Path $recoveryRoot 'recovery.json'
    [void] (Assert-SPatchMutationPathSafe $GameRoot $manifestPath 'Recovery manifest')
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Recovery directory has no committed manifest: $recoveryRoot"
    }
    try {
        $manifest = [IO.File]::ReadAllText(
            $manifestPath, [Text.UTF8Encoding]::new($false, $true)) |
            ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "Recovery manifest is invalid: $($_.Exception.Message)"
    }
    if ([int] $manifest.schema -ne 1 -or
        -not (Test-SPatchPathEqual ([string] $manifest.game_root) $GameRoot)) {
        throw 'Recovery manifest schema or game-root identity is invalid.'
    }
    $entries = @($manifest.targets)
    if ($entries.Count -ne $Targets.Count) {
        throw "Recovery manifest target count is invalid: $($entries.Count)"
    }
    $expectedByLabel = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::Ordinal)
    $expectedBackupByLabel =
        [Collections.Generic.Dictionary[string, string]]::new(
            [StringComparer]::Ordinal)
    for ($targetIndex = 0; $targetIndex -lt $Targets.Count; ++$targetIndex) {
        $target = $Targets[$targetIndex]
        if ($expectedByLabel.ContainsKey([string] $target.Label)) {
            throw "Recovery target label is duplicated: $($target.Label)"
        }
        $expectedByLabel.Add([string] $target.Label, $target)
        $expectedBackupByLabel.Add(
            [string] $target.Label, ('{0}.bin' -f $targetIndex))
        [void] (Assert-SPatchMutationPathSafe `
            $GameRoot ([string] $target.Path) ([string] $target.Label))
    }
    $seenManifestLabels = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($entry in $entries) {
        $label = [string] $entry.label
        if (-not $expectedByLabel.ContainsKey($label)) {
            throw "Recovery manifest contains an unexpected target: $label"
        }
        if (-not $seenManifestLabels.Add($label)) {
            throw "Recovery manifest target label is duplicated: $label"
        }
        $target = $expectedByLabel[$label]
        if ([string] $entry.relative_path -cne [string] $target.RelativePath) {
            throw "Recovery relative path differs for $label."
        }
        if ([bool] $entry.existed) {
            if ([string] $entry.backup -cne $expectedBackupByLabel[$label] -or
                [string] $entry.sha256 -cnotmatch '^[0-9A-F]{64}$' -or
                [int64] $entry.length -lt 0) {
                throw "Recovery identity is invalid for $label."
            }
            $backupPath = Join-Path $recoveryRoot ([string] $entry.backup)
            [void] (Assert-SPatchMutationPathSafe $GameRoot $backupPath "$label backup")
            if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
                throw "Recovery backup is missing for $label."
            }
            $backup = Get-Item -LiteralPath $backupPath -Force
            if ($backup.Length -ne [int64] $entry.length -or
                (Get-SPatchSha256 $backupPath) -cne [string] $entry.sha256) {
                throw "Recovery backup identity differs for $label."
            }
        } elseif (-not [string]::IsNullOrEmpty([string] $entry.backup) -or
            [int64] $entry.length -ne 0 -or
            -not [string]::IsNullOrEmpty([string] $entry.sha256)) {
            throw "Absent recovery target has backup data: $label"
        }
    }
    if ($seenManifestLabels.Count -ne $expectedByLabel.Count) {
        $missingLabels = @($expectedByLabel.Keys | Where-Object {
                -not $seenManifestLabels.Contains($_)
            })
        throw ('Recovery manifest omits expected target(s): ' +
            ($missingLabels -join ', '))
    }
    return [pscustomobject]@{
        Root = $recoveryRoot
        Manifest = $manifest
        TargetsByLabel = $expectedByLabel
    }
}

function New-SPatchRecoveryBackup(
    [string] $GameRoot,
    [object[]] $Targets,
    [string] $HarnessName) {
    Assert-SPatchNoLiveGame 'Pre-mutation recovery backup creation'
    $recoveryRoot = Get-SPatchRecoveryRoot $GameRoot
    if (Test-Path -LiteralPath $recoveryRoot) {
        throw "A pending recovery backup must be restored before mutation: $recoveryRoot"
    }
    foreach ($stale in @(Get-ChildItem -LiteralPath $GameRoot -Directory -Force `
            -Filter ($script:SPatchRecoveryDirectoryName + '.staging-*') `
            -ErrorAction SilentlyContinue)) {
        Remove-SPatchSafeTree $GameRoot $stale.FullName 'Stale recovery staging directory'
    }
    $stage = Join-Path $GameRoot (
        $script:SPatchRecoveryDirectoryName + '.staging-' +
            [Guid]::NewGuid().ToString('N'))
    [void] (Assert-SPatchMutationPathSafe $GameRoot $stage 'Recovery staging directory')
    [void] (New-Item -ItemType Directory -Path $stage -ErrorAction Stop)
    try {
        $entries = [Collections.Generic.List[object]]::new()
        for ($index = 0; $index -lt $Targets.Count; ++$index) {
            $target = $Targets[$index]
            $path = Assert-SPatchMutationPathSafe `
                $GameRoot ([string] $target.Path) ([string] $target.Label)
            $exists = Test-Path -LiteralPath $path -PathType Leaf
            if (-not $exists -and (Test-Path -LiteralPath $path)) {
                throw "$($target.Label) is not a regular file: $path"
            }
            $backupName = if ($exists) { '{0}.bin' -f $index } else { '' }
            $length = [int64] 0
            $sha256 = ''
            if ($exists) {
                $bytes = [IO.File]::ReadAllBytes($path)
                $length = [int64] $bytes.Length
                $backupPath = Join-Path $stage $backupName
                Write-SPatchAtomicBytes $stage $backupPath $bytes "$($target.Label) recovery backup"
                $sha256 = Get-SPatchSha256 $backupPath
                if ((Get-SPatchSha256 $path) -cne $sha256) {
                    throw "$($target.Label) changed while its recovery backup was captured."
                }
            }
            $entries.Add([ordered]@{
                    label = [string] $target.Label
                    relative_path = [string] $target.RelativePath
                    existed = [bool] $exists
                    backup = $backupName
                    length = $length
                    sha256 = $sha256
                })
        }
        $manifest = [ordered]@{
            schema = 1
            harness = $HarnessName
            game_root = [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/')
            created_utc = [DateTime]::UtcNow.ToString('o')
            targets = @($entries.ToArray())
        }
        $manifestPath = Join-Path $stage 'recovery.json'
        Write-SPatchAtomicText $stage $manifestPath `
            (($manifest | ConvertTo-Json -Depth 5) + [Environment]::NewLine) `
            ([Text.UTF8Encoding]::new($false)) 'Recovery manifest'
        [IO.Directory]::Move($stage, $recoveryRoot)
    } finally {
        if (Test-Path -LiteralPath $stage) {
            Remove-SPatchSafeTree $GameRoot $stage 'Recovery staging directory'
        }
    }
    return Read-SPatchRecoveryManifest $GameRoot $Targets
}

function Restore-SPatchRecoveryBackup(
    [string] $GameRoot,
    [object[]] $Targets,
    [switch] $KeepBackup) {
    $recovery = Read-SPatchRecoveryManifest $GameRoot $Targets
    if ($null -eq $recovery) {
        return $false
    }
    Assert-SPatchNoLiveGame 'Recovery restoration'
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($entry in @($recovery.Manifest.targets)) {
        try {
            Assert-SPatchNoLiveGame "Recovery restoration of $($entry.label)"
            $target = $recovery.TargetsByLabel[[string] $entry.label]
            $targetPath = Assert-SPatchMutationPathSafe `
                $GameRoot ([string] $target.Path) ([string] $target.Label)
            if ([bool] $entry.existed) {
                $backupPath = Join-Path $recovery.Root ([string] $entry.backup)
                $bytes = [IO.File]::ReadAllBytes($backupPath)
                Write-SPatchAtomicBytes `
                    $GameRoot $targetPath $bytes ([string] $target.Label)
            } elseif (Test-Path -LiteralPath $targetPath) {
                if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
                    throw "$($target.Label) restoration target is not a regular file."
                }
                Remove-Item -LiteralPath $targetPath -Force -ErrorAction Stop
            }
        } catch {
            $failures.Add("$($entry.label): $($_.Exception.Message)")
            if (@(Get-SPatchLiveGameProcesses).Count -ne 0) {
                break
            }
        }
    }
    if (@(Get-SPatchLiveGameProcesses).Count -ne 0) {
        $failures.Add(
            'independent verification skipped because a game process became live')
    } else {
        foreach ($entry in @($recovery.Manifest.targets)) {
            try {
                $target = $recovery.TargetsByLabel[[string] $entry.label]
                if ([bool] $entry.existed) {
                    if (-not (Test-Path -LiteralPath $target.Path -PathType Leaf)) {
                        throw "$($target.Label) was not restored."
                    }
                    $item = Get-Item -LiteralPath $target.Path -Force
                    if ($item.Length -ne [int64] $entry.length -or
                        (Get-SPatchSha256 $target.Path) -cne [string] $entry.sha256) {
                        throw "$($target.Label) restored identity differs from recovery."
                    }
                } elseif (Test-Path -LiteralPath $target.Path) {
                    throw "$($target.Label) existed after absent-state restoration."
                }
            } catch {
                $failures.Add("$($entry.label) verification: $($_.Exception.Message)")
            }
        }
    }
    if ($failures.Count -eq 0 -and -not $KeepBackup) {
        Remove-SPatchSafeTree $GameRoot $recovery.Root 'Completed recovery directory'
    }
    if ($failures.Count -ne 0) {
        throw ('Recovery restoration failed: ' + ($failures -join ' | '))
    }
    return $true
}

function Complete-SPatchRecoveryBackup(
    [string] $GameRoot,
    [object[]] $Targets) {
    $recovery = Read-SPatchRecoveryManifest $GameRoot $Targets
    if ($null -eq $recovery) {
        return
    }
    Assert-SPatchNoLiveGame 'Recovery completion'
    foreach ($entry in @($recovery.Manifest.targets)) {
        $target = $recovery.TargetsByLabel[[string] $entry.label]
        if ([bool] $entry.existed) {
            if (-not (Test-Path -LiteralPath $target.Path -PathType Leaf) -or
                (Get-Item -LiteralPath $target.Path -Force).Length -ne
                    [int64] $entry.length -or
                (Get-SPatchSha256 $target.Path) -cne [string] $entry.sha256) {
                throw "$($target.Label) is not restored; recovery cannot be completed."
            }
        } elseif (Test-Path -LiteralPath $target.Path) {
            throw "$($target.Label) should be absent; recovery cannot be completed."
        }
    }
    Remove-SPatchSafeTree $GameRoot $recovery.Root 'Completed recovery directory'
}

function Get-SPatchFileIdentitySnapshot(
    [string] $Root,
    [string] $Filter) {
    $snapshot = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -Filter $Filter -File `
            -ErrorAction SilentlyContinue)) {
        if ($snapshot.ContainsKey($file.Name)) {
            throw "Identity snapshot contains a duplicate name: $($file.Name)"
        }
        $snapshot.Add($file.Name, [pscustomobject]@{
                Name = $file.Name
                Length = [int64] $file.Length
                Sha256 = Get-SPatchSha256 $file.FullName
                LastWriteTimeUtc = $file.LastWriteTimeUtc
                FullName = $file.FullName
            })
    }
    return $snapshot
}

function Compare-SPatchFileIdentitySnapshot(
    [Collections.Generic.Dictionary[string, object]] $Before,
    [Collections.Generic.Dictionary[string, object]] $After) {
    $changes = [Collections.Generic.List[object]]::new()
    foreach ($name in $After.Keys) {
        $current = $After[$name]
        if (-not $Before.ContainsKey($name)) {
            $changes.Add([pscustomobject]@{
                    Kind = 'new'
                    Name = $name
                    Before = $null
                    After = $current
                })
            continue
        }
        $previous = $Before[$name]
        if ($previous.Length -ne $current.Length -or
            $previous.Sha256 -cne $current.Sha256) {
            $changes.Add([pscustomobject]@{
                    Kind = 'changed'
                    Name = $name
                    Before = $previous
                    After = $current
                })
        }
    }
    return $changes.ToArray()
}

function Convert-SPatchSnapshotToEvidence(
    [Collections.Generic.Dictionary[string, object]] $Snapshot) {
    return @($Snapshot.Keys | Sort-Object | ForEach-Object {
            $identity = $Snapshot[$_]
            [ordered]@{
                name = $identity.Name
                length = [int64] $identity.Length
                sha256 = $identity.Sha256
            }
        })
}

function Get-SPatchBenchmarkReleaseDelayMilliseconds(
    [IO.FileInfo] $LatestResult,
    [datetime] $NowUtc = [DateTime]::UtcNow,
    [ValidateRange(0, 60)]
    [int] $SettleSeconds = 15,
    [ValidateRange(0, 30)]
    [int] $FutureToleranceSeconds = 5,
    [ValidateRange(1, 120)]
    [int] $MaximumDelaySeconds = 20) {
    if ($null -eq $LatestResult) {
        return 0
    }
    $writeUtc = $LatestResult.LastWriteTimeUtc
    if ($writeUtc -gt $NowUtc.AddSeconds($FutureToleranceSeconds)) {
        throw ("Latest benchmark result timestamp is too far in the future: " +
            "$($LatestResult.Name) $($writeUtc.ToString('o'))")
    }
    $delay = $writeUtc.AddSeconds($SettleSeconds) - $NowUtc
    if ($delay.TotalSeconds -gt $MaximumDelaySeconds) {
        throw ("The Steamworks result-release delay exceeded its " +
            "$MaximumDelaySeconds-second bound.")
    }
    if ($delay.TotalMilliseconds -le 0) {
        return 0
    }
    return [int] [Math]::Ceiling($delay.TotalMilliseconds)
}

function Get-SPatchProcessIdentity([Diagnostics.Process] $Process) {
    $Process.Refresh()
    if ($Process.HasExited) {
        throw "Process exited before identity capture: $($Process.Id)"
    }
    return [pscustomobject]@{
        ProcessId = [int] $Process.Id
        StartTimeUtcTicks = [int64] $Process.StartTime.ToUniversalTime().Ticks
        Executable = [IO.Path]::GetFullPath($Process.Path)
    }
}

function Wait-SPatchProcessIdentity(
    [Diagnostics.Process] $Process,
    [ValidateRange(1, 10)]
    [int] $TimeoutSeconds = 5,
    [string] $Label = 'process') {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            return Get-SPatchProcessIdentity $Process
        } catch {
            $lastError = $_.Exception.Message
            try {
                $Process.Refresh()
                if ($Process.HasExited) {
                    break
                }
            } catch {
                break
            }
        }
        Start-Sleep -Milliseconds 50
    }
    throw ("Could not capture the $Label identity within " +
           "$TimeoutSeconds seconds. Last error: $lastError")
}

function Test-SPatchProcessIdentity(
    [Diagnostics.Process] $Process,
    [int] $ProcessId,
    [int64] $StartTimeUtcTicks,
    [string] $Executable,
    [switch] $AllowExited) {
    try {
        $Process.Refresh()
        if ($Process.Id -ne $ProcessId -or
            (-not $AllowExited -and $Process.HasExited)) {
            return $false
        }
        if ($Process.StartTime.ToUniversalTime().Ticks -ne $StartTimeUtcTicks) {
            return $false
        }
        if (-not $Process.HasExited -and
            -not (Test-SPatchPathEqual $Process.Path $Executable)) {
            return $false
        }
        return $true
    } catch {
        return $false
    }
}

function Stop-SPatchExactOwnedProcess(
    [Diagnostics.Process] $Process,
    [object] $Identity,
    [string] $Description) {
    if ($null -eq $Process -or $null -eq $Identity) {
        return $false
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            return $false
        }
    } catch {
        return $false
    }
    if (-not (Test-SPatchProcessIdentity `
            $Process ([int] $Identity.ProcessId) `
            ([int64] $Identity.StartTimeUtcTicks) `
            ([string] $Identity.Executable))) {
        throw "Refusing to stop a $Description process whose PID/start/path identity changed."
    }
    Stop-Process -Id $Process.Id -Force -ErrorAction Stop
    if (-not $Process.WaitForExit(5000)) {
        throw "The task-owned $Description process did not exit within five seconds."
    }
    return $true
}

function Test-SPatchMutexHeld([string] $Name) {
    $probe = $null
    $acquired = $false
    try {
        $probe = [Threading.Mutex]::OpenExisting($Name)
        try {
            $acquired = $probe.WaitOne(0)
        } catch [Threading.AbandonedMutexException] {
            $acquired = $true
        }
        if ($acquired) {
            $probe.ReleaseMutex()
            return $false
        }
        return $true
    } catch [Threading.WaitHandleCannotBeOpenedException] {
        return $false
    } finally {
        if ($null -ne $probe) {
            $probe.Dispose()
        }
    }
}

function Get-SPatchBenchmarkReceiptRoot {
    return [IO.Path]::GetFullPath((Join-Path (
        [IO.Path]::GetTempPath()) 'SPatchBenchmarkLaunchReceipts'))
}

function Get-SPatchBenchmarkReceiptPath([string] $LeaseId) {
    $leaseGuid = [Guid]::Empty
    if (-not [Guid]::TryParseExact($LeaseId, 'N', [ref] $leaseGuid)) {
        throw 'The benchmark mutation-owner lease ID is invalid.'
    }
    return Join-Path (Get-SPatchBenchmarkReceiptRoot) (
        $leaseGuid.ToString('N') + '.json')
}

function Get-SPatchJoinedMutationLease {
    $names = @(
        'SPATCH_BENCHMARK_LEASE_ID',
        'SPATCH_BENCHMARK_OWNER_PID',
        'SPATCH_BENCHMARK_OWNER_START_UTC_TICKS',
        'SPATCH_BENCHMARK_OWNER_PATH',
        'SPATCH_BENCHMARK_RECEIPT',
        'SPATCH_BENCHMARK_SHORTCUT')
    $values = @{}
    $present = 0
    foreach ($name in $names) {
        $value = [Environment]::GetEnvironmentVariable($name, 'Process')
        $values[$name] = $value
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            ++$present
        }
    }
    if ($present -eq 0) {
        return $null
    }
    if ($present -ne $names.Count) {
        throw 'The benchmark mutation-owner lease environment is incomplete.'
    }
    $leaseGuid = [Guid]::Empty
    if (-not [Guid]::TryParseExact(
            $values['SPATCH_BENCHMARK_LEASE_ID'], 'N', [ref] $leaseGuid)) {
        throw 'The benchmark mutation-owner lease ID is invalid.'
    }
    [int] $ownerPid = 0
    [int64] $ownerTicks = 0
    if (-not [int]::TryParse(
            $values['SPATCH_BENCHMARK_OWNER_PID'], [ref] $ownerPid) -or
        $ownerPid -le 0 -or
        -not [int64]::TryParse(
            $values['SPATCH_BENCHMARK_OWNER_START_UTC_TICKS'],
            [ref] $ownerTicks) -or
        $ownerTicks -le 0) {
        throw 'The benchmark mutation-owner process identity is invalid.'
    }
    $owner = Get-Process -Id $ownerPid -ErrorAction SilentlyContinue
    if (-not $owner) {
        throw 'The benchmark mutation owner is no longer running.'
    }
    $ownerPath = [IO.Path]::GetFullPath(
        $values['SPATCH_BENCHMARK_OWNER_PATH'])
    if (-not (Test-SPatchProcessIdentity `
            $owner $ownerPid $ownerTicks $ownerPath)) {
        throw 'The benchmark mutation-owner PID/start/path identity does not match.'
    }
    if (-not (Test-SPatchPathEqual `
            $values['SPATCH_BENCHMARK_SHORTCUT'] `
            $script:SPatchBenchmarkShortcutPath)) {
        throw 'The benchmark lease was not issued for the required desktop shortcut.'
    }
    $leaseMutexName = 'Local\SPatch.LiveGraphicsHarness.Lease.' +
        $leaseGuid.ToString('N')
    if (-not (Test-SPatchMutexHeld $script:SPatchLiveHarnessMutexName) -or
        -not (Test-SPatchMutexHeld $leaseMutexName)) {
        throw 'The benchmark mutation-owner lease mutexes are not held.'
    }
    $receiptPath = [IO.Path]::GetFullPath(
        $values['SPATCH_BENCHMARK_RECEIPT'])
    $expectedReceiptPath = Get-SPatchBenchmarkReceiptPath (
        $leaseGuid.ToString('N'))
    if (-not (Test-SPatchPathEqual $receiptPath $expectedReceiptPath)) {
        throw 'The benchmark mutation-owner receipt path is outside the fixed receipt directory.'
    }
    return [pscustomobject]@{
        LeaseId = $leaseGuid.ToString('N')
        LeaseMutexName = $leaseMutexName
        OwnerPid = $ownerPid
        OwnerStartTimeUtcTicks = $ownerTicks
        OwnerPath = $ownerPath
        ReceiptPath = $receiptPath
        ShortcutPath = $script:SPatchBenchmarkShortcutPath
    }
}

function Assert-SPatchJoinedMutationLeaseActive([object] $Lease) {
    if ($null -eq $Lease) {
        throw 'The benchmark mutation-owner lease is missing.'
    }
    $current = Get-SPatchJoinedMutationLease
    if ($null -eq $current -or
        [string] $current.LeaseId -cne [string] $Lease.LeaseId -or
        [int] $current.OwnerPid -ne [int] $Lease.OwnerPid -or
        [int64] $current.OwnerStartTimeUtcTicks -ne
            [int64] $Lease.OwnerStartTimeUtcTicks -or
        -not (Test-SPatchPathEqual $current.OwnerPath $Lease.OwnerPath) -or
        -not (Test-SPatchPathEqual $current.ReceiptPath $Lease.ReceiptPath) -or
        -not (Test-SPatchPathEqual $current.ShortcutPath $Lease.ShortcutPath)) {
        throw 'The benchmark mutation-owner lease identity changed.'
    }
    return $current
}

function Write-SPatchLaunchReceipt(
    [object] $Lease,
    [Collections.IDictionary] $Values) {
    if ($null -eq $Lease) {
        return
    }
    $receiptPath = [IO.Path]::GetFullPath([string] $Lease.ReceiptPath)
    $expectedReceiptPath = Get-SPatchBenchmarkReceiptPath (
        [string] $Lease.LeaseId)
    if (-not (Test-SPatchPathEqual $receiptPath $expectedReceiptPath)) {
        throw 'Benchmark receipt path is outside the fixed receipt directory.'
    }
    $receiptRoot = Split-Path -Parent $receiptPath
    if (-not (Test-Path -LiteralPath $receiptRoot -PathType Container)) {
        throw "Benchmark receipt directory is missing: $receiptRoot"
    }
    $receiptRootItem = Get-Item -LiteralPath $receiptRoot -Force
    if (($receiptRootItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Benchmark receipt directory is a reparse point: $receiptRoot"
    }
    $receipt = [ordered]@{
        schema = 1
        lease_id = [string] $Lease.LeaseId
        shortcut_path = [string] $Lease.ShortcutPath
    }
    foreach ($key in $Values.Keys) {
        $receipt[$key] = $Values[$key]
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(
        (($receipt | ConvertTo-Json -Depth 5) + [Environment]::NewLine))
    $temporary = Join-Path $receiptRoot (
        '.' + [IO.Path]::GetFileName($receiptPath) + '.' +
            [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllBytes($temporary, $bytes)
        if (Test-Path -LiteralPath $receiptPath) {
            throw "Benchmark receipt already exists: $receiptPath"
        }
        [IO.File]::Move($temporary, $receiptPath)
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
}

function Write-SPatchUnverifiedLaunchFailure([string] $ErrorMessage) {
    $rawLeaseId = [Environment]::GetEnvironmentVariable(
        'SPATCH_BENCHMARK_LEASE_ID', 'Process')
    $rawShortcut = [Environment]::GetEnvironmentVariable(
        'SPATCH_BENCHMARK_SHORTCUT', 'Process')
    if ([string]::IsNullOrWhiteSpace($rawLeaseId) -or
        [string]::IsNullOrWhiteSpace($rawShortcut) -or
        -not (Test-SPatchPathEqual `
            $rawShortcut $script:SPatchBenchmarkShortcutPath)) {
        return $false
    }

    try {
        $receiptPath = Get-SPatchBenchmarkReceiptPath $rawLeaseId
        $receiptRoot = Split-Path -Parent $receiptPath
        if (-not (Test-Path -LiteralPath $receiptRoot -PathType Container)) {
            return $false
        }
        $receiptRootItem = Get-Item -LiteralPath $receiptRoot -Force
        if (($receiptRootItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $false
        }
        $lease = [pscustomobject]@{
            LeaseId = $rawLeaseId
            ReceiptPath = $receiptPath
            ShortcutPath = $script:SPatchBenchmarkShortcutPath
        }
        Write-SPatchLaunchReceipt $lease ([ordered]@{
                status = 'failed'
                error = $ErrorMessage
                launcher_pid = 0
                launcher_start_utc_ticks = 0
                launcher_path = ''
                game_pid = 0
                game_start_utc_ticks = 0
                game_path = ''
                started_steam = $false
                steam_pid = 0
                steam_start_utc_ticks = 0
                steam_path = ''
            })
        return $true
    } catch {
        return $false
    }
}

function Invoke-SPatchBenchmarkShortcut(
    [string] $ExpectedGameExecutable,
    [datetime] $RequestedUtc,
    [ValidateRange(5, 120)]
    [int] $ReceiptTimeoutSeconds = 30) {
    $shortcut = $script:SPatchBenchmarkShortcutPath
    if (-not (Test-Path -LiteralPath $shortcut -PathType Leaf)) {
        throw "Required unattended benchmark shortcut is missing: $shortcut"
    }
    $shortcutItem = Get-Item -LiteralPath $shortcut -Force
    if (($shortcutItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Required unattended benchmark shortcut is a reparse point: $shortcut"
    }
    $owner = [Diagnostics.Process]::GetCurrentProcess()
    $ownerIdentity = Get-SPatchProcessIdentity $owner
    $leaseId = [Guid]::NewGuid().ToString('N')
    $leaseMutexName = 'Local\SPatch.LiveGraphicsHarness.Lease.' + $leaseId
    $leaseMutex = [Threading.Mutex]::new($false, $leaseMutexName)
    $ownsLease = $false
    $receiptRoot = Get-SPatchBenchmarkReceiptRoot
    if (Test-Path -LiteralPath $receiptRoot) {
        $receiptRootItem = Get-Item -LiteralPath $receiptRoot -Force
        if (($receiptRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Benchmark receipt directory is a reparse point: $receiptRoot"
        }
    } else {
        [void] (New-Item -ItemType Directory -Path $receiptRoot -ErrorAction Stop)
    }
    $receiptPath = Get-SPatchBenchmarkReceiptPath $leaseId
    $launcherProcess = $null
    $launcherIdentity = $null
    $launchAccepted = $false
    $preexistingGameIdentities =
        [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
    foreach ($existingGame in @(Get-SPatchLiveGameProcesses)) {
        try {
            $existingIdentity = Get-SPatchProcessIdentity $existingGame
            [void] $preexistingGameIdentities.Add(
                ('{0}:{1}' -f $existingIdentity.ProcessId,
                    $existingIdentity.StartTimeUtcTicks))
        } catch { }
    }
    $environmentNames = @(
        'SPATCH_BENCHMARK_LEASE_ID',
        'SPATCH_BENCHMARK_OWNER_PID',
        'SPATCH_BENCHMARK_OWNER_START_UTC_TICKS',
        'SPATCH_BENCHMARK_OWNER_PATH',
        'SPATCH_BENCHMARK_RECEIPT',
        'SPATCH_BENCHMARK_SHORTCUT')
    $previousEnvironment = @{}
    try {
        try {
            $ownsLease = $leaseMutex.WaitOne(0)
        } catch [Threading.AbandonedMutexException] {
            $ownsLease = $true
        }
        if (-not $ownsLease) {
            throw 'Could not acquire the per-launch benchmark lease.'
        }
        $environment = [ordered]@{
            SPATCH_BENCHMARK_LEASE_ID = $leaseId
            SPATCH_BENCHMARK_OWNER_PID = [string] $ownerIdentity.ProcessId
            SPATCH_BENCHMARK_OWNER_START_UTC_TICKS =
                [string] $ownerIdentity.StartTimeUtcTicks
            SPATCH_BENCHMARK_OWNER_PATH = $ownerIdentity.Executable
            SPATCH_BENCHMARK_RECEIPT = $receiptPath
            SPATCH_BENCHMARK_SHORTCUT = $shortcut
        }
        foreach ($name in $environmentNames) {
            $previousEnvironment[$name] =
                [Environment]::GetEnvironmentVariable($name, 'Process')
            [Environment]::SetEnvironmentVariable(
                $name, [string] $environment[$name], 'Process')
        }
        try {
            $launcherProcess = Start-Process -FilePath $shortcut -PassThru
            # ShellExecute can return the newly created shortcut target before
            # Windows has populated Process.Path. Keep the per-launch lease
            # held while that identity becomes observable.
            $launcherIdentity = Wait-SPatchProcessIdentity `
                $launcherProcess 5 'desktop shortcut launcher'
        } finally {
            foreach ($name in $environmentNames) {
                [Environment]::SetEnvironmentVariable(
                    $name, $previousEnvironment[$name], 'Process')
            }
        }

        $deadline = [DateTime]::UtcNow.AddSeconds($ReceiptTimeoutSeconds)
        $receipt = $null
        $lastReadError = ''
        while ([DateTime]::UtcNow -lt $deadline) {
            if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
                try {
                    $receipt = [IO.File]::ReadAllText(
                        $receiptPath,
                        [Text.UTF8Encoding]::new($false, $true)) |
                        ConvertFrom-Json -ErrorAction Stop
                    break
                } catch {
                    $lastReadError = $_.Exception.Message
                }
            }
            Start-Sleep -Milliseconds 100
        }
        if ($null -eq $receipt) {
            $launcherExit = 'still running'
            if ($launcherProcess) {
                try {
                    $launcherProcess.Refresh()
                    if ($launcherProcess.HasExited) {
                        $launcherExit = [string] $launcherProcess.ExitCode
                    }
                } catch { }
            }
            throw ("The desktop shortcut did not publish a launch receipt within " +
                "$ReceiptTimeoutSeconds seconds (launcher exit=$launcherExit; " +
                "last receipt error=$lastReadError).")
        }
        if ([int] $receipt.schema -ne 1 -or
            [string] $receipt.lease_id -cne $leaseId -or
            -not (Test-SPatchPathEqual ([string] $receipt.shortcut_path) $shortcut)) {
            throw 'The desktop shortcut launch receipt identity is invalid.'
        }
        if ([string] $receipt.status -cne 'launched') {
            throw "The desktop shortcut launcher failed: $($receipt.error)"
        }
        $gamePid = [int] $receipt.game_pid
        $gameTicks = [int64] $receipt.game_start_utc_ticks
        $gamePath = [IO.Path]::GetFullPath([string] $receipt.game_path)
        if (-not (Test-SPatchPathEqual $gamePath $ExpectedGameExecutable)) {
            throw "The desktop shortcut launched the wrong game path: $gamePath"
        }
        $gameProcess = Get-Process -Id $gamePid -ErrorAction SilentlyContinue
        if (-not $gameProcess -or
            -not (Test-SPatchProcessIdentity `
                $gameProcess $gamePid $gameTicks $gamePath)) {
            throw 'The desktop shortcut game PID/start/path identity is no longer live.'
        }
        $startUtc = $gameProcess.StartTime.ToUniversalTime()
        if ($startUtc -lt $RequestedUtc.AddSeconds(-2) -or
            $startUtc -gt [DateTime]::UtcNow.AddSeconds(2)) {
            throw 'The desktop shortcut game start time is outside the bounded launch window.'
        }
        $launchAccepted = $true
        return [pscustomobject]@{
            Process = $gameProcess
            ProcessIdentity = [pscustomobject]@{
                ProcessId = $gamePid
                StartTimeUtcTicks = $gameTicks
                Executable = $gamePath
            }
            Receipt = $receipt
            ShortcutPath = $shortcut
            LauncherProcess = $launcherProcess
        }
    } finally {
        foreach ($name in $environmentNames) {
            if ($previousEnvironment.ContainsKey($name)) {
                [Environment]::SetEnvironmentVariable(
                    $name, $previousEnvironment[$name], 'Process')
            }
        }
        try {
            if (-not $launchAccepted) {
                [void] (Stop-SPatchExactOwnedProcess `
                    $launcherProcess $launcherIdentity 'desktop shortcut launcher')
                foreach ($candidateGame in @(Get-SPatchLiveGameProcesses)) {
                    try {
                        $candidateIdentity = Get-SPatchProcessIdentity $candidateGame
                        $candidateKey = '{0}:{1}' -f `
                            $candidateIdentity.ProcessId,
                            $candidateIdentity.StartTimeUtcTicks
                        $candidateStartUtc = [DateTime]::new(
                            $candidateIdentity.StartTimeUtcTicks,
                            [DateTimeKind]::Utc)
                        if (-not $preexistingGameIdentities.Contains($candidateKey) -and
                            (Test-SPatchPathEqual `
                                $candidateIdentity.Executable $ExpectedGameExecutable) -and
                            $candidateStartUtc -ge $RequestedUtc.AddSeconds(-2)) {
                            [void] (Stop-SPatchExactOwnedProcess `
                                $candidateGame $candidateIdentity `
                                'unaccepted benchmark game')
                        }
                    } catch {
                        throw ("Could not prove cleanup of a post-launch game " +
                            "candidate: $($_.Exception.Message)")
                    }
                }
            }
        } catch {
            # A launch error is already in flight whenever launchAccepted is
            # false. Preserve that primary diagnosis while reporting a
            # best-effort cleanup race (most commonly, the game exited while
            # its identity was being captured).
            Write-Warning (
                "Benchmark launch cleanup was incomplete: $($_.Exception.Message)")
        } finally {
            if ($ownsLease) {
                $leaseMutex.ReleaseMutex()
            }
            $leaseMutex.Dispose()
            if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
                Remove-Item -LiteralPath $receiptPath -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

function Wait-SPatchNaturalProcessExit(
    [Diagnostics.Process] $Process,
    [object] $Identity,
    [ValidateRange(1, 120)]
    [int] $TimeoutSeconds = 30) {
    if (-not (Test-SPatchProcessIdentity `
            $Process ([int] $Identity.ProcessId) `
            ([int64] $Identity.StartTimeUtcTicks) `
            ([string] $Identity.Executable))) {
        try {
            $Process.Refresh()
            if ($Process.HasExited -and
                $Process.StartTime.ToUniversalTime().Ticks -eq
                    [int64] $Identity.StartTimeUtcTicks) {
                return [pscustomobject]@{
                    Exited = $true
                    ExitCode = [int] $Process.ExitCode
                }
            }
        } catch { }
        throw 'The owned process identity changed before natural-exit waiting.'
    }
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        return [pscustomobject]@{
            Exited = $false
            ExitCode = $null
        }
    }
    return [pscustomobject]@{
        Exited = $true
        ExitCode = [int] $Process.ExitCode
    }
}
