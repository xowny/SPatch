[CmdletBinding()]
param(
    [string]$ShaderPath,
    [string]$ControlShaderPath,
    [string]$OutputPath,
    [ValidateRange(0.0, 1.0)]
    [double]$MinimumHaloReduction = 0.5,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Find-MSBuild {
    $programFilesRoots = @(
        ${env:ProgramFiles(x86)},
        $env:ProgramFiles
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique

    foreach ($root in $programFilesRoots) {
        foreach ($visualStudioVersion in @('18', '2022')) {
            $candidate = Join-Path $root (
                "Microsoft Visual Studio\$visualStudioVersion\BuildTools\" +
                'MSBuild\Current\Bin\MSBuild.exe')
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    $vswhereCandidates = @()
    $vswhereOnPath = Get-Command -Name 'vswhere.exe' -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $vswhereOnPath) {
        $vswhereCandidates += $vswhereOnPath.Source
    }
    foreach ($root in $programFilesRoots) {
        $vswhereCandidates += Join-Path $root `
            'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    foreach ($vswhere in ($vswhereCandidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            continue
        }
        $installRoots = @(& $vswhere -latest -products * `
            -requires Microsoft.Component.MSBuild `
                Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null)
        if ($LASTEXITCODE -ne 0) {
            continue
        }
        foreach ($installRoot in $installRoots) {
            if ([string]::IsNullOrWhiteSpace($installRoot)) {
                continue
            }
            $candidate = Join-Path $installRoot.Trim() `
                'MSBuild\Current\Bin\MSBuild.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    throw 'MSBuild with the Visual C++ x64 toolchain was not found.'
}

function Convert-HalfBitsToDouble {
    param([Parameter(Mandatory = $true)][uint16]$Bits)

    $sign = if (($Bits -band 0x8000) -ne 0) { -1.0 } else { 1.0 }
    $exponent = ($Bits -shr 10) -band 0x1F
    $fraction = $Bits -band 0x03FF
    if ($exponent -eq 0) {
        if ($fraction -eq 0) { return $sign * 0.0 }
        return $sign * [Math]::Pow(2.0, -14.0) * ($fraction / 1024.0)
    }
    if ($exponent -eq 31) {
        if ($fraction -eq 0) { return $sign * [double]::PositiveInfinity }
        return [double]::NaN
    }
    return $sign * [Math]::Pow(2.0, [double]($exponent - 15)) *
        (1.0 + $fraction / 1024.0)
}

function Convert-R16BytesToValues {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    if (($Bytes.Length % 2) -ne 0) {
        throw "R16_FLOAT payload has an odd byte count: $($Bytes.Length)."
    }
    $values = [double[]]::new($Bytes.Length / 2)
    for ($index = 0; $index -lt $values.Length; ++$index) {
        $offset = $index * 2
        $bits = [uint16]([uint16]$Bytes[$offset] -bor
            ([uint16]$Bytes[$offset + 1] -shl 8))
        $values[$index] = Convert-HalfBitsToDouble -Bits $bits
    }
    return $values
}

function Convert-FinalBytesToVisibility {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    if (($Bytes.Length % 4) -ne 0) {
        throw "RGBA8 payload byte count is not divisible by four: $($Bytes.Length)."
    }
    $values = [double[]]::new($Bytes.Length / 4)
    for ($index = 0; $index -lt $values.Length; ++$index) {
        $values[$index] = $Bytes[$index * 4 + 2] / 255.0
    }
    return $values
}

function Get-RegionMean {
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][int[]]$Xs,
        [Parameter(Mandatory = $true)][int[]]$Ys,
        [Parameter(Mandatory = $true)][int]$Width
    )

    $sum = 0.0
    foreach ($y in $Ys) {
        foreach ($x in $Xs) {
            $value = $Values[$y * $Width + $x]
            if (-not [double]::IsFinite($value)) {
                throw "Fixture output contains a non-finite value at ($x, $y)."
            }
            $sum += $value
        }
    }
    return $sum / ($Xs.Count * $Ys.Count)
}

function Get-HaloMetrics {
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][int]$Width
    )

    # The foreground occupies x=[24,31], y=[12,51]. Measure only flat-wall
    # receivers: four columns beside each long silhouette edge versus symmetric
    # far-wall controls. Excluding the end caps prevents contact-edge filtering
    # from contaminating the detached-silhouette property.
    $receiverXs = [int[]](@(20..23) + @(32..35))
    $controlXs = [int[]](@(4..15) + @(48..59))
    $ys = [int[]](16..47)
    $receiverVisibility = Get-RegionMean `
        -Values $Values -Xs $receiverXs -Ys $ys -Width $Width
    $controlVisibility = Get-RegionMean `
        -Values $Values -Xs $controlXs -Ys $ys -Width $Width
    return [ordered]@{
        ReceiverVisibilityMean = $receiverVisibility
        FarWallVisibilityMean = $controlVisibility
        HaloExcessOcclusion = $controlVisibility - $receiverVisibility
        ReceiverPixels = $receiverXs.Count * $ys.Count
        FarWallPixels = $controlXs.Count * $ys.Count
    }
}

