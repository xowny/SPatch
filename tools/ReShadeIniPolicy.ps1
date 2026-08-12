Set-StrictMode -Version Latest

function ConvertFrom-PinnedReShadeIni([string] $Text) {
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) {
        $Text = $Text.Substring(1)
    }

    $section = ''
    $addonSectionCount = 0
    $addonPaths = [Collections.Generic.List[string]]::new()
    $loadFromDllMain = [Collections.Generic.List[string]]::new()
    $trimLine = [char[]]" `t`r`n"
    $trimSection = [char[]]" `t[]"
    $trimToken = [char[]]" `t"

    foreach ($rawLine in @($Text -split '\r?\n')) {
        # ReShade ini_file::load trims a complete line before classifying it.
        $line = $rawLine.Trim($trimLine)
        if ($line.Length -eq 0 -or
            $line[0] -eq [char]';' -or
            $line[0] -eq [char]'/' -or
            $line[0] -eq [char]'#') {
            continue
        }

        if ($line[0] -eq [char]'[') {
            $close = $line.IndexOf([char]']')
            $sectionPrefix = if ($close -ge 0) {
                $line.Substring(0, $close)
            } else {
                $line
            }
            $section = $sectionPrefix.Trim($trimSection)
            if ($section -ceq 'ADDON') {
                ++$addonSectionCount
            }
            continue
        }

        $equals = $line.IndexOf([char]'=')
        if ($equals -ge 0) {
            $key = $line.Substring(0, $equals).Trim($trimToken)
            $value = $line.Substring($equals + 1).Trim($trimToken)
        } else {
            $key = $line
            $value = ''
        }

        # ReShade's section and key maps use the default case-sensitive comparer.
        if ($section -ceq 'ADDON') {
            if ($key -ceq 'AddonPath') {
                $addonPaths.Add($value)
            } elseif ($key -ceq 'LoadFromDllMain') {
                $loadFromDllMain.Add($value)
            }
        }
    }

    return [pscustomobject]@{
        AddonSectionCount = $addonSectionCount
        AddonPaths = @($addonPaths)
        LoadFromDllMain = @($loadFromDllMain)
    }
}

function Assert-ReShadeRootAddonPolicy(
    [string] $Text,
    [string] $Label,
    [switch] $RequireAddonSection,
    [switch] $RequireAddonPath) {
    $parsed = ConvertFrom-PinnedReShadeIni $Text

    if (($RequireAddonSection -and $parsed.AddonSectionCount -ne 1) -or
        (-not $RequireAddonSection -and $parsed.AddonSectionCount -gt 1)) {
        $requirement = if ($RequireAddonSection) {
            'exactly one'
        } else {
            'at most one'
        }
        throw ("$Label must contain $requirement effective [ADDON] section; " +
            "found $($parsed.AddonSectionCount).")
    }

    if ($parsed.AddonPaths.Count -gt 1) {
        throw "$Label contains duplicate effective AddonPath entries."
    }
    if ($RequireAddonPath -and $parsed.AddonPaths.Count -ne 1) {
        throw "$Label must contain exactly one effective AddonPath entry."
    }
    if ($parsed.AddonPaths.Count -eq 1) {
        $addonPath = $parsed.AddonPaths[0]
        $allowed = @('.', '.\', './')
        if (-not ($allowed -ccontains $addonPath)) {
            throw "$Label redirects add-ons away from the game root."
        }
    }
    if ($parsed.LoadFromDllMain.Count -ne 0) {
        throw "$Label may not contain effective LoadFromDllMain entries."
    }

    return $parsed
}
