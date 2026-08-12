param(
    [string]$BuildScript = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Build-Luma.ps1')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$mutexName = 'Local\ShenLong.GraphicsBuild'
$expectedFailure =
    'Another ShenLong build is already in progress; shared validation and publication outputs are locked.'
$buildScriptPath = [IO.Path]::GetFullPath($BuildScript)
if (-not (Test-Path -LiteralPath $buildScriptPath -PathType Leaf)) {
    throw "Build-Luma script is missing: $buildScriptPath"
}
$buildText = [IO.File]::ReadAllText($buildScriptPath)
if ($buildText.IndexOf($mutexName, [StringComparison]::Ordinal) -lt 0 -or
    $buildText.IndexOf($expectedFailure, [StringComparison]::Ordinal) -lt 0) {
    throw 'Build-Luma no longer exposes the reviewed mutex name and contention failure.'
}
foreach ($offlineContract in @(
        'OfflineDependencies requires the pinned local ReShade checkout',
        'OfflineDependencies requires the pinned local ReShade setup',
        '"/p:MinHookOffline=$minHookOffline"')) {
    if ($buildText.IndexOf($offlineContract, [StringComparison]::Ordinal) -lt 0) {
        throw "Build-Luma no longer exposes the reviewed offline dependency contract: $offlineContract"
    }
}
foreach ($nativePolicyContract in @(
        'standalone\ShenLongNativePolicyTests.vcxproj',
        'build\shenlong-native-tests\ShenLongNativePolicyTests.exe',
        'ShenLong native-policy test build failed',
        'ShenLong native-policy tests failed')) {
    if ($buildText.IndexOf($nativePolicyContract, [StringComparison]::Ordinal) -lt 0) {
        throw "Build-Luma no longer runs the native-policy release gate: $nativePolicyContract"
    }
}

$ready = [Threading.ManualResetEventSlim]::new($false)
$release = [Threading.ManualResetEventSlim]::new($false)
$holder = [PowerShell]::Create()
$holderScript = {
    param(
        [string]$Name,
        [Threading.ManualResetEventSlim]$Ready,
        [Threading.ManualResetEventSlim]$Release
    )

    $mutex = [Threading.Mutex]::new($false, $Name)
    $ownsMutex = $false
    try {
        try {
            $ownsMutex = $mutex.WaitOne(0)
        } catch [Threading.AbandonedMutexException] {
            $ownsMutex = $true
        }
        if (-not $ownsMutex) {
            throw 'The contention-test holder could not acquire the graphics build mutex.'
        }
        $Ready.Set()
        if (-not $Release.Wait([TimeSpan]::FromSeconds(30))) {
            throw 'The contention test did not release its holder within 30 seconds.'
        }
    } finally {
        if ($ownsMutex) {
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
    }
}

[void]$holder.AddScript($holderScript.ToString())
[void]$holder.AddArgument($mutexName)
[void]$holder.AddArgument($ready)
[void]$holder.AddArgument($release)
$holderInvocation = $holder.BeginInvoke()
$testFailure = $null
try {
    if (-not $ready.Wait([TimeSpan]::FromSeconds(10))) {
        throw 'The contention-test holder did not acquire the mutex within 10 seconds.'
    }

    try {
        & $buildScriptPath -Configuration Publishing-Release -OfflineDependencies
        throw 'Build-Luma accepted a concurrent invocation while its mutex was held.'
    } catch {
        if ($_.Exception.Message -cne $expectedFailure) {
            throw "Build-Luma contention failed for an unexpected reason: $($_.Exception.Message)"
        }
    }
} catch {
    $testFailure = $_
} finally {
    $release.Set()
    try {
        [void]$holder.EndInvoke($holderInvocation)
    } catch {
        if ($null -eq $testFailure) {
            $testFailure = $_
        }
    }
    $holder.Dispose()
    $ready.Dispose()
    $release.Dispose()
}

if ($null -ne $testFailure) {
    throw $testFailure
}
Write-Host 'Rejected a concurrent Build-Luma invocation while the whole-build graphics mutex was held.'