function Test-ExactFileMatch {
    param(
        [Parameter(Mandatory = $true)][string]$First,
        [Parameter(Mandatory = $true)][string]$Second
    )

    $firstInfo = Get-Item -LiteralPath $First
    $secondInfo = Get-Item -LiteralPath $Second
    if ($firstInfo.Length -ne $secondInfo.Length) { return $false }
    $firstHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $First).Hash
    $secondHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Second).Hash
    return $firstHash -eq $secondHash
}

function Invoke-FixtureRun {
    param(
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [Parameter(Mandatory = $true)][string]$InputShaderPath,
        [Parameter(Mandatory = $true)][string]$RunRoot,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][ValidateSet('sdao', 'gtao-lite')]
        [string]$Mode,
        [Parameter(Mandatory = $true)]
        [ValidateSet('detached-front', 'hidden-shell')]
        [string]$Case
    )

    $directory = Join-Path $RunRoot $Label
    $bytecodeDirectory = Join-Path $directory 'bytecode'
    New-Item -ItemType Directory -Path $bytecodeDirectory -Force | Out-Null
    $rawPath = Join-Path $directory 'raw-ao-r16.bin'
    $finalPath = Join-Path $directory 'final-ao-rgba8.bin'
    $arguments = @(
        $InputShaderPath,
        $rawPath,
        $finalPath,
        $bytecodeDirectory,
        $Mode,
        $Case
    )
    $executionOutput = @(& $ExecutablePath $arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Detached-halo fixture failed for $Label`n$($executionOutput -join [Environment]::NewLine)"
    }
    if ((Get-Item -LiteralPath $rawPath).Length -ne 64 * 64 * 2) {
        throw "Unexpected raw AO payload length for ${Label}: $((Get-Item -LiteralPath $rawPath).Length)."
    }
    if ((Get-Item -LiteralPath $finalPath).Length -ne 64 * 64 * 4) {
        throw "Unexpected final AO payload length for ${Label}: $((Get-Item -LiteralPath $finalPath).Length)."
    }
    $mainBytecodeName = if ($Mode -eq 'gtao-lite') {
        'main_pass_cs.q2.gtao-lite.cso'
    } else {
        'main_pass_cs.q2.sdao.cso'
    }
    $mainBytecodePath = Join-Path $bytecodeDirectory $mainBytecodeName
    if (-not (Test-Path -LiteralPath $mainBytecodePath -PathType Leaf)) {
        throw "Expected $Mode main-pass bytecode was not emitted: $mainBytecodePath"
    }
    return [pscustomobject][ordered]@{
        Label = $Label
        Mode = $Mode
        Case = $Case
        RawPath = $rawPath
        FinalPath = $finalPath
        RawSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $rawPath).Hash
        FinalSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $finalPath).Hash
        MainBytecodeSha256 = (Get-FileHash -Algorithm SHA256 `
            -LiteralPath $mainBytecodePath).Hash
        Execution = ($executionOutput -join [Environment]::NewLine).Trim()
    }
}

