[CmdletBinding()]
param(
    [string] $MinHookRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$sourcePath = Join-Path $repoRoot 'luma\standalone\SPatchShadowScale.cpp'
$resolverPath = Join-Path $repoRoot 'tools\Resolve-MinHook.ps1'
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$source = [IO.File]::ReadAllText($sourcePath, $utf8)

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

# This fork auto-enables inside MH_CreateHook. Keep its exact binary identity
# coupled to the source-level publication contract below.
$resolverArguments = @{
    RepoRoot = $repoRoot
    Offline = $true
}
if (-not [string]::IsNullOrWhiteSpace($MinHookRoot)) {
    $resolverArguments.MinHookRoot = $MinHookRoot
}
$minHook = & $resolverPath @resolverArguments
Assert-True ($minHook.MinHookArtifactCommit -ceq
    'd5d8e1d67ddcea89fbef656b85052a5845dd34ee') `
    'The pinned MinHook artifact commit drifted.'
Assert-True ($minHook.MinHookHeaderSha256 -ceq
    'F2642BB69230017E52F8FE2F1208F6FDEA146302CC670E2003D2A69B5AE860E8') `
    'The pinned MinHook header identity drifted.'
Assert-True ($minHook.MinHookLibrarySha256 -ceq
    'DCF47C6ACDA033310E7C0FA3F7EE6E6C7F89AEA9F8C043D714A39FA01A5FECE2') `
    'The auto-enabling MinHook library identity drifted.'

$createHookCalls = [regex]::Matches(
    $source,
    '(?s)\bMH_CreateHook\s*\(\s*target\s*,\s*detour\s*,\s*original\s*\)')
$allCreateHookCalls = [regex]::Matches($source, '(?s)\bMH_CreateHook\s*\(')
Assert-True ($allCreateHookCalls.Count -eq 2) `
    "Expected exactly two shadow MH_CreateHook call sites; found $($allCreateHookCalls.Count)."
Assert-True ($createHookCalls.Count -eq $allCreateHookCalls.Count) `
    'Every shadow MH_CreateHook must publish ppOriginal directly into the detour-visible slot.'

Assert-True ($source -match
    '(?s)void\*\s+OriginalFor\s*\([^)]*void\*\s+ContextHookSet::\*\s*member[^)]*\)\s*noexcept\s*\{.*?preferred\.\*member.*?alternate\.\*member') `
    'Context detours lack preferred-or-alternate trampoline forwarding.'
Assert-True ($source -notmatch '\bHooksFor\s*\(\s*context\s*\)') `
    'A context detour still routes through a whole hook set and can observe a null shared slot.'
$contextDetourStart = $source.IndexOf(
    'void STDMETHODCALLTYPE DetourPsSetShader(',
    [StringComparison]::Ordinal)
$contextDetourEnd = $source.IndexOf(
    'bool PinNativeHookModule() noexcept',
    [StringComparison]::Ordinal)
Assert-True ($contextDetourStart -ge 0 -and
    $contextDetourEnd -gt $contextDetourStart) `
    'Could not isolate the context-detour forwarding region.'
$contextDetours = $source.Substring(
    $contextDetourStart, $contextDetourEnd - $contextDetourStart)
$contextForwardingCalls = [regex]::Matches(
    $contextDetours, '\bOriginalFor\s*\(')
Assert-True ($contextForwardingCalls.Count -eq 18) `
    "Expected 18 context-detour trampoline selections; found $($contextForwardingCalls.Count)."
Assert-True ($contextDetours -notmatch
    '\bg_(?:immediate|deferred)_hooks\s*\.') `
    'A context detour bypasses preferred-or-alternate trampoline selection.'

Assert-True ($source -match
    '(?s)ContextState&\s+StateFor\s*\([^)]*\)\s*noexcept\s*\{.*?catch\s*\(\.\.\.\).*?OverflowState') `
    'StateFor must fall back to the overflow state when context-map allocation fails.'
Assert-True ($source -match
    '(?s)if\s*\(\s*!TryTrackScaledTexture\s*\(.*?RetryNativeTextureCreation') `
    'A scaled texture registry failure must release the scaled texture and retry the native descriptor.'
Assert-True ($source -match
    'spatch::graphics::detail::CreatePrivateData<DeviceData>\s*\(\s*device\s*\)') `
    'Shadow device data must use leak-safe private-data publication.'
Assert-True ($source -notmatch '\bcreate_private_data<DeviceData>\s*\(') `
    'Shadow device data still uses raw private-data creation.'

[pscustomobject]@{
    Status = 'pass'
    MinHookArtifactCommit = $minHook.MinHookArtifactCommit
    DirectOriginalPublications = $createHookCalls.Count
    AlternateContextForwarding = $contextForwardingCalls.Count
    AllocationFailOpenContracts = $true
}
