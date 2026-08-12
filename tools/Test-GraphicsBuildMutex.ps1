[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$buildScript = Join-Path $repoRoot 'luma\Build-Luma.ps1'
$manifests = @(
    Join-Path $repoRoot 'artifacts\shenlong\Publishing-Release\ShenLong-Package\SHA256SUMS.txt'
    Join-Path $repoRoot 'artifacts\shenlong\Development-Release\ShenLong-Package\SHA256SUMS.txt'
)
$before = @($manifests | ForEach-Object {
        if (Test-Path -LiteralPath $_ -PathType Leaf) {
            (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
        } else {
            ''
        }
    })
$mutex = [Threading.Mutex]::new($false, 'Local\ShenLong.GraphicsBuild')
$owned = $false
$child = $null
try {
    $owned = $mutex.WaitOne(0)
    if (-not $owned) {
        throw 'The mutex test could not acquire the graphics-build mutex.'
    }
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = (Get-Process -Id $PID).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($argument in @(
            '-NoProfile', '-NonInteractive', '-File', $buildScript,
            '-Configuration', 'Publishing-Release', '-OfflineDependencies')) {
        $psi.ArgumentList.Add($argument)
    }
    $child = [Diagnostics.Process]::Start($psi)
    if (-not $child.WaitForExit(30000)) {
        $child.Kill($true)
        $child.WaitForExit()
        throw 'The graphics-build contention child did not exit within 30 seconds.'
    }
    $output = $child.StandardOutput.ReadToEnd() +
        $child.StandardError.ReadToEnd()
    if ($child.ExitCode -eq 0 -or
        $output.IndexOf(
            'Another ShenLong build is already in progress',
            [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "The graphics-build contention child failed incorrectly: $output"
    }
} finally {
    if ($owned) {
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
}

$after = @($manifests | ForEach-Object {
        if (Test-Path -LiteralPath $_ -PathType Leaf) {
            (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
        } else {
            ''
        }
    })
if ($before.Count -ne $after.Count) {
    throw 'The package-manifest census changed during mutex contention.'
}
for ($index = 0; $index -lt $before.Count; ++$index) {
    if ($before[$index] -cne $after[$index]) {
        throw 'A graphics package manifest changed during mutex contention.'
    }
}

[pscustomobject]@{
    Status = 'pass'
    ChildExitCode = $child.ExitCode
    PackageManifestsUnchanged = $true
}