function Add-Assertion {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$Assertions,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [Parameter(Mandatory = $true)][string]$Evidence
    )

    $Assertions.Add([pscustomobject][ordered]@{
        Name = $Name
        Passed = $Passed
        Evidence = $Evidence
    })
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$harnessRoot = Join-Path $PSScriptRoot 'sdao-detached-halo'
$projectPath = Join-Path $harnessRoot 'SdaoDetachedHaloValidation.vcxproj'
$executablePath = Join-Path $repoRoot `
    '.tmp\sdao-detached-halo\build\bin\Release\SdaoDetachedHaloValidation.exe'
if ([string]::IsNullOrWhiteSpace($ShaderPath)) {
    $ShaderPath = Join-Path $repoRoot `
        'luma\overlay\Shaders\Sleeping Dogs Definitive Edition\Luma_SD_SDAO.hlsl'
}
if ([string]::IsNullOrWhiteSpace($ControlShaderPath)) {
    $ControlShaderPath = Join-Path $harnessRoot `
        'SdaoDetachedHaloControl.hlsl'
}
$ShaderPath = Get-FullPath -Path $ShaderPath
$ControlShaderPath = Get-FullPath -Path $ControlShaderPath
foreach ($requiredPath in @($projectPath, $ShaderPath, $ControlShaderPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required detached-halo fixture input does not exist: $requiredPath"
    }
}

$buildOutput = @()
if (-not $SkipBuild) {
    $msbuild = Find-MSBuild
    $buildOutput = @(& $msbuild $projectPath /nologo /m /t:Build `
        /p:Configuration=Release /p:Platform=x64 /verbosity:minimal 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Detached-halo WARP fixture build failed:`n$($buildOutput -join [Environment]::NewLine)"
    }
}
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Detached-halo fixture executable does not exist: $executablePath"
}

$runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'),
    ([Guid]::NewGuid().ToString('N').Substring(0, 8))
$runRoot = Join-Path $repoRoot ".tmp\sdao-detached-halo\runs\$runId"
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$runs = [ordered]@{}
foreach ($case in @('detached-front', 'hidden-shell')) {
    foreach ($mode in @('sdao', 'gtao-lite')) {
        $runs["$case-current-$mode-a"] = Invoke-FixtureRun `
            -ExecutablePath $executablePath -InputShaderPath $ShaderPath `
            -RunRoot $runRoot -Label "$case-current-$mode-a" `
            -Mode $mode -Case $case
        $runs["$case-current-$mode-b"] = Invoke-FixtureRun `
            -ExecutablePath $executablePath -InputShaderPath $ShaderPath `
            -RunRoot $runRoot -Label "$case-current-$mode-b" `
            -Mode $mode -Case $case
        $runs["$case-control-$mode"] = Invoke-FixtureRun `
            -ExecutablePath $executablePath -InputShaderPath $ControlShaderPath `
            -RunRoot $runRoot -Label "$case-control-$mode" `
            -Mode $mode -Case $case
    }
}

