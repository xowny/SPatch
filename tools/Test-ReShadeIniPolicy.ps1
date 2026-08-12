[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReShadeIniPolicy.ps1')

function Assert-Pass([string] $Name, [string] $Text, [switch] $Packaged) {
    if ($Packaged) {
        [void](Assert-ReShadeRootAddonPolicy $Text $Name `
            -RequireAddonSection -RequireAddonPath)
    } else {
        [void](Assert-ReShadeRootAddonPolicy $Text $Name)
    }
}

function Assert-Fail(
    [string] $Name,
    [string] $Text,
    [string] $Expected,
    [switch] $Packaged) {
    $message = ''
    try {
        Assert-Pass $Name $Text -Packaged:$Packaged
    } catch {
        $message = $_.Exception.Message
    }
    if ($message.IndexOf($Expected, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Name failed incorrectly: $message"
    }
}

Assert-Pass 'no-addon-section' "[GENERAL]`r`nPerformanceMode=1`r`n"
Assert-Pass 'literal-root' "[ADDON]`r`nAddonPath=.\`r`n"
Assert-Pass 'trimmed-root' " [ ADDON ] trailing `r`n AddonPath = ./ `r`n"
Assert-Pass 'comment-prefixes' (
    ";[ADDON]`n;AddonPath=mods`n" +
    "/[ADDON]`n/AddonPath=mods`n" +
    "#[ADDON]`n#AddonPath=mods`n")
Assert-Pass 'case-sensitive-names' (
    "[addon]`nAddonPath=mods`n[ADDON]`naddonpath=mods`nloadfromdllmain=x.addon`n")
Assert-Pass 'other-section-keys' (
    "[OTHER]`nAddonPath=mods`nLoadFromDllMain=x.addon`n")
Assert-Pass 'packaged-root' "[[ADDON]]`nAddonPath=.`n" -Packaged

foreach ($header in @('[ADDON]', '[ ADDON ]', '[[ADDON]]', '[ADDON] trailing')) {
    Assert-Fail "redirect-$header" "$header`nAddonPath=mods`n" 'redirects'
}
Assert-Fail 'equivalent-duplicate-sections' (
    "[ADDON]`nAddonPath=.`n[ ADDON ]`nCustom=1`n") 'at most one'
Assert-Fail 'duplicate-paths' (
    "[ADDON]`nAddonPath=.`nAddonPath=mods`n") 'duplicate'
Assert-Fail 'effective-load-from-dllmain' (
    "[ADDON]`nLoadFromDllMain=x.addon`n") 'LoadFromDllMain'
Assert-Fail 'packaged-missing-section' "[GENERAL]`nX=1`n" 'exactly one' -Packaged
Assert-Fail 'packaged-missing-path' "[ADDON]`nX=1`n" 'exactly one effective AddonPath' -Packaged

[pscustomobject]@{ Status = 'pass'; Cases = 16 }
