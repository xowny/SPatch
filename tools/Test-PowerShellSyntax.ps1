[CmdletBinding()]
param(
    [switch] $WindowsPowerShellChild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$roots = @(
    (Join-Path $repoRoot 'tools'),
    (Join-Path $repoRoot 'luma')
)
$files = @($roots | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -File -Recurse -Force |
            Where-Object { $_.Extension -in '.ps1', '.psm1', '.psd1' }
    } | Sort-Object FullName -Unique)
if ($files.Count -eq 0) {
    throw 'No PowerShell sources were found.'
}

$failures = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $tokens = $null
    $errors = $null
    [void] [Management.Automation.Language.Parser]::ParseFile(
        $file.FullName, [ref] $tokens, [ref] $errors)
    foreach ($parseError in @($errors)) {
        $failures.Add(('{0}:{1}:{2}: {3}' -f
                $file.FullName,
                $parseError.Extent.StartLineNumber,
                $parseError.Extent.StartColumnNumber,
                $parseError.Message))
    }
}
if ($failures.Count -ne 0) {
    throw ("PowerShell parser failures:`n" +
        ($failures -join [Environment]::NewLine))
}

$edition = [string] $PSVersionTable.PSEdition
$version = [string] $PSVersionTable.PSVersion
Write-Output ("POWERSHELL_PARSE=PASS edition=$edition version=$version files=$($files.Count)")

if (-not $WindowsPowerShellChild -and $edition -ceq 'Core') {
    $windowsPowerShell =
        'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
        throw "Windows PowerShell is missing: $windowsPowerShell"
    }
    $childOutput = @(& $windowsPowerShell -NoLogo -NoProfile `
        -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath `
        -WindowsPowerShellChild 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw ("Windows PowerShell parser pass failed:`n" +
            ($childOutput -join [Environment]::NewLine))
    }
    Write-Output ($childOutput -join [Environment]::NewLine)
}
