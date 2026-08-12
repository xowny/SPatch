[CmdletBinding()]
param(
    [string] $GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\SleepingDogsDefinitiveEdition',
    [ValidateRange(5, 300)]
    [int] $SteamStartupTimeoutSeconds = 60,
    [ValidateRange(1, 30)]
    [int] $GameStartupValidationSeconds = 3,
    [ValidateRange(30, 1800)]
    [int] $BenchmarkTimeoutSeconds = 360,
    [switch] $ActivateWindow,
    [switch] $EnsureFullscreen,
    [switch] $Wait,
    [switch] $PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$safetyHelper = Join-Path $PSScriptRoot 'LiveHarnessSafety.ps1'
if (-not (Test-Path -LiteralPath $safetyHelper -PathType Leaf)) {
    throw "Live harness safety helper is missing: $safetyHelper"
}
. $safetyHelper
if ($script:SPatchLiveHarnessSafetyVersion -cne '2026.08.10.6' -or
    -not (Test-SPatchPathEqual `
        $script:SPatchLiveHarnessSafetyPath $safetyHelper)) {
    throw 'The exact live harness safety helper was not loaded.'
}

$GameRoot = [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/')
$gogDisplaySettings = Join-Path $GameRoot 'Save\DisplaySettings.xml'
$gogMarkers = @(
    Get-ChildItem -LiteralPath $GameRoot -Filter 'goggame-*.info' -File `
        -ErrorAction SilentlyContinue)
if ((Test-Path -LiteralPath $gogDisplaySettings -PathType Leaf) -or
    $gogMarkers.Count -ne 0) {
    throw 'The unattended benchmark launcher is Steamworks-only; no live GOG launch path has been validated.'
}
$gameExe = Join-Path $GameRoot 'sdhdship.exe'
if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw "Sleeping Dogs: Definitive Edition executable is missing: $gameExe"
}

function Get-SteamProcesses {
    return @(Get-Process -Name steam -ErrorAction SilentlyContinue)
}

function Resolve-SteamExecutable {
    $candidates = [Collections.Generic.List[string]]::new()

    # Derive the normal Steam root from
    # <Steam>\steamapps\common\<game> before consulting the registry.
    $commonRoot = Split-Path -Path $GameRoot -Parent
    $steamAppsRoot = Split-Path -Path $commonRoot -Parent
    $derivedSteamRoot = Split-Path -Path $steamAppsRoot -Parent
    if (-not [string]::IsNullOrWhiteSpace($derivedSteamRoot)) {
        $candidates.Add((Join-Path $derivedSteamRoot 'steam.exe'))
    }

    foreach ($entry in @(
            @('HKCU:\Software\Valve\Steam', 'SteamExe', $false),
            @('HKCU:\Software\Valve\Steam', 'SteamPath', $true),
            @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', $true),
            @('HKLM:\SOFTWARE\Valve\Steam', 'InstallPath', $true))) {
        try {
            $properties = Get-ItemProperty -LiteralPath $entry[0] `
                -ErrorAction Stop
            $property = $properties.PSObject.Properties[$entry[1]]
            if ($null -eq $property -or
                [string]::IsNullOrWhiteSpace([string] $property.Value)) {
                continue
            }
            $candidate = [string] $property.Value
            if ($entry[2]) {
                $candidate = Join-Path $candidate 'steam.exe'
            }
            $candidates.Add($candidate)
        } catch {
            # A registry view can be absent on a valid Steam installation.
        }
    }

    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $checked = [Collections.Generic.List[string]]::new()
    foreach ($candidate in $candidates) {
        try {
            $normalized = [IO.Path]::GetFullPath($candidate)
        } catch {
            continue
        }
        if (-not $seen.Add($normalized)) {
            continue
        }
        $checked.Add($normalized)
        if (Test-Path -LiteralPath $normalized -PathType Leaf) {
            return $normalized
        }
    }

    throw ('Steam is not running and steam.exe could not be located. Checked: ' +
        ($checked -join '; '))
}

function Get-SteamReadiness {
    $steamProcesses = @(Get-SteamProcesses)
    if ($steamProcesses.Count -eq 0) {
        return [pscustomobject]@{
            Ready = $false
            Status = 'no steam.exe process is running'
        }
    }

    try {
        $active = Get-ItemProperty -LiteralPath `
            'HKCU:\Software\Valve\Steam\ActiveProcess' -ErrorAction Stop
        $activePidProperty = $active.PSObject.Properties['pid']
        $activeUserProperty = $active.PSObject.Properties['ActiveUser']
        if ($null -eq $activePidProperty -or
            [int] $activePidProperty.Value -le 0) {
            throw 'the ActiveProcess PID is unavailable'
        }
        if ($null -eq $activeUserProperty -or
            [int64] $activeUserProperty.Value -le 0) {
            throw 'Steam has no signed-in active user'
        }
        $activePid = [int] $activePidProperty.Value
    } catch {
        return [pscustomobject]@{
            Ready = $false
            Status = "Steam IPC is not initialized: $($_.Exception.Message)"
        }
    }

    $activeProcess = @($steamProcesses | Where-Object Id -EQ $activePid)
    if ($activeProcess.Count -ne 1) {
        return [pscustomobject]@{
            Ready = $false
            Status = "Steam's registered active PID $activePid is not running"
        }
    }
    try {
        $activeProcess[0].Refresh()
        if (-not $activeProcess[0].Responding) {
            throw 'the active Steam process is not responding'
        }
        $hasClientModule = @($activeProcess[0].Modules | Where-Object {
                $_.ModuleName -ieq 'steamclient.dll' -or
                $_.ModuleName -ieq 'steamclient64.dll'
            }).Count -gt 0
        if (-not $hasClientModule) {
            throw 'the Steam client module is not loaded yet'
        }
    } catch {
        return [pscustomobject]@{
            Ready = $false
            Status = "Steam is still starting: $($_.Exception.Message)"
        }
    }

    return [pscustomobject]@{
        Ready = $true
        Status = "Steam IPC is ready on PID $activePid"
    }
}

function Wait-SteamReady([object] $MutationOwnerLease = $null) {
    $deadline = (Get-Date).AddSeconds($SteamStartupTimeoutSeconds)
    $consecutiveReadyChecks = 0
    $lastStatus = 'Steam readiness has not been checked'
    while ((Get-Date) -lt $deadline) {
        if ($null -ne $MutationOwnerLease) {
            [void] (Assert-SPatchJoinedMutationLeaseActive $MutationOwnerLease)
        }
        $readiness = Get-SteamReadiness
        $lastStatus = $readiness.Status
        if ($readiness.Ready) {
            ++$consecutiveReadyChecks
            # Avoid racing a transient ActiveProcess registry update during a
            # Steam self-update or process handoff.
            if ($consecutiveReadyChecks -ge 2) {
                Write-Verbose $lastStatus
                return
            }
        } else {
            $consecutiveReadyChecks = 0
        }
        Start-Sleep -Milliseconds 250
    }

    throw ("Steam did not become ready within $SteamStartupTimeoutSeconds " +
        "seconds. Last state: $lastStatus. Sign in to Steam once, then retry.")
}

function Test-BenchmarkResult {
    param([Parameter(Mandatory)][IO.FileInfo] $Result)

    try {
        [xml] $document = Get-Content -LiteralPath $Result.FullName -Raw `
            -ErrorAction Stop
        $fps = $document.Benchmark.FPS
        if ($null -eq $fps) {
            return $false
        }
        $values = @{}
        foreach ($metric in @('Average', 'Max', 'Min')) {
            $node = $fps.$metric
            if ($null -eq $node) {
                return $false
            }
            $match = [regex]::Match(
                [string] $node.value,
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
                return $false
            }
            $expectedBits = '{0:X8}' -f [BitConverter]::ToUInt32(
                [BitConverter]::GetBytes($parsed), 0)
            if ($match.Groups['bits'].Value.ToUpperInvariant() -cne
                $expectedBits) {
                return $false
            }
            $values[$metric] = $parsed
        }
        if ($values['Average'] -le 0 -or $values['Max'] -le 0 -or
            $values['Min'] -lt 0 -or
            $values['Min'] -gt $values['Average'] -or
            $values['Average'] -gt $values['Max']) {
            return $false
        }
        return $true
    } catch {
        return $false
    }
}

function Test-TaskOwnedProcess {
    param(
        [Parameter(Mandatory)]
        [Diagnostics.Process] $Process,
        [Parameter(Mandatory)]
        [string] $ExpectedExecutable,
        [Parameter(Mandatory)]
        [DateTime] $NotBeforeUtc
    )

    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            return $false
        }
        $actualExecutable = [IO.Path]::GetFullPath($Process.Path)
        $expected = [IO.Path]::GetFullPath($ExpectedExecutable)
        $startTimeUtc = $Process.StartTime.ToUniversalTime()
        return $actualExecutable.Equals(
            $expected, [StringComparison]::OrdinalIgnoreCase) -and
            $startTimeUtc -ge $NotBeforeUtc.AddSeconds(-2)
    } catch {
        # If ownership cannot be proven, leave the process alone.
        return $false
    }
}

function Stop-TaskOwnedProcess {
    param(
        [Parameter(Mandatory)]
        [Diagnostics.Process] $Process,
        [Parameter(Mandatory)]
        [string] $ExpectedExecutable,
        [Parameter(Mandatory)]
        [DateTime] $NotBeforeUtc,
        [Parameter(Mandatory)]
        [string] $Description
    )

    if (-not (Test-TaskOwnedProcess -Process $Process `
            -ExpectedExecutable $ExpectedExecutable `
            -NotBeforeUtc $NotBeforeUtc)) {
        return
    }
    try {
        Stop-Process -Id $Process.Id -Force -ErrorAction Stop
        if (-not $Process.WaitForExit(5000)) {
            throw 'the process did not exit within five seconds'
        }
        Write-Verbose "Stopped task-owned $Description process $($Process.Id)."
    } catch {
        Write-Warning ("Could not stop task-owned $Description process " +
            "$($Process.Id): $($_.Exception.Message)")
    }
}

function Stop-TaskOwnedSteamProcess {
    param(
        [Parameter(Mandatory)]
        [Diagnostics.Process] $Process,
        [Parameter(Mandatory)]
        [string] $ExpectedExecutable,
        [Parameter(Mandatory)]
        [DateTime] $NotBeforeUtc
    )

    Stop-TaskOwnedProcess -Process $Process `
        -ExpectedExecutable $ExpectedExecutable `
        -NotBeforeUtc $NotBeforeUtc `
        -Description 'Steam'
    $remaining = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if ($remaining -and
        (Test-TaskOwnedProcess -Process $remaining `
            -ExpectedExecutable $ExpectedExecutable `
            -NotBeforeUtc $NotBeforeUtc)) {
        throw "Task-owned Steam process remained after cleanup: $($Process.Id)"
    }
}

$joinedMutationOwnerLease = $null
try {
    $joinedMutationOwnerLease = Get-SPatchJoinedMutationLease
} catch {
    [void] (Write-SPatchUnverifiedLaunchFailure $_.Exception.Message)
    throw
}
if ($null -ne $joinedMutationOwnerLease) {
    # The required desktop shortcut intentionally has no harness-specific
    # arguments. A verified parent lease opts its launch into the same window
    # activation/fullscreen contract used by Final and PBR validation.
    $ActivateWindow = [switch]::Present
    $EnsureFullscreen = [switch]::Present
}
$liveHarnessMutex = [Threading.Mutex]::new(
    $false, $script:SPatchLiveHarnessMutexName)
$ownsLiveHarnessMutex = $false
$launchReceiptWritten = $false
$launcherStartTimeUtc = [DateTime]::UtcNow
$startedSteam = $false
$startedSteamExe = $null
$startedSteamProcess = $null
$startedSteamStartTimeUtc = $null
$process = $null
$completed = $false
try {
    if ($null -eq $joinedMutationOwnerLease) {
        try {
            $ownsLiveHarnessMutex = $liveHarnessMutex.WaitOne(0)
        } catch [Threading.AbandonedMutexException] {
            $ownsLiveHarnessMutex = $true
        }
        if (-not $ownsLiveHarnessMutex) {
            throw 'Another SPatch live mutation already owns the game installation.'
        }
    }
    if (Get-Process -Name sdhdship -ErrorAction SilentlyContinue) {
        throw 'A game process was active before the unattended benchmark launch.'
    }

    if ($null -ne $joinedMutationOwnerLease) {
        [void] (Assert-SPatchJoinedMutationLeaseActive `
            $joinedMutationOwnerLease)
    }

    $steamProcesses = @(Get-SteamProcesses)
    if ($steamProcesses.Count -eq 0) {
        $steamExe = Resolve-SteamExecutable
        try {
            $startedSteamProcess = Start-Process -FilePath $steamExe `
                -ArgumentList '-silent' `
                -WorkingDirectory (Split-Path -Path $steamExe -Parent) `
                -WindowStyle Hidden `
                -PassThru
            $startedSteam = $true
            $startedSteamExe = $steamExe
            $startedSteamStartTimeUtc =
                $startedSteamProcess.StartTime.ToUniversalTime()
            Write-Verbose "Started Steam silently for Steamworks initialization: $steamExe"
        } catch {
            throw "Steam was not running and could not be started: $($_.Exception.Message)"
        }
    } else {
        Write-Verbose 'Using the existing Steam client; it will not be stopped.'
    }
    Wait-SteamReady $joinedMutationOwnerLease
    if ($startedSteam) {
    # A cold Steam client can publish ActiveProcess before its Steamworks app
    # launch path is usable. Launching the game immediately then exits after
    # SteamWinsockInitFakeClass without creating a benchmark result.
        Write-Verbose 'Allowing the cold-start Steamworks launch path to settle.'
        foreach ($settleInterval in 1..40) {
            if ($null -ne $joinedMutationOwnerLease) {
                [void] (Assert-SPatchJoinedMutationLeaseActive `
                    $joinedMutationOwnerLease)
            }
            Start-Sleep -Milliseconds 250
        }
    }

if ($null -ne $joinedMutationOwnerLease) {
    [void] (Assert-SPatchJoinedMutationLeaseActive $joinedMutationOwnerLease)
}

# A raw sdhdship.exe launch exits during Steamworks initialization unless the
# Steam client is ready. Supplying the app identity keeps the game launch
# direct while avoiding Steam's -applaunch/Continue UI. The child inherits
# these process-scoped values; the caller's environment is restored after
# creation.
$previousAppId = [Environment]::GetEnvironmentVariable('SteamAppId', 'Process')
$previousGameId = [Environment]::GetEnvironmentVariable('SteamGameId', 'Process')
$benchmarkResultsBefore = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($result in @(Get-ChildItem -LiteralPath $GameRoot `
        -Filter 'BenchmarkResult-*.xml' -File -ErrorAction SilentlyContinue)) {
    [void] $benchmarkResultsBefore.Add($result.FullName)
}
$benchmarkLaunchTimeUtc = [DateTime]::UtcNow
try {
    [Environment]::SetEnvironmentVariable('SteamAppId', '307690', 'Process')
    [Environment]::SetEnvironmentVariable('SteamGameId', '307690', 'Process')
    $process = Start-Process -FilePath $gameExe `
        -ArgumentList '-benchmark', '-skipStartScreen' `
        -WorkingDirectory $GameRoot `
        -PassThru
} finally {
    [Environment]::SetEnvironmentVariable(
        'SteamAppId', $previousAppId, 'Process')
    [Environment]::SetEnvironmentVariable(
        'SteamGameId', $previousGameId, 'Process')
}

if ($null -ne $joinedMutationOwnerLease) {
    [void] (Assert-SPatchJoinedMutationLeaseActive $joinedMutationOwnerLease)
}

# Do not report a process that died during Steamworks/bootstrap initialization.
# Existing callers use this object as ownership evidence for cleanup.
$shell = $null
$activated = $false
if ($ActivateWindow -or $EnsureFullscreen) {
    # The game requests exclusive fullscreen roughly two seconds after process
    # creation. Activate its window as soon as it exists so a foreground Codex
    # or terminal window cannot make DXGI immediately fall back to windowed.
    $shell = New-Object -ComObject WScript.Shell
}
$startupDeadline = (Get-Date).AddSeconds($GameStartupValidationSeconds)
while ((Get-Date) -lt $startupDeadline) {
    $process.Refresh()
    if ($process.HasExited) {
        throw ("The direct unattended benchmark exited during its " +
            "$GameStartupValidationSeconds-second startup validation with " +
            "code $($process.ExitCode). Steam was ready; check the game and " +
            'SPatch/ReShade logs for the bootstrap failure.')
    }
    if ($null -ne $shell -and $process.MainWindowHandle -ne 0) {
        # Keep asserting foreground ownership through bootstrap. Fast arms can
        # create the window before DXGI changes display mode, and a one-shot
        # activation may be lost between those two events.
        $activated = $shell.AppActivate($process.Id) -or $activated
    }
    Start-Sleep -Milliseconds 100
}

if ($ActivateWindow -or $EnsureFullscreen) {
    $activationDeadline = (Get-Date).AddSeconds(10)
    while (-not $activated -and (Get-Date) -lt $activationDeadline) {
        $process.Refresh()
        if ($process.HasExited) {
            break
        }
        if ($process.MainWindowHandle -ne 0 -and
            $shell.AppActivate($process.Id)) {
            $activated = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $activated) {
        throw 'The unattended benchmark window could not be activated for exclusive-fullscreen validation.'
    }
    Start-Sleep -Milliseconds 250
}

if ($EnsureFullscreen) {
    $script:lastBenchmarkFullscreenProbe = 'not_checked'
    if ($null -eq ('SPatchBenchmarkWindow' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SPatchBenchmarkWindow
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Point
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetClientRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ClientToScreen(IntPtr window, ref Point point);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int index);
}
'@
    }

    function Test-BenchmarkFullscreen {
        $process.Refresh()
        if ($process.HasExited -or $process.MainWindowHandle -eq 0) {
            $script:lastBenchmarkFullscreenProbe =
                "window_unavailable exited=$($process.HasExited) handle=$($process.MainWindowHandle)"
            return $false
        }
        $window = $process.MainWindowHandle
        $windowRect = [SPatchBenchmarkWindow+Rect]::new()
        $clientRect = [SPatchBenchmarkWindow+Rect]::new()
        $clientOrigin = [SPatchBenchmarkWindow+Point]::new()
        if (-not [SPatchBenchmarkWindow]::GetWindowRect(
                $window, [ref] $windowRect) -or
            -not [SPatchBenchmarkWindow]::GetClientRect(
                $window, [ref] $clientRect) -or
            -not [SPatchBenchmarkWindow]::ClientToScreen(
                $window, [ref] $clientOrigin)) {
            $script:lastBenchmarkFullscreenProbe =
                "win32_query_failed last_error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            return $false
        }
        $screenWidth = [SPatchBenchmarkWindow]::GetSystemMetrics(0)
        $screenHeight = [SPatchBenchmarkWindow]::GetSystemMetrics(1)
        $windowCoversScreen =
            $windowRect.Left -eq 0 -and $windowRect.Top -eq 0 -and
            ($windowRect.Right - $windowRect.Left) -eq $screenWidth -and
            ($windowRect.Bottom - $windowRect.Top) -eq $screenHeight
        # DPI virtualization can round a 3840-pixel display to 1707 logical
        # pixels while the client width rounds to 1706. Accept only that
        # one-logical-pixel boundary error; a taskbar-sized/maximized client is
        # still far outside this tolerance.
        $logicalPixelTolerance = 1
        $clientCoversScreen =
            [Math]::Abs($clientOrigin.X) -le $logicalPixelTolerance -and
            [Math]::Abs($clientOrigin.Y) -le $logicalPixelTolerance -and
            [Math]::Abs(
                ($clientRect.Right - $clientRect.Left) - $screenWidth) -le
                    $logicalPixelTolerance -and
            [Math]::Abs(
                ($clientRect.Bottom - $clientRect.Top) - $screenHeight) -le
                    $logicalPixelTolerance
        $script:lastBenchmarkFullscreenProbe =
            "screen=${screenWidth}x${screenHeight} " +
            "window=$($windowRect.Left),$($windowRect.Top),$($windowRect.Right),$($windowRect.Bottom) " +
            "client_origin=$($clientOrigin.X),$($clientOrigin.Y) " +
            "client_size=$(($clientRect.Right - $clientRect.Left))x$(($clientRect.Bottom - $clientRect.Top)) " +
            "window_covers=$windowCoversScreen client_covers=$clientCoversScreen"
        # The drawable client area is the fullscreen contract. On current
        # Windows builds DWM may retain an invisible resize frame outside that
        # client area, so requiring the outer window rectangle to be exact can
        # reject a real 3840x2160 fullscreen swap chain as -8,-8..3848,2168.
        # Keep the outer-rectangle read above as a successful Win32 identity
        # check, but do not make invisible non-client pixels part of the gate.
        return $clientCoversScreen
    }

    if (-not (Test-BenchmarkFullscreen)) {
        # Foreground ownership can be lost between the startup activation loop
        # and this fallback (for example when the launcher is driven from an
        # automation host). Reassert it immediately before every Alt+Enter and
        # retry only while the window is still not fullscreen.
        foreach ($attempt in 1..3) {
            $process.Refresh()
            if ($process.HasExited -or $process.MainWindowHandle -eq 0) {
                break
            }
            [void] $shell.AppActivate($process.Id)
            Start-Sleep -Milliseconds 100
            $shell.SendKeys('%{ENTER}')
            # DXGI changes mode within a few frames when the hotkey lands. Give
            # the client area time to cover the display before retrying so a
            # delayed successful toggle is not immediately toggled back out.
            $fullscreenDeadline = (Get-Date).AddMilliseconds(1000)
            while ((Get-Date) -lt $fullscreenDeadline -and
                -not (Test-BenchmarkFullscreen)) {
                Start-Sleep -Milliseconds 100
            }
            if (Test-BenchmarkFullscreen) {
                break
            }
        }
        if (-not (Test-BenchmarkFullscreen)) {
            throw ('The unattended benchmark did not enter primary-display ' +
                'fullscreen after three foreground-reactivated Alt+Enter ' +
                "attempts. Last probe: $script:lastBenchmarkFullscreenProbe")
        }
    }
}

if ($null -ne $joinedMutationOwnerLease) {
    [void] (Assert-SPatchJoinedMutationLeaseActive $joinedMutationOwnerLease)
    $gameIdentity = Get-SPatchProcessIdentity $process
    $launcherIdentity = Get-SPatchProcessIdentity (
        [Diagnostics.Process]::GetCurrentProcess())
    Write-SPatchLaunchReceipt $joinedMutationOwnerLease ([ordered]@{
            status = 'launched'
            error = ''
            launcher_pid = $launcherIdentity.ProcessId
            launcher_start_utc_ticks = $launcherIdentity.StartTimeUtcTicks
            launcher_path = $launcherIdentity.Executable
            game_pid = $gameIdentity.ProcessId
            game_start_utc_ticks = $gameIdentity.StartTimeUtcTicks
            game_path = $gameIdentity.Executable
            started_steam = [bool] $startedSteam
            steam_pid = $(if ($startedSteamProcess) {
                    [int] $startedSteamProcess.Id
                } else { 0 })
            steam_start_utc_ticks = $(if ($startedSteamStartTimeUtc) {
                    [int64] $startedSteamStartTimeUtc.Ticks
                } else { 0 })
            steam_path = $(if ($startedSteamExe) {
                    [IO.Path]::GetFullPath($startedSteamExe)
                } else { '' })
        })
    $launchReceiptWritten = $true
    Write-Verbose ("Joined mutation owner PID " +
        "$($joinedMutationOwnerLease.OwnerPid) through $($joinedMutationOwnerLease.ShortcutPath).")
}

if ($Wait) {
    if (-not $process.WaitForExit($BenchmarkTimeoutSeconds * 1000)) {
        throw ("The unattended benchmark exceeded its " +
               "$BenchmarkTimeoutSeconds-second completion timeout.")
    }
    $newValidResults = @(Get-ChildItem -LiteralPath $GameRoot `
            -Filter 'BenchmarkResult-*.xml' -File -ErrorAction SilentlyContinue |
        Where-Object {
            -not $benchmarkResultsBefore.Contains($_.FullName) -and
            $_.LastWriteTimeUtc -ge $benchmarkLaunchTimeUtc.AddSeconds(-2) -and
            (Test-BenchmarkResult -Result $_)
        } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($newValidResults.Count -eq 0) {
        throw ("The unattended benchmark exited with code $($process.ExitCode) " +
            'without creating a valid new BenchmarkResult XML file.')
    }
    if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 1) {
        throw ("The unattended benchmark created a valid result but exited " +
            "with unexpected code $($process.ExitCode).")
    }
    # This game returns 1 after its normal benchmark-results path. Accept that
    # code only with the run-specific XML proof above so bootstrap/crash exits
    # cannot be mistaken for success.
    Write-Verbose ("Benchmark completed with game exit code " +
        "$($process.ExitCode): $($newValidResults[0].FullName)")
    if ($startedSteam) {
        Stop-TaskOwnedSteamProcess `
            -Process $startedSteamProcess `
            -ExpectedExecutable $startedSteamExe `
            -NotBeforeUtc $launcherStartTimeUtc
    }
}
    $completed = $true
    if ($PassThru) {
        Add-Member -InputObject $process `
            -NotePropertyName SPatchStartedSteam `
            -NotePropertyValue ([bool] $startedSteam) `
            -Force
        Add-Member -InputObject $process `
            -NotePropertyName SPatchStartedSteamExecutable `
            -NotePropertyValue $startedSteamExe `
            -Force
        Add-Member -InputObject $process `
            -NotePropertyName SPatchStartedSteamProcessId `
            -NotePropertyValue $(if ($startedSteamProcess) {
                    $startedSteamProcess.Id
                } else { $null }) `
            -Force
        Add-Member -InputObject $process `
            -NotePropertyName SPatchStartedSteamStartTimeUtc `
            -NotePropertyValue $startedSteamStartTimeUtc `
            -Force
        Add-Member -InputObject $process `
            -NotePropertyName SPatchBenchmarkShortcutPath `
            -NotePropertyValue $script:SPatchBenchmarkShortcutPath `
            -Force
        if ($Wait) {
            Add-Member -InputObject $process `
                -NotePropertyName SPatchGameExitCode `
                -NotePropertyValue ([int] $process.ExitCode) `
                -Force
        }
        $process
    }
} catch {
    $primaryFailure = $_
    if ($null -ne $joinedMutationOwnerLease -and -not $launchReceiptWritten) {
        try {
            $launcher = [Diagnostics.Process]::GetCurrentProcess()
            $launcherStartTicks = 0
            $launcherPath = ''
            try {
                $launcherIdentity = Get-SPatchProcessIdentity $launcher
                $launcherStartTicks = $launcherIdentity.StartTimeUtcTicks
                $launcherPath = $launcherIdentity.Executable
            } catch { }
            Write-SPatchLaunchReceipt $joinedMutationOwnerLease ([ordered]@{
                    status = 'failed'
                    error = $primaryFailure.Exception.Message
                    launcher_pid = $PID
                    launcher_start_utc_ticks = $launcherStartTicks
                    launcher_path = $launcherPath
                    game_pid = 0
                    game_start_utc_ticks = 0
                    game_path = ''
                    started_steam = [bool] $startedSteam
                    steam_pid = 0
                    steam_start_utc_ticks = 0
                    steam_path = ''
                })
            $launchReceiptWritten = $true
        } catch {
            Write-Warning ("Could not publish the shortcut launch failure receipt: " +
                $_.Exception.Message)
        }
    }
    throw $primaryFailure
} finally {
    if (-not $completed) {
        if ($null -ne $process) {
            Stop-TaskOwnedProcess -Process $process `
                -ExpectedExecutable $gameExe `
                -NotBeforeUtc $launcherStartTimeUtc `
                -Description 'game'
        }
        if ($startedSteam -and $null -ne $startedSteamProcess -and
            $null -ne $startedSteamExe) {
            try {
                Stop-TaskOwnedSteamProcess `
                    -Process $startedSteamProcess `
                    -ExpectedExecutable $startedSteamExe `
                    -NotBeforeUtc $launcherStartTimeUtc
            } catch {
                Write-Warning $_.Exception.Message
            }
        }
    }
    if ($ownsLiveHarnessMutex) {
        $liveHarnessMutex.ReleaseMutex()
    }
    $liveHarnessMutex.Dispose()
}
