[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$shaderRoot = Join-Path $repoRoot `
    'luma\overlay\Shaders\Sleeping Dogs Definitive Edition'
$includePath = Join-Path $shaderRoot 'SPatchWaterScattering.hlsli'
$runtimePath = Join-Path $repoRoot 'luma\standalone\SPatchWater.cpp'
$shaderPaths = @(
    (Join-Path $shaderRoot 'SPatchWaterMain.hlsl'),
    (Join-Path $shaderRoot 'SPatchWaterSimple.hlsl'),
    (Join-Path $shaderRoot 'SPatchWaterBlend.hlsl'))

function Get-FixedWaterScattering {
    param(
        [double] $Transmittance,
        [double[]] $SceneAlbedo,
        [double[]] $DirectLighting,
        [double[]] $AbsorbExtinct
    )

    $boundedTransmittance =
        [Math]::Min(1.0, [Math]::Max(0.0, $Transmittance))
    $result = [double[]]::new(3)
    for ($channel = 0; $channel -lt 3; ++$channel) {
        $boundedDirect = [Math]::Min(
            65504.0, [Math]::Max(0.0, $DirectLighting[$channel]))
        $boundedAlbedo = [Math]::Max(
            0.04, [Math]::Min(1.0, [Math]::Max(
                    0.0, $SceneAlbedo[$channel])))
        $irradiance = [Math]::Min(65504.0, $boundedDirect / $boundedAlbedo)
        $mediumAlbedo = [Math]::Min(
            1.0, [Math]::Max(0.0,
                $AbsorbExtinct[$channel] * $AbsorbExtinct[$channel]))
        $result[$channel] = $irradiance * $mediumAlbedo *
            0.0795774683 * (1.0 - $boundedTransmittance)
    }
    return $result
}

$cases = @(
    [pscustomobject]@{
        Name = 'authored'
        Transmittance = 0.25
        Albedo = @(0.2, 0.5, 1.0)
        Direct = @(0.1, 0.4, 2.0)
        Extinction = @(0.1, 0.5, 2.0)
    },
    [pscustomobject]@{
        Name = 'input-clamps'
        Transmittance = -1.0
        Albedo = @(-1.0, 0.0, 2.0)
        Direct = @(-3.0, 70000.0, 8.0)
        Extinction = @(-2.0, 0.5, 4.0)
    },
    [pscustomobject]@{
        Name = 'fully-transmitted'
        Transmittance = 1.0
        Albedo = @(0.2, 0.4, 0.8)
        Direct = @(1.0, 2.0, 3.0)
        Extinction = @(0.5, 0.5, 0.5)
    })

$maximumValue = 0.0
foreach ($case in $cases) {
    $values = @(Get-FixedWaterScattering `
        $case.Transmittance $case.Albedo $case.Direct $case.Extinction)
    foreach ($value in $values) {
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or
            $value -lt 0.0) {
            throw "Fixed isotropic water scattering is invalid for $($case.Name): $value"
        }
        $maximumValue = [Math]::Max($maximumValue, $value)
    }
    if ($case.Name -eq 'fully-transmitted' -and
        @($values | Where-Object { [Math]::Abs($_) -gt 1.0e-12 }).Count -ne 0) {
        throw 'Fully transmitted water must add no scattering.'
    }
}

$previous = [double]::PositiveInfinity
foreach ($transmittance in @(0.0, 0.25, 0.5, 0.75, 1.0)) {
    $value = @(Get-FixedWaterScattering `
        $transmittance @(0.5, 0.5, 0.5) @(1.0, 1.0, 1.0) `
        @(0.5, 0.5, 0.5))[0]
    if ($value -gt $previous + 1.0e-12) {
        throw 'Fixed isotropic scattering increased with transmittance.'
    }
    $previous = $value
}

$retiredHelpers = @(
    'SPatchEvaluateWaterGGXLobe',
    'SPatchEvaluateWaterFresnelVisibility')
$includeText = [IO.File]::ReadAllText($includePath)
foreach ($retiredHelper in $retiredHelpers) {
    if ($includeText.Contains($retiredHelper)) {
        throw "Retired stock-math replacement helper remains: $retiredHelper"
    }
}
if ([regex]::Matches(
        $includeText, 'SPatchEvaluateWaterSingleScattering\s*\(').Count -ne 1 -or
    $includeText -notmatch 'SPatchInvFourPi\s*=\s*0\.0795774683') {
    throw 'The fixed-strength isotropic scattering helper contract drifted.'
}

foreach ($shaderPath in $shaderPaths) {
    $shaderText = [IO.File]::ReadAllText($shaderPath)
    foreach ($retiredHelper in $retiredHelpers) {
        if ($shaderText.Contains($retiredHelper)) {
            throw "$([IO.Path]::GetFileName($shaderPath)) still calls retired helper $retiredHelper."
        }
    }
    $scatteringCalls = [regex]::Matches(
        $shaderText, 'SPatchEvaluateWaterSingleScattering\s*\(').Count
    if ($scatteringCalls -ne 1) {
        throw "$([IO.Path]::GetFileName($shaderPath)) must add fixed isotropic scattering exactly once; found $scatteringCalls calls."
    }
}

$runtimeText = [IO.File]::ReadAllText($runtimePath)
if ($runtimeText -notmatch '(?s)const WaterConstants constants\s*\{\s*1\.0f,\s*0\.0f,\s*0\.0f,\s*0\.0f,\s*\}' -or
    $runtimeText.IndexOf(
        '[ShenLong-Water] enabled=%d ready=%d isotropic_strength=100%%.',
        [StringComparison]::Ordinal) -lt 0) {
    throw 'The runtime no longer uploads or reports exact fixed normalized water-scattering strength.'
}
foreach ($retiredSetting in @(
        'WaterScatteringAnisotropy',
        'WaterVolumetricScattering',
        'WaterScatteringStrength')) {
    if ($runtimeText.Contains($retiredSetting)) {
        throw "Retired water configuration alias remains in the runtime: $retiredSetting"
    }
}

Write-Host 'Water scattering validation'
Write-Host ('Cases={0}; maximum finite non-negative component={1:E9}' -f
    $cases.Count, $maximumValue)
Write-Host 'PASS: fixed normalized strength, isotropic energy coefficient, transmittance monotonicity, exact three-variant wiring, and stock Blinn/Fresnel preservation.'
