[CmdletBinding()]
param(
    [switch] $FakeShortcutChild,
    [switch] $FakeDelayedShortcutChild,
    [switch] $CompatibilityChild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$toolsRoot = $PSScriptRoot
$helperPath = Join-Path $toolsRoot 'LiveHarnessSafety.ps1'
. $helperPath

if ($CompatibilityChild) {
    if ($script:SPatchLiveHarnessSafetyVersion -cne '2026.08.10.6') {
        throw 'Compatibility child loaded the wrong helper version.'
    }
    foreach ($compatibilityPath in @(
            $helperPath,
            (Join-Path $toolsRoot 'Start-UnattendedBenchmark.ps1'),
            (Join-Path $toolsRoot 'Invoke-FinalSmoke.ps1'),
            (Join-Path $toolsRoot 'Invoke-PBRBenchmark.ps1'))) {
        $compatibilityTokens = $null
        $compatibilityErrors = $null
        [void] [Management.Automation.Language.Parser]::ParseFile(
            $compatibilityPath,
            [ref] $compatibilityTokens,
            [ref] $compatibilityErrors)
        if ($compatibilityErrors.Count -ne 0) {
            throw "Windows PowerShell parse failure: $compatibilityPath"
        }
    }
    'WINDOWS_POWERSHELL_HELPER_LOAD=PASS'
    return
}

if ($FakeShortcutChild -or $FakeDelayedShortcutChild) {
    $script:SPatchBenchmarkShortcutPath =
        [Environment]::GetEnvironmentVariable(
            'SPATCH_BENCHMARK_SHORTCUT', 'Process')
    try {
        $lease = Get-SPatchJoinedMutationLease
    } catch {
        [void] (Write-SPatchUnverifiedLaunchFailure $_.Exception.Message)
        throw
    }
    if ($null -eq $lease) {
        throw 'Fake shortcut child did not receive a verified parent lease.'
    }
    $identity = Get-SPatchProcessIdentity (
        [Diagnostics.Process]::GetCurrentProcess())
    if ($FakeDelayedShortcutChild) {
        $delayMarker = [Environment]::GetEnvironmentVariable(
            'SPATCH_TEST_DELAY_MARKER', 'Process')
        if ([string]::IsNullOrWhiteSpace($delayMarker)) {
            throw 'Delayed shortcut child did not receive its test marker.'
        }
        [IO.File]::WriteAllText(
            $delayMarker,
            (($identity | ConvertTo-Json -Compress) +
                [Environment]::NewLine),
            [Text.UTF8Encoding]::new($false))
        Start-Sleep -Seconds 8
        [IO.File]::WriteAllText(
            ($delayMarker + '.late'), 'late launch path reached')
        [void] (Assert-SPatchJoinedMutationLeaseActive $lease)
    }
    Write-SPatchLaunchReceipt $lease ([ordered]@{
            status = 'launched'
            error = ''
            launcher_pid = $identity.ProcessId
            launcher_start_utc_ticks = $identity.StartTimeUtcTicks
            launcher_path = $identity.Executable
            game_pid = $identity.ProcessId
            game_start_utc_ticks = $identity.StartTimeUtcTicks
            game_path = $identity.Executable
            started_steam = $false
            steam_pid = 0
            steam_start_utc_ticks = 0
            steam_path = ''
        })
    Start-Sleep -Seconds 30
    return
}

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Throws(
    [scriptblock] $Action,
    [string] $Pattern,
    [string] $Label) {
    $caught = $null
    try {
        & $Action
    } catch {
        $caught = $_
    }
    if ($null -eq $caught) {
        throw "$Label did not throw."
    }
    if ($caught.Exception.Message -notmatch $Pattern) {
        throw "$Label threw the wrong error: $($caught.Exception.Message)"
    }
}

function Assert-Bytes(
    [string] $Path,
    [byte[]] $Expected,
    [string] $Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $actual = [IO.File]::ReadAllBytes($Path)
    if ($actual.Length -ne $Expected.Length) {
        throw "$Label length differs."
    }
    for ($index = 0; $index -lt $actual.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw "$Label differs at byte $index."
        }
    }
}

function Assert-ScriptParses([string] $Path) {
    $tokens = $null
    $errors = $null
    [void] [Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref] $tokens, [ref] $errors)
    if ($errors.Count -ne 0) {
        throw ("PowerShell parse errors in ${Path}: " +
            (($errors | ForEach-Object {
                        "$($_.Extent.StartLineNumber):$($_.Message)"
                    }) -join '; '))
    }
}

$repoRoot = Split-Path -Parent $toolsRoot
$finalPath = Join-Path $toolsRoot 'Invoke-FinalSmoke.ps1'
$pbrPath = Join-Path $toolsRoot 'Invoke-PBRBenchmark.ps1'
$startPath = Join-Path $toolsRoot 'Start-UnattendedBenchmark.ps1'
foreach ($path in @($helperPath, $finalPath, $pbrPath, $startPath)) {
    Assert-ScriptParses $path
}

$helperText = [IO.File]::ReadAllText($helperPath)
$finalText = [IO.File]::ReadAllText($finalPath)
$pbrText = [IO.File]::ReadAllText($pbrPath)
$startText = [IO.File]::ReadAllText($startPath)
$helperTokens = $null
$helperErrors = $null
$helperAst = [Management.Automation.Language.Parser]::ParseFile(
    $helperPath, [ref] $helperTokens, [ref] $helperErrors)
