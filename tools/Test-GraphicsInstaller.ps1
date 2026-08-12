[CmdletBinding()]
param(
    [ValidateSet('Development-Release', 'Publishing-Release')]
    [string] $Configuration = 'Publishing-Release',
    [string] $PackageRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $repoRoot `
        "artifacts\shenlong\$Configuration\ShenLong-Package"
}
$PackageRoot = [IO.Path]::GetFullPath($PackageRoot).TrimEnd([char[]]'\/')
$installer = Join-Path $repoRoot 'tools\Install-ShenLong.ps1'
$manifest = Join-Path $PackageRoot 'SHA256SUMS.txt'
$tmpRoot = Join-Path $repoRoot '.tmp'
$fixtureRoot = Join-Path $tmpRoot (
    'shenlong-installer-tests-' + [Guid]::NewGuid().ToString('N'))
$testLocalAppData = Join-Path $fixtureRoot 'LocalAppData'
$allowedPrefix = [IO.Path]::GetFullPath($tmpRoot).TrimEnd([char[]]'\/') +
    [IO.Path]::DirectorySeparatorChar + 'shenlong-installer-tests-'
$utf8 = [Text.UTF8Encoding]::new($false)
$originalLocalAppData = [Environment]::GetEnvironmentVariable(
    'LOCALAPPDATA', [EnvironmentVariableTarget]::Process)

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Get-ExpectedFnv1a64Utf16LePathHash([string] $Path) {
    $key = [IO.Path]::GetFullPath($Path).Replace('/', '\').ToLowerInvariant()
    [Numerics.BigInteger] $hash = [Numerics.BigInteger]::Parse(
        '14695981039346656037')
    [Numerics.BigInteger] $prime = 1099511628211
    [Numerics.BigInteger] $modulus = [Numerics.BigInteger]::Pow(2, 64)
    foreach ($character in $key.ToCharArray()) {
        $unit = [uint16][char]$character
        foreach ($byte in @(
                [byte]($unit -band 0xFF),
                [byte](($unit -shr 8) -band 0xFF))) {
            $hash = ([Numerics.BigInteger]::op_ExclusiveOr(
                    $hash, [Numerics.BigInteger]$byte) * $prime) % $modulus
        }
    }
    $hex = $hash.ToString('x', [Globalization.CultureInfo]::InvariantCulture)
    if ($hex.Length -gt 16) {
        $hex = $hex.Substring($hex.Length - 16)
    }
    return $hex.PadLeft(16, '0')
}

function Get-ExpectedExternalBackupPath(
    [string] $Owner,
    [string] $SourcePath,
    [string] $FileName) {
    $hash = Get-ExpectedFnv1a64Utf16LePathHash $SourcePath
    return Join-Path $testLocalAppData `
        "$Owner\ConfigBackups\$hash\$FileName"
}

function Assert-NoInstallerResidue([string] $Root) {
    $residue = @(Get-ChildItem -LiteralPath $Root -Force -Recurse |
        Where-Object {
            $_.Name -like '.ShenLong-install-backup-*' -or
            $_.Name -like '.ShenLong-install-work-*' -or
            $_.Name -ieq 'ReShade.ini.pre-ShenLong.bak'
        })
    $residuePaths = @($residue | ForEach-Object { $_.FullName })
    Assert-True ($residue.Count -eq 0) `
        "Installer residue remained under the game root: $($residuePaths -join ', ')"
}

function Get-TreeIdentity([string] $Root) {
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($item in @(Get-ChildItem -LiteralPath $fullRoot -Force -Recurse |
            Sort-Object FullName)) {
        $relative = $item.FullName.Substring($fullRoot.Length + 1)
        if ($item.PSIsContainer) {
            $lines.Add("D|$relative|$([int]$item.Attributes)")
        } else {
            $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
            $lines.Add("F|$relative|$([int]$item.Attributes)|$($item.Length)|$hash|$($item.LastWriteTimeUtc.Ticks)")
        }
    }
    return [string]::Join("`n", $lines)
}