$assertions = [System.Collections.Generic.List[object]]::new()
$caseResults = [ordered]@{}
foreach ($case in @('detached-front', 'hidden-shell')) {
    $modeResults = [ordered]@{}
    foreach ($mode in @('sdao', 'gtao-lite')) {
        $currentA = $runs["$case-current-$mode-a"]
        $currentB = $runs["$case-current-$mode-b"]
        $control = $runs["$case-control-$mode"]
        $rawDeterministic = Test-ExactFileMatch `
            -First $currentA.RawPath -Second $currentB.RawPath
        $finalDeterministic = Test-ExactFileMatch `
            -First $currentA.FinalPath -Second $currentB.FinalPath
        $bytecodeDeterministic = $currentA.MainBytecodeSha256 -eq
            $currentB.MainBytecodeSha256
        Add-Assertion -Assertions $assertions `
            -Name "$case $mode raw output deterministic" `
            -Passed $rawDeterministic `
            -Evidence "$($currentA.RawSha256) versus $($currentB.RawSha256)"
        Add-Assertion -Assertions $assertions `
            -Name "$case $mode final output deterministic" `
            -Passed $finalDeterministic `
            -Evidence "$($currentA.FinalSha256) versus $($currentB.FinalSha256)"
        Add-Assertion -Assertions $assertions `
            -Name "$case $mode main bytecode deterministic" `
            -Passed $bytecodeDeterministic `
            -Evidence "$($currentA.MainBytecodeSha256) versus $($currentB.MainBytecodeSha256)"

        $currentRaw = Convert-R16BytesToValues `
            -Bytes ([System.IO.File]::ReadAllBytes($currentA.RawPath))
        $controlRaw = Convert-R16BytesToValues `
            -Bytes ([System.IO.File]::ReadAllBytes($control.RawPath))
        $currentFinal = Convert-FinalBytesToVisibility `
            -Bytes ([System.IO.File]::ReadAllBytes($currentA.FinalPath))
        $controlFinal = Convert-FinalBytesToVisibility `
            -Bytes ([System.IO.File]::ReadAllBytes($control.FinalPath))
        $currentRawMetrics = Get-HaloMetrics -Values $currentRaw -Width 64
        $controlRawMetrics = Get-HaloMetrics -Values $controlRaw -Width 64
        $currentFinalMetrics = Get-HaloMetrics -Values $currentFinal -Width 64
        $controlFinalMetrics = Get-HaloMetrics -Values $controlFinal -Width 64
        $rawReduction = if ($controlRawMetrics.HaloExcessOcclusion -gt 0.0) {
            ($controlRawMetrics.HaloExcessOcclusion -
                $currentRawMetrics.HaloExcessOcclusion) /
                $controlRawMetrics.HaloExcessOcclusion
        } else { -1.0 }
        $finalReduction = if ($controlFinalMetrics.HaloExcessOcclusion -gt 0.0) {
            ($controlFinalMetrics.HaloExcessOcclusion -
                $currentFinalMetrics.HaloExcessOcclusion) /
                $controlFinalMetrics.HaloExcessOcclusion
        } else { -1.0 }

        if ($case -eq 'detached-front' -or $mode -eq 'sdao') {
            Add-Assertion -Assertions $assertions `
                -Name "$case $mode candidate-6 raw fixture sensitivity" `
                -Passed ($controlRawMetrics.HaloExcessOcclusion -ge 0.005) `
                -Evidence "candidate-6 raw halo=$($controlRawMetrics.HaloExcessOcclusion)"
            Add-Assertion -Assertions $assertions `
                -Name "$case $mode raw halo materially lower" `
                -Passed ($rawReduction -ge $MinimumHaloReduction) `
                -Evidence "reduction=$rawReduction; required=$MinimumHaloReduction"
            Add-Assertion -Assertions $assertions `
                -Name "$case $mode candidate-6 final fixture sensitivity" `
                -Passed ($controlFinalMetrics.HaloExcessOcclusion -ge 0.005) `
                -Evidence "candidate-6 final halo=$($controlFinalMetrics.HaloExcessOcclusion)"
            Add-Assertion -Assertions $assertions `
                -Name "$case $mode final halo materially lower" `
                -Passed ($finalReduction -ge $MinimumHaloReduction) `
                -Evidence "reduction=$finalReduction; required=$MinimumHaloReduction"
        } else {
            $rawUnaffected = Test-ExactFileMatch `
                -First $currentA.RawPath -Second $control.RawPath
            $finalUnaffected = Test-ExactFileMatch `
                -First $currentA.FinalPath -Second $control.FinalPath
            Add-Assertion -Assertions $assertions `
                -Name 'hidden-shell GTAO-lite raw output unaffected by compensation' `
                -Passed $rawUnaffected `
                -Evidence "$($currentA.RawSha256) versus $($control.RawSha256)"
            Add-Assertion -Assertions $assertions `
                -Name 'hidden-shell GTAO-lite final output unaffected by compensation' `
                -Passed $finalUnaffected `
                -Evidence "$($currentA.FinalSha256) versus $($control.FinalSha256)"
            Add-Assertion -Assertions $assertions `
                -Name 'hidden-shell GTAO-lite raw wall has no positive halo excess' `
                -Passed ($currentRawMetrics.HaloExcessOcclusion -le 0.001) `
                -Evidence "raw halo=$($currentRawMetrics.HaloExcessOcclusion)"
            Add-Assertion -Assertions $assertions `
                -Name 'hidden-shell GTAO-lite final wall has no positive halo excess' `
                -Passed ($currentFinalMetrics.HaloExcessOcclusion -le 0.001) `
                -Evidence "final halo=$($currentFinalMetrics.HaloExcessOcclusion)"
        }

        $modeResults[$mode] = [ordered]@{
            CurrentRaw = $currentRawMetrics
            ControlRaw = $controlRawMetrics
            RawHaloReductionFraction = $rawReduction
            CurrentFinal = $currentFinalMetrics
            ControlFinal = $controlFinalMetrics
            FinalHaloReductionFraction = $finalReduction
            CurrentRawSha256 = $currentA.RawSha256
            CurrentFinalSha256 = $currentA.FinalSha256
            ControlRawSha256 = $control.RawSha256
            ControlFinalSha256 = $control.FinalSha256
            CurrentMainBytecodeSha256 = $currentA.MainBytecodeSha256
            ControlMainBytecodeSha256 = $control.MainBytecodeSha256
        }
    }
    $caseResults[$case] = $modeResults
}

$detachedSdao = $runs['detached-front-current-sdao-a']
$detachedGtao = $runs['detached-front-current-gtao-lite-a']
$rawModesMatch = Test-ExactFileMatch `
    -First $detachedSdao.RawPath -Second $detachedGtao.RawPath
$finalModesMatch = Test-ExactFileMatch `
    -First $detachedSdao.FinalPath -Second $detachedGtao.FinalPath
$modeBytecodesDiffer = $detachedSdao.MainBytecodeSha256 -ne
    $detachedGtao.MainBytecodeSha256
Add-Assertion -Assertions $assertions -Name 'SDAO and GTAO-lite compile distinct main variants' `
    -Passed $modeBytecodesDiffer `
    -Evidence "$($detachedSdao.MainBytecodeSha256) versus $($detachedGtao.MainBytecodeSha256)"
Add-Assertion -Assertions $assertions -Name 'far stochastic layers preserve raw GTAO/SDAO equivalence' `
    -Passed $rawModesMatch `
    -Evidence "$($detachedSdao.RawSha256) versus $($detachedGtao.RawSha256)"
Add-Assertion -Assertions $assertions -Name 'far stochastic layers preserve final GTAO/SDAO equivalence' `
    -Passed $finalModesMatch `
    -Evidence "$($detachedSdao.FinalSha256) versus $($detachedGtao.FinalSha256)"

$regularFrontSeparation = 4.4 - 3.0
$hiddenShellSeparation = 4.4 - 4.2
$hiddenDepthGapEligible = $regularFrontSeparation -gt 0.7285 -and
    $hiddenShellSeparation -lt 0.7285
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell depth gaps are eligible for stochastic fallback' `
    -Passed $hiddenDepthGapEligible `
    -Evidence "regular dz=$regularFrontSeparation; hidden dz=$hiddenShellSeparation; radius=0.7285; execution proven separately"

$hiddenCurrentSdao = $runs['hidden-shell-current-sdao-a']
$hiddenCurrentGtao = $runs['hidden-shell-current-gtao-lite-a']
$hiddenControlSdao = $runs['hidden-shell-control-sdao']
$hiddenControlGtao = $runs['hidden-shell-control-gtao-lite']
$hiddenCurrentRawContributes = -not (Test-ExactFileMatch `
    -First $hiddenCurrentSdao.RawPath -Second $hiddenCurrentGtao.RawPath)
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell current SDAO fallback executes in raw AO' `
    -Passed $hiddenCurrentRawContributes `
    -Evidence "$($hiddenCurrentSdao.RawSha256) versus $($hiddenCurrentGtao.RawSha256)"
$hiddenCurrentFinalMatches = Test-ExactFileMatch `
    -First $hiddenCurrentSdao.FinalPath -Second $hiddenCurrentGtao.FinalPath
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell current filtered output matches GTAO-lite' `
    -Passed $hiddenCurrentFinalMatches `
    -Evidence "$($hiddenCurrentSdao.FinalSha256) versus $($hiddenCurrentGtao.FinalSha256)"
$currentReceiverVisibilityDelta =
    $caseResults['hidden-shell'].'gtao-lite'.CurrentRaw.ReceiverVisibilityMean -
    $caseResults['hidden-shell'].sdao.CurrentRaw.ReceiverVisibilityMean
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell current receiver darkening is negligible' `
    -Passed ([Math]::Abs($currentReceiverVisibilityDelta) -le 0.0001) `
    -Evidence "GTAO minus SDAO receiver visibility=$currentReceiverVisibilityDelta; maximum absolute delta=0.0001"

$hiddenControlRawContributes = -not (Test-ExactFileMatch `
    -First $hiddenControlSdao.RawPath -Second $hiddenControlGtao.RawPath)