$launcherIdentityWaitAssignment = $helperAst.Find({
        param($node)
        if ($node -isnot
            [Management.Automation.Language.AssignmentStatementAst] -or
            $node.Left.Extent.Text -cne '$launcherIdentity') {
            return $false
        }
        return $null -ne $node.Right.Find({
                param($child)
                $child -is [Management.Automation.Language.CommandAst] -and
                $child.GetCommandName() -ceq 'Wait-SPatchProcessIdentity'
            }, $true)
    }, $true)
Assert-True ($null -ne $launcherIdentityWaitAssignment) `
    'Desktop-shortcut launcher identity is not captured through the bounded wait.'
$currentProcess = [Diagnostics.Process]::GetCurrentProcess()
$currentIdentity = Wait-SPatchProcessIdentity `
    $currentProcess 1 'live-harness test process'
Assert-True (
    $currentIdentity.ProcessId -eq $currentProcess.Id -and
    -not [string]::IsNullOrWhiteSpace($currentIdentity.Executable) -and
    (Test-SPatchPathEqual $currentIdentity.Executable $currentProcess.Path)) `
    'Bounded process-identity wait returned the wrong live process.'
$originalIdentityReader =
    (Get-Command Get-SPatchProcessIdentity -CommandType Function).ScriptBlock
try {
    $script:identityReadAttempts = 0
    $script:injectedProcessIdentity = $currentIdentity
    function Get-SPatchProcessIdentity([Diagnostics.Process] $Process) {
        ++$script:identityReadAttempts
        if ($script:identityReadAttempts -le 2) {
            throw 'Injected transient process-path publication race.'
        }
        return $script:injectedProcessIdentity
    }
    $retriedIdentity = Wait-SPatchProcessIdentity `
        $currentProcess 1 'transient identity test process'
    Assert-True (
        $script:identityReadAttempts -eq 3 -and
        $retriedIdentity.ProcessId -eq $currentIdentity.ProcessId -and
        (Test-SPatchPathEqual `
            $retriedIdentity.Executable $currentIdentity.Executable)) `
        'Bounded process-identity wait did not retry transient failure.'

    $script:identityReadAttempts = 0
    function Get-SPatchProcessIdentity([Diagnostics.Process] $Process) {
        ++$script:identityReadAttempts
        throw 'Injected permanent process identity failure.'
    }
    $permanentFailureStart = [DateTime]::UtcNow
    Assert-Throws {
        Wait-SPatchProcessIdentity `
            $currentProcess 1 'permanent identity test process'
    } 'Could not capture the permanent identity test process identity' `
        'Permanent process-identity failure'
    $permanentFailureElapsed =
        [DateTime]::UtcNow - $permanentFailureStart
    Assert-True (
        $script:identityReadAttempts -gt 1 -and
        $permanentFailureElapsed.TotalSeconds -ge 0.8 -and
        $permanentFailureElapsed.TotalSeconds -lt 3.0) `
        'Permanent process-identity failure was not retried and bounded.'
} finally {
    Set-Item -Path Function:Get-SPatchProcessIdentity `
        -Value $originalIdentityReader
    Remove-Variable -Name identityReadAttempts -Scope Script `
        -ErrorAction SilentlyContinue
    Remove-Variable -Name injectedProcessIdentity -Scope Script `
        -ErrorAction SilentlyContinue
}
$finalTokens = $null
$finalErrors = $null
$finalAst = [Management.Automation.Language.Parser]::ParseFile(
    $finalPath, [ref] $finalTokens, [ref] $finalErrors)
$smokeFailureFunction = $finalAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-NoSmokeFailures'
    }, $true)
Assert-True ($null -ne $smokeFailureFunction) `
    'Final smoke has no Assert-NoSmokeFailures function.'
$smokeFailureDefinition = [scriptblock]::Create(
    $smokeFailureFunction.Extent.Text)
. $smokeFailureDefinition
foreach ($failureLine in @(
        '[ShenLong] initialization failed.',
        '[ShenLong-PBR] feature disabled.',
        '[ShenLong-Water] shader unavailable for 120 consecutive frames.')) {
    Assert-Throws {
        Assert-NoSmokeFailures '' $failureLine
    } 'failure evidence' "ShenLong warning scanner: $failureLine"
}
Assert-NoSmokeFailures '' '[ShenLong-PBR] initialized successfully.'
$containsLiteralFunction = $finalAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-ContainsLiteral'
    }, $true)
$vehicleCameraRuntimeFunction = $finalAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-VehicleCameraRuntimeEvidence'
    }, $true)
Assert-True ($null -ne $containsLiteralFunction) `
    'Final smoke has no Assert-ContainsLiteral function.'
