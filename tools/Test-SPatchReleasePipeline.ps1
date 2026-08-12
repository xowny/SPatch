[CmdletBinding()]
param(
    [string] $RepoRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $scriptRoot
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot).TrimEnd([char[]]'\/')
$projectPath = Join-Path $RepoRoot 'SPatch.vcxproj'
$buildScript = Join-Path $RepoRoot 'tools\Build-SPatchRelease.ps1'
$iniDesignValidator = Join-Path $RepoRoot 'tools\Test-ModIniDesign.ps1'
$deployScript = Join-Path $RepoRoot 'tools\Deploy-SPatchArtifact.ps1'
$targetValidator = Join-Path $RepoRoot 'tools\Test-SPatchDeployTarget.ps1'
$tmpRoot = Join-Path $RepoRoot '.tmp'
[IO.Directory]::CreateDirectory($tmpRoot) | Out-Null
$fixtureRoot = Join-Path $tmpRoot `
    ('.release-pipeline-test-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Get-TreeFingerprint([string[]] $Roots) {
    $records = [Collections.Generic.List[string]]::new()
    foreach ($root in $Roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            $records.Add("MISSING|$root")
            continue
        }
        $item = Get-Item -LiteralPath $root -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Fingerprint root is a reparse point: $root"
        }
        if ($item.PSIsContainer) {
            foreach ($child in Get-ChildItem -LiteralPath $root -File -Recurse -Force) {
                $records.Add(('{0}|{1}|{2}' -f $child.FullName, $child.Length,
                    (Get-FileHash -LiteralPath $child.FullName -Algorithm SHA256).Hash))
            }
        } else {
            $records.Add(('{0}|{1}|{2}' -f $item.FullName, $item.Length,
                (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash))
        }
    }
    $array = $records.ToArray()
    [Array]::Sort($array, [StringComparer]::Ordinal)
    return ($array -join "`n")
}