$hiddenControlFinalContributes = -not (Test-ExactFileMatch `
    -First $hiddenControlSdao.FinalPath -Second $hiddenControlGtao.FinalPath)
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell candidate-6 SDAO fallback contributes to raw AO' `
    -Passed $hiddenControlRawContributes `
    -Evidence "$($hiddenControlSdao.RawSha256) versus $($hiddenControlGtao.RawSha256)"
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell candidate-6 SDAO fallback contributes to final AO' `
    -Passed $hiddenControlFinalContributes `
    -Evidence "$($hiddenControlSdao.FinalSha256) versus $($hiddenControlGtao.FinalSha256)"
$controlReceiverVisibilityDelta =
    $caseResults['hidden-shell'].'gtao-lite'.ControlRaw.ReceiverVisibilityMean -
    $caseResults['hidden-shell'].sdao.ControlRaw.ReceiverVisibilityMean
Add-Assertion -Assertions $assertions `
    -Name 'hidden-shell candidate-6 SDAO fallback darkens wall receivers' `
    -Passed ($controlReceiverVisibilityDelta -ge 0.001) `
    -Evidence "GTAO minus SDAO receiver visibility=$controlReceiverVisibilityDelta"

$detachedResults = $caseResults['detached-front']
$hiddenResults = $caseResults['hidden-shell']
$failedAssertions = @($assertions | Where-Object { -not $_.Passed })
$record = [ordered]@{
    SchemaVersion = 2
    FixtureId = 'SPatch-AO-detached-and-hidden-shell-WARP-64x64-v2'
    Passed = $failedAssertions.Count -eq 0
    Width = 64
    Height = 64
    WallViewDepth = 4.4
    DetachedForegroundViewDepth = 4.2
    HiddenRegularFrontViewDepth = 3.0
    HiddenStochasticShellViewDepth = 4.2
    ConfiguredRadius = 0.5
    RadiusMultiplier = 1.457
    EffectiveRadius = 0.7285
    ForegroundBounds = [ordered]@{ Left = 24; RightExclusive = 32; Top = 12; BottomExclusive = 52 }
    StochasticContract = [ordered]@{
        DetachedFront = 'Two logical layers packed into R32G32_UINT and fixed at far depth'
        HiddenShell = 'Layer 0 contains a 4.2 rear shell behind a 3.0 regular front; layer 1 is far'
        GtaoLite = 'SD_GTAO_LITE=1 with null t2 binding'
    }
    Driver = 'D3D11 WARP feature level 11_0'
    CompilerContract = 'D3DCompileFromFile cs_5_0 strict O3 warnings-as-errors'
    ShaderSource = $ShaderPath
    ShaderSourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ShaderPath).Hash
    ControlShaderSource = $ControlShaderPath
    ControlShaderSourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ControlShaderPath).Hash
    MinimumHaloReduction = $MinimumHaloReduction
    Cases = $caseResults
    Assertions = $assertions
    Build = ($buildOutput -join [Environment]::NewLine).Trim()
    RunRoot = $runRoot
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $runRoot 'result.json'
}
$OutputPath = Get-FullPath -Path $OutputPath
$outputParent = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $outputParent)) {
    New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
}
$record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $OutputPath -Encoding utf8