Assert-True ($null -ne $vehicleCameraRuntimeFunction) `
    'Final smoke has no Assert-VehicleCameraRuntimeEvidence function.'
. ([scriptblock]::Create($containsLiteralFunction.Extent.Text))
. ([scriptblock]::Create($vehicleCameraRuntimeFunction.Extent.Text))
$vehicleCameraRuntimeLines = @(
    '[INFO] requested_config input gta_iv_car_camera=1 gta_iv_bike_camera=0',
    ('[INFO] gtaiv_vehicle_camera car_requested=1 bike_requested=0 ' +
        'installed=1 setter_installed=1 update_installed=1 ' +
        'desired_pose_installed=1 angular_approach_installed=1 ' +
        'dynamics_mutation=1 layout=legacy_researched'),
    ('[INFO] gtaiv_vehicle_camera_probe event=state_change mode=active ' +
        'mutation=0 car_enabled=1 bike_enabled=0 class_enabled=0 ' +
        'policy_evaluated=0 base_drive_branch_readable=0 ' +
        'base_drive_branch_selected=0 target_profile=none ' +
        'target_slot_match_mask=0x0000 selected_slot_match_mask=0x0000 ' +
        'readable=0 blend_factor=0.000 base_offset_m=-0.350 ' +
        'applied_delta_m=0.000 active_fields_readable=0 ' +
        'source_weight_valid=0 target_parameters=0x0000000000000000 ' +
        'source_weight=0.000000 update_eye=0 looking_back=0 ' +
        'state_b5a=0 state_b5b=0 state_b5c=0'))
foreach ($lineEnding in @("`r`n", "`n")) {
    $vehicleCameraRuntimeText =
        ($vehicleCameraRuntimeLines -join $lineEnding) + $lineEnding
    Assert-VehicleCameraRuntimeEvidence `
        $vehicleCameraRuntimeText $true $false
}
$vehicleCameraRuntimeAtEof = @(
    $vehicleCameraRuntimeLines[0],
    $vehicleCameraRuntimeLines[2],
    $vehicleCameraRuntimeLines[1]) -join "`r`n"
Assert-VehicleCameraRuntimeEvidence $vehicleCameraRuntimeAtEof $true $false
$duplicateVehicleCameraRuntimeText =
    (($vehicleCameraRuntimeLines + $vehicleCameraRuntimeLines[1]) -join "`r`n") +
    "`r`n"
Assert-Throws {
    Assert-VehicleCameraRuntimeEvidence `
        $duplicateVehicleCameraRuntimeText $true $false
} 'found 2' 'Duplicate vehicle-camera installation record'
$missingAngularHookLines = @($vehicleCameraRuntimeLines)
$missingAngularHookLines[1] = $missingAngularHookLines[1].Replace(
    ' angular_approach_installed=1', '')
Assert-Throws {
    Assert-VehicleCameraRuntimeEvidence `
        (($missingAngularHookLines -join "`r`n") + "`r`n") $true $false
} 'angular_approach_installed=1' 'Missing angular camera hook evidence'
$poisonedInactiveCameraLines = @($vehicleCameraRuntimeLines)
$poisonedInactiveCameraLines[2] = $poisonedInactiveCameraLines[2].Replace(
    'looking_back=0', 'looking_back=222')
Assert-Throws {
    Assert-VehicleCameraRuntimeEvidence `
        (($poisonedInactiveCameraLines -join "`r`n") + "`r`n") $true $false
} 'looking_back=0' 'Poisoned inactive vehicle-camera state'
$runtimeReadyFunction = $finalAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Wait-ForRuntimeReady'
    }, $true)
Assert-True ($null -ne $runtimeReadyFunction) `
    'Final smoke has no Wait-ForRuntimeReady function.'
$runtimeReadyParameters = @(
    $runtimeReadyFunction.Parameters | ForEach-Object {
        $_.Name.VariablePath.UserPath
    })
Assert-True ('CleanupFailures' -notin $runtimeReadyParameters) `
    'Final runtime readiness still requires an unbound cleanup list.'
$receiptFunction = $finalAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-FinalReleaseIdentity'
    }, $true)
$expectedKeysAssignment = $receiptFunction.Body.Find({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq '$expectedKeys'
    }, $true)
$releaseReceiptKeys = @(
    $expectedKeysAssignment.Right.FindAll({
            param($node)
            $node -is [Management.Automation.Language.StringConstantExpressionAst]
        }, $true) | ForEach-Object { $_.Value })