function Get-ManifestRecord([string] $Line) {
    $match = [regex]::Match(
        $Line, '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$')
    if (-not $match.Success) {
        throw "Malformed fixture manifest line: $Line"
    }
    return [pscustomobject]@{
        Hash = $match.Groups['hash'].Value
        RelativePath = $match.Groups['path'].Value.Replace('/', '\')
        Line = $Line
    }
}

function Test-DeploymentPath([string] $RelativePath) {
    return $RelativePath -ieq 'ShenLong.asi' -or
        $RelativePath -ieq 'ShenLong.ini' -or
        $RelativePath -ieq 'dxgi.dll' -or
        $RelativePath -ieq 'ReShade.ini' -or
        $RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
}

function Write-FakeAsiLoader([string] $Path) {
    $bytes = [byte[]]::new(1024)
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($bytes, 0x98)
    [Text.Encoding]::ASCII.GetBytes(
        "DirectInput8Create`0Ultimate ASI Loader`0.asi`0").CopyTo(
            $bytes, 0x180)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function New-CaseRoot([string] $Name) {
    $root = Join-Path $fixtureRoot $Name
    [IO.Directory]::CreateDirectory($root) | Out-Null
    [IO.File]::WriteAllBytes(
        (Join-Path $root 'sdhdship.exe'),
        [byte[]](0x53, 0x50, 0x41, 0x54, 0x43, 0x48))
    Write-FakeAsiLoader (Join-Path $root 'dinput8.dll')
    return $root
}

function Invoke-Install(
    [string] $GameRoot,
    [switch] $ReplaceReShadeIni,
    [switch] $ValidateOnly,
    [string] $SelectedPackageRoot = $PackageRoot,
    [string] $SelectedInstaller = $installer) {
    $arguments = @{
        Configuration = $Configuration
        PackageRoot = $SelectedPackageRoot
        GameRoot = $GameRoot
        ReplaceReShadeIni = $ReplaceReShadeIni
        ValidateOnly = $ValidateOnly
    }
    return & $SelectedInstaller @arguments
}

function Get-InstalledManifestPaths([string] $GameRoot) {
    $path = Join-Path $GameRoot 'ShenLong-SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Fixture installed manifest is missing: $path"
    }
    return @([IO.File]::ReadAllLines($path, $utf8) | ForEach-Object {
            (Get-ManifestRecord $_).RelativePath
        })
}

function Invoke-ExpectedFailure(
    [string] $Name,
    [string] $ExpectedText,
    [scriptblock] $Arrange) {
    $root = New-CaseRoot $Name
    & $Arrange $root
    $before = Get-TreeIdentity $root
    $message = ''
    try {
        Invoke-Install $root | Out-Null
    } catch {
        $message = $_.Exception.Message
    }
    Assert-True (-not [string]::IsNullOrWhiteSpace($message)) `
        "$Name unexpectedly succeeded."
    Assert-True ($message.IndexOf(
            $ExpectedText, [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        "$Name failed for the wrong reason: $message"
    $after = Get-TreeIdentity $root
    Assert-True ($after -ceq $before) `
        "$Name changed the fake game tree before rejection. BEFORE=[$before] AFTER=[$after]"
    return [pscustomobject]@{ Case = $Name; Result = 'pass'; Detail = $message }
}

if (-not (Test-Path -LiteralPath $installer -PathType Leaf) -or
    -not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    throw 'Graphics installer test prerequisites are missing.'
}
$packagedInstaller = Join-Path $PackageRoot 'Install-ShenLong.ps1'
$policy = Join-Path $repoRoot 'tools\ReShadeIniPolicy.ps1'
$packagedPolicy = Join-Path $PackageRoot 'ReShadeIniPolicy.ps1'
foreach ($pair in @(
        [pscustomobject]@{ Name = 'installer'; Source = $installer; Package = $packagedInstaller },
        [pscustomobject]@{ Name = 'ReShade policy'; Source = $policy; Package = $packagedPolicy })) {
    if (-not (Test-Path -LiteralPath $pair.Package -PathType Leaf) -or
        (Get-FileHash -LiteralPath $pair.Source -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $pair.Package -Algorithm SHA256).Hash) {
        throw "Packaged graphics $($pair.Name) is stale or missing."
    }
}
$packageLines = @([IO.File]::ReadAllLines($manifest, $utf8))
$packageRecords = @($packageLines | ForEach-Object { Get-ManifestRecord $_ })
$deploymentRecords = @($packageRecords | Where-Object {
        Test-DeploymentPath $_.RelativePath
    })
$shenLongRecord = @($deploymentRecords | Where-Object {
        $_.RelativePath.StartsWith(
            'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
    })[0]
$ownedDeploymentRecords = @($deploymentRecords | Where-Object {
        $_.RelativePath -ine 'ShenLong.ini'
    })
$byPath = [Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($record in $packageRecords) {
    $byPath.Add($record.RelativePath, $record)
}
$packageConfigText = [IO.File]::ReadAllText(
    (Join-Path $PackageRoot 'ShenLong.ini'), $utf8)
Assert-True ($packageConfigText -notmatch
    '(?im)^\s*\[TextureFiltering\]\s*$|^\s*(?:AnisotropicFiltering|ForceAnisotropicFiltering)\s*=') `
    'Packaged ShenLong.ini retained SPatch-owned texture-filter configuration.'

[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
[IO.Directory]::CreateDirectory($testLocalAppData) | Out-Null
[Environment]::SetEnvironmentVariable(
    'LOCALAPPDATA', $testLocalAppData, [EnvironmentVariableTarget]::Process)
try {
    $packageIdentity = Get-TreeIdentity $PackageRoot
    $results = [Collections.Generic.List[object]]::new()

    foreach ($locationCase in @(
            [pscustomobject]@{ Name = 'reject-package-root-equal-game-root'; Child = $false },
            [pscustomobject]@{ Name = 'reject-package-root-inside-game-root'; Child = $true })) {
        $locationRoot = New-CaseRoot $locationCase.Name
        $selectedPackage = if ($locationCase.Child) {
            $childPackage = Join-Path $locationRoot 'downloaded-package'
            [IO.Directory]::CreateDirectory($childPackage) | Out-Null
            $childPackage
        } else {
            $locationRoot
        }
        $locationBefore = Get-TreeIdentity $locationRoot
        $locationMessage = ''
        try {
            Invoke-Install $locationRoot -ValidateOnly `
                -SelectedPackageRoot $selectedPackage | Out-Null
        } catch {
            $locationMessage = $_.Exception.Message
        }
        Assert-True ($locationMessage.IndexOf(
                'PackageRoot must be outside GameRoot',
                [StringComparison]::OrdinalIgnoreCase) -ge 0) `
            "$($locationCase.Name) failed incorrectly: $locationMessage"
        Assert-True ((Get-TreeIdentity $locationRoot) -ceq $locationBefore) `
            "$($locationCase.Name) mutated the fake game tree."
        $results.Add([pscustomobject]@{
                Case = $locationCase.Name; Result = 'pass'
            })
    }

    $validation = Invoke-Install $fixtureRoot -ValidateOnly
    Assert-True (-not [bool]$validation.MutationPerformed) `
        'ValidateOnly reported a mutation.'
    $results.Add([pscustomobject]@{ Case = 'validate-only'; Result = 'pass' })

    $fresh = New-CaseRoot 'fresh-install'
    $freshResult = Invoke-Install $fresh
    $freshPaths = @(Get-InstalledManifestPaths $fresh)
    Assert-True ($freshPaths.Count -eq $ownedDeploymentRecords.Count) `
        'Fresh install did not own every file it created.'
    Assert-True ($freshPaths -inotcontains 'ShenLong.ini') `
        'Fresh install incorrectly claimed user configuration ownership.'
    Assert-True ((Get-FileHash -LiteralPath (Join-Path $fresh 'ShenLong.ini') `
            -Algorithm SHA256).Hash -ceq
        (Get-FileHash -LiteralPath (Join-Path $PackageRoot 'ShenLong.ini') `
            -Algorithm SHA256).Hash) `
        'Fresh install did not copy the exact packaged ShenLong.ini default.'
    foreach ($metadata in @(
            'SHENLONG-README.md', 'Install-ShenLong.ps1',
            'ReShadeIniPolicy.ps1',
            'THIRD_PARTY_NOTICES.md', 'licenses')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $fresh $metadata))) `
            "Fresh install copied package metadata: $metadata"
    }
    Assert-NoInstallerResidue $fresh
    $results.Add([pscustomobject]@{
            Case = 'fresh-deployment-only-install'
            Result = 'pass'
            OwnedFiles = $freshResult.InstalledFiles
        })

    $preservedConfigRoot = New-CaseRoot 'existing-shenlong-config'
    $preservedConfig = Join-Path $preservedConfigRoot 'ShenLong.ini'
    $preservedConfigBytes = [Text.Encoding]::UTF8.GetBytes(
        ("[ShenLong]`r`nConfigVersion=1`r`n`r`n" +
         "[Tonemapping]`r`nAgX=0`r`n; user-owned sentinel`r`n"))
    [IO.File]::WriteAllBytes($preservedConfig, $preservedConfigBytes)
    $preservedConfigTime = [datetime]::new(
        2021, 3, 4, 5, 6, 7, [DateTimeKind]::Utc)
    [IO.File]::SetLastWriteTimeUtc($preservedConfig, $preservedConfigTime)
    $preservedConfigResult = Invoke-Install $preservedConfigRoot
    Assert-True ([bool]$preservedConfigResult.ShenLongIniPreserved) `
        'Existing ShenLong.ini was not reported as preserved.'
    Assert-True ([IO.File]::ReadAllBytes($preservedConfig).Length -eq
        $preservedConfigBytes.Length -and
        (Get-FileHash -LiteralPath $preservedConfig -Algorithm SHA256).Hash -ceq
        ([BitConverter]::ToString(
            [Security.Cryptography.SHA256]::Create().ComputeHash(
                $preservedConfigBytes))).Replace('-', '') -and
        [IO.File]::GetLastWriteTimeUtc($preservedConfig) -eq
            $preservedConfigTime) `
        'Existing ShenLong.ini bytes or timestamp changed during upgrade.'
    Assert-True ((Get-InstalledManifestPaths $preservedConfigRoot) `
            -inotcontains 'ShenLong.ini') `
        'Preserved ShenLong.ini was claimed in the installed manifest.'
    $results.Add([pscustomobject]@{
            Case = 'preserve-user-shenlong-config'; Result = 'pass'
        })

    $canonicalConfigRoot = New-CaseRoot `
        'canonicalize-existing-shenlong-config'
    $canonicalConfig = Join-Path $canonicalConfigRoot 'ShenLong.ini'
    $staleConfigText =
        ("[ShenLong]`r`nConfigVersion=1`r`n`r`n" +
         "[Tonemapping]`r`nAgX=0`r`n; preserve-before`r`n`r`n" +
         "[TextureFiltering]`r`n" +
         "AnisotropicFiltering=16`r`n" +
         "ForceAnisotropicFiltering=1`r`n`r`n" +
         "[FutureUserSection]`r`nKeepMe=exact`r`n" +
         "ForceAnisotropicFiltering=0`r`n; preserve-after`r`n")
    $expectedCanonicalText =
        ("[ShenLong]`r`nConfigVersion=1`r`n`r`n" +
         "[Tonemapping]`r`nAgX=0`r`n; preserve-before`r`n`r`n" +
         "[FutureUserSection]`r`nKeepMe=exact`r`n" +
         "; preserve-after`r`n")
    [IO.File]::WriteAllText($canonicalConfig, $staleConfigText, $utf8)
    $staleConfigHash = (Get-FileHash -LiteralPath $canonicalConfig `
        -Algorithm SHA256).Hash
    $canonicalBackup = Get-ExpectedExternalBackupPath `
        'ShenLong' $canonicalConfig `
        "ShenLong-pre-texture-filtering-$staleConfigHash.ini"
    $canonicalResult = Invoke-Install $canonicalConfigRoot
    Assert-True (-not [bool]$canonicalResult.ShenLongIniPreserved -and
        [bool]$canonicalResult.ShenLongIniCanonicalized -and
        $canonicalResult.RemovedTextureFilteringSections -eq 1 -and
        $canonicalResult.RemovedTextureFilteringKeys -eq 1) `
        'Existing ShenLong.ini cleanup reported the wrong migration result.'
    Assert-True ([IO.File]::ReadAllText($canonicalConfig, $utf8) -ceq
        $expectedCanonicalText) `
        'Existing ShenLong.ini cleanup changed unrelated user bytes.'
    Assert-True ($canonicalResult.PersistentShenLongConfigBackup -ceq
        $canonicalBackup -and
        (Test-Path -LiteralPath $canonicalBackup -PathType Leaf) -and
        (Get-FileHash -LiteralPath $canonicalBackup -Algorithm SHA256).Hash `
            -ceq $staleConfigHash) `
        'Existing ShenLong.ini cleanup did not retain an exact external backup.'
    Assert-True ((Get-InstalledManifestPaths $canonicalConfigRoot) `
            -inotcontains 'ShenLong.ini') `
        'Canonicalized ShenLong.ini was claimed in the installed manifest.'
    Assert-NoInstallerResidue $canonicalConfigRoot
    $results.Add([pscustomobject]@{
            Case = 'canonicalize-retired-shenlong-texture-filtering'
            Result = 'pass'
        })

    foreach ($invalidConfig in @(
            [pscustomobject]@{
                Name = 'reject-missing-shenlong-config-version'
                Text = "[ShenLong]`r`nEnabled=1`r`n"
            },
            [pscustomobject]@{
                Name = 'reject-wrong-shenlong-config-version'
                Text = "[ShenLong]`r`nConfigVersion=2`r`n"
            },
            [pscustomobject]@{
                Name = 'reject-duplicate-shenlong-config-version'
                Text = "[ShenLong]`r`nConfigVersion=1`r`nConfigVersion=1`r`n"
            })) {
        $results.Add((Invoke-ExpectedFailure $invalidConfig.Name `
            '[ShenLong] ConfigVersion=1' {
                param($root)
                [IO.File]::WriteAllText(
                    (Join-Path $root 'ShenLong.ini'),
                    $invalidConfig.Text, $utf8)
            }))
    }

    $migrationRoot = New-CaseRoot 'migrate-spatch-graphics-config'
    $currentSpatchConfig = Join-Path $migrationRoot 'SPatch.ini'
    $externalSpatchBackup = Get-ExpectedExternalBackupPath `
        'SPatch' $currentSpatchConfig 'SPatch-pre-v42.ini'
    [IO.Directory]::CreateDirectory(
        [IO.Path]::GetDirectoryName($externalSpatchBackup)) | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $migrationRoot 'SPatch.ini.previous.bak'),
        ("[Tonemapping]`r`nAgX=0`r`nAgXExposure=125`r`n" +
         "[Graphics]`r`nAnisotropicFiltering=8`r`n" +
         "[Shadows]`r`nShadowResolution=4096`r`n" +
         "[AmbientOcclusion]`r`nAmbientOcclusion=SDAO`r`nSDAOQuality=1`r`n" +
         "[PhysicallyBasedRendering]`r`nPhysicallyBasedRendering=0`r`n" +
         "[Debug]`r`nDumpShaders=1`r`nCensusShadowConsumers=1`r`n"), $utf8)
    [IO.File]::WriteAllText(
        $externalSpatchBackup,
        ("[Tonemapping]`r`nAgX=0`r`nAgXExposure=150`r`n" +
         "[Graphics]`r`nForceAnisotropicFiltering=1`r`n" +
         "[Shadows]`r`nShadowResolution=2048`r`n" +
         "[AmbientOcclusion]`r`nAmbientOcclusion=GTAOLite`r`n" +
         "[PhysicallyBasedRendering]`r`nPhysicallyBasedRendering=1`r`n" +
         "[SubsurfaceScattering]`r`nStockHairBlur=1`r`n"), $utf8)
    [IO.File]::WriteAllText(
        $currentSpatchConfig,
        ("[Tonemapping]`r`nAgX=1`r`nAgXLook=Neutral`r`n" +
         "[Graphics]`r`nAnisotropicFiltering=16`r`n" +
         "[AmbientOcclusion]`r`nAmbientOcclusion=Original`r`n" +
         "[GlobalIllumination]`r`nGlobalIllumination=1`r`nGIQuality=4`r`n" +
         "[Shadows]`r`nShadowFilterScale=2`r`n"), $utf8)
    $migrationResult = Invoke-Install $migrationRoot
    $migratedText = [IO.File]::ReadAllText(
        (Join-Path $migrationRoot 'ShenLong.ini'), $utf8)
    foreach ($expectedLine in @(
            'AgX=1', 'AgXLook=Neutral', 'AgXExposure=150',
            'ShadowResolution=2048', 'AmbientOcclusion=Original',
            'SDAOQuality=1', 'GlobalIllumination=1', 'GIQuality=4',
            'PhysicallyBasedRendering=1', 'StockHairBlur=1',
            'DumpShaders=1', 'CensusShadowConsumers=1')) {
        Assert-True ($migratedText -cmatch
            ('(?m)^' + [regex]::Escape($expectedLine) + '\r?$')) `
            "Migrated ShenLong.ini omitted expected value: $expectedLine"
    }
    Assert-True ($migratedText -notmatch
        '(?im)^\s*ShadowFilterScale\s*=|^\s*\[TextureFiltering\]\s*$|^\s*(?:AnisotropicFiltering|ForceAnisotropicFiltering)\s*=') `
        'Migration retained a retired shadow or texture-filter setting.'
    $expectedMigrationSources = @(
        $currentSpatchConfig,
        $externalSpatchBackup,
        (Join-Path $migrationRoot 'SPatch.ini.previous.bak'))
    Assert-True ($migrationResult.MigratedGraphicsValues -eq 12 -and
        $migrationResult.MigrationSources.Count -eq 3) `
        'Migration did not report all three graphics sources and exact values.'
    for ($index = 0; $index -lt $expectedMigrationSources.Count; ++$index) {
        Assert-True ($migrationResult.MigrationSources[$index] -ceq
            $expectedMigrationSources[$index]) `
            "Migration source precedence was wrong at index $index."
    }
    Assert-True ((Get-InstalledManifestPaths $migrationRoot) `
            -inotcontains 'ShenLong.ini') `
        'Migrated ShenLong.ini was claimed in the installed manifest.'
    Assert-NoInstallerResidue $migrationRoot
    $results.Add([pscustomobject]@{
            Case = 'migrate-spatch-config-with-current-precedence'
            Result = 'pass'
            MigratedValues = $migrationResult.MigratedGraphicsValues
        })

    foreach ($existingConfig in @(
            [pscustomobject]@{
                Name = 'existing-reshade-default-root'
                Text = "[GENERAL]`r`nPerformanceMode=1`r`n"
            },
            [pscustomobject]@{
                Name = 'existing-reshade-extra-addon-key'
                Text = "[ADDON]`r`nAddonPath=.\`r`nCustomSetting=1`r`n"
            },
            [pscustomobject]@{
                Name = 'existing-reshade-pinned-comments-and-case'
                Text = (";[ADDON]`r`n;AddonPath=mods`r`n" +
                    "/[ADDON]`r`n/AddonPath=mods`r`n" +
                    "#[ADDON]`r`n#AddonPath=mods`r`n" +
                    "[addon]`r`nAddonPath=mods`r`n")
            })) {
        $root = New-CaseRoot $existingConfig.Name
        $path = Join-Path $root 'ReShade.ini'
        [IO.File]::WriteAllText($path, $existingConfig.Text, $utf8)
        $before = Get-TreeIdentity $root
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $result = Invoke-Install $root
        Assert-True ([bool]$result.ReShadeIniPreserved) `
            "$($existingConfig.Name) did not report preserve mode."
        Assert-True ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $hash) `
            "$($existingConfig.Name) changed ReShade.ini."
        Assert-True ((Get-InstalledManifestPaths $root) -inotcontains 'ReShade.ini') `
            "$($existingConfig.Name) claimed ownership of an external ReShade.ini."
        $results.Add([pscustomobject]@{
                Case = $existingConfig.Name
                Result = 'pass'
                OriginalTreeRecorded = -not [string]::IsNullOrEmpty($before)
            })
    }

    $results.Add((Invoke-ExpectedFailure 'redirected-reshade' `
        'redirects add-ons away from the game root' {
            param($root)
            [IO.File]::WriteAllText(
                (Join-Path $root 'ReShade.ini'),
                "[ADDON]`r`nAddonPath=mods`r`n", $utf8)
        }))
    foreach ($redirectHeader in @('[ ADDON ]', '[[ADDON]]', '[ADDON] trailing')) {
        $caseName = 'redirected-reshade-' + ($redirectHeader -replace '[^A-Za-z]', '')
        $results.Add((Invoke-ExpectedFailure $caseName `
            'redirects add-ons away from the game root' {
                param($root)
                [IO.File]::WriteAllText(
                    (Join-Path $root 'ReShade.ini'),
                    "$redirectHeader`r`nAddonPath=mods`r`n", $utf8)
            }))
    }
    $results.Add((Invoke-ExpectedFailure 'duplicate-equivalent-addon-section' `
        'at most one effective' {
            param($root)
            [IO.File]::WriteAllText(
                (Join-Path $root 'ReShade.ini'),
                "[ADDON]`r`nAddonPath=.`r`n[ ADDON ]`r`nCustom=1`r`n", $utf8)
        }))
    $results.Add((Invoke-ExpectedFailure 'duplicate-addon-path' `
        'duplicate effective AddonPath' {
            param($root)
            [IO.File]::WriteAllText(
                (Join-Path $root 'ReShade.ini'),
                "[ADDON]`r`nAddonPath=.`r`nAddonPath=.\`r`n", $utf8)
        }))
    $results.Add((Invoke-ExpectedFailure 'load-from-dll-main' `
        'may not contain effective LoadFromDllMain' {
            param($root)
            [IO.File]::WriteAllText(
                (Join-Path $root 'ReShade.ini'),
                "[ADDON]`r`nLoadFromDllMain=ShenLong.asi`r`n", $utf8)
        }))
    $results.Add((Invoke-ExpectedFailure 'conflicting-unowned-dxgi' `
        'unowned graphics root file' {
            param($root)
            [IO.File]::WriteAllBytes(
                (Join-Path $root 'dxgi.dll'), [byte[]](1, 2, 3, 4))
        }))
    $results.Add((Invoke-ExpectedFailure 'regular-file-reshade-cache' `
        'path is not a directory: ReShadeCache' {
            param($root)
            [IO.Directory]::CreateDirectory((Join-Path $root 'ShenLong')) | Out-Null
            [IO.File]::WriteAllBytes(
                (Join-Path $root 'ShenLong\ReShadeCache'), [byte[]](9))
        }))
    $results.Add((Invoke-ExpectedFailure 'unmanaged-shenlong-file' `
        'unmanaged file' {
            param($root)
            [IO.Directory]::CreateDirectory((Join-Path $root 'ShenLong')) | Out-Null
            [IO.File]::WriteAllBytes(
                (Join-Path $root 'ShenLong\foreign.bin'), [byte[]](7, 8))
        }))

    $reparseRoot = New-CaseRoot 'shenlong-reparse-point'
    $reparseTarget = Join-Path $fixtureRoot 'shenlong-reparse-target'
    [IO.Directory]::CreateDirectory($reparseTarget) | Out-Null
    [IO.File]::WriteAllBytes(
        (Join-Path $reparseTarget 'outside.bin'), [byte[]](4, 2))
    [IO.Directory]::CreateDirectory((Join-Path $reparseRoot 'ShenLong')) | Out-Null
    New-Item -ItemType Junction `
        -Path (Join-Path $reparseRoot 'ShenLong\ShaderCache') `
        -Target $reparseTarget | Out-Null
    $reparseBefore = Get-TreeIdentity $reparseRoot
    $reparseMessage = ''
    try {
        Invoke-Install $reparseRoot | Out-Null
    } catch {
        $reparseMessage = $_.Exception.Message
    }
    Assert-True ($reparseMessage.IndexOf(
            'reparse point', [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        "ShenLong reparse point failed incorrectly: $reparseMessage"
    Assert-True ((Get-TreeIdentity $reparseRoot) -ceq $reparseBefore) `
        'ShenLong reparse-point rejection changed the fake game tree.'
    $results.Add([pscustomobject]@{
            Case = 'shenlong-reparse-point'; Result = 'pass'
        })

    $rootJunctionGame = New-CaseRoot 'shenlong-root-junction-game'
    $rootJunctionTarget = Join-Path $fixtureRoot 'shenlong-root-junction-target'
    $rootJunctionFile = Join-Path `
        $rootJunctionTarget $shenLongRecord.RelativePath.Substring('ShenLong\'.Length)
    [IO.Directory]::CreateDirectory(
        [IO.Path]::GetDirectoryName($rootJunctionFile)) | Out-Null
    [IO.File]::Copy(
        (Join-Path $PackageRoot $shenLongRecord.RelativePath), $rootJunctionFile)
    New-Item -ItemType Junction `
        -Path (Join-Path $rootJunctionGame 'ShenLong') `
        -Target $rootJunctionTarget | Out-Null
    $rootJunctionGameBefore = Get-TreeIdentity $rootJunctionGame
    $rootJunctionTargetBefore = Get-TreeIdentity $rootJunctionTarget
    $rootJunctionMessage = ''
    try {
        Invoke-Install $rootJunctionGame | Out-Null
    } catch {
        $rootJunctionMessage = $_.Exception.Message
    }
    Assert-True ($rootJunctionMessage.IndexOf(
            'reparse point', [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        "ShenLong-root junction failed incorrectly: $rootJunctionMessage"
    Assert-True ((Get-TreeIdentity $rootJunctionGame) -ceq
        $rootJunctionGameBefore) `
        'ShenLong-root junction rejection changed the fake game tree.'
    Assert-True ((Get-TreeIdentity $rootJunctionTarget) -ceq
        $rootJunctionTargetBefore) `
        'ShenLong-root junction rejection changed its external target.'
    $results.Add([pscustomobject]@{
            Case = 'shenlong-root-reparse-point'; Result = 'pass'
        })

    $gameRootTarget = New-CaseRoot 'game-root-junction-target'
    $gameRootJunction = Join-Path $fixtureRoot 'game-root-junction'
    New-Item -ItemType Junction -Path $gameRootJunction `
        -Target $gameRootTarget | Out-Null
    $gameRootTargetBefore = Get-TreeIdentity $gameRootTarget
    $gameRootMessage = ''
    try {
        Invoke-Install $gameRootJunction | Out-Null
    } catch {
        $gameRootMessage = $_.Exception.Message
    }
    Assert-True ($gameRootMessage.IndexOf(
            'reparse point', [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        "Game-root junction failed incorrectly: $gameRootMessage"
    Assert-True ((Get-TreeIdentity $gameRootTarget) -ceq $gameRootTargetBefore) `
        'Game-root junction rejection changed its target.'
    $results.Add([pscustomobject]@{
            Case = 'game-root-reparse-point'; Result = 'pass'
        })

    $packageRootJunction = Join-Path $fixtureRoot 'package-root-junction'
    New-Item -ItemType Junction -Path $packageRootJunction `
        -Target $PackageRoot | Out-Null
    $packageRootJunctionGame = New-CaseRoot 'package-root-junction-game'
    $packageJunctionMessage = ''
    try {
        Invoke-Install $packageRootJunctionGame -ValidateOnly `
            -SelectedPackageRoot $packageRootJunction | Out-Null
    } catch {
        $packageJunctionMessage = $_.Exception.Message
    }
    Assert-True ($packageJunctionMessage.IndexOf(
            'reparse point', [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        "Package-root junction failed incorrectly: $packageJunctionMessage"
    $results.Add([pscustomobject]@{
            Case = 'package-root-reparse-point'; Result = 'pass'
        })

    $root = New-CaseRoot 'matching-unowned-root-file'
    $dxgi = Join-Path $root 'dxgi.dll'
    [IO.File]::Copy((Join-Path $PackageRoot 'dxgi.dll'), $dxgi)
    $dxgiTime = [datetime]::new(2020, 1, 2, 3, 4, 5, [DateTimeKind]::Utc)
    [IO.File]::SetLastWriteTimeUtc($dxgi, $dxgiTime)
    $dxgiHash = (Get-FileHash -LiteralPath $dxgi -Algorithm SHA256).Hash
    $result = Invoke-Install $root
    Assert-True ($result.PreservedUnownedRootFiles -icontains 'dxgi.dll') `
        'Matching external dxgi.dll was not reported as preserved.'
    Assert-True ((Get-InstalledManifestPaths $root) -inotcontains 'dxgi.dll') `
        'Matching external dxgi.dll was incorrectly claimed as owned.'
    Assert-True ((Get-FileHash -LiteralPath $dxgi -Algorithm SHA256).Hash -ceq $dxgiHash -and
        [IO.File]::GetLastWriteTimeUtc($dxgi) -eq $dxgiTime) `
        'Matching external dxgi.dll changed during installation.'
    $results.Add([pscustomobject]@{
            Case = 'matching-unowned-root-file'; Result = 'pass'
        })

    $root = New-CaseRoot 'matching-unowned-shenlong-file'
    $shenLongPath = Join-Path $root $shenLongRecord.RelativePath
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($shenLongPath)) | Out-Null
    [IO.File]::Copy((Join-Path $PackageRoot $shenLongRecord.RelativePath), $shenLongPath)
    $shenLongTime = [datetime]::new(2020, 2, 3, 4, 5, 6, [DateTimeKind]::Utc)
    [IO.File]::SetLastWriteTimeUtc($shenLongPath, $shenLongTime)
    $result = Invoke-Install $root
    Assert-True ($result.PreservedUnownedShenLongFiles -icontains
        $shenLongRecord.RelativePath) `
        'Matching external ShenLong file was not reported as preserved.'
    Assert-True ((Get-InstalledManifestPaths $root) -inotcontains
        $shenLongRecord.RelativePath) `
        'Matching external ShenLong file was incorrectly claimed as owned.'
    Assert-True ([IO.File]::GetLastWriteTimeUtc($shenLongPath) -eq $shenLongTime) `
        'Matching external ShenLong file timestamp changed.'
    $results.Add([pscustomobject]@{
            Case = 'matching-unowned-shenlong-file'; Result = 'pass'
        })

    $results.Add((Invoke-ExpectedFailure 'conflicting-unowned-shenlong-file' `
        'conflicting unowned file' {
            param($root)
            $record = @($deploymentRecords | Where-Object {
                    $_.RelativePath.StartsWith(
                        'ShenLong\', [StringComparison]::OrdinalIgnoreCase)
                })[0]
            $path = Join-Path $root $record.RelativePath
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllBytes($path, [byte[]](0, 1, 2))
        }))

    foreach ($replaceCase in @(
            [pscustomobject]@{ Name = 'replace-custom-reshade'; Exact = $false },
            [pscustomobject]@{ Name = 'adopt-matching-reshade'; Exact = $true })) {
        $root = New-CaseRoot $replaceCase.Name
        $reshade = Join-Path $root 'ReShade.ini'
        if ($replaceCase.Exact) {
            [IO.File]::Copy((Join-Path $PackageRoot 'ReShade.ini'), $reshade)
        } else {
            [IO.File]::WriteAllText(
                $reshade, "[GENERAL]`r`nPerformanceMode=1`r`n", $utf8)
        }
        $originalHash = (Get-FileHash -LiteralPath $reshade -Algorithm SHA256).Hash
        $result = Invoke-Install $root -ReplaceReShadeIni
        $backup = Get-ExpectedExternalBackupPath `
            'ShenLong' $reshade 'ReShade-pre-ShenLong.ini'
        Assert-True ($result.PersistentReShadeBackup -ceq $backup) `
            "$($replaceCase.Name) reported the wrong deterministic backup path."
        $gameRootPrefix = [IO.Path]::GetFullPath($root).TrimEnd([char[]]'\/') +
            [IO.Path]::DirectorySeparatorChar
        Assert-True (-not [IO.Path]::GetFullPath($backup).StartsWith(
                $gameRootPrefix, [StringComparison]::OrdinalIgnoreCase)) `
            "$($replaceCase.Name) retained its displaced ReShade.ini in GameRoot."
        Assert-True (-not $backup.EndsWith(
                '.bak', [StringComparison]::OrdinalIgnoreCase)) `
            "$($replaceCase.Name) used a .bak extension for its external backup."
        Assert-True ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ceq
            $originalHash) "$($replaceCase.Name) did not retain the original bytes."
        Assert-True ((Get-InstalledManifestPaths $root) -icontains 'ReShade.ini') `
            "$($replaceCase.Name) did not record explicit ownership."
        Assert-True ((Get-FileHash -LiteralPath $reshade -Algorithm SHA256).Hash -ceq
            $byPath['ReShade.ini'].Hash) `
            "$($replaceCase.Name) did not install the packaged configuration."
        Assert-NoInstallerResidue $root
        Assert-True (@(Get-ChildItem -LiteralPath $root -File -Force -Recurse `
                -Filter '*.bak').Count -eq 0) `
            "$($replaceCase.Name) left a .bak file under GameRoot."
        $results.Add([pscustomobject]@{
                Case = $replaceCase.Name; Result = 'pass'; BackupRetained = $true
            })
    }

    $old = New-CaseRoot 'current-full-manifest-upgrade'
    $oldLines = [Collections.Generic.List[string]]::new()
    foreach ($record in $packageRecords) {
        $destination = Join-Path $old $record.RelativePath
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) |
            Out-Null
        [IO.File]::Copy((Join-Path $PackageRoot $record.RelativePath), $destination)
        $oldLines.Add($record.Line)
    }
    [IO.File]::WriteAllLines(
        (Join-Path $old 'ShenLong-SHA256SUMS.txt'), $oldLines, $utf8)
    $shared = @('THIRD_PARTY_NOTICES.md') + @($packageRecords |
        Where-Object {
            $_.RelativePath.StartsWith(
                'licenses\', [StringComparison]::OrdinalIgnoreCase)
        } | ForEach-Object { $_.RelativePath })
    $sharedHashes = @{}
    foreach ($relative in $shared) {
        $sharedHashes[$relative] = (Get-FileHash -LiteralPath (
                Join-Path $old $relative) -Algorithm SHA256).Hash
    }
    Invoke-Install $old | Out-Null
    foreach ($retiredMetadata in @(
            'SHENLONG-README.md', 'Install-ShenLong.ps1',
            'ReShadeIniPolicy.ps1')) {
        Assert-True (-not (Test-Path -LiteralPath (
                    Join-Path $old $retiredMetadata))) `
            "Upgrade retained retired package metadata: $retiredMetadata"
    }
    foreach ($relative in $shared) {
        Assert-True ((Get-FileHash -LiteralPath (
                    Join-Path $old $relative) -Algorithm SHA256).Hash -ceq
            $sharedHashes[$relative]) "Upgrade changed shared metadata: $relative"
    }
    $installedLines = @(Get-InstalledManifestPaths $old)
    Assert-True ($installedLines.Count -eq $ownedDeploymentRecords.Count) `
        'Upgrade did not rewrite the manifest to the deployment-only set.'
    $results.Add([pscustomobject]@{
            Case = 'current-full-manifest-upgrade'; Result = 'pass'
            SharedFilesPreserved = $shared.Count
        })

    $legacy = New-CaseRoot 'owned-legacy-spatchgraphics-upgrade'
    $legacyAddon = Join-Path $legacy 'SPatchGraphics.addon'
    [IO.File]::Copy((Join-Path $PackageRoot 'ShenLong.asi'), $legacyAddon)
    $cacheRecord = @($packageRecords | Where-Object {
            $_.RelativePath.StartsWith(
                'ShenLong\ShaderCache\v1\',
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetExtension($_.RelativePath) -ieq '.cso'
        })[0]
    $legacyCacheRelative = $cacheRecord.RelativePath -replace
        '^ShenLong\\', 'SPatch\'
    $legacyCache = Join-Path $legacy $legacyCacheRelative
    [IO.Directory]::CreateDirectory(
        [IO.Path]::GetDirectoryName($legacyCache)) | Out-Null
    [IO.File]::Copy((Join-Path $PackageRoot $cacheRecord.RelativePath),
        $legacyCache)
    foreach ($sharedRuntime in @('dxgi.dll', 'ReShade.ini')) {
        [IO.File]::Copy((Join-Path $PackageRoot $sharedRuntime),
            (Join-Path $legacy $sharedRuntime))
    }
    [IO.File]::WriteAllText((Join-Path $legacy 'SPatch.ini'),
        "[Fixes]`r`nSentinel=keep`r`n", $utf8)
    $legacyLines = @(
        ('{0} *SPatchGraphics.addon' -f
            (Get-FileHash -LiteralPath $legacyAddon -Algorithm SHA256).Hash),
        ('{0} *{1}' -f
            (Get-FileHash -LiteralPath $legacyCache -Algorithm SHA256).Hash,
            $legacyCacheRelative.Replace('\', '/')),
        ('{0} *dxgi.dll' -f
            (Get-FileHash -LiteralPath (Join-Path $legacy 'dxgi.dll') `
                -Algorithm SHA256).Hash),
        ('{0} *ReShade.ini' -f
            (Get-FileHash -LiteralPath (Join-Path $legacy 'ReShade.ini') `
                -Algorithm SHA256).Hash))
    [IO.File]::WriteAllLines(
        (Join-Path $legacy 'SPatchGraphics-SHA256SUMS.txt'),
        $legacyLines, $utf8)
    Invoke-Install $legacy | Out-Null
    foreach ($retiredPath in @(
            'SPatchGraphics.addon', $legacyCacheRelative,
            'SPatchGraphics-SHA256SUMS.txt')) {
        Assert-True (-not (Test-Path -LiteralPath (
                    Join-Path $legacy $retiredPath))) `
            "Owned legacy upgrade retained: $retiredPath"
    }
    Assert-True (Test-Path -LiteralPath (Join-Path $legacy 'ShenLong.asi') `
        -PathType Leaf) 'Owned legacy upgrade did not install ShenLong.asi.'
    Assert-True ([IO.File]::ReadAllText(
            (Join-Path $legacy 'SPatch.ini'), $utf8) -ceq
        "[Fixes]`r`nSentinel=keep`r`n") `
        'Owned legacy cleanup changed the base SPatch.ini.'
    $results.Add([pscustomobject]@{
            Case = 'retire-proven-legacy-runtime'; Result = 'pass'
        })

    $results.Add((Invoke-ExpectedFailure 'reject-unowned-legacy-addon' `
        'unowned or changed' {
            param($root)
            [IO.File]::Copy((Join-Path $PackageRoot 'ShenLong.asi'),
                (Join-Path $root 'SPatchGraphics.addon'))
        }))
    $results.Add((Invoke-ExpectedFailure 'reject-changed-owned-legacy-addon' `
        'cannot prove ownership' {
            param($root)
            $path = Join-Path $root 'SPatchGraphics.addon'
            [IO.File]::Copy((Join-Path $PackageRoot 'ShenLong.asi'), $path)
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            [IO.File]::WriteAllLines(
                (Join-Path $root 'SPatchGraphics-SHA256SUMS.txt'),
                @("$hash *SPatchGraphics.addon"), $utf8)
            [IO.File]::WriteAllBytes($path, [byte[]](1, 2, 3, 4))
        }))

    $missingLoaderResult = Invoke-ExpectedFailure 'missing-asi-loader' `
        'install a compatible x64 ASI loader separately' {
            param($root)
            [IO.File]::Delete((Join-Path $root 'dinput8.dll'))
        }
    Assert-True ($missingLoaderResult.Detail.IndexOf(
            'SPatch base', [StringComparison]::OrdinalIgnoreCase) -lt 0) `
        'Missing-loader guidance incorrectly required the SPatch base mod.'
    $results.Add($missingLoaderResult)
    $results.Add((Invoke-ExpectedFailure 'invalid-asi-loader' `
        'not a valid PE image' {
            param($root)
            [IO.File]::WriteAllBytes(
                (Join-Path $root 'dinput8.dll'), [byte[]](1, 2, 3, 4))
        }))

    $rollbackRoot = New-CaseRoot 'late-failure-rollback'
    $rollbackReShade = Join-Path $rollbackRoot 'ReShade.ini'
    [IO.File]::WriteAllText(
        $rollbackReShade, "[GENERAL]`r`nPerformanceMode=1`r`n", $utf8)
    $rollbackExternalBackup = Get-ExpectedExternalBackupPath `
        'ShenLong' $rollbackReShade 'ReShade-pre-ShenLong.ini'
    $rollbackBefore = Get-TreeIdentity $rollbackRoot
    $injectedManifestDirectory = Join-Path `
        $rollbackRoot 'ShenLong-SHA256SUMS.txt'
    $rollbackJob = Start-Job -ScriptBlock {
        param($GameRoot, $ManifestDirectory)
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while ([DateTime]::UtcNow -lt $deadline) {
            $backup = @(Get-ChildItem -LiteralPath $GameRoot -Directory -Force `
                -Filter '.ShenLong-install-backup-*' `
                -ErrorAction SilentlyContinue)
            if ($backup.Count -ne 0) {
                [IO.Directory]::CreateDirectory($ManifestDirectory) | Out-Null
                return 'injected'
            }
            Start-Sleep -Milliseconds 2
        }
        throw 'Timed out waiting for the installer transaction.'
    } -ArgumentList $rollbackRoot, $injectedManifestDirectory
    $rollbackMessage = ''
    try {
        Invoke-Install $rollbackRoot -ReplaceReShadeIni | Out-Null
    } catch {
        $rollbackMessage = $_.Exception.Message
    }
    $jobResult = Receive-Job -Job $rollbackJob -Wait -AutoRemoveJob
    Assert-True ($jobResult -ceq 'injected') `
        'Rollback fixture did not inject its late failure.'
    Assert-True ($rollbackMessage.IndexOf(
            'manifest path is not a file',
            [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        "Late rollback fixture failed incorrectly: $rollbackMessage"
    Assert-True (Test-Path -LiteralPath $injectedManifestDirectory -PathType Container) `
        'Rollback removed the external failure-injection directory.'
    [IO.Directory]::Delete($injectedManifestDirectory)
    Assert-True ((Get-TreeIdentity $rollbackRoot) -ceq $rollbackBefore) `
        'Late installer failure did not restore the original fake game tree.'
    Assert-True (-not (Test-Path -LiteralPath $rollbackExternalBackup)) `
        'Late rollback retained the external ReShade.ini backup it created.'
    Assert-NoInstallerResidue $rollbackRoot
    $results.Add([pscustomobject]@{
            Case = 'late-failure-rollback'; Result = 'pass'
        })

    $mutexRoot = New-CaseRoot 'mutex-contention'
    $mutex = [Threading.Mutex]::new($false, 'Local\SPatch.LiveGraphicsHarness')
    $ownsMutex = $false
    try {
        $ownsMutex = $mutex.WaitOne(0)
        Assert-True $ownsMutex 'Test process could not acquire the graphics mutex.'
        $mutexJob = Start-Job -ScriptBlock {
            param($Installer, $SelectedConfiguration, $SelectedPackage, $GameRoot)
            try {
                & $Installer -Configuration $SelectedConfiguration `
                    -PackageRoot $SelectedPackage -GameRoot $GameRoot | Out-Null
                return [pscustomobject]@{ Succeeded = $true; Text = '' }
            } catch {
                return [pscustomobject]@{
                    Succeeded = $false
                    Text = $_.Exception.Message
                }
            }
        } -ArgumentList $installer, $Configuration, $PackageRoot, $mutexRoot
        $completedMutexJob = Wait-Job -Job $mutexJob -Timeout 30
        Assert-True ($null -ne $completedMutexJob) `
            'Mutex-contention child did not exit within 30 seconds.'
        $childResult = Receive-Job -Job $mutexJob -Wait -AutoRemoveJob
        Assert-True (-not [bool]$childResult.Succeeded -and
            $childResult.Text.IndexOf(
                'Another SPatch/ShenLong live mutation already owns',
                [StringComparison]::OrdinalIgnoreCase) -ge 0) `
            "Mutex-contention child failed incorrectly: $($childResult.Text)"
    } finally {
        if ($ownsMutex) {
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
    }
    Assert-True (@(Get-ChildItem -LiteralPath $mutexRoot -Force).Count -eq 2) `
        'Mutex-contention rejection changed the fake game root.'
    $results.Add([pscustomobject]@{
            Case = 'mutex-contention'; Result = 'pass'
        })

    $isolatedPackage = Join-Path $fixtureRoot 'isolated-package'
    Copy-Item -LiteralPath $PackageRoot -Destination $isolatedPackage -Recurse
    $isolatedInstaller = Join-Path $isolatedPackage 'Install-ShenLong.ps1'
    $isolatedValidationGame = New-CaseRoot 'isolated-package-validation-game'
    $isolatedValidation = Invoke-Install $isolatedValidationGame -ValidateOnly `
        -SelectedPackageRoot $isolatedPackage `
        -SelectedInstaller $isolatedInstaller
    Assert-True (-not [bool]$isolatedValidation.MutationPerformed) `
        'Isolated package ValidateOnly reported a mutation.'
    $isolatedGame = New-CaseRoot 'isolated-package-install'
    Invoke-Install $isolatedGame `
        -SelectedPackageRoot $isolatedPackage `
        -SelectedInstaller $isolatedInstaller | Out-Null
    Assert-True ((Get-InstalledManifestPaths $isolatedGame).Count -eq
        $ownedDeploymentRecords.Count) 'Isolated package could not install itself.'
    $results.Add([pscustomobject]@{
            Case = 'isolated-package'; Result = 'pass'
        })

    Assert-True ((Get-TreeIdentity $PackageRoot) -ceq $packageIdentity) `
        'Graphics installer tests changed the source package.'
    [pscustomobject]@{
        Status = 'pass'
        Configuration = $Configuration
        Cases = $results.Count
        Results = @($results)
    }
} finally {
    [Environment]::SetEnvironmentVariable(
        'LOCALAPPDATA', $originalLocalAppData,
        [EnvironmentVariableTarget]::Process)
    if (Test-Path -LiteralPath $fixtureRoot) {
        $resolved = [IO.Path]::GetFullPath($fixtureRoot)
        if (-not $resolved.StartsWith(
                $allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing unsafe fixture cleanup: $resolved"
        }
        foreach ($reparsePoint in @(Get-ChildItem -LiteralPath $resolved `
                -Force -Recurse -Attributes ReparsePoint |
                Sort-Object { $_.FullName.Length } -Descending)) {
            if ($reparsePoint.PSIsContainer) {
                [IO.Directory]::Delete($reparsePoint.FullName, $false)
            } else {
                [IO.File]::Delete($reparsePoint.FullName)
            }
        }
        [IO.Directory]::Delete($resolved, $true)
    }
}