function Find-MSBuild {
    $known = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
        'C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
    )
    foreach ($candidate in $known) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    foreach ($root in @(
            'C:\Program Files (x86)\Microsoft Visual Studio',
            'C:\Program Files\Microsoft Visual Studio')) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        $candidate = Get-ChildItem -LiteralPath $root -Filter MSBuild.exe `
            -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\MSBuild\\Current\\Bin\\MSBuild\.exe$' } |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }
    }
    throw 'MSBuild was not found for direct-target negative tests.'
}

function Invoke-ExpectedFailure(
    [string] $Name,
    [string] $Pattern,
    [scriptblock] $Action) {
    try {
        & $Action
    } catch {
        if ($_.Exception.Message.IndexOf(
                $Pattern, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            throw "$Name failed with the wrong error: $($_.Exception.Message)"
        }
        return
    }
    throw "$Name unexpectedly succeeded."
}

function Write-MinimalX64Pe([string] $Path) {
    $bytes = [byte[]]::new(512)
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($bytes, 0x98)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Remove-FixtureSafely([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($tmpRoot).TrimEnd([char[]]'\/') +
              [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($full) -cnotmatch
            '^\.release-pipeline-test-[0-9a-f]{32}$') {
        throw "Refusing unsafe release-test cleanup: $full"
    }
    $reparseItems = @(Get-ChildItem -LiteralPath $full -Force -Recurse |
        Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        } | Sort-Object { $_.FullName.Length } -Descending)
    foreach ($reparseItem in $reparseItems) {
        Remove-Item -LiteralPath $reparseItem.FullName -Force
    }
    $remainingReparse = @(Get-ChildItem -LiteralPath $full -Force -Recurse |
        Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        })
    if ($remainingReparse.Count -ne 0) {
        throw 'Release-test fixture still contains a reparse point; cleanup stopped.'
    }
    Remove-Item -LiteralPath $full -Recurse -Force
}

$publicationRoots = @(
    (Join-Path $RepoRoot 'artifacts\release\SPatch-Base'),
    (Join-Path $RepoRoot 'artifacts\release\SPatch-Base.zip'),
    (Join-Path $RepoRoot 'build\Release\SPatch.asi'),
    (Join-Path $RepoRoot 'build\Release\SPatch.final-release.sha256'))
$beforeFingerprint = Get-TreeFingerprint $publicationRoots
$results = [Collections.Generic.List[string]]::new()

try {
    foreach ($path in @($buildScript, $deployScript, $targetValidator)) {
        $tokens = $null
        $errors = $null
        [void][Management.Automation.Language.Parser]::ParseFile(
            $path, [ref]$tokens, [ref]$errors)
        Assert-True ($errors.Count -eq 0) `
            "PowerShell AST parse failed for ${path}: $($errors -join '; ')"
    }
    [xml]$project = [IO.File]::ReadAllText($projectPath)
    $namespace = [Xml.XmlNamespaceManager]::new($project.NameTable)
    $namespace.AddNamespace(
        'm', 'http://schemas.microsoft.com/developer/msbuild/2003')
    $results.Add('ast-and-xml')

    foreach ($targetName in @(
            'VerifyBeforeDeploy', 'VerifyFinalRelease',
            'AssembleFinalReleasePackage', 'DeployToGame',
            'WriteFinalReleaseIdentity')) {
        $target = $project.SelectSingleNode(
            "//m:Target[@Name='$targetName']", $namespace)
        Assert-True ($null -ne $target) "Missing direct-target guard: $targetName"
        Assert-True ([string]::IsNullOrWhiteSpace(
                $target.GetAttribute('AfterTargets')) -and
            [string]::IsNullOrWhiteSpace(
                $target.GetAttribute('BeforeTargets')) -and
            [string]::IsNullOrWhiteSpace(
                $target.GetAttribute('DependsOnTargets')) -and
            $target.SelectNodes('m:Error', $namespace).Count -eq 1) `
            "$targetName is not an unconditional direct-entry rejection."
    }
    $finalizer = $project.SelectSingleNode(
        "//m:Target[@Name='_FinalizeCurrentFinalBuild']", $namespace)
    Assert-True ($finalizer.GetAttribute('AfterTargets') -ceq 'Build') `
        'Final publication is not bound to the current Build target.'
    $releaseLtcg = @($project.SelectNodes(
            "//m:ItemDefinitionGroup[contains(@Condition, 'Release|x64')]/m:Link/m:LinkTimeCodeGeneration",
            $namespace))
    Assert-True ($releaseLtcg.Count -eq 1 -and
        $releaseLtcg[0].InnerText -ceq 'UseLinkTimeCodeGeneration' -and
        $releaseLtcg[0].GetAttribute('Condition') -ceq
            "'`$(FinalRelease)'=='true'") `
        'FinalRelease must use explicit full LTCG without slowing diagnostic builds.'
    $configurationWpo = @($project.SelectNodes(
            "//m:PropertyGroup[contains(@Condition, 'Release|x64')]/m:WholeProgramOptimization",
            $namespace))
    $releaseWpo = @($project.SelectNodes(
            "//m:ItemDefinitionGroup[contains(@Condition, 'Release|x64')]/m:ClCompile/m:WholeProgramOptimization",
            $namespace))
    Assert-True ($configurationWpo.Count -eq 0 -and
        $releaseWpo.Count -eq 1 -and
        $releaseWpo[0].InnerText -ceq 'true' -and
        $releaseWpo[0].GetAttribute('Condition') -ceq
            "'`$(FinalRelease)'=='true'") `
        'Whole-program compilation must be FinalRelease-only.'
    $deployExec = @($finalizer.SelectNodes('m:Exec', $namespace) |
        Where-Object { $_.Command -match 'Deploy-SPatchArtifact\.ps1' })
    Assert-True ($deployExec.Count -eq 1 -and
        $deployExec[0].Command.Contains(
            '-GameRoot "$(NormalizedGameDir)."')) `
        'GameRoot trailing-slash argument is not protected with the \. suffix.'
    $buildText = [IO.File]::ReadAllText($buildScript)
    Assert-True ($buildText -match 'Get-DeclaredConfigVersion' -and
        $buildText -match 'SPatch-default-v\$declaredConfigVersion\.ini' -and
        $buildText -notmatch 'SPatch-default-v[0-9]+\.ini') `
        'Release packaging no longer derives its staging INI from Config.h.'
    $iniDesignText = [IO.File]::ReadAllText($iniDesignValidator)
    Assert-True ($iniDesignText.Contains("permittedSections.Add('Debug')") -and
        -not $iniDesignText.Contains("permittedSections.Add('Diagnostics')")) `
        'INI design validation does not enforce the canonical v41 [Debug] section.'
    foreach ($field in @(
            'CONFIGURATION=Release', 'PLATFORM=x64', 'PE_MACHINE=8664',
            'PE_OPTIONAL_MAGIC=020B', 'FINAL_POLICY_ATTESTED=1',
            'PACKAGE_DIR=SPatch-Base', 'PACKAGE_ASI=SPatch-Base/SPatch.asi',
            'DEFAULT_INI=SPatch-Base/SPatch.ini',
            'MANIFEST=SPatch-Base/SHA256SUMS.txt')) {
        Assert-True $buildText.Contains($field) "Receipt field is missing: $field"
    }
    $deployText = [IO.File]::ReadAllText($deployScript)
    Assert-True ($deployText.Contains('[IO.File]::Replace') -and
        $deployText.Contains('Write-DurableJournal') -and
        $deployText.Contains("'COMMITTED'") -and
        $deployText -notmatch 'Remove-Item[^\r\n]+-Recurse') `
        'Deployment atomic-replacement/journal/no-recursive-delete contract drifted.'
    Assert-True (
        $deployText -notmatch (
            '\[IO\.File\]::Replace\(\s*\$temporaryJournal\s*,\s*' +
            '\$journalPath\s*,\s*\$null\s*,') -and
        $deployText.Contains(
            '".SPatch.deploy-journal.replace-backup-$($Values[''TRANSACTION''])"') -and
        $deployText -match (
            '\[IO\.File\]::Replace\(\s*\$temporaryJournal\s*,\s*' +
            '\$journalPath\s*,\s*\$journalBackup\s*,\s*\$true\s*\)') -and
        $deployText -match (
            '(?s)function Remove-JournalTemps\s*\{.*?' +
            '\(\?:tmp\|replace-backup\)-\[0-9a-f\]\{32\}\$.*?' +
            'Remove-SafeLeaf')) `
        'Deployment journal replacement backup/cleanup contract drifted.'
    Assert-True ([IO.File]::ReadAllText($targetValidator).Contains(
        'Assert-NoReparsePath')) `
        'Deployment target validator does not reject reparse paths.'
    $results.Add('release-contracts')

    $lockToken = [Guid]::NewGuid().ToString('N')
    $wrongLockToken = [Guid]::NewGuid().ToString('N')
    $lockAcquired = $false
    try {
        & $buildScript -BuildLockAction Acquire -LockToken $lockToken `
            -LockOwnerPid $PID
        $lockAcquired = $true
        Invoke-ExpectedFailure 'concurrent final-build lock' `
            'already owns the canonical outputs' {
            & $buildScript -BuildLockAction Acquire -LockToken $wrongLockToken `
                -LockOwnerPid $PID
        }
        Invoke-ExpectedFailure 'foreign final-build lock release' `
            'owned by another invocation' {
            & $buildScript -BuildLockAction Release -LockToken $wrongLockToken `
                -LockOwnerPid $PID
        }
    } finally {
        if ($lockAcquired) {
            & $buildScript -BuildLockAction Release -LockToken $lockToken `
                -LockOwnerPid $PID
        }
    }
    $releaseDirectory = Join-Path $RepoRoot 'artifacts\release'
    $leftoverBuildLocks = @(Get-ChildItem -LiteralPath $releaseDirectory `
        -Force -File |
        Where-Object {
            $_.Name -match '^\.SPatch\.final-build\.[0-9A-Fa-f]{24}\.lock$'
        })
    Assert-True ($leftoverBuildLocks.Count -eq 0) `
        'Final-build lock unit test left a canonical lock behind.'
    $results.Add('root-scoped-build-lock')

    $archiveInput = Join-Path $fixtureRoot 'archive input'
    [IO.Directory]::CreateDirectory((Join-Path $archiveInput 'z')) | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $archiveInput 'z\last.txt'), 'last',
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        (Join-Path $archiveInput 'first.txt'), 'first',
        [Text.UTF8Encoding]::new($false))
    [IO.File]::SetLastWriteTimeUtc(
        (Join-Path $archiveInput 'first.txt'), [DateTime]::UtcNow.AddYears(-5))
    [IO.File]::SetLastWriteTimeUtc(
        (Join-Path $archiveInput 'z\last.txt'), [DateTime]::UtcNow)
    $archiveOne = Join-Path $fixtureRoot 'one.zip'
    $archiveTwo = Join-Path $fixtureRoot 'two.zip'
    & $buildScript -ArchiveFixtureRoot $archiveInput `
        -ArchiveFixtureOutputPath $archiveOne | Out-Null
    & $buildScript -ArchiveFixtureRoot $archiveInput `
        -ArchiveFixtureOutputPath $archiveTwo | Out-Null
    $hashOne = (Get-FileHash -LiteralPath $archiveOne -Algorithm SHA256).Hash
    $hashTwo = (Get-FileHash -LiteralPath $archiveTwo -Algorithm SHA256).Hash
    Assert-True ($hashOne -ceq $hashTwo) `
        'Identical inputs did not produce an identical deterministic ZIP hash.'
    Add-Type -AssemblyName System.IO.Compression
    $stream = [IO.File]::OpenRead($archiveOne)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $stream, [IO.Compression.ZipArchiveMode]::Read, $false)
        try {
            $names = @($archive.Entries | ForEach-Object { $_.FullName })
            Assert-True (($names -join ',') -ceq 'first.txt,z/last.txt') `
                'Deterministic ZIP entries are not ordinally sorted.'
            foreach ($entry in $archive.Entries) {
                $timestamp = $entry.LastWriteTime.DateTime
                Assert-True ($timestamp.Year -eq 1980 -and
                    $timestamp.Month -eq 1 -and $timestamp.Day -eq 1 -and
                    $timestamp.Hour -eq 0 -and $timestamp.Minute -eq 0 -and
                    $timestamp.Second -eq 0) `
                    "ZIP entry timestamp drifted: $($entry.FullName)"
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    $results.Add("deterministic-zip:$hashOne")

    $junctionTarget = Join-Path $fixtureRoot 'junction-target'
    [IO.Directory]::CreateDirectory($junctionTarget) | Out-Null
    [IO.File]::WriteAllText((Join-Path $junctionTarget 'outside.txt'), 'outside')
    $junction = Join-Path $archiveInput 'reparse'
    $null = New-Item -ItemType Junction -Path $junction -Target $junctionTarget
    $reparseArchive = Join-Path $fixtureRoot 'reparse.zip'
    Invoke-ExpectedFailure 'reparse archive input' 'reparse point' {
        & $buildScript -ArchiveFixtureRoot $archiveInput `
            -ArchiveFixtureOutputPath $reparseArchive | Out-Null
    }
    Assert-True (-not (Test-Path -LiteralPath $reparseArchive)) `
        'Reparse rejection still created an archive.'
    Remove-Item -LiteralPath $junction -Force
    $results.Add('reparse-rejection')

    $probeScript = Join-Path $fixtureRoot 'Bind-Probe.ps1'
    $probeOutput = Join-Path $fixtureRoot 'bound.txt'
    $probeText = @'
param([Parameter(Mandatory)][string]$GameRoot,
      [Parameter(Mandatory)][string]$OutputPath)
[IO.File]::WriteAllText($OutputPath, $GameRoot, [Text.UTF8Encoding]::new($false))
'@
    [IO.File]::WriteAllText(
        $probeScript, $probeText, [Text.UTF8Encoding]::new($false))
    $bindingRoot = Join-Path $fixtureRoot 'game root with spaces'
    [IO.Directory]::CreateDirectory($bindingRoot) | Out-Null
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass `
        -File $probeScript -GameRoot ($bindingRoot + '\.') `
        -OutputPath $probeOutput
    Assert-True ($LASTEXITCODE -eq 0) 'Native PowerShell argument probe failed.'
    $boundValue = [IO.File]::ReadAllText($probeOutput)
    Assert-True ($boundValue.EndsWith('\.') -and
        [IO.Path]::GetFullPath($boundValue).TrimEnd([char[]]'\/') -ieq
            [IO.Path]::GetFullPath($bindingRoot).TrimEnd([char[]]'\/')) `
        "Trailing-slash GameRoot bound incorrectly: $boundValue"
    $results.Add('trailing-slash-binding')

    $isolatedRepo = Join-Path $fixtureRoot 'isolated-repo'
    foreach ($relativeDirectory in @(
            'tools', 'src', 'build\Release',
            'artifacts\release\SPatch-Base\licenses')) {
        [IO.Directory]::CreateDirectory(
            (Join-Path $isolatedRepo $relativeDirectory)) | Out-Null
    }
    [IO.File]::Copy($deployScript,
        (Join-Path $isolatedRepo 'tools\Deploy-SPatchArtifact.ps1'))
    [IO.File]::Copy($targetValidator,
        (Join-Path $isolatedRepo 'tools\Test-SPatchDeployTarget.ps1'))
    [IO.File]::Copy((Join-Path $RepoRoot 'src\BuildInfo.h'),
        (Join-Path $isolatedRepo 'src\BuildInfo.h'))
    $isolatedArtifact = Join-Path $isolatedRepo 'build\Release\SPatch.asi'
    Write-MinimalX64Pe $isolatedArtifact
    $isolatedPackage = Join-Path $isolatedRepo 'artifacts\release\SPatch-Base'
    [IO.File]::Copy($isolatedArtifact, (Join-Path $isolatedPackage 'SPatch.asi'))
    foreach ($payload in @(
            @{ Path = 'SPatch.ini'; Text = "[SPatch]`nConfigVersion=1`n" }
            @{ Path = 'README.md'; Text = 'fixture readme' }
            @{ Path = 'THIRD_PARTY_NOTICES.md'; Text = 'fixture notices' }
            @{ Path = 'licenses\MinHook-BSD-2-Clause.txt'; Text = 'fixture minhook' }
            @{ Path = 'licenses\SMAA-MIT.txt'; Text = 'fixture smaa' })) {
        [IO.File]::WriteAllText(
            (Join-Path $isolatedPackage $payload.Path), $payload.Text,
            [Text.UTF8Encoding]::new($false))
    }
    $manifestLines = [Collections.Generic.List[string]]::new()
    foreach ($relative in @(
            'licenses/MinHook-BSD-2-Clause.txt', 'licenses/SMAA-MIT.txt',
            'README.md', 'SPatch.asi', 'SPatch.ini',
            'THIRD_PARTY_NOTICES.md')) {
        $payloadPath = Join-Path $isolatedPackage $relative.Replace('/', '\')
        $payloadHash = (Get-FileHash -LiteralPath $payloadPath `
            -Algorithm SHA256).Hash
        $manifestLines.Add("$payloadHash *$relative")
    }
    $isolatedManifest = Join-Path $isolatedPackage 'SHA256SUMS.txt'
    [IO.File]::WriteAllLines(
        $isolatedManifest, $manifestLines, [Text.UTF8Encoding]::new($false))
    $isolatedArchive =
        Join-Path $isolatedRepo 'artifacts\release\SPatch-Base.zip'
    & $buildScript -ArchiveFixtureRoot $isolatedPackage `
        -ArchiveFixtureOutputPath $isolatedArchive | Out-Null
    $artifactHash = (Get-FileHash -LiteralPath $isolatedArtifact `
        -Algorithm SHA256).Hash
    $isolatedIni = Join-Path $isolatedPackage 'SPatch.ini'
    $iniHash = (Get-FileHash -LiteralPath $isolatedIni `
        -Algorithm SHA256).Hash
    $manifestHash = (Get-FileHash -LiteralPath $isolatedManifest `
        -Algorithm SHA256).Hash
    $archiveHash = (Get-FileHash -LiteralPath $isolatedArchive `
        -Algorithm SHA256).Hash
    $isolatedReceipt =
        Join-Path $isolatedRepo 'build\Release\SPatch.final-release.sha256'
    $receiptLines = @(
        'SPATCH_FINAL_RELEASE=1', 'CONFIGURATION=Release', 'PLATFORM=x64',
        'PE_MACHINE=8664', 'PE_OPTIONAL_MAGIC=020B',
        'FINAL_POLICY_ATTESTED=1', "SHA256=$artifactHash", 'FILE=SPatch.asi',
        'TEST_MODES=normal,SPATCH_FINAL_RELEASE', 'PACKAGE_DIR=SPatch-Base',
        'PACKAGE=SPatch-Base.zip', "PACKAGE_SHA256=$archiveHash",
        'PACKAGE_ASI=SPatch-Base/SPatch.asi',
        "PACKAGE_ASI_SHA256=$artifactHash",
        'DEFAULT_INI=SPatch-Base/SPatch.ini', "DEFAULT_INI_SHA256=$iniHash",
        'MANIFEST=SPatch-Base/SHA256SUMS.txt',
        "MANIFEST_SHA256=$manifestHash")
    [IO.File]::WriteAllLines(
        $isolatedReceipt, $receiptLines, [Text.UTF8Encoding]::new($false))

    $fakeGame = Join-Path $fixtureRoot 'fake-game'
    [IO.Directory]::CreateDirectory($fakeGame) | Out-Null
    Write-MinimalX64Pe (Join-Path $fakeGame 'sdhdship.exe')
    Write-MinimalX64Pe (Join-Path $fakeGame 'dinput8.dll')
    [IO.File]::WriteAllText((Join-Path $fakeGame 'SPatch.asi'), 'prior asi')
    [IO.File]::WriteAllText((Join-Path $fakeGame 'SPatch.pdb'), 'prior pdb')
    $gameBefore = Get-TreeFingerprint @($fakeGame)
    Invoke-ExpectedFailure 'unsupported-game no-mutation deploy' `
        'Unsupported sdhdship.exe SHA-256' {
        & (Join-Path $isolatedRepo 'tools\Deploy-SPatchArtifact.ps1') `
            -ArtifactPath $isolatedArtifact `
            -FinalReleaseIdentityPath $isolatedReceipt `
            -GameRoot ($fakeGame + '\.') `
            -BuildInfoPath (Join-Path $isolatedRepo 'src\BuildInfo.h') `
            -AsiLoaderPath (Join-Path $fakeGame 'dinput8.dll') | Out-Null
    }
    $gameAfter = Get-TreeFingerprint @($fakeGame)
    Assert-True ($gameAfter -ceq $gameBefore) `
        'Rejected deployment mutated the fake game root.'
    $reparseGame = Join-Path $fixtureRoot 'reparse-game'
    $null = New-Item -ItemType Junction -Path $reparseGame -Target $fakeGame
    Invoke-ExpectedFailure 'reparse game root deploy' 'reparse point' {
        & (Join-Path $isolatedRepo 'tools\Deploy-SPatchArtifact.ps1') `
            -ArtifactPath $isolatedArtifact `
            -FinalReleaseIdentityPath $isolatedReceipt `
            -GameRoot ($reparseGame + '\.') `
            -BuildInfoPath (Join-Path $isolatedRepo 'src\BuildInfo.h') `
            -AsiLoaderPath (Join-Path $reparseGame 'dinput8.dll') | Out-Null
    }
    Remove-Item -LiteralPath $reparseGame -Force
    Assert-True ((Get-TreeFingerprint @($fakeGame)) -ceq $gameBefore) `
        'Reparse-root rejection mutated the fake game target.'
    $results.Add('deploy-receipt-and-no-mutation')

    $msbuild = Find-MSBuild
    foreach ($configuration in @('Debug', 'Release')) {
        foreach ($targetName in @(
                'AssembleFinalReleasePackage', 'WriteFinalReleaseIdentity',
                'DeployToGame')) {
            $output = @(& $msbuild $projectPath /nologo "/t:$targetName" `
                "/p:Configuration=$configuration" /p:Platform=x64 `
                /p:FinalRelease=true /p:DeployGameArtifacts=true `
                /verbosity:minimal 2>&1)
            Assert-True ($LASTEXITCODE -ne 0) `
                "$targetName direct invocation unexpectedly succeeded in $configuration."
            Assert-True (($output -join "`n") -match
                '(cannot|not a public entry point)') `
                "$targetName direct invocation failed outside its explicit guard."
        }
    }
    $results.Add('direct-target-negatives')

    $afterFingerprint = Get-TreeFingerprint $publicationRoots
    Assert-True ($afterFingerprint -ceq $beforeFingerprint) `
        'Release negative/fixture tests mutated canonical publication outputs.'
    $results.Add('canonical-no-mutation')

    [pscustomobject]@{
        Result = 'PASS'
        Checks = $results.ToArray()
        CanonicalMutation = $false
    }
} finally {
    Remove-FixtureSafely $fixtureRoot
}