$expectedReleaseReceiptKeys = @(
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
Assert-True (
    $releaseReceiptKeys.Count -eq 18 -and
    @(Compare-Object `
        -ReferenceObject $expectedReleaseReceiptKeys `
        -DifferenceObject $releaseReceiptKeys `
        -CaseSensitive).Count -eq 0) `
    'Final release identity does not require the exact 18-key receipt.'
$requiredShortcut =
    'C:\Users\Admin\Desktop\Sleeping Dogs DE - Unattended Benchmark.lnk'
Assert-True ($helperText.Contains("'$requiredShortcut'")) `
    'Safety helper does not bind the exact required desktop shortcut.'
foreach ($consumer in @(
        [pscustomobject]@{ Label = 'Final'; Text = $finalText },
        [pscustomobject]@{ Label = 'PBR'; Text = $pbrText })) {
    Assert-True ($consumer.Text.Contains('Invoke-SPatchBenchmarkShortcut')) `
        "$($consumer.Label) does not invoke the shortcut handshake."
    Assert-True (-not $consumer.Text.Contains('& $benchmarkLauncher')) `
        "$($consumer.Label) still invokes the benchmark script directly."
    Assert-True (-not $consumer.Text.Contains('& $launcher')) `
        "$($consumer.Label) still invokes a direct launcher variable."
    Assert-True (-not $consumer.Text.Contains(
            'Start-Process -FilePath $gameExe')) `
        "$($consumer.Label) directly starts the game executable."
    Assert-True ($consumer.Text.Contains('SPatch-*.dmp')) `
        "$($consumer.Label) has no per-attempt crash-dump snapshot."
    Assert-True ($consumer.Text.Contains('WriteCrashDumps')) `
        "$($consumer.Label) does not force crash-dump writing."
    Assert-True ($consumer.Text.Contains('New-SPatchRecoveryBackup')) `
        "$($consumer.Label) has no durable pre-mutation recovery backup."
    Assert-True ($consumer.Text.Contains('Write-SPatchAtomic')) `
        "$($consumer.Label) has no atomic configuration mutation."
}
Assert-True ($finalText.Contains('SPatch.ini.previous.bak')) `
    'Final smoke does not preserve SPatch.ini.previous.bak.'
foreach ($consumer in @(
        [pscustomobject]@{ Label = 'Final'; Text = $finalText },
        [pscustomobject]@{ Label = 'PBR'; Text = $pbrText })) {
    Assert-True ($consumer.Text.Contains("Join-Path `$GameRoot 'SPatch.ini'")) `
        "$($consumer.Label) does not preserve the base SPatch.ini path."
    Assert-True ($consumer.Text.Contains("Join-Path `$GameRoot 'ShenLong.ini'")) `
        "$($consumer.Label) does not mutate the separate ShenLong.ini path."
    Assert-True ($consumer.Text.Contains("Join-Path `$GameRoot 'ShenLong.asi'")) `
        "$($consumer.Label) does not verify the separate ShenLong.asi artifact."
    Assert-True (-not $consumer.Text.Contains('SPatchGraphics.addon')) `
        "$($consumer.Label) still references the retired combined graphics add-on."
}
Assert-True ($finalText.Contains('verified-native-2048')) `
    'Final smoke has no explicit native-2048 shadow pipeline classification.'
Assert-True ($finalText.Contains(
        '$observedConsumers -ne $classifiedTotal')) `
    'Final smoke does not reconcile observed and classified 2048 consumers.'
foreach ($identityPrefix in @(
        'loaded_game',
        'loaded_spatch',
        'loaded_asi_loader',
        'loaded_graphics_addon',
        'loaded_reshade_host',
        'loaded_native_d3d11')) {
    foreach ($identitySuffix in @('path', 'length', 'sha256')) {
        $identityField = "${identityPrefix}_${identitySuffix}"
        Assert-True ($pbrText.Contains($identityField)) `
            "PBR evidence omits $identityField."
    }
}
foreach ($resultIdentityField in @(
        'benchmark_result_name',
        'benchmark_result_length',
        'benchmark_result_sha256')) {
    Assert-True ($pbrText.Contains($resultIdentityField)) `
        "PBR evidence omits $resultIdentityField."
}
Assert-True ($pbrText.Contains(
        'Get-SPatchBenchmarkReleaseDelayMilliseconds')) `
    'PBR does not use the bounded future-timestamp delay helper.'
$waitIndex = $startText.IndexOf(
    '$process.WaitForExit($BenchmarkTimeoutSeconds * 1000)',
    [StringComparison]::Ordinal)
$releaseIndex = $startText.LastIndexOf(
    '$liveHarnessMutex.ReleaseMutex()',
    [StringComparison]::Ordinal)
Assert-True ($waitIndex -ge 0 -and $releaseIndex -gt $waitIndex) `
    'Standalone -Wait releases mutation ownership before completion.'

$temporaryBase = [IO.Path]::GetFullPath((Join-Path (
    [IO.Path]::GetTempPath()) (
        'SPatch-LiveHarnessSafety-{0}-{1}' -f
            $PID, [Guid]::NewGuid().ToString('N'))))
$temporaryPrefix = [IO.Path]::GetFullPath(
    [IO.Path]::GetTempPath()).TrimEnd([char[]]'\/') +
    [IO.Path]::DirectorySeparatorChar
Assert-True ($temporaryBase.StartsWith(
        $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) `
    'Fixture root is outside the system temporary directory.'
[void] (New-Item -ItemType Directory -Path $temporaryBase)
$ownedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$globalMutex = $null
$ownsGlobalMutex = $false
$junction = $null
$lockedTargetStream = $null
try {
    $gameRoot = Join-Path $temporaryBase 'game'
    $dataRoot = Join-Path $gameRoot 'data'
    [void] (New-Item -ItemType Directory -Path $dataRoot -Force)
    $displayPath = Join-Path $dataRoot 'DisplaySettings.xml'
    $targets = @(Get-SPatchUserStateTargets $gameRoot $displayPath)
    $originals = @{
        'SPatch.ini' = [Text.Encoding]::UTF8.GetBytes("[SPatch]`nWriteCrashDumps=0`n")
        'SPatch.ini.previous.bak' = [byte[]](1, 2, 3, 4)
        'ShenLong.ini' = [Text.Encoding]::UTF8.GetBytes(
            "[ShenLong]`nEnabled=1`nConfigVersion=1`n")
        'ReShade.ini' = [Text.Encoding]::UTF8.GetBytes("[ADDON]`nPath=x`n")
        'DisplaySettings.xml' = [Text.Encoding]::UTF8.GetBytes(
            '<DisplaySettings><Fullscreen>1</Fullscreen></DisplaySettings>')
    }
    foreach ($target in $targets) {
        [IO.File]::WriteAllBytes(
            [string] $target.Path, [byte[]] $originals[[string] $target.Label])
    }

    [void] (New-SPatchRecoveryBackup $gameRoot $targets 'fixture')
    foreach ($target in $targets) {
        Write-SPatchAtomicBytes `
            $gameRoot $target.Path ([byte[]](9, 8, 7)) $target.Label
    }
    [void] (Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup)
    foreach ($target in $targets) {
        Assert-Bytes $target.Path $originals[$target.Label] `
            "$($target.Label) retained-backup restoration"
    }
    Complete-SPatchRecoveryBackup $gameRoot $targets
    Assert-True (-not (Test-Path -LiteralPath (
                Get-SPatchRecoveryRoot $gameRoot))) `
        'Completed recovery backup was not removed.'

    Remove-Item -LiteralPath (Join-Path $gameRoot 'SPatch.ini.previous.bak') -Force
    $targets = @(Get-SPatchUserStateTargets $gameRoot $displayPath)
    [void] (New-SPatchRecoveryBackup $gameRoot $targets 'absent-previous-fixture')
    Write-SPatchAtomicBytes $gameRoot `
        (Join-Path $gameRoot 'SPatch.ini.previous.bak') `
        ([byte[]](5, 5, 5)) 'SPatch.ini.previous.bak fixture'
    [void] (Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup)
    Assert-True (-not (Test-Path -LiteralPath (
                Join-Path $gameRoot 'SPatch.ini.previous.bak'))) `
        'Absent SPatch.ini.previous.bak state was not restored.'
    Complete-SPatchRecoveryBackup $gameRoot $targets

    $targets = @(Get-SPatchUserStateTargets $gameRoot $displayPath)
    [void] (New-SPatchRecoveryBackup $gameRoot $targets 'startup-recovery-fixture')
    Write-SPatchAtomicBytes $gameRoot (Join-Path $gameRoot 'SPatch.ini') `
        ([byte[]](6, 6, 6)) 'SPatch.ini interrupted fixture'
    [void] (Restore-SPatchRecoveryBackup $gameRoot $targets)
    Assert-Bytes (Join-Path $gameRoot 'SPatch.ini') `
        $originals['SPatch.ini'] 'Startup recovery SPatch.ini'
    Assert-True (-not (Test-Path -LiteralPath (
                Get-SPatchRecoveryRoot $gameRoot))) `
        'Startup recovery did not consume its durable backup.'

    Write-SPatchAtomicBytes $gameRoot `
        (Join-Path $gameRoot 'SPatch.ini.previous.bak') `
        $originals['SPatch.ini.previous.bak'] 'SPatch.ini.previous.bak fixture reset'
    $targets = @(Get-SPatchUserStateTargets $gameRoot $displayPath)
    [void] (New-SPatchRecoveryBackup `
        $gameRoot $targets 'manifest-coverage-fixture')
    $coverageRecoveryRoot = Get-SPatchRecoveryRoot $gameRoot
    $coverageManifestPath = Join-Path $coverageRecoveryRoot 'recovery.json'
    $coverageManifestBytes = [IO.File]::ReadAllBytes($coverageManifestPath)
    $coverageManifest = [IO.File]::ReadAllText($coverageManifestPath) |
        ConvertFrom-Json
    $coverageManifest.targets[3].label = $coverageManifest.targets[0].label
    $coverageManifest.targets[3].relative_path =
        $coverageManifest.targets[0].relative_path
    Write-SPatchAtomicText `
        $coverageRecoveryRoot $coverageManifestPath `
        (($coverageManifest | ConvertTo-Json -Depth 5) +
            [Environment]::NewLine) `
        ([Text.UTF8Encoding]::new($false)) 'Tampered recovery manifest fixture'
    Assert-Throws {
        Read-SPatchRecoveryManifest $gameRoot $targets
    } 'duplicated' 'Duplicate recovery-manifest label'
    Write-SPatchAtomicBytes `
        $coverageRecoveryRoot $coverageManifestPath $coverageManifestBytes `
        'Recovery manifest fixture reset'
    [void] (Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup)
    Complete-SPatchRecoveryBackup $gameRoot $targets

    [void] (New-SPatchRecoveryBackup $gameRoot $targets 'aggregate-recovery-fixture')
    foreach ($target in $targets) {
        Write-SPatchAtomicBytes `
            $gameRoot $target.Path ([byte[]](7, 7, 7)) $target.Label
    }
    $lockedTargetStream = [IO.File]::Open(
        (Join-Path $gameRoot 'ReShade.ini'),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::None)
    Assert-Throws {
        Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup
    } 'ReShade.ini'
    Assert-Bytes (Join-Path $gameRoot 'SPatch.ini') `
        $originals['SPatch.ini'] 'Aggregate recovery SPatch.ini'
    Assert-Bytes (Join-Path $gameRoot 'SPatch.ini.previous.bak') `
        $originals['SPatch.ini.previous.bak'] `
        'Aggregate recovery SPatch.ini.previous.bak'
    Assert-Bytes $displayPath $originals['DisplaySettings.xml'] `
        'Aggregate recovery DisplaySettings.xml'
    $lockedTargetStream.Dispose()
    $lockedTargetStream = $null
    Assert-Bytes (Join-Path $gameRoot 'ReShade.ini') ([byte[]](7, 7, 7)) `
        'Aggregate recovery locked ReShade.ini'
    Assert-True (Test-Path -LiteralPath (Get-SPatchRecoveryRoot $gameRoot)) `
        'Partial recovery did not retain its durable backup.'
    [void] (Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup)
    foreach ($target in $targets) {
        Assert-Bytes $target.Path $originals[$target.Label] `
            "$($target.Label) aggregate-recovery retry"
    }
    Complete-SPatchRecoveryBackup $gameRoot $targets

    $resultPath = Join-Path $gameRoot 'BenchmarkResult-fixed.xml'
    [IO.File]::WriteAllBytes($resultPath, ([byte[]](1, 2, 3)))
    $resultIdentityBefore = Get-SPatchFileIdentitySnapshot `
        $gameRoot 'BenchmarkResult-*.xml'
    [IO.File]::WriteAllBytes($resultPath, ([byte[]](1, 2, 3, 4)))
    $resultIdentityAfter = Get-SPatchFileIdentitySnapshot `
        $gameRoot 'BenchmarkResult-*.xml'
    $resultChanges = @(Compare-SPatchFileIdentitySnapshot `
        $resultIdentityBefore $resultIdentityAfter)
    Assert-True ($resultChanges.Count -eq 1 -and
        $resultChanges[0].Kind -ceq 'changed' -and
        $resultChanges[0].Name -ceq 'BenchmarkResult-fixed.xml' -and
        $resultChanges[0].After.Length -eq 4 -and
        $resultChanges[0].After.Sha256 -cmatch '^[0-9A-F]{64}$') `
        'Benchmark result identity did not bind name, length, and SHA-256.'
    $fixedNowUtc = [DateTime]::UtcNow
    [IO.File]::SetLastWriteTimeUtc(
        $resultPath, $fixedNowUtc.AddSeconds(10))
    $futureResult = Get-Item -LiteralPath $resultPath
    Assert-Throws {
        Get-SPatchBenchmarkReleaseDelayMilliseconds `
            $futureResult $fixedNowUtc 15 5 20
    } 'too far in the future' 'Future result timestamp bound'
    [IO.File]::SetLastWriteTimeUtc(
        $resultPath, $fixedNowUtc.AddSeconds(-1))
    $recentResult = Get-Item -LiteralPath $resultPath
    $boundedDelay = Get-SPatchBenchmarkReleaseDelayMilliseconds `
        $recentResult $fixedNowUtc 15 5 20
    Assert-True ($boundedDelay -gt 0 -and $boundedDelay -le 20000) `
        'Recent result release delay exceeded its explicit bound.'

    $outside = Join-Path $temporaryBase 'outside'
    [void] (New-Item -ItemType Directory -Path $outside)
    $junction = Join-Path $gameRoot 'unsafe-link'
    [void] (New-Item -ItemType Junction -Path $junction -Target $outside)
    Assert-Throws {
        Assert-SPatchMutationPathSafe `
            $gameRoot (Join-Path $junction 'config.ini') 'Reparse fixture'
    } 'reparse point' 'Reparse mutation rejection'
    [IO.Directory]::Delete($junction)
    $junction = $null

    Assert-True (@(Get-Process -Name sdhdship -ErrorAction SilentlyContinue).Count -eq 0) `
        'A real or unrelated sdhdship process is active; resistant-process fixture cannot run safely.'
    $fakeGame = Join-Path $gameRoot 'sdhdship.exe'
    Copy-Item -LiteralPath 'C:\Windows\System32\cmd.exe' `
        -Destination $fakeGame
    $targets = @(Get-SPatchUserStateTargets $gameRoot $displayPath)
    [void] (New-SPatchRecoveryBackup $gameRoot $targets 'resistant-process-fixture')
    Write-SPatchAtomicBytes $gameRoot (Join-Path $gameRoot 'SPatch.ini') `
        ([byte[]](4, 4, 4)) 'SPatch.ini resistant fixture'
    $fakeGameProcess = Start-Process -FilePath $fakeGame `
        -ArgumentList '/d', '/c', 'ping 127.0.0.1 -n 30 > nul' `
        -WindowStyle Hidden -PassThru
    $ownedProcesses.Add($fakeGameProcess)
    Start-Sleep -Milliseconds 250
    Assert-Throws {
        Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup
    } 'refused while a game process is live' `
        'Live unowned/resistant-process restoration refusal'
    Stop-Process -Id $fakeGameProcess.Id -Force
    [void] $fakeGameProcess.WaitForExit(5000)
    [void] (Restore-SPatchRecoveryBackup $gameRoot $targets -KeepBackup)
    Complete-SPatchRecoveryBackup $gameRoot $targets
    Assert-Bytes (Join-Path $gameRoot 'SPatch.ini') `
        $originals['SPatch.ini'] 'Post-resistant-process recovery'

    $globalMutex = [Threading.Mutex]::new(
        $false, $script:SPatchLiveHarnessMutexName)
    try {
        $ownsGlobalMutex = $globalMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $ownsGlobalMutex = $true
    }
    Assert-True $ownsGlobalMutex 'Could not acquire the fixture live-harness mutex.'
    $fakeShortcut = Join-Path $temporaryBase 'Fake Benchmark.lnk'
    $windowsPowerShell =
        'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($fakeShortcut)
    $link.TargetPath = $windowsPowerShell
    $link.Arguments = ('-NoLogo -NoProfile -NonInteractive -File "{0}" -FakeShortcutChild' -f
        $PSCommandPath)
    $link.WorkingDirectory = $repoRoot
    $link.Save()
    $script:SPatchBenchmarkShortcutPath = $fakeShortcut
    $requestedUtc = [DateTime]::UtcNow
    $launch = Invoke-SPatchBenchmarkShortcut `
        $windowsPowerShell $requestedUtc 20
    $ownedProcesses.Add($launch.Process)
    Assert-True (Test-SPatchProcessIdentity `
            $launch.Process $launch.ProcessIdentity.ProcessId `
            $launch.ProcessIdentity.StartTimeUtcTicks `
            $launch.ProcessIdentity.Executable) `
        'Fake shortcut receipt did not prove exact PID/start/path ownership.'
    Assert-True (-not (Test-SPatchProcessIdentity `
            $launch.Process $launch.ProcessIdentity.ProcessId `
            ($launch.ProcessIdentity.StartTimeUtcTicks + 1) `
            $launch.ProcessIdentity.Executable)) `
        'Process ownership accepted a mismatched start time.'
    Stop-Process -Id $launch.Process.Id -Force
    [void] $launch.Process.WaitForExit(5000)

    $arbitraryReceiptPath = Join-Path $temporaryBase 'arbitrary-receipt.json'
    $arbitraryReceiptBytes = [Text.Encoding]::UTF8.GetBytes(
        'UNTRUSTED_RECEIPT_SENTINEL')
    [IO.File]::WriteAllBytes($arbitraryReceiptPath, $arbitraryReceiptBytes)
    $receiptLeaseId = [Guid]::NewGuid().ToString('N')
    Assert-Throws {
        Write-SPatchLaunchReceipt ([pscustomobject]@{
                LeaseId = $receiptLeaseId
                ReceiptPath = $arbitraryReceiptPath
                ShortcutPath = $fakeShortcut
            }) ([ordered]@{ status = 'failed' })
    } 'outside the fixed receipt directory' `
        'Arbitrary verified receipt-path rejection'
    Assert-Bytes $arbitraryReceiptPath $arbitraryReceiptBytes `
        'Arbitrary receipt sentinel after verified-path rejection'

    $receiptEnvironmentNames = @(
        'SPATCH_BENCHMARK_LEASE_ID',
        'SPATCH_BENCHMARK_RECEIPT',
        'SPATCH_BENCHMARK_SHORTCUT')
    $previousReceiptEnvironment = @{}
    foreach ($name in $receiptEnvironmentNames) {
        $previousReceiptEnvironment[$name] =
            [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    $fixedFailurePath = Get-SPatchBenchmarkReceiptPath $receiptLeaseId
    try {
        [Environment]::SetEnvironmentVariable(
            'SPATCH_BENCHMARK_LEASE_ID', $receiptLeaseId, 'Process')
        [Environment]::SetEnvironmentVariable(
            'SPATCH_BENCHMARK_RECEIPT', $arbitraryReceiptPath, 'Process')
        [Environment]::SetEnvironmentVariable(
            'SPATCH_BENCHMARK_SHORTCUT', $fakeShortcut, 'Process')
        Assert-True (Write-SPatchUnverifiedLaunchFailure `
                'fixture advisory failure') `
            'Unverified failure did not publish to the fixed receipt path.'
        $fixedFailure = [IO.File]::ReadAllText($fixedFailurePath) |
            ConvertFrom-Json -ErrorAction Stop
        Assert-True (
            [string] $fixedFailure.status -ceq 'failed' -and
            [string] $fixedFailure.error -ceq 'fixture advisory failure' -and
            [int] $fixedFailure.launcher_pid -eq 0 -and
            [int] $fixedFailure.game_pid -eq 0) `
            'Unverified fixed-path failure receipt has the wrong contract.'
        Assert-Bytes $arbitraryReceiptPath $arbitraryReceiptBytes `
            'Arbitrary receipt sentinel after unverified failure'
        Remove-Item -LiteralPath $fixedFailurePath -Force
        [Environment]::SetEnvironmentVariable(
            'SPATCH_BENCHMARK_SHORTCUT',
            (Join-Path $temporaryBase 'Wrong Benchmark.lnk'), 'Process')
        Assert-True (-not (Write-SPatchUnverifiedLaunchFailure `
                    'wrong-shortcut fixture')) `
            'Unverified failure accepted a non-required shortcut.'
    } finally {
        foreach ($name in $receiptEnvironmentNames) {
            [Environment]::SetEnvironmentVariable(
                $name, $previousReceiptEnvironment[$name], 'Process')
        }
        if (Test-Path -LiteralPath $fixedFailurePath -PathType Leaf) {
            Remove-Item -LiteralPath $fixedFailurePath -Force
        }
    }

    $delayedShortcut = Join-Path $temporaryBase 'Delayed Fake Benchmark.lnk'
    $delayedLink = $shell.CreateShortcut($delayedShortcut)
    $delayedLink.TargetPath = $windowsPowerShell
    $delayedLink.Arguments = ('-NoLogo -NoProfile -NonInteractive -File "{0}" -FakeDelayedShortcutChild' -f
        $PSCommandPath)
    $delayedLink.WorkingDirectory = $repoRoot
    $delayedLink.Save()
    $delayMarker = Join-Path $temporaryBase 'delayed-child.json'
    $previousDelayMarker = [Environment]::GetEnvironmentVariable(
        'SPATCH_TEST_DELAY_MARKER', 'Process')
    $script:SPatchBenchmarkShortcutPath = $delayedShortcut
    try {
        [Environment]::SetEnvironmentVariable(
            'SPATCH_TEST_DELAY_MARKER', $delayMarker, 'Process')
        Assert-Throws {
            Invoke-SPatchBenchmarkShortcut `
                $windowsPowerShell ([DateTime]::UtcNow) 5
        } 'did not publish a launch receipt' `
            'Delayed joined-child timeout cancellation'
        Assert-True (Test-Path -LiteralPath $delayMarker -PathType Leaf) `
            'Delayed child did not prove it joined the lease before timeout.'
        $delayedIdentity = [IO.File]::ReadAllText($delayMarker) |
            ConvertFrom-Json -ErrorAction Stop
        $delayedProcess = Get-Process -Id ([int] $delayedIdentity.ProcessId) `
            -ErrorAction SilentlyContinue
        Assert-True (
            $null -eq $delayedProcess -or
            -not (Test-SPatchProcessIdentity `
                $delayedProcess ([int] $delayedIdentity.ProcessId) `
                ([int64] $delayedIdentity.StartTimeUtcTicks) `
                ([string] $delayedIdentity.Executable))) `
            'Timed-out delayed shortcut launcher remained live.'
        Start-Sleep -Seconds 4
        Assert-True (-not (Test-Path -LiteralPath `
                ($delayMarker + '.late') -PathType Leaf)) `
            'Timed-out shortcut child reached its late launch path.'
    } finally {
        [Environment]::SetEnvironmentVariable(
            'SPATCH_TEST_DELAY_MARKER', $previousDelayMarker, 'Process')
        $script:SPatchBenchmarkShortcutPath = $fakeShortcut
    }

    $compatibilityOutput = Join-Path $temporaryBase 'windows-powershell.txt'
    $compatibilityError = Join-Path $temporaryBase 'windows-powershell.err.txt'
    $compatibilityProcess = Start-Process -FilePath $windowsPowerShell `
        -ArgumentList @(
            '-NoLogo',
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy', 'Bypass',
            '-File', ('"{0}"' -f $PSCommandPath),
            '-CompatibilityChild') `
        -RedirectStandardOutput $compatibilityOutput `
        -RedirectStandardError $compatibilityError `
        -WindowStyle Hidden -PassThru -Wait
    $compatibilityProcess.Refresh()
    Assert-True ($compatibilityProcess.ExitCode -eq 0) `
        ("Windows PowerShell helper load failed with code " +
            "$($compatibilityProcess.ExitCode): " +
            $(if (Test-Path -LiteralPath $compatibilityError) {
                    [IO.File]::ReadAllText($compatibilityError)
                } else { '' }))
    Assert-True (([IO.File]::ReadAllText($compatibilityOutput)).Contains(
            'WINDOWS_POWERSHELL_HELPER_LOAD=PASS')) `
        'Windows PowerShell compatibility child emitted no pass marker.'

    $globalMutex.ReleaseMutex()
    $ownsGlobalMutex = $false
    Assert-Throws {
        Invoke-SPatchBenchmarkShortcut `
            $windowsPowerShell ([DateTime]::UtcNow) 20
    } 'lease mutexes are not held' `
        'Missing global live-harness mutex observability'
    Assert-Bytes $arbitraryReceiptPath $arbitraryReceiptBytes `
        'Arbitrary receipt sentinel after missing-global-mutex launch'

    'LIVE_HARNESS_AST=PASS'
    'ATOMIC_RECOVERY_FIXTURES=PASS'
    'RECOVERY_MANIFEST_COVERAGE=PASS'
    'AGGREGATE_RECOVERY_FAILURE_INJECTION=PASS'
    'RESULT_IDENTITY_AND_FUTURE_DELAY=PASS'
    'REPARSE_REJECTION=PASS'
    'RESISTANT_PROCESS_RESTORE_REFUSAL=PASS'
    'SHORTCUT_LEASE_PID_START_PATH=PASS'
    'FIXED_FAILURE_RECEIPT=PASS'
    'DELAYED_SHORTCUT_CANCELLATION=PASS'
    'MISSING_GLOBAL_MUTEX_OBSERVABILITY=PASS'
    'WINDOWS_POWERSHELL_COMPATIBILITY=PASS'
} finally {
    if ($null -ne $lockedTargetStream) {
        $lockedTargetStream.Dispose()
    }
    foreach ($process in $ownedProcesses) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                [void] $process.WaitForExit(5000)
            }
        } catch { }
    }
    if ($ownsGlobalMutex) {
        $globalMutex.ReleaseMutex()
    }
    if ($null -ne $globalMutex) {
        $globalMutex.Dispose()
    }
    if ($null -ne $junction -and (Test-Path -LiteralPath $junction)) {
        [IO.Directory]::Delete($junction)
    }
    if (Test-Path -LiteralPath $temporaryBase) {
        $resolvedTemporary = [IO.Path]::GetFullPath($temporaryBase)
        if (-not $resolvedTemporary.StartsWith(
                $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove fixture path outside the temporary root: $resolvedTemporary"
        }
        Remove-Item -LiteralPath $resolvedTemporary -Recurse -Force
    }
}