[ordered]@{
    Passed = $record.Passed
    FixtureId = $record.FixtureId
    CurrentShaderSha256 = $record.ShaderSourceSha256
    ControlShaderSha256 = $record.ControlShaderSourceSha256
    DetachedSdaoRawHaloReduction = $detachedResults.sdao.RawHaloReductionFraction
    DetachedSdaoFinalHaloReduction = $detachedResults.sdao.FinalHaloReductionFraction
    DetachedGtaoLiteRawHaloReduction = $detachedResults.'gtao-lite'.RawHaloReductionFraction
    DetachedGtaoLiteFinalHaloReduction = $detachedResults.'gtao-lite'.FinalHaloReductionFraction
    HiddenSdaoRawHaloReduction = $hiddenResults.sdao.RawHaloReductionFraction
    HiddenSdaoFinalHaloReduction = $hiddenResults.sdao.FinalHaloReductionFraction
    HiddenGtaoLiteRawUnchanged = $hiddenResults.'gtao-lite'.CurrentRawSha256 -eq
        $hiddenResults.'gtao-lite'.ControlRawSha256
    HiddenGtaoLiteFinalUnchanged = $hiddenResults.'gtao-lite'.CurrentFinalSha256 -eq
        $hiddenResults.'gtao-lite'.ControlFinalSha256
    AssertionsPassed = $assertions.Count - $failedAssertions.Count
    AssertionsTotal = $assertions.Count
    ResultPath = $OutputPath
} | ConvertTo-Json -Depth 5

if ($failedAssertions.Count -ne 0) {
    $failedNames = $failedAssertions.Name -join ', '
    throw "Detached-front or hidden-shell AO regression failed: $failedNames. Details: $OutputPath"
}
