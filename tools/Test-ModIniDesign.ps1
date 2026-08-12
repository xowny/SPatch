[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Path,
    [Parameter(Mandatory = $true)]
    [string] $Primary,
    [string[]] $AllowedSection = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Path = [IO.Path]::GetFullPath($Path)
if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "INI file is missing: $Path"
}
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
try {
    $text = [IO.File]::ReadAllText($Path, $strictUtf8)
} catch {
    throw "INI is not valid UTF-8: $Path ($($_.Exception.Message))"
}
if ($text.Length -gt 0 -and $text[0] -eq [char] 0xFEFF) {
    $text = $text.Substring(1)
}

$publicNamePattern = '^[A-Z][A-Za-z0-9]*$'
$internalPrefixPattern = '^(?:Address|Debug|Detour|Developer|Hook|Internal|Offset|Probe|Rva)'
$lines = @($text -split '\r?\n')
$issues = [Collections.Generic.List[string]]::new()
$sections = [Collections.Generic.List[object]]::new()
$seenSections = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$keysBySection = [Collections.Generic.Dictionary[
    string, Collections.Generic.HashSet[string]]]::new(
        [StringComparer]::OrdinalIgnoreCase)
$orderedKeys = [Collections.Generic.Dictionary[
    string, Collections.Generic.List[string]]]::new(
        [StringComparer]::OrdinalIgnoreCase)
$currentSection = $null
$sawHeaderComment = $false

function Test-IsIniComment([string] $Line) {
    $trimmed = $Line.TrimStart()
    return $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')
}

for ($index = 0; $index -lt $lines.Count; ++$index) {
    $lineNumber = $index + 1
    $rawLine = $lines[$index]
    $stripped = $rawLine.Trim()
    if ([string]::IsNullOrWhiteSpace($stripped)) {
        continue
    }
    if (Test-IsIniComment $rawLine) {
        if ($sections.Count -eq 0) {
            $sawHeaderComment = $true
        }
        continue
    }

    if ($stripped.StartsWith('[') -and $stripped.EndsWith(']')) {
        $section = $stripped.Substring(1, $stripped.Length - 2).Trim()
        if ($section -cnotmatch $publicNamePattern) {
            $issues.Add("${Path}:${lineNumber}: section '$section' must be PascalCase")
        }
        if (-not $seenSections.Add($section)) {
            $issues.Add("${Path}:${lineNumber}: duplicate section '$section'")
        }
        $sections.Add([pscustomobject]@{
                Name = $section
                Line = $lineNumber
            })
        if (-not $keysBySection.ContainsKey($section)) {
            $keysBySection.Add(
                $section,
                [Collections.Generic.HashSet[string]]::new(
                    [StringComparer]::OrdinalIgnoreCase))
            $orderedKeys.Add(
                $section,
                [Collections.Generic.List[string]]::new())
        }
        $currentSection = $section
        continue
    }

    $equalsIndex = $rawLine.IndexOf('=')
    if ($equalsIndex -lt 0) {
        $issues.Add("${Path}:${lineNumber}: expected a section, comment, or Key=Value setting")
        continue
    }
    $key = $rawLine.Substring(0, $equalsIndex).Trim()
    $value = $rawLine.Substring($equalsIndex + 1).Trim()
    if ($null -eq $currentSection) {
        $issues.Add("${Path}:${lineNumber}: setting '$key' appears before the first section")
        continue
    }
    if ($rawLine -cne "$key=$value") {
        $issues.Add("${Path}:${lineNumber}: use compact Key=Value formatting for '$key'")
    }
    if ($key -cnotmatch $publicNamePattern) {
        $issues.Add("${Path}:${lineNumber}: key '$key' must be PascalCase with no spaces, underscores, or dashes")
    }
    if ($key -cmatch $internalPrefixPattern) {
        $issues.Add("${Path}:${lineNumber}: key '$key' exposes an internal/developer concept")
    }
    if ([string]::IsNullOrEmpty($value)) {
        $issues.Add("${Path}:${lineNumber}: key '$key' has an empty value")
    }
    if ($value.Contains(';') -or $value.Contains('#')) {
        $issues.Add("${Path}:${lineNumber}: put the comment for '$key' on the preceding line")
    }
    if ($index -eq 0 -or -not (Test-IsIniComment $lines[$index - 1])) {
        $issues.Add("${Path}:${lineNumber}: key '$key' needs a comment immediately above it")
    }
    if (-not $keysBySection[$currentSection].Add($key)) {
        $issues.Add("${Path}:${lineNumber}: duplicate key '$key' in [$currentSection]")
    }
    $orderedKeys[$currentSection].Add($key)
}

if (-not $sawHeaderComment) {
    $issues.Add("${Path}:1: add a short mod/game header comment before the first section")
}
if ($sections.Count -eq 0) {
    $issues.Add("${Path}:1: INI has no sections")
} else {
    $actualPrimary = $sections[0]
    if ($actualPrimary.Name -ine $Primary) {
        $issues.Add("${Path}:$($actualPrimary.Line): first section must be [$Primary]")
    }
    $primaryKeys = if ($orderedKeys.ContainsKey($Primary)) {
        $orderedKeys[$Primary]
    } else {
        [Collections.Generic.List[string]]::new()
    }
    if ($primaryKeys.Count -eq 0 -or $primaryKeys[0] -ine 'ConfigVersion') {
        $issues.Add("${Path}:$($actualPrimary.Line): ConfigVersion must be the first key in the primary section")
    }

    $permittedSections = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    [void] $permittedSections.Add($Primary)
    [void] $permittedSections.Add('Debug')
    foreach ($section in $AllowedSection) {
        [void] $permittedSections.Add($section)
    }
    foreach ($section in @($sections | Select-Object -Skip 1)) {
        if (-not $permittedSections.Contains($section.Name)) {
            $issues.Add(
                "${Path}:$($section.Line): extra section [$($section.Name)] needs a public purpose")
        }
    }
}

if ($issues.Count -ne 0) {
    throw ("INI design validation failed with $($issues.Count) issue(s):`n" +
           ($issues -join [Environment]::NewLine))
}
Write-Output "PASS: $Path"
