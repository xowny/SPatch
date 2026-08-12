[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$buildScript = Join-Path $repoRoot 'luma\Build-Luma.ps1'
if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
    throw "Build-Luma script is missing: $buildScript"
}

$tokens = $null
$parseErrors = $null
$buildAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $buildScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Build-Luma has parser errors: $($parseErrors -join [Environment]::NewLine)"
}
$buildText = [IO.File]::ReadAllText($buildScript)

$cleanupContract = 'Remove-CheckedDirectory -Path $graphicsOutputDirectory'
$buildContract = '& $msbuild $entry.Project'
$cleanupIndex = $buildText.IndexOf($cleanupContract, [StringComparison]::Ordinal)
$buildIndex = $buildText.IndexOf($buildContract, [StringComparison]::Ordinal)
if ($cleanupIndex -lt 0 -or $buildIndex -lt 0 -or $cleanupIndex -gt $buildIndex) {
    throw 'Build-Luma must remove the checked configuration output directory before invoking MSBuild.'
}
if ($buildText.IndexOf('$graphicsObjDirectory', [StringComparison]::Ordinal) -ge 0) {
    throw 'Build-Luma still limits pre-build cleanup to the intermediate object directory.'
}
foreach ($contract in @(
        'Assert-ExactLicenseFileSet -Path $licenseRoot',
        'Assert-ExactLicenseFileSet -Path $artifactLicenseRoot',
        'Copy-Item -LiteralPath $sourceLicense -Destination $packagedLicense -Force')) {
    if ($buildText.IndexOf($contract, [StringComparison]::Ordinal) -lt 0) {
        throw "Build-Luma is missing the exact license packaging contract: $contract"
    }
}
if ($buildText.IndexOf(
        "Copy-Item -LiteralPath (Join-Path `$scriptRoot 'licenses')",
        [StringComparison]::Ordinal) -ge 0) {
    throw 'Build-Luma still recursively publishes the source license directory.'
}

$requiredFunctionNames = @(
    'Assert-ExactLicenseFileSet',
    'Get-CheckedChildPath',
    'Assert-NoReparseTree',
    'Remove-CheckedDirectory')
$functionDefinitions = foreach ($functionName in $requiredFunctionNames) {
    $definition = $buildAst.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $functionName
        }, $true)
    if ($null -eq $definition) {
        throw "Build-Luma helper is missing: $functionName"
    }
    $definition.Extent.Text
}
Invoke-Expression ($functionDefinitions -join [Environment]::NewLine)

function Assert-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$ExpectedText,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $failure = $null
    try {
        & $Action
    } catch {
        $failure = $_
    }
    if ($null -eq $failure) {
        throw "$Description was accepted unexpectedly."
    }
    if ($failure.Exception.Message.IndexOf(
            $ExpectedText, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Description failed incorrectly: $($failure.Exception.Message)"
    }
}

$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
    [char[]]'\/')
$temporaryPrefix = $temporaryParent + [IO.Path]::DirectorySeparatorChar
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $temporaryParent (
            'shenlong-build-hygiene-' + [guid]::NewGuid().ToString('N'))))
if (-not $fixtureRoot.StartsWith(
        $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Fixture root escaped the temporary directory: $fixtureRoot"
}
[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null

try {
    $licenseRoot = Join-Path $fixtureRoot 'licenses'
    [IO.Directory]::CreateDirectory($licenseRoot) | Out-Null
    $expectedLicenses = @(
        'DiligentFX-Apache-2.0.txt',
        'MinHook-BSD-2-Clause.txt',
        'ThreeJS-MIT.txt',
        'XeGTAO-MIT.txt')
    foreach ($licenseName in $expectedLicenses) {
        [IO.File]::WriteAllText((Join-Path $licenseRoot $licenseName), 'fixture')
    }
    Assert-ExactLicenseFileSet -Path $licenseRoot `
        -ExpectedNames $expectedLicenses -Description 'Fixture license directory'

    $backupLicense = Join-Path $licenseRoot 'ThreeJS-MIT.txt.bak'
    [IO.File]::WriteAllText($backupLicense, 'unreviewed backup')
    Assert-ExpectedFailure -Description 'Extra license backup' `
        -ExpectedText 'only the exact whitelisted license files' -Action {
            Assert-ExactLicenseFileSet -Path $licenseRoot `
                -ExpectedNames $expectedLicenses `
                -Description 'Fixture license directory'
        }
    Remove-Item -LiteralPath $backupLicense -Force

    $workingLicenseDirectory = Join-Path $licenseRoot 'work'
    [IO.Directory]::CreateDirectory($workingLicenseDirectory) | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $workingLicenseDirectory 'draft.txt'), 'unreviewed draft')
    Assert-ExpectedFailure -Description 'Extra license working directory' `
        -ExpectedText 'only the exact whitelisted license files' -Action {
            Assert-ExactLicenseFileSet -Path $licenseRoot `
                -ExpectedNames $expectedLicenses `
                -Description 'Fixture license directory'
        }
    Remove-Item -LiteralPath $workingLicenseDirectory -Recurse -Force

    $missingLicense = Join-Path $licenseRoot $expectedLicenses[0]
    Remove-Item -LiteralPath $missingLicense -Force
    Assert-ExpectedFailure -Description 'Missing whitelisted license' `
        -ExpectedText 'only the exact whitelisted license files' -Action {
            Assert-ExactLicenseFileSet -Path $licenseRoot `
                -ExpectedNames $expectedLicenses `
                -Description 'Fixture license directory'
        }

    $graphicsBuildParent = Join-Path $fixtureRoot 'build\graphics-addon'
    $graphicsOutput = Join-Path $graphicsBuildParent 'x64-Publishing-Release'
    [IO.Directory]::CreateDirectory((Join-Path $graphicsOutput 'obj')) | Out-Null
    foreach ($productName in @(
            'ShenLong.asi',
            'SPatchGraphics.addon',
            'SPatchGraphics.exp',
            'SPatchGraphics.lib',
            'SPatchGraphics.pdb')) {
        [IO.File]::WriteAllText((Join-Path $graphicsOutput $productName), 'stale')
    }
    Remove-CheckedDirectory -Path $graphicsOutput -Parent $graphicsBuildParent `
        -Description 'fixture ShenLong configuration output directory'
    if (Test-Path -LiteralPath $graphicsOutput) {
        throw 'Checked configuration cleanup left stale graphics products behind.'
    }

    $outsideDirectory = Join-Path $fixtureRoot 'outside-build-parent'
    [IO.Directory]::CreateDirectory($outsideDirectory) | Out-Null
    $outsideSentinel = Join-Path $outsideDirectory 'sentinel.txt'
    [IO.File]::WriteAllText($outsideSentinel, 'preserve')
    Assert-ExpectedFailure -Description 'Outside-parent graphics cleanup' `
        -ExpectedText 'outside its checked parent' -Action {
            Remove-CheckedDirectory -Path $outsideDirectory `
                -Parent $graphicsBuildParent `
                -Description 'fixture ShenLong configuration output directory'
        }
    if (-not (Test-Path -LiteralPath $outsideSentinel -PathType Leaf)) {
        throw 'Rejected outside-parent cleanup changed its target.'
    }
} finally {
    if ($fixtureRoot.StartsWith(
            $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $fixtureRoot -PathType Container)) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

[pscustomobject]@{
    Status = 'pass'
    ConfigurationOutputCleanup = $true
    OutsideParentCleanupRejected = $true
    ExactLicenseWhitelist = $true
    ExtraLicenseInputsRejected = $true
}
