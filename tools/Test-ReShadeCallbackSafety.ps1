[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$standalone = Join-Path $repoRoot 'luma\standalone'
$utf8 = [Text.UTF8Encoding]::new($false, $true)

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

$componentNames = @(
    'SPatchSDAO.cpp',
    'SPatchGI.cpp',
    'SPatchSSS.cpp',
    'SPatchPBR.cpp',
    'SPatchTonemapping.cpp',
    'SPatchWater.cpp'
)

$registrationCount = 0
$guardedCount = 0
$privatePublicationCount = 0
foreach ($name in $componentNames) {
    $path = Join-Path $standalone $name
    $source = [IO.File]::ReadAllText($path, $utf8)
    Assert-True ($source.Contains('#include "SPatchReShadeCallbackSafety.hpp"')) `
        "$name does not include the shared callback boundary."
    Assert-True ($source -notmatch '\bcreate_private_data\s*<') `
        "$name uses ReShade's leak-prone raw private-data allocator."
    Assert-True ($source -notmatch `
        '(?s)\b(?:un)?register_event\s*<[^>]+>\s*\(\s*&?On[A-Za-z0-9_]*\s*\)') `
        "$name registers or unregisters an unguarded On* callback."

    $registrations = [regex]::Matches(
        $source, '(?s)\b(?:un)?register_event\s*<[^>]+>\s*\(')
    $guards = [regex]::Matches(
        $source, '\bGuardedCallback\s*<\s*On[A-Za-z0-9_]+\s*>::Invoke')
    Assert-True ($registrations.Count -eq $guards.Count) `
        "$name has $($registrations.Count) event operations but $($guards.Count) guarded callbacks."
    $registrationCount += $registrations.Count
    $guardedCount += $guards.Count
    $privatePublicationCount += [regex]::Matches(
        $source, '\bspatch::graphics::detail::CreatePrivateData\s*<').Count
}

Assert-True ($registrationCount -gt 0 -and
    $registrationCount -eq $guardedCount) `
    'The shared callback-boundary census is empty or inconsistent.'
Assert-True ($privatePublicationCount -gt 0) `
    'No leak-safe private-data publication sites were found.'

$helperPath = Join-Path $standalone 'SPatchReShadeCallbackSafety.hpp'
$helper = [IO.File]::ReadAllText($helperPath, $utf8)
foreach ($requiredPattern in @(
        'std::make_unique<State>',
        'object->set_private_data(',
        'state.release();',
        'catch (const std::bad_alloc& exception)',
        'catch (const std::exception& exception)',
        'catch (...)')) {
    Assert-True ($helper.Contains($requiredPattern)) `
        "The callback-safety helper lost required contract: $requiredPattern"
}

$shadowPath = Join-Path $standalone 'SPatchShadowScale.cpp'
$shadow = [IO.File]::ReadAllText($shadowPath, $utf8)
Assert-True ($shadow -match
    'spatch::graphics::detail::CreatePrivateData<DeviceData>\s*\(\s*device\s*\)') `
    'ShadowScale does not use leak-safe private-data publication.'
Assert-True ($shadow -notmatch '\bcreate_private_data\s*<') `
    'ShadowScale still uses raw private-data allocation.'

[pscustomobject]@{
    Status = 'pass'
    Components = $componentNames.Count
    GuardedEventOperations = $guardedCount
    SafePrivateDataPublications = $privatePublicationCount + 1
}
