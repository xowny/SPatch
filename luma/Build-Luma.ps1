param(
    [ValidateSet('Development-Release', 'Publishing-Release')]
    [string]$Configuration = 'Publishing-Release',
    [string]$ReShadeRoot = '',
    [string]$ReShadeSetupPath = '',
    [string]$MinHookRoot = '',
    [switch]$OfflineDependencies
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-ExactLicenseFileSet {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$ExpectedNames,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description is missing: $Path"
    }

    $expectedNameSet = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($expectedName in $ExpectedNames) {
        if ([string]::IsNullOrWhiteSpace($expectedName) -or
            -not $expectedNameSet.Add($expectedName)) {
            throw "$Description whitelist contains an empty or duplicate file name."
        }
    }

    $expectedSorted = @($expectedNameSet | Sort-Object)
    $items = @(Get-ChildItem -LiteralPath $Path -Force)
    $actualSorted = @($items | Select-Object -ExpandProperty Name | Sort-Object)
    $unsafeItems = @($items | Where-Object {
            $_.PSIsContainer -or
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        })
    $matchesWhitelist =
        $items.Count -eq $expectedSorted.Count -and
        $unsafeItems.Count -eq 0 -and
        @(Compare-Object -CaseSensitive -ReferenceObject $expectedSorted `
            -DifferenceObject $actualSorted).Count -eq 0
    if (-not $matchesWhitelist) {
        throw "$Description must contain only the exact whitelisted license files. Expected [$($expectedSorted -join ', ')], found [$($actualSorted -join ', ')]."
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot
$graphicsInstallerPath = Join-Path $repoRoot 'tools\Install-ShenLong.ps1'
$reShadeIniPolicyPath = Join-Path $repoRoot 'tools\ReShadeIniPolicy.ps1'
$shadowHookSafetyTestPath = Join-Path $repoRoot `
    'tools\Test-ShadowHookSafety.ps1'
$reShadeCallbackSafetyTestPath = Join-Path $repoRoot `
    'tools\Test-ReShadeCallbackSafety.ps1'
foreach ($requiredTool in @(
        $graphicsInstallerPath,
        $reShadeIniPolicyPath,
        $shadowHookSafetyTestPath,
        $reShadeCallbackSafetyTestPath)) {
    if (-not (Test-Path -LiteralPath $requiredTool -PathType Leaf)) {
        throw "Graphics packaging tool is missing: $requiredTool"
    }
}
$buildMutex = [Threading.Mutex]::new(
    $false, 'Local\ShenLong.GraphicsBuild')
$ownsBuildMutex = $false

try {
    try {
        $ownsBuildMutex = $buildMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        # The abandoning process is gone and this thread now owns the mutex.
        $ownsBuildMutex = $true
    }
    if (-not $ownsBuildMutex) {
        throw 'Another ShenLong build is already in progress; shared validation and publication outputs are locked.'
    }

$minHookResolver = Join-Path $repoRoot 'tools\Resolve-MinHook.ps1'
if (-not (Test-Path -LiteralPath $minHookResolver -PathType Leaf)) {
    throw "Pinned MinHook resolver is missing: $minHookResolver"
}
$minHookResolverArgs = @{
    RepoRoot = $repoRoot
}
if (-not [string]::IsNullOrWhiteSpace($MinHookRoot)) {
    $minHookResolverArgs.MinHookRoot = $MinHookRoot
}
if ($OfflineDependencies) {
    $minHookResolverArgs.Offline = $true
}
$resolvedMinHook = & $minHookResolver @minHookResolverArgs
if ($null -eq $resolvedMinHook -or
    [string]::IsNullOrWhiteSpace($resolvedMinHook.MinHookRoot)) {
    throw 'Pinned MinHook resolver returned an invalid result.'
}
$MinHookRoot = $resolvedMinHook.MinHookRoot
$shadowHookSafetyOutput = @(& powershell.exe -NoProfile -NonInteractive `
    -ExecutionPolicy Bypass -File $shadowHookSafetyTestPath `
    -MinHookRoot $MinHookRoot 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "Shadow-hook safety regression failed: $($shadowHookSafetyOutput -join [Environment]::NewLine)"
}
Write-Host ($shadowHookSafetyOutput -join [Environment]::NewLine)
$reShadeCallbackSafetyOutput = @(& powershell.exe -NoProfile -NonInteractive `
    -ExecutionPolicy Bypass -File $reShadeCallbackSafetyTestPath 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "ReShade callback safety regression failed: $($reShadeCallbackSafetyOutput -join [Environment]::NewLine)"
}
Write-Host ($reShadeCallbackSafetyOutput -join [Environment]::NewLine)
$pinnedReShadeCommit = '9fcd6ad935cfa19801e5e59a89a885dbdd6e731b'
$expectedShaderVariantCount = 104
$expectedCompiledShaderCacheVariantCount = 100
$expectedPackagedShaderCacheVariantCount = 94
$expectedCompiledShaderCacheFeatureCounts = [ordered]@{
    PBR = 18
    GI = 36
    SDAO = 26
    SSS = 17
    Water = 3
}
$expectedPackagedShaderCacheFeatureCounts = [ordered]@{
    PBR = 18
    GI = 36
    SDAO = 26
    SSS = 11
    Water = 3
}
$reShadeVersion = '6.7.3'
$reShadeRuntimeFileVersion = '6.7.3.2148'
$reShadeSetupUrl = "https://reshade.me/downloads/ReShade_Setup_${reShadeVersion}_Addon.exe"
$reShadeSetupSha256 = 'C78DB69BD127E98054BD496FB422655F4A1CC664E28F8D12CE9835B2647BC571'
$reShadeRuntimeSha256 = 'EC9245D05C11751F2AC0D2256E6921AD8FB36BE9172EF6D587856591EB729A25'
# This is a modified SDmodding prebuilt, not canonical MinHook. Its reduced
# header makes MH_CreateHook install immediately, and the SDK snapshot has no
# matching fork source. Resolve-MinHook.ps1 owns its immutable revision and
# artifact hashes so every build path verifies one dependency identity.

if ([string]::IsNullOrWhiteSpace($ReShadeRoot)) {
    $ReShadeRoot = Join-Path $repoRoot '.tmp\ReShade-API18'
}
if ([string]::IsNullOrWhiteSpace($ReShadeSetupPath)) {
    $ReShadeSetupPath = Join-Path $repoRoot ".tmp\ReShade-$reShadeVersion-Addon\ReShade_Setup_${reShadeVersion}_Addon.exe"
}

$reShadeConfigPath = Join-Path $scriptRoot 'ReShade.ini'
if (-not (Test-Path -LiteralPath $reShadeConfigPath -PathType Leaf)) {
    throw "Required ReShade configuration is missing: $reShadeConfigPath"
}
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$reShadeConfigText = [IO.File]::ReadAllText($reShadeConfigPath, $strictUtf8)
$addonSections = @([regex]::Matches(
        $reShadeConfigText,
        '(?ms)^[ \t]*\[ADDON\][^\r\n]*(?:\r?\n|$)(?<body>.*?)(?=^[ \t]*\[[^\]\r\n]+\][^\r\n]*(?:\r?\n|$)|\z)'))
if ($addonSections.Count -ne 1) {
    throw "Source ReShade.ini must contain exactly one [ADDON] section; found $($addonSections.Count)."
}
$activeAddonLines = @(
    $addonSections[0].Groups['body'].Value -split '\r?\n' |
        Where-Object {
            $trimmed = $_.Trim()
            -not [string]::IsNullOrWhiteSpace($trimmed) -and
            -not $trimmed.StartsWith(';') -and
            -not $trimmed.StartsWith('#')
        })
if ($activeAddonLines.Count -ne 1 -or
    $activeAddonLines[0].Trim() -cne 'AddonPath=.') {
    throw 'Source ReShade.ini must load add-ons only from the game root.'
}
if ($reShadeConfigText -match '(?im)^[ \t]*LoadFromDllMain[ \t]*=') {
    throw 'Source ReShade.ini may not contain LoadFromDllMain entries.'
}
$shenLongConfigPath = Join-Path $scriptRoot 'ShenLong.ini'
if (-not (Test-Path -LiteralPath $shenLongConfigPath -PathType Leaf)) {
    throw "Required ShenLong configuration is missing: $shenLongConfigPath"
}
$shenLongConfigText = [IO.File]::ReadAllText($shenLongConfigPath, $strictUtf8)
if ($shenLongConfigText -match '(?im)^\s*(?:ShadowFilterScale|PCSS\w*)\s*=') {
    throw 'Source ShenLong.ini contains a retired ShadowFilterScale or PCSS key.'
}
$shenLongSections = @([regex]::Matches(
    $shenLongConfigText,
    '(?ms)^[ \t]*\[ShenLong\][^\r\n]*(?:\r?\n|$)(?<body>.*?)(?=^[ \t]*\[[^\]\r\n]+\][^\r\n]*(?:\r?\n|$)|\z)'))
if ($shenLongSections.Count -ne 1) {
    throw "Source ShenLong.ini must contain exactly one [ShenLong] section; found $($shenLongSections.Count)."
}
$shenLongConfigVersions = @([regex]::Matches(
    $shenLongSections[0].Groups['body'].Value,
    '(?m)^[ \t]*ConfigVersion[ \t]*=[ \t]*(?<value>[^;#\r\n]*?)[ \t]*(?:[;#].*)?\r?$'))
if ($shenLongConfigVersions.Count -ne 1 -or
    $shenLongConfigVersions[0].Groups['value'].Value.Trim() -cne '1') {
    throw 'Source ShenLong.ini must contain exactly one [ShenLong] ConfigVersion=1 setting.'
}

$requiredLicenses = @(
    [pscustomobject]@{
        Name = 'Apache 2.0'
        RelativePath = 'DiligentFX-Apache-2.0.txt'
        Pattern = 'APPENDIX: How to apply the Apache License to your work\.'
        Sha256 = 'D5D84B9A0DF2A79A1E6208C4B0AECF7C520A93A074E582AAD22EE6893717CE53'
    },
    [pscustomobject]@{
        Name = 'MinHook BSD 2-Clause'
        RelativePath = 'MinHook-BSD-2-Clause.txt'
        Pattern = 'Copyright \(C\) 2009-2017 Tsuda Kageyu'
        Sha256 = 'DCB58D94398D7ECE7135E1C6FB4B11497E23708930A9A7107C0D7A81079D565F'
    },
    [pscustomobject]@{
        Name = 'three.js MIT'
        RelativePath = 'ThreeJS-MIT.txt'
        Pattern = 'Copyright \(c\) 2010-2026 three\.js authors'
        Sha256 = '7F05FC2885CD764E3A2FBE9CD64B6E99A423D284C718E5F909A4EE83C89433F3'
    },
    [pscustomobject]@{
        Name = 'XeGTAO MIT'
        RelativePath = 'XeGTAO-MIT.txt'
        Pattern = 'Copyright \(C\) 2016-2021, Intel Corporation'
        Sha256 = 'A6D7F9AB93DDF633DD9542FE8CB19428819065245B3CCCEAD7470FE13B1CEF8A'
    }
)
$licenseRoot = Join-Path $scriptRoot 'licenses'
$requiredLicenseRelativePaths = @(
    $requiredLicenses | ForEach-Object { $_.RelativePath })
if ($requiredLicenseRelativePaths.Count -ne 4) {
    throw "Graphics license whitelist drifted: expected exactly four entries, found $($requiredLicenseRelativePaths.Count)."
}
Assert-ExactLicenseFileSet -Path $licenseRoot `
    -ExpectedNames $requiredLicenseRelativePaths `
    -Description 'Source graphics license directory'
foreach ($license in $requiredLicenses) {
    $licensePath = Join-Path $licenseRoot $license.RelativePath
    if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
        throw "Required $($license.Name) license is missing: $licensePath"
    }
    if (-not ([IO.File]::ReadAllText($licensePath) -match $license.Pattern)) {
        throw "Required $($license.Name) license is incomplete or unexpected: $licensePath"
    }
    $licenseHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $licensePath).Hash
    if ($licenseHash -cne $license.Sha256) {
        throw "Required $($license.Name) license text drifted. Expected $($license.Sha256), found $licenseHash."
    }
}

$noticePath = Join-Path $scriptRoot 'THIRD_PARTY_NOTICES.md'
$noticeText = [IO.File]::ReadAllText($noticePath)
$expectedNoticeSha256 = '6484F29520FA7171EA45209991E62AD2F2A98571FA2102847F2A6448EE77CA2A'
$noticeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $noticePath).Hash
if ($noticeHash -cne $expectedNoticeSha256) {
    throw "Third-party notices drifted. Expected $expectedNoticeSha256, found $noticeHash."
}
foreach ($requiredNotice in @(
        $resolvedMinHook.MinHookArtifactCommit,
        'Copyright 2023 The Android Open Source Project.',
        '## Opaque Cook-Torrance GGX lighting',
        '## Stochastic-Depth Ambient Occlusion',
        'https://doi.org/10.1145/3451268',
        'per-layer selection probability of 0.2',
        'alpha-discard behavior')) {
    if ($noticeText.IndexOf($requiredNotice, [StringComparison]::Ordinal) -lt 0) {
        throw "Third-party notices are missing required text: $requiredNotice"
    }
}

# Only the official ReShade API headers are a build dependency. Keeping them in
# their own pinned checkout prevents accidental linkage to a general framework.
if (-not (Test-Path -LiteralPath (Join-Path $ReShadeRoot '.git'))) {
    if ($OfflineDependencies) {
        throw "OfflineDependencies requires the pinned local ReShade checkout at $ReShadeRoot."
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReShadeRoot) | Out-Null
    & git clone --filter=blob:none --no-checkout https://github.com/crosire/reshade.git $ReShadeRoot
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone ReShade.' }
    & git -C $ReShadeRoot fetch --depth 1 origin $pinnedReShadeCommit
    if ($LASTEXITCODE -ne 0) { throw 'Failed to fetch the pinned ReShade revision.' }
    & git -C $ReShadeRoot checkout --detach $pinnedReShadeCommit
    if ($LASTEXITCODE -ne 0) { throw 'Failed to check out the pinned ReShade revision.' }
}

$actualCommit = (& git -C $ReShadeRoot rev-parse HEAD).Trim()
if ($actualCommit -ne $pinnedReShadeCommit) {
    throw "ReShade revision mismatch. Expected $pinnedReShadeCommit, found $actualCommit."
}
$dirtyFiles = @(& git -C $ReShadeRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $dirtyFiles.Count -ne 0) {
    throw "The pinned ReShade checkout is dirty: $($dirtyFiles -join ', ')"
}
$apiHeader = Join-Path $ReShadeRoot 'include\reshade.hpp'
if (-not (Test-Path -LiteralPath $apiHeader) -or
    -not (Test-Path -LiteralPath (Join-Path $ReShadeRoot 'examples\utils\crc32_hash.hpp'))) {
    throw 'Pinned ReShade API headers are missing.'
}
$apiVersionFound = Select-String -LiteralPath $apiHeader -Pattern '^#define RESHADE_API_VERSION 18$' -Quiet
if (-not $apiVersionFound) {
    throw 'The pinned headers do not expose the reviewed ReShade API version 18.'
}

$reShadeSetupDirectory = Split-Path -Parent $ReShadeSetupPath
if (-not (Test-Path -LiteralPath $ReShadeSetupPath)) {
    if ($OfflineDependencies) {
        throw "OfflineDependencies requires the pinned local ReShade setup at $ReShadeSetupPath."
    }
    New-Item -ItemType Directory -Force -Path $reShadeSetupDirectory | Out-Null
    $downloadPath = "$ReShadeSetupPath.download"
    Invoke-WebRequest -Uri $reShadeSetupUrl -OutFile $downloadPath -UseBasicParsing
    $downloadHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $downloadPath).Hash
    if ($downloadHash -ne $reShadeSetupSha256) {
        Remove-Item -LiteralPath $downloadPath -Force
        throw "Unexpected ReShade setup hash: $downloadHash"
    }
    Move-Item -LiteralPath $downloadPath -Destination $ReShadeSetupPath
}

$setupHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ReShadeSetupPath).Hash
if ($setupHash -ne $reShadeSetupSha256) {
    throw "Unexpected ReShade setup hash: $setupHash"
}

$reShadeRuntimePath = Join-Path $reShadeSetupDirectory 'ReShade64.dll'
$runtimeValid = (Test-Path -LiteralPath $reShadeRuntimePath) -and
    ((Get-FileHash -Algorithm SHA256 -LiteralPath $reShadeRuntimePath).Hash -eq $reShadeRuntimeSha256)
if (-not $runtimeValid) {
    $sevenZipCandidates = @(
        (Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
        'C:\Program Files\7-Zip\7z.exe',
        'C:\Program Files (x86)\7-Zip\7z.exe'
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) }
    $sevenZip = $sevenZipCandidates | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($sevenZip)) {
        throw '7-Zip is required once to extract the pinned ReShade full-add-on runtime.'
    }
    & $sevenZip e $ReShadeSetupPath 'ReShade64.dll' "-o$reShadeSetupDirectory" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Failed to extract ReShade64.dll.' }
}

$runtimeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $reShadeRuntimePath).Hash
if ($runtimeHash -ne $reShadeRuntimeSha256) {
    throw "Unexpected ReShade full-add-on runtime hash: $runtimeHash"
}
$runtimeVersion = (Get-Item -LiteralPath $reShadeRuntimePath).VersionInfo.FileVersion
if ($runtimeVersion -ne $reShadeRuntimeFileVersion) {
    throw "Unexpected ReShade runtime version. Expected $reShadeRuntimeFileVersion, found $runtimeVersion."
}

$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'MSBuild was not found.' }
    $installRoot = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath).Trim()
    $msbuild = Join-Path $installRoot 'MSBuild\Current\Bin\MSBuild.exe'
}

$nativePolicyTestsProject = Join-Path $scriptRoot `
    'standalone\ShenLongNativePolicyTests.vcxproj'
$nativePolicyTestsExe = Join-Path $repoRoot `
    'build\shenlong-native-tests\ShenLongNativePolicyTests.exe'
$nativePolicyBuildOutput = @(& $msbuild $nativePolicyTestsProject /m /t:Rebuild `
    /p:Configuration=Release /p:Platform=x64 /v:minimal 2>&1)
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $nativePolicyTestsExe -PathType Leaf)) {
    throw "ShenLong native-policy test build failed: $($nativePolicyBuildOutput -join [Environment]::NewLine)"
}
$nativePolicyOutput = @(& $nativePolicyTestsExe 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "ShenLong native-policy tests failed: $($nativePolicyOutput -join [Environment]::NewLine)"
}
Write-Host ($nativePolicyOutput -join [Environment]::NewLine)

function Find-Fxc {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path -LiteralPath $kitsRoot)) {
        throw 'The Windows SDK shader compiler (fxc.exe) was not found.'
    }
    $versions = Get-ChildItem -LiteralPath $kitsRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($version in $versions) {
        $candidate = Join-Path $version.FullName 'x64\fxc.exe'
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw 'The x64 Windows SDK shader compiler (fxc.exe) was not found.'
}

$fxc = Find-Fxc

function Assert-CapturedShadowShaderContracts {
    param(
        [Parameter(Mandatory = $true)][string]$FxcPath,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ShadowSourcePath
    )

    # These are runtime-compiled shaders captured from sdhdship.exe. The
    # native hook can only patch an exact identity, so publishing must prove
    # that its C++ table still describes the reviewed captures byte-for-byte.
    $captures = @(
        [pscustomobject]@{ Hash = 'EE242C4F'; Size = 19596; Checksum = '7C4C7EADF9FF5978EBEB8DB1789774CA'; Override = 0; Sha256 = '7A68E5E944EADB76D61DDED513AB404D2FCB17F9DFD4D26C7F3F3B2C060EFBA1' },
        [pscustomobject]@{ Hash = '0B309D0E'; Size = 19596; Checksum = 'A893AA962B61A9614D58325F5FBCE242'; Override = 0; Sha256 = 'DC22390953F6FC5858F8EAE4598204AD513A57E3E5B3B849FAC7E9A4365E378A' },
        [pscustomobject]@{ Hash = '98F2BF47'; Size = 19940; Checksum = '18D5FB52990DCEAB368D0A2F2912D938'; Override = 0; Sha256 = '96600509C61661EB3462992E3020B54F8A2CDBCD3BC70408152D981C778B69BE' },
        [pscustomobject]@{ Hash = '72D70119'; Size = 20520; Checksum = 'ABE3B97E138C9B43EF31F3762B0E7C71'; Override = 2048; Sha256 = '74AE2D04E25031E7ED7696C812D80F3D8831B67BFEC1780C84A9D0F180813341' },
        [pscustomobject]@{ Hash = 'DD6C8356'; Size = 5448; Checksum = '242CB5DBFE75280BC84B1D2315B3A69E'; Override = 0; Sha256 = '61A9AAC0A22C166BA057433077749A9E88FDAA28DD3AA25FAED1097044E39F74' },
        [pscustomobject]@{ Hash = '5EBBA455'; Size = 6592; Checksum = '3F6D720A81352DC277618BB16E26F15C'; Override = 0; Sha256 = 'C8F70E6838AC9D32C43E572C5DDE3D8E51C72574DCFAE9AFF9C589FAB92EE2C7' },
        [pscustomobject]@{ Hash = '056F4AC7'; Size = 11284; Checksum = '1053945AA24C53C8674421E4F2361D9F'; Override = 0; Sha256 = 'B5027C09901802378BDB4B1D32F36A4DBE06B06180FF34E16BCFEAD044C9F6FD' },
        [pscustomobject]@{ Hash = '193BFE44'; Size = 5624; Checksum = 'F794B56A65072A249438A625E7E8FB0C'; Override = 0; Sha256 = 'F4576A9D301090FDE061937BDD272692CC3241EDF78BDEEA7C34334C36D73D30' },
        [pscustomobject]@{ Hash = '223AA776'; Size = 12572; Checksum = '7B47D72B48130DB3E617619BDBA0D176'; Override = 0; Sha256 = '7FC2BF2F2CDEDE09A62A578E439B8DDB4F60E6DBAF1A6DEFF71A826EBDAA53CB' },
        [pscustomobject]@{ Hash = 'B671C5AE'; Size = 11504; Checksum = 'B0D9D37E42A26461E662B582F9522A26'; Override = 0; Sha256 = '92AD46F8B33C5E7EA263C736DA43BBC582406AF081786EED8882AEE07D6F2357' },
        [pscustomobject]@{ Hash = 'DCF9CD0C'; Size = 12400; Checksum = 'F11B4D1EADCAEA7145CE4ED807F12B1B'; Override = 0; Sha256 = '555B509B371012FE9796F476CB376A353F004C3646215157D0276735B570C6AE' },
        [pscustomobject]@{ Hash = 'E5E2CE1C'; Size = 6768; Checksum = '6C0B404C4FF01734C94ADE40CFF632A5'; Override = 0; Sha256 = 'E46DD31048516A36C031AD47BA7BEEEA53F485D9F4B9FD7D1326CAE42C127698' }
    )
    if ($captures.Count -ne 12) {
        throw "Shadow shader capture coverage drifted: expected 12, found $($captures.Count)."
    }

    $shadowSource = [IO.File]::ReadAllText($ShadowSourcePath)
    $identityPattern = '(?s)\{0x(?<hash>[0-9A-F]{8}),\s*(?<size>\d+),\s*\{(?<checksum>(?:\s*0x[0-9A-F]{2},?){16})\}(?:,\s*(?<override>\d+))?\}'
    $sourceIdentities = [regex]::Matches($shadowSource, $identityPattern)
    if ($sourceIdentities.Count -ne $captures.Count) {
        throw "Shadow C++ identity-table coverage drifted: expected $($captures.Count), found $($sourceIdentities.Count)."
    }
    $sourceByHash = @{}
    foreach ($identity in $sourceIdentities) {
        $hash = $identity.Groups['hash'].Value
        if ($sourceByHash.ContainsKey($hash)) {
            throw "Shadow C++ identity table contains duplicate hash 0x$hash."
        }
        $checksumBytes = @(
            [regex]::Matches($identity.Groups['checksum'].Value, '0x(?<byte>[0-9A-F]{2})') |
                ForEach-Object { $_.Groups['byte'].Value })
        $overrideText = $identity.Groups['override'].Value
        $sourceByHash[$hash] = [pscustomobject]@{
            Size = [int]$identity.Groups['size'].Value
            Checksum = $checksumBytes -join ''
            Override = if ([string]::IsNullOrWhiteSpace($overrideText)) { 0 } else { [int]$overrideText }
        }
    }

    $captureRoot = Join-Path $RepositoryRoot 'artifacts\reverse-engineering\2026-08-06-shadow-shader-census'
    $constantBits = @(
        [Convert]::ToUInt32('3A000000', 16),
        [Convert]::ToUInt32('3AC00000', 16),
        [Convert]::ToUInt32('3B200000', 16),
        [Convert]::ToUInt32('3A51B717', 16),
        [Convert]::ToUInt32('BA000000', 16),
        [Convert]::ToUInt32('BAC00000', 16),
        [Convert]::ToUInt32('BB200000', 16),
        [Convert]::ToUInt32('45000000', 16))
    foreach ($capture in $captures) {
        $capturePath = Join-Path $captureRoot "ps-$($capture.Hash)-$($capture.Size).dxbc"
        if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf)) {
            throw "Captured shadow shader 0x$($capture.Hash) is missing: $capturePath"
        }
        $bytes = [IO.File]::ReadAllBytes($capturePath)
        if ($bytes.Length -ne $capture.Size -or
            [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -cne 'DXBC') {
            throw "Captured shadow shader 0x$($capture.Hash) has an invalid DXBC container or size."
        }
        $sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $capturePath).Hash
        if ($sha256 -cne $capture.Sha256) {
            throw "Captured shadow shader 0x$($capture.Hash) identity drifted. Expected $($capture.Sha256), found $sha256."
        }
        $headerChecksum = ([BitConverter]::ToString($bytes, 4, 16)).Replace('-', '')
        if ($headerChecksum -cne $capture.Checksum) {
            throw "Captured shadow shader 0x$($capture.Hash) DXBC checksum drifted. Expected $($capture.Checksum), found $headerChecksum."
        }
        if (-not $sourceByHash.ContainsKey($capture.Hash)) {
            throw "Captured shadow shader 0x$($capture.Hash) is absent from the C++ identity table."
        }
        $sourceIdentity = $sourceByHash[$capture.Hash]
        if ($sourceIdentity.Size -ne $capture.Size -or
            $sourceIdentity.Checksum -cne $capture.Checksum -or
            $sourceIdentity.Override -ne $capture.Override) {
            throw "Shadow C++ identity 0x$($capture.Hash) does not match its captured size, DXBC checksum, or map override."
        }

        $hasPatchableConstant = $false
        for ($offset = 0; $offset + 4 -le $bytes.Length; $offset += 4) {
            if ($constantBits -contains [BitConverter]::ToUInt32($bytes, $offset)) {
                $hasPatchableConstant = $true
                break
            }
        }
        if (-not $hasPatchableConstant) {
            throw "Captured shadow shader 0x$($capture.Hash) no longer contains a reviewed aligned shadow-filter constant."
        }

        $dumpLines = @(& $FxcPath /nologo /dumpbin $capturePath 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "FXC could not disassemble captured shadow shader 0x$($capture.Hash)."
        }
        $dumpText = $dumpLines -join "`n"
        if ($dumpText -notmatch '(?m)^//\s+texShadow(?:Atlas)?\s+texture\s+\S+\s+2d\s+t0\s+1\s*$') {
            throw "Captured shadow shader 0x$($capture.Hash) no longer reflects texShadow/texShadowAtlas as a 2D texture at t0."
        }
    }

    # 0x8507FE03 is the tempting 0.0008 false positive: its exact capture has
    # diffuse/normal/depth/ambient-mask bindings and no shadow texture. Keep
    # this negative proof so a broad constant scan cannot add it by mistake.
    $falsePositivePath = Join-Path $captureRoot 'ps-8507FE03-10296.dxbc'
    $falsePositiveSha256 = 'DFB09F4961DDF207B10922F41ADE70E3ED5D8C04A4DD8400C6C1F8AD61F6F347'
    if (-not (Test-Path -LiteralPath $falsePositivePath -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $falsePositivePath).Hash -cne $falsePositiveSha256) {
        throw 'The reviewed non-shadow 0x8507FE03 false-positive capture is missing or has drifted.'
    }
    if ($sourceByHash.ContainsKey('8507FE03')) {
        throw 'Non-shadow shader 0x8507FE03 must not appear in the shadow patch identity table.'
    }
    $falsePositiveDump = @(& $FxcPath /nologo /dumpbin $falsePositivePath 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw 'FXC could not disassemble the reviewed non-shadow 0x8507FE03 capture.'
    }
    if (($falsePositiveDump -join "`n") -match '(?m)^//\s+texShadow(?:Atlas)?\s+texture\s+') {
        throw 'The reviewed 0x8507FE03 negative control now reflects a shadow texture; re-audit the exclusion.'
    }

    Write-Host 'Validated 12 exact captured shadow-shader identities and the 0x8507FE03 non-shadow negative control against SHA-256, DXBC checksum, reflected t0 binding, patchable constants, and the C++ identity table.'
}

Assert-CapturedShadowShaderContracts -FxcPath $fxc -RepositoryRoot $repoRoot `
    -ShadowSourcePath (Join-Path $scriptRoot 'standalone\SPatchShadowScale.cpp')

function Get-CheckedChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd([char[]]'\/')
    $parentPrefix = $fullParent + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to use $Description outside its checked parent: $fullPath"
    }
    return $fullPath
}

function Assert-NoReparseTree {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($Path)
    while ($pending.Count -ne 0) {
        $current = $pending.Dequeue()
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description contains a reparse point: $current"
        }
        if (-not $item.PSIsContainer) {
            continue
        }
        foreach ($child in Get-ChildItem -LiteralPath $current -Force) {
            if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $($child.FullName)"
            }
            if ($child.PSIsContainer) {
                $pending.Enqueue($child.FullName)
            }
        }
    }
}

function Remove-CheckedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $checkedPath = Get-CheckedChildPath -Path $Path -Parent $Parent -Description $Description
    if (Test-Path -LiteralPath $checkedPath) {
        Assert-NoReparseTree -Path $checkedPath -Description $Description
        if (-not (Test-Path -LiteralPath $checkedPath -PathType Container)) {
            throw "$Description is not a directory: $checkedPath"
        }
        Remove-Item -LiteralPath $checkedPath -Recurse -Force
    }
}

function Move-CheckedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $checkedSource = Get-CheckedChildPath -Path $Source -Parent $Parent `
        -Description "$Description source"
    $checkedDestination = Get-CheckedChildPath -Path $Destination -Parent $Parent `
        -Description "$Description destination"
    if (-not (Test-Path -LiteralPath $checkedSource -PathType Container)) {
        throw "$Description source directory is missing: $checkedSource"
    }
    Assert-NoReparseTree -Path $checkedSource -Description "$Description source"
    if (Test-Path -LiteralPath $checkedDestination) {
        throw "$Description destination already exists: $checkedDestination"
    }
    Move-Item -LiteralPath $checkedSource -Destination $checkedDestination
}

function Publish-ValidatedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$StagingPath,
        [Parameter(Mandatory = $true)][string]$ActivePath,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $checkedStaging = Get-CheckedChildPath -Path $StagingPath -Parent $Parent `
        -Description 'validated graphics staging directory'
    $checkedActive = Get-CheckedChildPath -Path $ActivePath -Parent $Parent `
        -Description 'active graphics package'
    $backupName = ".previous-$([IO.Path]::GetFileName($checkedActive))-$PID-$([guid]::NewGuid().ToString('N'))"
    $checkedBackup = Get-CheckedChildPath -Path (Join-Path $Parent $backupName) `
        -Parent $Parent -Description 'graphics package rollback directory'
    $hadActivePackage = Test-Path -LiteralPath $checkedActive -PathType Container

    if ((Test-Path -LiteralPath $checkedActive) -and -not $hadActivePackage) {
        throw "The active graphics package path is not a directory: $checkedActive"
    }

    if ($hadActivePackage) {
        Move-CheckedDirectory -Source $checkedActive -Destination $checkedBackup `
            -Parent $Parent -Description 'graphics package backup'
    }

    try {
        Move-CheckedDirectory -Source $checkedStaging -Destination $checkedActive `
            -Parent $Parent -Description 'validated graphics package promotion'
    } catch {
        $promotionError = $_
        if ($hadActivePackage -and
            -not (Test-Path -LiteralPath $checkedActive) -and
            (Test-Path -LiteralPath $checkedBackup -PathType Container)) {
            Move-CheckedDirectory -Source $checkedBackup -Destination $checkedActive `
                -Parent $Parent -Description 'graphics package rollback'
        }
        throw $promotionError
    }

    if ($hadActivePackage -and (Test-Path -LiteralPath $checkedBackup)) {
        try {
            Remove-CheckedDirectory -Path $checkedBackup -Parent $Parent `
                -Description 'superseded graphics package backup'
        } catch {
            Write-Warning "The validated graphics package is active, but its rollback directory could not be removed: $checkedBackup ($($_.Exception.Message))"
        }
    }
}

function Publish-DeterministicZip {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    Add-Type -AssemblyName System.IO.Compression
    $checkedSource = Get-CheckedChildPath -Path $SourceDirectory -Parent $Parent `
        -Description 'ShenLong archive source'
    if (-not (Test-Path -LiteralPath $checkedSource -PathType Container)) {
        throw "ShenLong archive source is missing: $checkedSource"
    }
    Assert-NoReparseTree -Path $checkedSource `
        -Description 'ShenLong archive source'
    $checkedDestination = Get-CheckedChildPath -Path $DestinationPath `
        -Parent $Parent -Description 'ShenLong archive'
    $temporaryName = '.ShenLong-archive-' + $PID + '-' +
        [Guid]::NewGuid().ToString('N') + '.zip'
    $temporaryPath = Get-CheckedChildPath `
        -Path (Join-Path $Parent $temporaryName) -Parent $Parent `
        -Description 'ShenLong archive staging file'
    $backupPath = $null
    try {
        $relativeFiles = @(
            Get-ChildItem -LiteralPath $checkedSource -File -Force -Recurse |
                ForEach-Object {
                    $_.FullName.Substring($checkedSource.Length + 1)
                })
        [Array]::Sort($relativeFiles, [StringComparer]::Ordinal)
        $archiveRoots = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($relativePath in $relativeFiles) {
            $separator = $relativePath.IndexOf(
                [IO.Path]::DirectorySeparatorChar)
            if ($separator -le 0) {
                throw "ShenLong archive contains a file outside its package envelope: $relativePath"
            }
            [void]$archiveRoots.Add($relativePath.Substring(0, $separator))
        }
        if ($archiveRoots.Count -ne 1 -or
            -not $archiveRoots.Contains('ShenLong-Package')) {
            throw 'ShenLong archive must contain exactly one top-level ShenLong-Package directory.'
        }
        $stream = [IO.File]::Open(
            $temporaryPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try {
            $archive = [IO.Compression.ZipArchive]::new(
                $stream, [IO.Compression.ZipArchiveMode]::Create, $true)
            try {
                $fixedTimestamp = [DateTimeOffset]::new(
                    2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                foreach ($relativePath in $relativeFiles) {
                    $entryName = $relativePath.Replace('\', '/')
                    $entry = $archive.CreateEntry(
                        $entryName, [IO.Compression.CompressionLevel]::Optimal)
                    $entry.LastWriteTime = $fixedTimestamp
                    $sourcePath = Join-Path $checkedSource $relativePath
                    $sourceStream = [IO.File]::OpenRead($sourcePath)
                    $entryStream = $entry.Open()
                    try {
                        $sourceStream.CopyTo($entryStream)
                    } finally {
                        $entryStream.Dispose()
                        $sourceStream.Dispose()
                    }
                }
            } finally {
                $archive.Dispose()
            }
        } finally {
            $stream.Dispose()
        }

        if (Test-Path -LiteralPath $checkedDestination) {
            if (-not (Test-Path -LiteralPath $checkedDestination -PathType Leaf)) {
                throw "ShenLong archive destination is not a file: $checkedDestination"
            }
            $destinationItem = Get-Item -LiteralPath $checkedDestination -Force
            if (($destinationItem.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "ShenLong archive destination is a reparse point: $checkedDestination"
            }
            $backupName = '.ShenLong-archive-previous-' + $PID + '-' +
                [Guid]::NewGuid().ToString('N') + '.zip'
            $backupPath = Get-CheckedChildPath `
                -Path (Join-Path $Parent $backupName) -Parent $Parent `
                -Description 'previous ShenLong archive'
            Move-Item -LiteralPath $checkedDestination -Destination $backupPath
        }
        try {
            Move-Item -LiteralPath $temporaryPath -Destination $checkedDestination
        } catch {
            if ($null -ne $backupPath -and
                (Test-Path -LiteralPath $backupPath -PathType Leaf) -and
                -not (Test-Path -LiteralPath $checkedDestination)) {
                Move-Item -LiteralPath $backupPath -Destination $checkedDestination
            }
            throw
        }
        if ($null -ne $backupPath -and
            (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
            Remove-Item -LiteralPath $backupPath -Force
        }
        return [pscustomobject]@{
            Path = $checkedDestination
            Sha256 = (Get-FileHash -LiteralPath $checkedDestination `
                -Algorithm SHA256).Hash
            Entries = $relativeFiles.Count
        }
    } finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Invoke-FxcVariant {
    param(
        [Parameter(Mandatory = $true)][string]$Shader,
        [Parameter(Mandatory = $true)][string]$EntryPoint,
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][hashtable]$Defines,
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [switch]$IeeeStrictness
    )
    $arguments = @(
        '/nologo', '/WX', '/Ges', '/O3',
        '/T', $Target,
        '/E', $EntryPoint)
    if ($IeeeStrictness) {
        $arguments += '/Gis'
    }
    foreach ($name in @($Defines.Keys | Sort-Object)) {
        $arguments += @('/D', "$name=$($Defines[$name])")
    }
    $arguments += @('/Fo', $OutputPath, $Shader)
    & $fxc @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "FXC failed for $([IO.Path]::GetFileName($Shader)) entry=$EntryPoint target=$Target defines=$($Defines | Out-String)"
    }
}

function Invoke-RuntimeShaderVariant {
    param(
        [Parameter(Mandatory = $true)][string]$Shader,
        [Parameter(Mandatory = $true)][string]$EntryPoint,
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][hashtable]$Defines,
        [Parameter(Mandatory = $true)][string]$ValidationOutputPath,
        [Parameter(Mandatory = $true)][string]$CacheRelativePath
    )

    $normalizedCachePath = $CacheRelativePath.Replace('\', '/')
    if ($normalizedCachePath -notmatch '^(?:PBR|GI|SDAO|SSS|Water)/[A-Za-z0-9_.-]+\.cso$' -or
        $normalizedCachePath.Contains('..')) {
        throw "Invalid runtime shader cache path: $CacheRelativePath"
    }

    Invoke-FxcVariant -Shader $Shader -EntryPoint $EntryPoint -Target $Target `
        -Defines $Defines -OutputPath $ValidationOutputPath
    if (-not (Test-Path -LiteralPath $ValidationOutputPath -PathType Leaf)) {
        throw "FXC did not produce runtime shader bytecode: $ValidationOutputPath"
    }

    $defineContract = @($Defines.Keys | Sort-Object | ForEach-Object {
            "$_=$($Defines[$_])"
        }) -join ';'
    if ([string]::IsNullOrEmpty($defineContract)) {
        $defineContract = '<none>'
    }
    $validationFile = Get-Item -LiteralPath $ValidationOutputPath
    return [pscustomobject]@{
        CacheRelativePath = $normalizedCachePath
        Source = [IO.Path]::GetFileName($Shader)
        EntryPoint = $EntryPoint
        Profile = $Target
        Defines = $defineContract
        ValidationOutputPath = $validationFile.FullName
        Length = [long]$validationFile.Length
        Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $validationFile.FullName).Hash
    }
}

function Get-Crc32 {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    [uint64]$crc = [uint32]::MaxValue
    foreach ($value in $Bytes) {
        $crc = $crc -bxor [uint64]$value
        for ($bit = 0; $bit -lt 8; $bit += 1) {
            if (($crc -band [uint64]1) -ne [uint64]0) {
                $crc = (($crc -shr 1) -bxor [uint64]3988292384) -band 4294967295L
            } else {
                $crc = ($crc -shr 1) -band 4294967295L
            }
        }
    }
    return [uint32](($crc -bxor [uint64][uint32]::MaxValue) -band 4294967295L)
}

function Assert-WaterCacheIdentity {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($Bytes.Length -lt 20 -or
        [Text.Encoding]::ASCII.GetString($Bytes, 0, 4) -cne 'DXBC') {
        throw "$Description is not valid DXBC bytecode."
    }
    if ($Bytes.Length -ne $Expected.Size) {
        throw "$Description size drifted: expected $($Expected.Size), found $($Bytes.Length)."
    }
    $actualChecksum = ([BitConverter]::ToString($Bytes, 4, 16)).Replace('-', '')
    if ($actualChecksum -cne $Expected.Checksum) {
        throw "$Description DXBC checksum drifted: expected $($Expected.Checksum), found $actualChecksum."
    }
    $actualCrc32 = '{0:X8}' -f (Get-Crc32 -Bytes $Bytes)
    if ($actualCrc32 -cne $Expected.Crc32) {
        throw "$Description CRC32 drifted: expected $($Expected.Crc32), found $actualCrc32."
    }
}

function Get-WaterCacheIdentityTable {
    param([Parameter(Mandatory = $true)][string]$SourcePath)

    $sourceText = [IO.File]::ReadAllText($SourcePath)
    $byteSequence = '(?:\s*0x[0-9A-F]{2},?){16}'
    $pattern = '(?s)\{\s*WaterVariant::(?<variant>main|simple|blend),\s*' +
        '0x[0-9A-F]{8},\s*\d+,\s*\{' + $byteSequence + '\},\s*' +
        '0x(?<crc>[0-9A-F]{8}),\s*(?<size>\d+),\s*\{(?<checksum>' +
        $byteSequence + ')\},\s*L"(?<cache>Water(?:Main|Simple|Blend)\.ps_4_0\.cso)"'
    $matches = [regex]::Matches($sourceText, $pattern)
    if ($matches.Count -ne 3) {
        throw "Water C++ cache identity-table coverage drifted: expected 3 entries, found $($matches.Count)."
    }

    $identities = @{}
    foreach ($match in $matches) {
        $cacheName = $match.Groups['cache'].Value
        if ($identities.ContainsKey($cacheName)) {
            throw "Water C++ cache identity table contains duplicate entry $cacheName."
        }
        $identities[$cacheName] = [pscustomobject]@{
            Crc32 = $match.Groups['crc'].Value
            Size = [int]$match.Groups['size'].Value
            Checksum = @(
                [regex]::Matches(
                    $match.Groups['checksum'].Value,
                    '0x(?<byte>[0-9A-F]{2})') |
                    ForEach-Object { $_.Groups['byte'].Value }) -join ''
        }
    }
    return $identities
}

function Assert-SdaoCaptureDonorBytecode {
    param([Parameter(Mandatory = $true)][string]$BytecodePath)

    $dumpLines = @(& $fxc /nologo /dumpbin $BytecodePath 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "FXC reflection dump failed for the SDAO capture donor: $BytecodePath"
    }
    $dump = ($dumpLines | ForEach-Object { [string]$_ }) -join "`n"
    foreach ($required in @(
            '(?m)^ps_5_0\s*$',
            '(?m)^dcl_globalFlags refactoringAllowed\s*$',
            '(?m)^dcl_input_ps_siv linear noperspective v0\.xyz, position\s*$',
            '(?m)^dcl_output o0\.xy\s*$',
            '(?m)^dcl_output o1\.xy\s*$',
            '(?m)^discard_nz\s+',
            '(?m)^mov o0\.[xy]+,',
            '(?m)^mov o1\.[xy]+,',
            '(?m)^ret\s*$')) {
        if ($dump -notmatch $required) {
            throw "SDAO capture donor is missing required DXBC contract '$required': $BytecodePath"
        }
    }
    $target0Writes = [regex]::Matches(
        $dump, '(?m)^mov o0\.(?<mask>[xy]+),')
    $target1Writes = [regex]::Matches(
        $dump, '(?m)^mov o1\.(?<mask>[xy]+),')
    $target0Mask = ($target0Writes | ForEach-Object {
            $_.Groups['mask'].Value
        }) -join ''
    $target1Mask = ($target1Writes | ForEach-Object {
            $_.Groups['mask'].Value
        }) -join ''
    if ([regex]::Matches($dump, '(?m)^dcl_[^\r\n]*$').Count -ne 5 -or
        [regex]::Matches($dump, '(?m)^dcl_output o[0-9]+\.[xyzw]+\s*$').Count -ne 2 -or
        [regex]::Matches($dump, '(?m)^dcl_input[^\r\n]*$').Count -ne 1 -or
        [regex]::Matches($dump, '(?m)^dcl_temps\s+[0-9]+\s*$').Count -ne 1 -or
        [regex]::Matches($dump, '(?m)^dcl_output o0\.xy\s*$').Count -ne 1 -or
        [regex]::Matches($dump, '(?m)^dcl_output o1\.xy\s*$').Count -ne 1 -or
        [regex]::Matches($dump, '(?m)^discard_(?:nz|z)\s+').Count -ne 1 -or
        $target0Writes.Count -lt 1 -or $target0Writes.Count -gt 2 -or
        $target1Writes.Count -lt 1 -or $target1Writes.Count -gt 2 -or
        -not $target0Mask.Contains('x') -or -not $target0Mask.Contains('y') -or
        -not $target1Mask.Contains('x') -or -not $target1Mask.Contains('y') -or
        [regex]::Matches($dump, '(?m)^mov o[0-9]+\.[xyzw]+,').Count -ne
            ($target0Writes.Count + $target1Writes.Count) -or
        [regex]::Matches($dump, '(?m)^ret\s*$').Count -ne 1) {
        throw "SDAO capture donor must contain exactly two float2 target declarations, one selection discard, complete xy writes to only o0/o1, and one final ret: $BytecodePath"
    }
    $captureTail = [regex]::Match(
        $dump,
        '(?ms)^discard_nz\s+.*?^mov o0\.[xy]+,.*?^mov o1\.[xy]+,.*?^ret\s*$')
    if (-not $captureTail.Success) {
        throw "SDAO capture donor must discard unselected fragments before writing both MIN-blend targets and returning: $BytecodePath"
    }
    foreach ($forbidden in @(
            '(?m)^(?:if_nz|if_z|else|endif|loop|endloop|switch|case|default|endswitch|call|callc)\b',
            '(?m)^(?:discard_z|retc|customdata)\b',
            '(?m)^dcl_(?:constantbuffer\b|resource(?:_|\b)|sampler\b|uav(?:_|\b)|output_(?:sgv|siv)\b)',
            '(?m)^[ \t]*atomic_',
            '(?m)^(?!mov\s)(?!dcl_output\b)[a-z][a-z0-9_]*\s+o[0-9]+\.',
            '(?m)^(?:store_uav|ld_uav)')) {
        if ($dump -match $forbidden) {
            throw "SDAO capture donor contains forbidden DXBC shape '$forbidden': $BytecodePath"
        }
    }
    if ($dump -notmatch '(?m)^// SV_Position\s+0\s+xyzw\s+0\s+POS\s+float\s+xyz\s*$' -or
        $dump -notmatch '(?m)^// SV_Target\s+0\s+xy\s+0\s+TARGET\s+float\s+xy\s*$' -or
        $dump -notmatch '(?m)^// SV_Target\s+1\s+xy\s+1\s+TARGET\s+float\s+xy\s*$') {
        throw "SDAO capture donor reflection must expose only SV_Position.xyz input and float2 SV_Target0/SV_Target1 outputs: $BytecodePath"
    }
}

function Assert-ShaderCacheFeatureCounts {
    param(
        [Parameter(Mandatory = $true)][object[]]$Variants,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedCounts,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $matchedCount = 0
    foreach ($feature in $ExpectedCounts.Keys) {
        $prefix = "$feature/"
        $actualCount = @($Variants | Where-Object {
                $_.CacheRelativePath.StartsWith(
                    $prefix, [StringComparison]::Ordinal)
            }).Count
        if ($actualCount -ne $ExpectedCounts[$feature]) {
            throw "$Description $feature coverage drifted: expected $($ExpectedCounts[$feature]) variants, found $actualCount."
        }
        $matchedCount += $actualCount
    }
    if ($matchedCount -ne $Variants.Count) {
        throw "$Description contains $($Variants.Count - $matchedCount) variants outside the reviewed PBR/GI/SDAO/SSS/Water feature roots."
    }
}

function Assert-AgxReflectedBindings {
    param([Parameter(Mandatory = $true)][string]$BytecodePath)

    $dumpLines = @(& $fxc /nologo /dumpbin $BytecodePath 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "FXC reflection dump failed for AgX validation bytecode: $BytecodePath"
    }

    $inResourceTable = $false
    $sawResourceRow = $false
    $reflectedBindings = [ordered]@{}
    foreach ($lineObject in $dumpLines) {
        $line = [string]$lineObject
        $body = [regex]::Replace($line, '^\s*//\s?', '').Trim()
        if (-not $inResourceTable) {
            if ($body -ceq 'Resource Bindings:') {
                $inResourceTable = $true
            }
            continue
        }

        if ([string]::IsNullOrWhiteSpace($body)) {
            if ($sawResourceRow) { break }
            continue
        }
        if ($body -match '^Name\s+Type\s+' -or $body -match '^-{3,}') {
            continue
        }

        $columns = @($body -split '\s+')
        if ($columns.Count -lt 6) {
            throw "Could not parse the AgX FXC resource-binding row: $line"
        }
        $name = $columns[0]
        $binding = $columns[$columns.Count - 2]
        $count = $columns[$columns.Count - 1]
        if ($binding -notmatch '^(?:cb|[bstu])\d+$' -or $count -notmatch '^\d+$') {
            throw "Could not parse the AgX FXC resource-binding row: $line"
        }
        if ([int]$count -ne 1) {
            throw "AgX resource $name unexpectedly reflected as an array of $count bindings."
        }
        if ($binding.StartsWith('cb', [StringComparison]::Ordinal)) {
            $binding = 'b' + $binding.Substring(2)
        }
        if ($reflectedBindings.Contains($name)) {
            throw "AgX reflection contains duplicate resource name: $name"
        }
        $reflectedBindings.Add($name, $binding)
        $sawResourceRow = $true
    }

    $expectedBindings = [ordered]@{
        '_texDiffuse_s' = 's0'
        '_texHDRBloom_s' = 's1'
        'texDiffuse' = 't0'
        'texHDRBloom' = 't1'
        'cbShaderParams' = 'b0'
    }
    if (-not $inResourceTable -or -not $sawResourceRow -or
        $reflectedBindings.Count -ne $expectedBindings.Count) {
        throw "AgX reflection must contain exactly b0, s0, s1, t0, and t1: $BytecodePath"
    }
    foreach ($name in $expectedBindings.Keys) {
        if (-not $reflectedBindings.Contains($name) -or
            $reflectedBindings[$name] -cne $expectedBindings[$name]) {
            $actual = if ($reflectedBindings.Contains($name)) {
                $reflectedBindings[$name]
            } else {
                '<missing>'
            }
            throw "AgX reflected binding drifted for $name. Expected $($expectedBindings[$name]), found $actual."
        }
    }
}

function Get-FxcAbiSection {
    param(
        [Parameter(Mandatory = $true)][string[]]$DumpLines,
        [Parameter(Mandatory = $true)][string]$SectionName,
        [switch]$BufferDefinitions
    )

    $sectionStart = -1
    for ($index = 0; $index -lt $DumpLines.Count; ++$index) {
        $body = [regex]::Replace(
            [string]$DumpLines[$index], '^\s*//\s?', '').Trim()
        if ($body -ceq "${SectionName}:") {
            $sectionStart = $index
            break
        }
    }
    if ($sectionStart -lt 0) {
        throw "FXC reflection is missing the $SectionName section."
    }

    $rows = [Collections.Generic.List[string]]::new()
    for ($index = $sectionStart + 1; $index -lt $DumpLines.Count; ++$index) {
        $line = [string]$DumpLines[$index]
        if ($line -notmatch '^\s*//') {
            if ($line.Trim() -match '^(?:ps|vs|cs|gs|hs|ds)_\d_\d$') {
                break
            }
            continue
        }

        $body = [regex]::Replace($line, '^\s*//\s?', '').Trim()
        if ($body -match '^(?:Buffer Definitions|Resource Bindings|Input signature|Output signature):$') {
            break
        }

        if ($BufferDefinitions) {
            if ($body -notmatch '^cbuffer\s+\S+' -and
                $body -notmatch '\bOffset:\s*\d+') {
                continue
            }
        } elseif ([string]::IsNullOrWhiteSpace($body) -or
            $body -match '^Name\s+' -or $body -match '^-{3,}') {
            continue
        }

        $normalized = [regex]::Replace($body, '\s+', ' ')
        $normalized = [regex]::Replace($normalized, '\s*;\s*//', '; //')
        $rows.Add($normalized)
    }
    if ($rows.Count -eq 0) {
        throw "FXC reflection contains no rows in the $SectionName section."
    }
    return [string[]]$rows.ToArray()
}

function Get-FxcAbiContract {
    param(
        [Parameter(Mandatory = $true)][string]$BytecodePath,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $dumpLines = @(& $fxc /nologo /dumpbin $BytecodePath 2>&1 |
        ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne 0) {
        throw "FXC reflection dump failed for ${Description}: $BytecodePath"
    }
    $profiles = @($dumpLines | Where-Object {
            $_.Trim() -match '^(?:ps|vs|cs|gs|hs|ds)_\d_\d$'
        } | ForEach-Object { $_.Trim() })
    if ($profiles.Count -ne 1) {
        throw "$Description must contain exactly one reflected shader profile: $BytecodePath"
    }

    return [pscustomobject]@{
        Profile = $profiles[0]
        ConstantBuffers = @(Get-FxcAbiSection -DumpLines $dumpLines `
            -SectionName 'Buffer Definitions' -BufferDefinitions)
        ResourceBindings = @(Get-FxcAbiSection -DumpLines $dumpLines `
            -SectionName 'Resource Bindings')
        InputSignature = @(Get-FxcAbiSection -DumpLines $dumpLines `
            -SectionName 'Input signature')
        OutputSignature = @(Get-FxcAbiSection -DumpLines $dumpLines `
            -SectionName 'Output signature')
    }
}

function Assert-ExactFxcAbiSequence {
    param(
        [Parameter(Mandatory = $true)][string[]]$Expected,
        [Parameter(Mandatory = $true)][string[]]$Actual,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($Expected.Count -ne $Actual.Count) {
        throw "$Description row count drifted: expected $($Expected.Count), found $($Actual.Count)."
    }
    for ($index = 0; $index -lt $Expected.Count; ++$index) {
        if ($Expected[$index] -cne $Actual[$index]) {
            throw "$Description drifted at row ${index}: expected '$($Expected[$index])', found '$($Actual[$index])'."
        }
    }
}

function Assert-PbrNativeAbi {
    param(
        [Parameter(Mandatory = $true)][string]$CompiledBytecodePath,
        [Parameter(Mandatory = $true)][string]$NativeBytecodePath,
        [Parameter(Mandatory = $true)][int]$Variant,
        [Parameter(Mandatory = $true)][string]$NativeShaderHash,
        [Parameter(Mandatory = $true)][string]$ExpectedNativeSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedReplacementSha256
    )

    if (-not (Test-Path -LiteralPath $NativeBytecodePath -PathType Leaf)) {
        throw "PBR variant $Variant native shader 0x$NativeShaderHash is missing: $NativeBytecodePath"
    }
    $nativeSha256 =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $NativeBytecodePath).Hash
    if ($nativeSha256 -cne $ExpectedNativeSha256) {
        throw "PBR variant $Variant native shader 0x$NativeShaderHash identity drifted. Expected $ExpectedNativeSha256, found $nativeSha256."
    }
    $replacementSha256 =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $CompiledBytecodePath).Hash
    if ($replacementSha256 -cne $ExpectedReplacementSha256) {
        throw "PBR variant $Variant replacement identity drifted. Expected $ExpectedReplacementSha256, found $replacementSha256."
    }

    $native = Get-FxcAbiContract -BytecodePath $NativeBytecodePath `
        -Description "PBR variant $Variant native shader 0x$NativeShaderHash"
    $compiled = Get-FxcAbiContract -BytecodePath $CompiledBytecodePath `
        -Description "PBR variant $Variant compiled replacement"
    if ($native.Profile -cne 'ps_4_0' -or $compiled.Profile -cne $native.Profile) {
        throw "PBR variant $Variant profile drifted: native=$($native.Profile), compiled=$($compiled.Profile), required=ps_4_0."
    }

    $nativeTargetCount = @($native.OutputSignature | Where-Object {
            $_ -match '^SV_Target\s+\d+\s+'
        }).Count
    $compiledTargetCount = @($compiled.OutputSignature | Where-Object {
            $_ -match '^SV_Target\s+\d+\s+'
        }).Count
    if ($nativeTargetCount -lt 1 -or $compiledTargetCount -ne $nativeTargetCount) {
        throw "PBR variant $Variant output-target count drifted: native=$nativeTargetCount, compiled=$compiledTargetCount."
    }

    foreach ($section in @(
            'ConstantBuffers',
            'ResourceBindings',
            'InputSignature',
            'OutputSignature')) {
        Assert-ExactFxcAbiSequence -Expected @($native.$section) `
            -Actual @($compiled.$section) `
            -Description "PBR variant $Variant $section ABI"
    }
}

function Assert-PbrDerivativeUniformity {
    param(
        [Parameter(Mandatory = $true)][string]$BytecodePath,
        [Parameter(Mandatory = $true)][int]$Variant,
        [int]$ExpectedDerivativeCount = 2
    )

    $dumpLines = @(& $fxc /nologo /dumpbin $BytecodePath 2>&1 |
        ForEach-Object { ([string]$_).Trim() })
    if ($LASTEXITCODE -ne 0) {
        throw "FXC disassembly failed for PBR derivative-uniformity variant $Variant."
    }

    $dynamicIfDepth = 0
    $derivativeCount = 0
    foreach ($line in $dumpLines) {
        if ($line -match '^if_(?:nz|z)\b') {
            ++$dynamicIfDepth
            continue
        }
        if ($line -match '^endif\b') {
            --$dynamicIfDepth
            if ($dynamicIfDepth -lt 0) {
                throw "PBR variant $Variant has malformed dynamic-flow disassembly."
            }
            continue
        }
        if ($line -match '^deriv_rt[xy]\b') {
            ++$derivativeCount
            if ($dynamicIfDepth -ne 0) {
                throw "PBR variant $Variant compiled a normal derivative inside divergent control flow."
            }
        }
    }
    if ($dynamicIfDepth -ne 0 -or
        $derivativeCount -ne $ExpectedDerivativeCount) {
        throw "PBR variant $Variant derivative contract drifted: dynamic-depth=$dynamicIfDepth derivatives=$derivativeCount expected=$ExpectedDerivativeCount."
    }
}

function Get-SourceContractSlice {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$StartMarker,
        [Parameter(Mandatory = $true)][string]$EndMarker,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $start = $Text.IndexOf($StartMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) {
        throw "$Description start marker is missing: $StartMarker"
    }
    $end = $Text.IndexOf(
        $EndMarker, $start + $StartMarker.Length, [StringComparison]::Ordinal)
    if ($end -lt 0 -or $end -le $start) {
        throw "$Description end marker is missing or out of order: $EndMarker"
    }
    return $Text.Substring($start, $end - $start)
}

function Assert-OrderedSourceContract {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string[]]$RequiredLiterals,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $position = 0
    foreach ($literal in $RequiredLiterals) {
        $found = $Text.IndexOf($literal, $position, [StringComparison]::Ordinal)
        if ($found -lt 0) {
            throw "$Description contract is missing or reordered: $literal"
        }
        $position = $found + $literal.Length
    }
}

function Assert-HostPredicationContracts {
    param(
        [Parameter(Mandatory = $true)][string]$GiSource,
        [Parameter(Mandatory = $true)][string]$SdaoSource,
        [Parameter(Mandatory = $true)][string]$SssSource
    )

    $giText = [IO.File]::ReadAllText($GiSource)
    $giComputeState = Get-SourceContractSlice -Text $giText `
        -StartMarker 'class ScopedComputeState {' -EndMarker 'class SavedGraphicsState {' `
        -Description 'GI compute-state predication'
    Assert-OrderedSourceContract -Text $giComputeState -Description 'GI predication' `
        -RequiredLiterals @(
            'context_->GetPredication(predicate_.GetAddressOf(), &predicate_value_);',
            'context_->SetPredication(nullptr, FALSE);',
            'context_->SetPredication(predicate_.Get(), predicate_value_);'
        )

    $sdaoText = [IO.File]::ReadAllText($SdaoSource)
    $sdaoComputeState = Get-SourceContractSlice -Text $sdaoText `
        -StartMarker 'class ScopedComputeState {' -EndMarker 'bool GetTextureDescription(' `
        -Description 'SDAO compute-state predication'
    Assert-OrderedSourceContract -Text $sdaoComputeState -Description 'SDAO predication' `
        -RequiredLiterals @(
            'context_->GetPredication(predicate_.GetAddressOf(), &predicate_value_);',
            'context_->SetPredication(nullptr, FALSE);',
            'context_->SetPredication(predicate_.Get(), predicate_value_);'
        )

    $sdaoNativeAoCapture = Get-SourceContractSlice -Text $sdaoText `
        -StartMarker 'bool CaptureNativeAoConstants(' `
        -EndMarker '#if defined(SPATCH_SDAO_DEVELOPMENT)' `
        -Description 'SDAO native AO constant snapshot predication'
    Assert-OrderedSourceContract -Text $sdaoNativeAoCapture `
        -Description 'SDAO native AO constant snapshot predication' `
        -RequiredLiterals @(
            'context->GetPredication(predicate.GetAddressOf(), &predicate_value);',
            'context->SetPredication(nullptr, FALSE);',
            'context->CopyResource(data.native_ao_constants.Get(), source.Get());',
            'context->SetPredication(predicate.Get(), predicate_value);'
        )

    $sdaoCaptureInitialization = Get-SourceContractSlice -Text $sdaoText `
        -StartMarker 'bool BeginCaptureFrame(' `
        -EndMarker 'bool DrawWithStochasticCapture(' `
        -Description 'SDAO stochastic-depth initialization predication'
    Assert-OrderedSourceContract -Text $sdaoCaptureInitialization `
        -Description 'SDAO stochastic-depth initialization predication' `
        -RequiredLiterals @(
            'context->GetPredication(predicate.GetAddressOf(), &predicate_value);',
            'context->SetPredication(nullptr, FALSE);',
            'context->ClearRenderTargetView(',
            'context->SetPredication(predicate.Get(), predicate_value);'
        )

    $sdaoCaptureDraw = Get-SourceContractSlice -Text $sdaoText `
        -StartMarker 'bool DrawWithStochasticCapture(' `
        -EndMarker 'bool IsSupportedDepthFormat(' `
        -Description 'SDAO full-resolution MIN-blend capture replay predication inheritance'
    foreach ($forbiddenLiteral in @('GetPredication(', 'SetPredication(')) {
        if ($sdaoCaptureDraw.IndexOf(
                $forbiddenLiteral, [StringComparison]::Ordinal) -ge 0) {
            throw "SDAO MIN-blend capture replay must inherit the game predicate; found forbidden state mutation: $forbiddenLiteral"
        }
    }

    $sssText = [IO.File]::ReadAllText($SssSource)
    $sssGraphicsState = Get-SourceContractSlice -Text $sssText `
        -StartMarker 'struct SavedGraphicsState {' -EndMarker 'bool InitializeMaskForFrame(' `
        -Description 'SSS graphics-state predication'
    Assert-OrderedSourceContract -Text $sssGraphicsState -Description 'SSS graphics-state predication' `
        -RequiredLiterals @(
            'context->GetPredication(&predicate, &predicate_value);',
            'context->SetPredication(nullptr, FALSE);',
            'context->SetPredication(predicate, predicate_value);'
        )

    $sssMaskInitialization = Get-SourceContractSlice -Text $sssText `
        -StartMarker 'bool InitializeMaskForFrame(' -EndMarker 'bool WriteMaterialRangeToMask(' `
        -Description 'SSS mask initialization predication'
    Assert-OrderedSourceContract -Text $sssMaskInitialization `
        -Description 'SSS mask initialization predication' -RequiredLiterals @(
            'context->GetPredication(&predicate, &predicate_value);',
            'context->SetPredication(nullptr, FALSE);',
            'context->SetPredication(predicate, predicate_value);'
        )

    $sssMaskReplay = Get-SourceContractSlice -Text $sssText `
        -StartMarker 'bool WriteMaterialRangeToMask(' -EndMarker 'bool UpdateConstants(' `
        -Description 'SSS mask replay predication'
    foreach ($forbiddenLiteral in @('GetPredication(', 'SetPredication(')) {
        if ($sssMaskReplay.IndexOf($forbiddenLiteral, [StringComparison]::Ordinal) -ge 0) {
            throw "SSS mask replay must inherit the game predicate; found forbidden state mutation: $forbiddenLiteral"
        }
    }
}

function Get-ProjectCompileSources {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath
    )

    $checkedProjectPath = Get-CheckedChildPath -Path $ProjectPath -Parent $scriptRoot `
        -Description 'graphics add-on project'
    if (-not (Test-Path -LiteralPath $checkedProjectPath -PathType Leaf)) {
        throw "Graphics add-on project is missing: $checkedProjectPath"
    }

    [xml]$projectDocument = [IO.File]::ReadAllText($checkedProjectPath)
    $compileNodes = @($projectDocument.SelectNodes(
            '/*[local-name()="Project"]/*[local-name()="ItemGroup"]/*[local-name()="ClCompile"][@Include]'))
    if ($compileNodes.Count -eq 0) {
        throw "Graphics add-on project has no compiled C++ sources: $checkedProjectPath"
    }

    $projectDirectory = Split-Path -Parent $checkedProjectPath
    $compileSources = foreach ($compileNode in $compileNodes) {
        $include = $compileNode.GetAttribute('Include')
        if ([string]::IsNullOrWhiteSpace($include) -or $include.Contains('$(')) {
            throw "Graphics add-on project has an unresolved C++ source include: '$include'."
        }
        $sourcePath = Get-CheckedChildPath -Path (Join-Path $projectDirectory $include) `
            -Parent $repoRoot -Description 'graphics add-on C++ source'
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Graphics add-on C++ source is missing: $sourcePath"
        }
        Assert-NoReparseTree -Path $sourcePath `
            -Description 'graphics add-on C++ source'
        $sourcePath
    }
    return @($compileSources | Sort-Object -Unique)
}

$addonProjects = @(
    [pscustomobject]@{
        Name = 'graphics'
        Project = Join-Path $scriptRoot 'standalone\SPatchGraphics.vcxproj'
    }
)
Assert-HostPredicationContracts `
    -GiSource (Join-Path $scriptRoot 'standalone\SPatchGI.cpp') `
    -SdaoSource (Join-Path $scriptRoot 'standalone\SPatchSDAO.cpp') `
    -SssSource (Join-Path $scriptRoot 'standalone\SPatchSSS.cpp')
foreach ($entry in $addonProjects) {
    $compileSources = Get-ProjectCompileSources -ProjectPath $entry.Project
    if (Select-String -LiteralPath $compileSources -Pattern 'Core[\\/]core\.hpp|Luma::' -Quiet) {
        throw "The ShenLong $($entry.Name) module unexpectedly depends on Luma Core."
    }
    $graphicsBuildParent = Join-Path $repoRoot 'build\graphics-addon'
    $graphicsOutputDirectory = Join-Path `
        $graphicsBuildParent "x64-$Configuration"
    Remove-CheckedDirectory -Path $graphicsOutputDirectory `
        -Parent $graphicsBuildParent `
        -Description 'ShenLong configuration output directory'
    $minHookOffline = if ($OfflineDependencies) { 'true' } else { 'false' }
    & $msbuild $entry.Project /m /t:Rebuild "/p:Configuration=$Configuration" `
        /p:Platform=x64 "/p:ReShadeRoot=$ReShadeRoot" `
        "/p:MinHookRoot=$MinHookRoot" "/p:MinHookOffline=$minHookOffline" /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "The ShenLong $($entry.Name) module build failed."
    }
}

$graphicsAddon = Join-Path $repoRoot "build\graphics-addon\x64-$Configuration\ShenLong.asi"
$requiredAddons = @($graphicsAddon)
foreach ($addon in $requiredAddons) {
    if (-not (Test-Path -LiteralPath $addon)) {
        throw "A ShenLong output is missing: $addon"
    }
}

$allowedArtifactRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'artifacts\shenlong'))
$activeArtifactRoot = Get-CheckedChildPath `
    -Path (Join-Path $allowedArtifactRoot $Configuration) `
    -Parent $allowedArtifactRoot -Description 'active graphics package'
$stagingName = ".staging-$Configuration-$PID-$([guid]::NewGuid().ToString('N'))"
$artifactStagingRoot = Get-CheckedChildPath -Path (Join-Path $allowedArtifactRoot $stagingName) `
    -Parent $allowedArtifactRoot -Description 'graphics package staging directory'
$artifactRoot = Get-CheckedChildPath `
    -Path (Join-Path $artifactStagingRoot 'ShenLong-Package') `
    -Parent $artifactStagingRoot -Description 'ShenLong package envelope'

try {

$artifactGameSdaoShaders = Join-Path $artifactRoot 'ShenLong\Shaders\SDAO'
$artifactGameGiShaders = Join-Path $artifactRoot 'ShenLong\Shaders\GI'
$artifactGamePbrShaders = Join-Path $artifactRoot 'ShenLong\Shaders\PBR'
$artifactGameSssShaders = Join-Path $artifactRoot 'ShenLong\Shaders\SSS'
$artifactGameWaterShaders = Join-Path $artifactRoot 'ShenLong\Shaders\Water'
$artifactShaderCacheRoot = Join-Path $artifactRoot 'ShenLong\ShaderCache\v1'
$artifactShaderCacheGi = Join-Path $artifactShaderCacheRoot 'GI'
$artifactShaderCachePbr = Join-Path $artifactShaderCacheRoot 'PBR'
$artifactShaderCacheSdao = Join-Path $artifactShaderCacheRoot 'SDAO'
$artifactShaderCacheSss = Join-Path $artifactShaderCacheRoot 'SSS'
$artifactShaderCacheWater = Join-Path $artifactShaderCacheRoot 'Water'
$artifactLicenseRoot = Join-Path $artifactRoot 'licenses'
$artifactDirectories = @(
    $artifactRoot,
    $artifactLicenseRoot,
    $artifactShaderCacheGi,
    $artifactShaderCachePbr,
    $artifactShaderCacheSdao,
    $artifactShaderCacheSss,
    $artifactShaderCacheWater)
if ($Configuration -eq 'Development-Release') {
    $artifactDirectories += @(
        $artifactGameGiShaders,
        $artifactGamePbrShaders,
        $artifactGameSdaoShaders,
        $artifactGameSssShaders,
        $artifactGameWaterShaders)
}
New-Item -ItemType Directory -Force -Path $artifactDirectories | Out-Null

Copy-Item -LiteralPath $graphicsAddon -Destination $artifactRoot -Force
Copy-Item -LiteralPath $reShadeRuntimePath -Destination (Join-Path $artifactRoot 'dxgi.dll') -Force
$shaderSource = Join-Path $scriptRoot 'overlay\Shaders\Sleeping Dogs Definitive Edition'
$shaderValidationRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot "build\shader-validation\$Configuration"))
$allowedValidationRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\shader-validation'))
if (-not $shaderValidationRoot.StartsWith(
        $allowedValidationRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean unexpected shader-validation path: $shaderValidationRoot"
}
Remove-CheckedDirectory -Path $shaderValidationRoot -Parent $allowedValidationRoot `
    -Description 'shader validation directory'
New-Item -ItemType Directory -Force -Path $shaderValidationRoot | Out-Null

$giShader = Join-Path $shaderSource 'SPatchGI.hlsl'
$pbrShader = Join-Path $shaderSource 'SPatchPBR.hlsl'
$sdaoShader = Join-Path $shaderSource 'Luma_SD_SDAO.hlsl'
$sssShader = Join-Path $shaderSource 'SPatchSSS.hlsl'
$waterMainShader = Join-Path $shaderSource 'SPatchWaterMain.hlsl'
$waterSimpleShader = Join-Path $shaderSource 'SPatchWaterSimple.hlsl'
$waterBlendShader = Join-Path $shaderSource 'SPatchWaterBlend.hlsl'
$pbrNativeCaptureRoot = Join-Path $repoRoot `
    'artifacts\reverse-engineering\2026-07-22-sss-shader-capture'
$pbrVariants = @(
    [pscustomobject]@{ Variant = 0; Hash = '12489767'; NativeSha256 = 'E52B18615D6FA131453B9525026DE034043DFBAD488B862BFA9895427F92D28F'; ReplacementSha256 = '519709090A34D620426A74EB576A892D443A15F716C4D8FC684DC54C53E9DDDC' },
    [pscustomobject]@{ Variant = 1; Hash = '223AA776'; NativeSha256 = '7FC2BF2F2CDEDE09A62A578E439B8DDB4F60E6DBAF1A6DEFF71A826EBDAA53CB'; ReplacementSha256 = '86F2C4B056496D82ED05891336064800FA6AE6CDDED1960495B9FF57EAB66DF1' },
    [pscustomobject]@{ Variant = 2; Hash = '2AF235E8'; NativeSha256 = '0BEC3E9237A729E25B2D7940B121E9671330CE381B1059EFF3D425048180BAB8'; ReplacementSha256 = '9C57C086AB0C22240348487D4B566A3D3A4F947D0E0E0B542FC224308B47F165' },
    [pscustomobject]@{ Variant = 3; Hash = '2D062589'; NativeSha256 = '2FE43BC48DECE8364CE6C7ACAD46B67A5B946977602EE7B255CB4F1048FD650C'; ReplacementSha256 = 'FC839AE472199DFDD85F33DD5B37F35AEF58E3D42E605861E0DACDFE21A7FF66' },
    [pscustomobject]@{ Variant = 4; Hash = '32E195A0'; NativeSha256 = 'D347B7AF78F383EC849B7F8B2D4CF9E53801AAAFACD035CF0D64E1DF304EEBD4'; ReplacementSha256 = 'FE30AA2AB1F40797C2AF71D76BE539863B7FC9ECE1467D2DF7BB812690C33F62' },
    [pscustomobject]@{ Variant = 5; Hash = '398DA3BF'; NativeSha256 = '2A64A4FF3C83C11BE1C9750FFED3A75D7DADAF8CE2600E8F2402E280AF42DF3A'; ReplacementSha256 = '719DFBD3184E2EF9AB418F39BCBB3BF8897DD01DB25D5F122F13BE50C89FE72D' },
    [pscustomobject]@{ Variant = 6; Hash = '386DA32C'; NativeSha256 = '6F34AC419E46378AC06C8AAB1667482E6B0DE2C73D71020CD11DA2AC2CE3EA5D'; ReplacementSha256 = 'DE24264F7C85477D7940C25FBB6529A92A2879B324A34921FEC146991B20C61F' },
    [pscustomobject]@{ Variant = 7; Hash = '5167FBBE'; NativeSha256 = '0005E3D4E920377740FF931E72CC6CA39808F6F95EDF00A563E6B02844E601DD'; ReplacementSha256 = '450DFE014228CB6C909F994C5A8080549DA034F3758422E5F6137B2B8F873308' },
    [pscustomobject]@{ Variant = 8; Hash = '5EBBA455'; NativeSha256 = 'C8F70E6838AC9D32C43E572C5DDE3D8E51C72574DCFAE9AFF9C589FAB92EE2C7'; ReplacementSha256 = '11CFC7F74D21475F0C6EBDFDE6D54D31DB21A1B767F2304A20CB9F15511EEB47' },
    [pscustomobject]@{ Variant = 9; Hash = '66072A23'; NativeSha256 = '6E0B370350A852278225847EC822627D8C82A4F29939B8385BCC4FCB915F14CD'; ReplacementSha256 = '8FF3E0791D550F9BCB56825B5D0A8255D02A443B1AD62E7C3E21FE96B457ED73' },
    [pscustomobject]@{ Variant = 10; Hash = '8A331B0F'; NativeSha256 = '8DEF86CB04BF6355A05C6F0E8DC68E39F9310B47A6158D4C36C9BD4056CC59D8'; ReplacementSha256 = 'E3373982BF72F6B1B77FDB508E4E5971EFCA19597E5A5A31F5AC93F799973568' },
    [pscustomobject]@{ Variant = 11; Hash = 'A30CEF48'; NativeSha256 = '5B7C150B7227AE2C173933A167D687D646CAF64FBEF4AE255997B5470B2C8E64'; ReplacementSha256 = '99003081B7EB1512D8B971A1E3C469B1B70A834A892076B840A47EA20370133E' },
    [pscustomobject]@{ Variant = 12; Hash = 'DCF9CD0C'; NativeSha256 = '555B509B371012FE9796F476CB376A353F004C3646215157D0276735B570C6AE'; ReplacementSha256 = 'FD149A68ECA9C5E03E378A745DDE41A55941610ACECE3348B40EB45DDC4E7C67' },
    [pscustomobject]@{ Variant = 13; Hash = 'D71D285B'; NativeSha256 = '26849A9628385CB2EE1B2DF0BC27F3A68DA54AE292AB9CA4C4AEA223D624E023'; ReplacementSha256 = '0AAFB202101003B900138064B9D83E8CA0297A5E9BB7ACC59B8D104403344CA6' },
    [pscustomobject]@{ Variant = 14; Hash = 'E5E2CE1C'; NativeSha256 = 'E46DD31048516A36C031AD47BA7BEEEA53F485D9F4B9FD7D1326CAE42C127698'; ReplacementSha256 = 'CD7A885DDF707567B87A502C4C29EAC5A8CB1A6C039BDA82C4E18A4019CB0D14' },
    [pscustomobject]@{ Variant = 15; Hash = 'EFD8577D'; NativeSha256 = '71E5700B625F6062F392765EBC1D4AACF8C1967DAE0203291D77E581CAE733BE'; ReplacementSha256 = 'FC426DE231CCFB2A5B148A3423D163980918DFA3B1135ACE0F8D35DD41EAD258' },
    [pscustomobject]@{ Variant = 16; Hash = 'F74BCE96'; NativeSha256 = 'DB0A0ECBB0DA12B96B42C3AB9C2392A3FAB23004C6F79A3FB7F08E14BB5F0F28'; ReplacementSha256 = 'AB62D3083BA9F9D6797E0320336B80018AF316062DAA918BD5C038B1CC60D6CD' },
    [pscustomobject]@{ Variant = 17; Hash = '282EE2DC'; NativeSha256 = '604E7DA287E6626E93531FCFD856549CB374567E91AA27C79D897ED9027E71F4'; ReplacementSha256 = 'BAAEAFB2EA157C5B726E3EF268952E697DC281C955FE30F3517A2300444EC45A' },
    [pscustomobject]@{ Variant = 18; Hash = '5DB1CB6E'; NativeSha256 = '5C7DCF33C05D3C770CD8DF8059CA1013213338B08A1D38E3B513B8858AB99564'; ReplacementSha256 = '10BE7168019F6D0D0C0F68716DFCC197EC6FF546037E3D0D0869972BA4663A98' },
    [pscustomobject]@{ Variant = 19; Hash = 'E611C192'; NativeSha256 = '138AB220A2B2B5BA47367D1297139F89A2729AD047D10B309E64E46641F84572'; ReplacementSha256 = 'F6E3DEE4CD061AD022CD19CABCC61B361351D05F24D01509836D1196A627C20D' }
)
if ($pbrVariants.Count -ne 20) {
    throw "PBR shader mapping drifted: expected 20 variants, found $($pbrVariants.Count)."
}
for ($variantIndex = 0; $variantIndex -lt $pbrVariants.Count; ++$variantIndex) {
    $pbrVariant = $pbrVariants[$variantIndex]
    if ($pbrVariant.Variant -ne $variantIndex -or
        $pbrVariant.Hash -notmatch '^[0-9A-F]{8}$' -or
        $pbrVariant.NativeSha256 -notmatch '^[0-9A-F]{64}$' -or
        $pbrVariant.ReplacementSha256 -notmatch '^[0-9A-F]{64}$') {
        throw "PBR shader mapping is malformed or out of order at index ${variantIndex}."
    }
}
$replaceablePbrVariants = @($pbrVariants | Where-Object {
        $_.Variant -ne 0 -and $_.Variant -ne 13
    })
if ($replaceablePbrVariants.Count -ne 18 -or
    @($replaceablePbrVariants | Where-Object {
            $_.Variant -eq 0 -or $_.Variant -eq 13
        }).Count -ne 0) {
    throw 'PBR runtime-cache selection must contain exactly the 18 replaceable variants and exclude native-only variants 0 and 13.'
}
foreach ($pbrVariant in $pbrVariants) {
    $nativePath = Join-Path $pbrNativeCaptureRoot `
        "PS_0x$($pbrVariant.Hash).cso"
    if (-not (Test-Path -LiteralPath $nativePath -PathType Leaf)) {
        throw "PBR variant $($pbrVariant.Variant) native shader 0x$($pbrVariant.Hash) is missing: $nativePath"
    }
    $nativeSha256 =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $nativePath).Hash
    if ($nativeSha256 -cne $pbrVariant.NativeSha256) {
        throw "PBR variant $($pbrVariant.Variant) native shader 0x$($pbrVariant.Hash) identity drifted. Expected $($pbrVariant.NativeSha256), found $nativeSha256."
    }
}
$pbrText = [IO.File]::ReadAllText($pbrShader)
$pbrHostText = [IO.File]::ReadAllText(
    (Join-Path $scriptRoot 'standalone\SPatchPBR.cpp'))
foreach ($hostContract in @(
        'kShaderCount\s*=\s*20',
        'kNativeAmbientShaderMask\s*=\s*1u\s*<<\s*0u',
        'kNativeCompatibilityShaderMask\s*=\s*1u\s*<<\s*13u',
        'kReplaceableShaderMask\s*=\s*kAllShaderMask\s*&\s*~kNativeAmbientShaderMask\s*&\s*~kNativeCompatibilityShaderMask\s*;',
        'kReplaceableShaderCount\s*=\s*kShaderCount\s*-\s*2u',
        'static_assert\(kReplaceableShaderMask\s*==\s*0xFDFFEu\)',
        'static_assert\(kReplaceableShaderCount\s*==\s*18u\)',
        'native_compatibility_mask=0x%05X',
        '(?s)if\s*\(!IsReplaceableVariant\(\*variant\)\)\s*\{[^}]*\bcontinue\s*;\s*\}'
        '0x282EE2DCu,\s*6408u',
        '0x68, 0xDA, 0x85, 0x0F, 0x28, 0xFB, 0xBE, 0x99',
        'ReplacementIdentity\{0xE87B3E61u,\s*6808u',
        '0x03, 0xD6, 0xE7, 0xDD, 0x20, 0xE5, 0xD1, 0x8F',
        '0x5DB1CB6Eu,\s*6672u',
        '0x61, 0xBE, 0xF4, 0x77, 0x4A, 0x18, 0x8E, 0x2A',
        '0xE611C192u,\s*4916u',
        '0xBC, 0xE8, 0x9F, 0x85, 0x12, 0xCB, 0x38, 0xCE',
        'ReplacementIdentity\{0x308BFF59u,\s*6776u',
        '0x92, 0x49, 0xC7, 0xA7, 0x72, 0x5B, 0x7D, 0x3F',
        'ReplacementIdentity\{0xD70A96E5u,\s*4960u',
        '0xB6, 0x52, 0x4C, 0xDD, 0xE2, 0xDB, 0x66, 0x60',
        '\[ShenLong-PBR\] present=%llu enabled=1 ready=1 strength=100 validated=%zu/%zu native_passthrough=2',
        '(?s)static_cast<unsigned long long>\(presents\),\s*kReplaceableShaderCount,\s*kReplaceableShaderCount,\s*discovered,',
        'direct_specular_aa=opaque-normal-derivative,vehicle-glass=none')) {
    if ($pbrHostText -notmatch $hostContract) {
        throw "The PBR host identity/replacement policy contract drifted: $hostContract"
    }
}
foreach ($shaderContract in @(
        'SPATCH_PBR_SPECULAR_AA_VARIANCE_CAP = 0.18',
        'float3 normalDx = ddx(normal);',
        'float3 normalDy = ddy(normal);',
        'float geometricVariance)',
        '2.0 / (safeExponent + 2.0) + max(geometricVariance, 0.0)',
        'return min(50.0 * SPatchPBRBoundF0(f0), 1.0);',
        'float3 boundedF90 = min(max(f90, boundedF0), 1.0);')) {
    if ($pbrText.IndexOf($shaderContract, [StringComparison]::Ordinal) -lt 0) {
        throw "The PBR direct-light geometric specular-AA contract drifted: $shaderContract"
    }
}
$flattenedPbrDerivativeBranches = [regex]::Matches(
    $pbrText,
    '(?m)^\s*\[flatten\]\r?\n\s*if \((?:r0\.x|r2\.y) != 0\) \{').Count
if ($flattenedPbrDerivativeBranches -ne 5) {
    throw "The PBR derivative-uniformity source contract drifted: expected 5 flattened light-volume branches, found $flattenedPbrDerivativeBranches."
}
$pbrVariantZero = Get-SourceContractSlice -Text $pbrText `
    -StartMarker '#if SPATCH_PBR_VARIANT == 0' `
    -EndMarker '#elif SPATCH_PBR_VARIANT == 1' `
    -Description 'PBR native ambient-only variant 0'
if ($pbrVariantZero.IndexOf(
        'SPatchPBRDirectSpecular(', [StringComparison]::Ordinal) -ge 0) {
    throw 'PBR variant 0 must remain native and may not evaluate direct-light GGX.'
}
foreach ($ambientSunVariant in @(5, 10)) {
    $variantBody = Get-SourceContractSlice -Text $pbrText `
        -StartMarker "#elif SPATCH_PBR_VARIANT == $ambientSunVariant" `
        -EndMarker "#elif SPATCH_PBR_VARIANT == $($ambientSunVariant + 1)" `
        -Description "PBR native-ambient/direct-sun variant $ambientSunVariant"
    $directCallCount = [regex]::Matches(
        $variantBody, 'SPatchPBRDirectSpecular\(').Count
    if ($directCallCount -ne 1 -or
        $variantBody.IndexOf(
            'spatchPbrAmbient', [StringComparison]::Ordinal) -ge 0) {
        throw "PBR variant $ambientSunVariant must preserve native ambient lighting and replace exactly one direct-sun lobe."
    }
}
$pbrMaterialDecodeContracts = @(
    [pscustomobject]@{ Variant = 1; Capture = 'float spatchMetallic = r1.y ? 1.0 : 0.0;'; Overwrite = 'r1.yzw = r1.yyy ? r4.xyz : r3.www;'; F0 = 'float3 spatchF0 = spatchMetallic ? r3.xyz : r3.www;' },
    [pscustomobject]@{ Variant = 6; Capture = 'float spatchMetallic = r0.y ? 1.0 : 0.0;'; Overwrite = 'r0.yzw = r0.yyy ? r2.xyz : r2.www;'; F0 = 'float3 spatchF0 = r0.yzw;' },
    [pscustomobject]@{ Variant = 7; Capture = 'float spatchMetallic = r0.y ? 1.0 : 0.0;'; Overwrite = 'r0.yzw = r0.yyy ? r2.xyz : r2.www;'; F0 = 'float3 spatchF0 = r0.yzw;' },
    [pscustomobject]@{ Variant = 12; Capture = 'float spatchMetallic = r1.y ? 1.0 : 0.0;'; Overwrite = 'r1.yzw = r1.yyy ? r4.xyz : r3.www;'; F0 = 'float3 spatchF0 = spatchMetallic ? r3.xyz : r3.www;' },
    [pscustomobject]@{ Variant = 13; Capture = 'float spatchMetallic = r0.y ? 1.0 : 0.0;'; Overwrite = 'r0.yzw = r0.yyy ? r5.xyz : r5.www;'; F0 = 'float3 spatchF0 = spatchMetallic ? r2.xyz : r5.www;' }
)
foreach ($contract in $pbrMaterialDecodeContracts) {
    $variantBody = Get-SourceContractSlice -Text $pbrText -StartMarker "#elif SPATCH_PBR_VARIANT == $($contract.Variant)" -EndMarker "#elif SPATCH_PBR_VARIANT == $($contract.Variant + 1)" -Description "PBR material decode variant $($contract.Variant)"
    $captureIndex = $variantBody.IndexOf($contract.Capture, [StringComparison]::Ordinal)
    $overwriteIndex = $variantBody.IndexOf($contract.Overwrite, [StringComparison]::Ordinal)
    $f0Index = $variantBody.IndexOf($contract.F0, [StringComparison]::Ordinal)
    if ($captureIndex -lt 0 -or $overwriteIndex -lt 0 -or $f0Index -lt 0 -or
        $captureIndex -ge $overwriteIndex -or $overwriteIndex -ge $f0Index -or
        -not [regex]::IsMatch(
            $variantBody,
            'SPatchPBRDiffuseWeight(?:UnitGrazing)?\([\s\S]*?spatchF0,\s*spatchMetallic\)')) {
        throw "PBR variant $($contract.Variant) must preserve its scalar material predicate before the vector register overwrite and use that predicate for diffuse energy partitioning."
    }
}

foreach ($opaqueF90Variant in @(1..12 + 14..16)) {
    $variantBody = Get-SourceContractSlice -Text $pbrText `
        -StartMarker "#elif SPATCH_PBR_VARIANT == $opaqueF90Variant" `
        -EndMarker "#elif SPATCH_PBR_VARIANT == $($opaqueF90Variant + 1)" `
        -Description "PBR native-opaque F90 variant $opaqueF90Variant"
    if ($variantBody.IndexOf('UnitGrazing', [StringComparison]::Ordinal) -ge 0) {
        throw "PBR variant $opaqueF90Variant must use the native opaque min(1, 50 * F0) grazing contract."
    }
}
$pbrVolumeLight = Get-SourceContractSlice -Text $pbrText `
    -StartMarker '#elif SPATCH_PBR_VARIANT == 13' `
    -EndMarker '#elif SPATCH_PBR_VARIANT == 14' `
    -Description 'PBR cached volume-light unit-grazing variant 13'
if ([regex]::Matches(
        $pbrVolumeLight, 'SPatchPBRDirectSpecularUnitGrazing\(').Count -ne 1 -or
    [regex]::Matches(
        $pbrVolumeLight, 'SPatchPBRDiffuseWeightUnitGrazing\(').Count -ne 1) {
    throw 'PBR cached volume-light variant 13 must retain its native unit-grazing Fresnel contract even though runtime compatibility policy leaves this pass native.'
}

$pbrVehicleGlassHelper = Get-SourceContractSlice -Text $pbrText `
    -StartMarker 'float3 SPatchPBRGlassDirectSpecular(' `
    -EndMarker 'float3 SPatchPBRDiffuseWeight(' `
    -Description 'PBR vehicle-glass GGX helper'
foreach ($glassHelperContract in @(
        'float safeExponent = min(max(nativeExponent, 0.0), 65536.0);',
        'float alphaSquared = saturate(2.0 / (safeExponent + 2.0));',
        'SPatchPBRFresnelUnitGrazing(viewDirection, lightDirection, f0);',
        'noH * noH * (alphaSquared - 1.0) + 1.0, 1.0e-6);')) {
    if ($pbrVehicleGlassHelper.IndexOf(
            $glassHelperContract, [StringComparison]::Ordinal) -lt 0) {
        throw "The PBR vehicle-glass GGX numeric contract drifted: $glassHelperContract"
    }
}

$pbrVehicleGlass = Get-SourceContractSlice -Text $pbrText `
    -StartMarker '#elif SPATCH_PBR_VARIANT == 17' `
    -EndMarker '#elif SPATCH_PBR_VARIANT == 18' `
    -Description 'PBR runtime-proven vehicle-glass variant 17'
$glassPbrCallCount = [regex]::Matches(
    $pbrVehicleGlass, 'SPatchPBRGlassDirectSpecular\(').Count
$glassDiffuseSampleCount = [regex]::Matches(
    $pbrVehicleGlass, 'texDiffuse\.Sample\(').Count
$glassSphericalSampleCount = [regex]::Matches(
    $pbrVehicleGlass, 'texSphericalMap\.SampleLevel\(').Count
$glassFogSampleCount = [regex]::Matches(
    $pbrVehicleGlass, 'texFogCube\.Sample\(').Count
if ($glassPbrCallCount -ne 1 -or
    $glassDiffuseSampleCount -ne 3 -or
    $glassSphericalSampleCount -ne 1 -or
    $glassFogSampleCount -ne 1 -or
    $pbrVehicleGlass.IndexOf(
        'SPatchPBRDirectSpecular(', [StringComparison]::Ordinal) -ge 0 -or
    $pbrVehicleGlass.IndexOf(
        'SPatchPBRGeometricVariance(', [StringComparison]::Ordinal) -ge 0 -or
    $pbrVehicleGlass.IndexOf('ddx(', [StringComparison]::Ordinal) -ge 0 -or
    $pbrVehicleGlass.IndexOf('ddy(', [StringComparison]::Ordinal) -ge 0) {
    throw 'PBR variant 17 must replace exactly one vehicle-glass direct-sun lobe plus its environment Fresnel partition, preserve the native three-sample diffuse/decal and spherical lookup paths, and remain derivative-free.'
}
foreach ($glassSourceContract in @(
        'float spatchGlassNoL = r2.x;',
        'float spatchGlassNativeExponent = max(r3.x - 0.999, 0.0);',
        'spatchGlassNativeExponent, float3(0.05,0.05,0.05));',
        'r2.xyz = lerp(r2.xyz, spatchGlassPbrDirectSun, SPatchPBRBlendStrength());',
        'r1.xyz = r1.xyz * r1.www + r2.xyz;',
        'float spatchGlassNoV = saturate(r2.x);',
        'texSphericalMap.SampleLevel(',
        'float3 spatchGlassEnvironmentRadiance = r0.yzw;',
        'SPatchPBRFresnelCosineUnitGrazing(',
        'r1.xyz * (1.0 - spatchGlassPbrEnvironmentFresnel)',
        'spatchGlassEnvironmentRadiance * spatchGlassPbrEnvironmentFresnel',
        'spatchGlassNativeEnvironmentComposite,',
        'spatchGlassPbrEnvironmentComposite,',
        'texFogCube.Sample(',
        'o0.w = r1.w;')) {
    if ($pbrVehicleGlass.IndexOf(
            $glassSourceContract, [StringComparison]::Ordinal) -lt 0) {
        throw "The PBR vehicle-glass native-path contract drifted: $glassSourceContract"
    }
}

foreach ($vehiclePaintVariant in @(18, 19)) {
    $vehiclePaintEndMarker = if ($vehiclePaintVariant -eq 18) {
        '#elif SPATCH_PBR_VARIANT == 19'
    } else {
        '#else'
    }
    $vehiclePaintBody = Get-SourceContractSlice -Text $pbrText `
        -StartMarker "#elif SPATCH_PBR_VARIANT == $vehiclePaintVariant" `
        -EndMarker $vehiclePaintEndMarker `
        -Description "PBR runtime-proven vehicle-paint variant $vehiclePaintVariant"
    if ([regex]::Matches(
            $vehiclePaintBody, 'texSphericalMap\.SampleLevel\(').Count -ne 1 -or
        [regex]::Matches(
            $vehiclePaintBody, 'texSpecular\.Sample\(').Count -ne 1 -or
        [regex]::Matches(
            $vehiclePaintBody, 'texBump\.Sample\(').Count -ne 1 -or
        [regex]::Matches(
            $vehiclePaintBody, 'texFadeDitherMask\.Sample\(').Count -ne 1 -or
        $vehiclePaintBody.IndexOf(
            'SPatchPBRDirectSpecular(', [StringComparison]::Ordinal) -ge 0 -or
        $vehiclePaintBody.IndexOf(
            'SPatchPBRGeometricVariance(', [StringComparison]::Ordinal) -ge 0 -or
        $vehiclePaintBody.IndexOf('ddx(', [StringComparison]::Ordinal) -ge 0 -or
        $vehiclePaintBody.IndexOf('ddy(', [StringComparison]::Ordinal) -ge 0 -or
        $vehiclePaintBody.IndexOf(
            'SPatchPBRFresnelCosineUnitGrazing(',
            [StringComparison]::Ordinal) -ge 0) {
        throw "PBR variant $vehiclePaintVariant must replace only the vehicle-paint view Fresnel/energy partition, preserve the native texture and spherical lookup paths, use native-family zero-F0-safe grazing behavior, and remain derivative-free."
    }
    foreach ($vehiclePaintContract in @(
            'float spatchVehiclePaintF0 =',
            'float spatchVehiclePaintNoV =',
            'float spatchVehiclePaintNativeFresnel =',
            'float spatchVehiclePaintPbrFresnel = SPatchPBRFresnelCosine(',
            'float spatchVehiclePaintEnvironmentFresnel = lerp(',
            'float spatchVehiclePaintNativeDiffuseWeight =',
            '1.0 - min(0.5, spatchVehiclePaintNativeFresnel);',
            'float spatchVehiclePaintPbrDiffuseWeight =',
            '1.0 - spatchVehiclePaintPbrFresnel;',
            'r0.x = 6 * r0.x;',
            'o0.w =',
            'o1.w =',
            'o2.w = 0;')) {
        if ($vehiclePaintBody.IndexOf(
                $vehiclePaintContract,
                [StringComparison]::Ordinal) -lt 0) {
            throw "The PBR vehicle-paint native-path contract drifted for variant ${vehiclePaintVariant}: $vehiclePaintContract"
        }
    }
}
$pbrNumericVerifier = Join-Path $repoRoot 'tools\Test-PBRBRDF.ps1'
if (-not (Test-Path -LiteralPath $pbrNumericVerifier -PathType Leaf)) {
    throw "The PBR numeric verifier is missing: $pbrNumericVerifier"
}
& $pbrNumericVerifier -NoExit
$waterNumericVerifier = Join-Path $repoRoot 'tools\Test-WaterBRDF.ps1'
if (-not (Test-Path -LiteralPath $waterNumericVerifier -PathType Leaf)) {
    throw "The Water numeric verifier is missing: $waterNumericVerifier"
}
& $waterNumericVerifier
$tonemappingSource = Join-Path $scriptRoot 'standalone\SPatchTonemapping.cpp'
$tonemappingText = [IO.File]::ReadAllText($tonemappingSource)
$capturedTonemappingShader = Join-Path $repoRoot `
    'artifacts\reverse-engineering\2026-07-22-sss-shader-capture\PS_0x67843125.cso'
$capturedTonemappingSha256 =
    '2EF78A31BBDFD3B8C65C5815727207B690F1F0EE6369CE95B3C99CE6AF951C35'
if (-not (Test-Path -LiteralPath $capturedTonemappingShader -PathType Leaf) -or
    (Get-Item -LiteralPath $capturedTonemappingShader).Length -ne 1852 -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $capturedTonemappingShader).Hash -ne
        $capturedTonemappingSha256) {
    throw 'The captured final pre-HUD shader identity is missing or has drifted.'
}
foreach ($identityContract in @(
        'kFinalPreHudPixelShaderHash\s*=\s*0x67843125',
        'kFinalPreHudPixelShaderSize\s*=\s*1852',
        '0x08, 0xCD, 0x71, 0xA4, 0x19, 0x9D, 0x73, 0xDF',
        '0x31, 0x4A, 0x99, 0xE7, 0x7C, 0x92, 0x3E, 0xCA')) {
    if ($tonemappingText -notmatch $identityContract) {
        throw "The AgX pipeline identity contract drifted: $identityContract"
    }
}
foreach ($colorContract in @(
        'SPATCH_AGX_LOOK',
        'AgxMiddleGrayLog',
        'AgxMidtoneSaturation = 1\.04',
        'AgxHighlightSaturation = 1\.02',
        'smoothstep\(AgxShadowChromaStart, AgxShadowChromaEnd, lookPeak\)',
        'smoothstep\(AgxHighlightChromaStart, AgxHighlightChromaEnd, lookPeak\)',
        'lerp\(1\.0, AgxMidtoneSaturation, shadowWeight\)',
        'lerp\(shadowToMidtoneSaturation, AgxHighlightSaturation, highlightWeight\)',
        'AgxDisplayPeakLimit = 252\.0 / 255\.0',
        'LimitDisplayPeak\(composed\)',
        'MaxFiniteSceneEncoding = 1\.04 - 1e-5',
        'FiniteOrZero\(sampledBloom\.rgb\)',
        'DecodeSceneEncoding\(encodedScene\)',
        'GameGammaExponent\(\)',
        'AgxToDisplaySrgb\(max\(exposedScene, 0\.0\)\) / gameWhiteScale')) {
    if ($tonemappingText -notmatch $colorContract) {
        throw "The AgX color-fidelity contract drifted: $colorContract"
    }
}
$tonemappingMatch = [regex]::Match(
    $tonemappingText,
    'R"SPATCH_HLSL\((?<shader>[\s\S]*?)\)SPATCH_HLSL";')
if (-not $tonemappingMatch.Success) {
    throw 'The embedded AgX final-composition shader was not found.'
}
$tonemappingShader = Join-Path $shaderValidationRoot 'SPatchTonemapping.hlsl'
[IO.File]::WriteAllText(
    $tonemappingShader,
    $tonemappingMatch.Groups['shader'].Value,
    [Text.UTF8Encoding]::new($false))
$shaderVariantCount = 0
$runtimeShaderVariants = @()

$agxOutputPath = Join-Path $shaderValidationRoot 'tonemapping-agx-medium-high.cso'
Invoke-FxcVariant -Shader $tonemappingShader -EntryPoint 'main' `
    -Target 'ps_4_0' -Defines @{ SPATCH_AGX_LOOK = '1' } `
    -OutputPath $agxOutputPath -IeeeStrictness
Assert-AgxReflectedBindings -BytecodePath $agxOutputPath
++$shaderVariantCount
$agxOutputPath = Join-Path $shaderValidationRoot 'tonemapping-agx-medium-high-blended.cso'
Invoke-FxcVariant -Shader $tonemappingShader -EntryPoint 'main' `
    -Target 'ps_4_0' -Defines @{
        SPATCH_AGX_EXPOSURE = '1.0'
        SPATCH_AGX_STRENGTH = '0.5'
        SPATCH_AGX_FULL_STRENGTH = '0'
        SPATCH_AGX_LOOK = '1'
    } -OutputPath $agxOutputPath -IeeeStrictness
Assert-AgxReflectedBindings -BytecodePath $agxOutputPath
++$shaderVariantCount
$agxOutputPath = Join-Path $shaderValidationRoot 'tonemapping-agx-neutral.cso'
Invoke-FxcVariant -Shader $tonemappingShader -EntryPoint 'main' `
    -Target 'ps_4_0' -Defines @{ SPATCH_AGX_LOOK = '0' } `
    -OutputPath $agxOutputPath -IeeeStrictness
Assert-AgxReflectedBindings -BytecodePath $agxOutputPath
++$shaderVariantCount
$agxOutputPath = Join-Path $shaderValidationRoot 'tonemapping-agx-neutral-blended.cso'
Invoke-FxcVariant -Shader $tonemappingShader -EntryPoint 'main' `
    -Target 'ps_4_0' -Defines @{
        SPATCH_AGX_EXPOSURE = '1.0'
        SPATCH_AGX_STRENGTH = '0.5'
        SPATCH_AGX_FULL_STRENGTH = '0'
        SPATCH_AGX_LOOK = '0'
    } -OutputPath $agxOutputPath -IeeeStrictness
Assert-AgxReflectedBindings -BytecodePath $agxOutputPath
++$shaderVariantCount

$agxValidationProject = Join-Path $repoRoot 'tools\AgxShaderValidation.vcxproj'
$agxValidationExe = Join-Path $repoRoot `
    'build\agx-validation\x64-Release\AgxShaderValidation.exe'
$agxBuildOutput = @(& $msbuild $agxValidationProject /m /t:Rebuild `
    /p:Configuration=Release /p:Platform=x64 /v:minimal 2>&1)
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $agxValidationExe -PathType Leaf)) {
    throw "AgX WARP validation harness build failed: $($agxBuildOutput -join [Environment]::NewLine)"
}
$agxValidationOutput = @(& $agxValidationExe `
    (Join-Path $shaderValidationRoot 'tonemapping-agx-medium-high.cso') `
    (Join-Path $shaderValidationRoot 'tonemapping-agx-medium-high-blended.cso') `
    (Join-Path $shaderValidationRoot 'tonemapping-agx-neutral.cso') `
    (Join-Path $shaderValidationRoot 'tonemapping-agx-neutral-blended.cso') 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "AgX GPU/CPU numeric validation failed: $($agxValidationOutput -join [Environment]::NewLine)"
}
Write-Host ($agxValidationOutput -join [Environment]::NewLine)

foreach ($pbrVariant in $replaceablePbrVariants) {
    $outputPath = Join-Path $shaderValidationRoot `
        "pbr-0x$($pbrVariant.Hash).cso"
    $compiledVariant = Invoke-RuntimeShaderVariant `
        -Shader $pbrShader -EntryPoint 'main' -Target 'ps_4_0' `
        -Defines @{
            SPATCH_PBR_VARIANT = "$($pbrVariant.Variant)"
            SPATCH_PBR_STRENGTH = '1.0'
        } -ValidationOutputPath $outputPath `
        -CacheRelativePath "PBR/PBR-0x$($pbrVariant.Hash).ps_4_0.cso"
    Assert-PbrNativeAbi -CompiledBytecodePath $outputPath `
        -NativeBytecodePath (Join-Path $pbrNativeCaptureRoot `
            "PS_0x$($pbrVariant.Hash).cso") `
        -Variant $pbrVariant.Variant -NativeShaderHash $pbrVariant.Hash `
        -ExpectedNativeSha256 $pbrVariant.NativeSha256 `
        -ExpectedReplacementSha256 $pbrVariant.ReplacementSha256
    $expectedPbrDerivativeCount = if ($pbrVariant.Variant -ge 17) { 0 } else { 2 }
    Assert-PbrDerivativeUniformity -BytecodePath $outputPath `
        -Variant $pbrVariant.Variant `
        -ExpectedDerivativeCount $expectedPbrDerivativeCount
    $runtimeShaderVariants += $compiledVariant
    ++$shaderVariantCount
}
Write-Host 'Validated all 20 exact-hash-pinned native PBR identities and compiled the 18 replaceable cache variants against captured native profile, cbuffer, resource-binding, input-signature, and output-signature ABI contracts; native-only variants 0 and 13 are not cached.'

$outputPath = Join-Path $shaderValidationRoot 'gi-prepare-normals.cso'
$runtimeShaderVariants += Invoke-RuntimeShaderVariant `
    -Shader $giShader -EntryPoint 'prepare_normals_cs' -Target 'cs_5_0' `
    -Defines @{} -ValidationOutputPath $outputPath `
    -CacheRelativePath 'GI/prepare_normals_cs.cs_5_0.cso'
++$shaderVariantCount

foreach ($quality in 0..4) {
    $halfResolution = if ($quality -le 3) { '1' } else { '0' }
    $common = @{
        SPATCH_GI_QUALITY = "$quality"
        SPATCH_GI_HALF_RES = $halfResolution
    }
    foreach ($job in @(
            [pscustomobject]@{ Entry = 'prepare_gbuffer_cs'; Target = 'cs_5_0' },
            [pscustomobject]@{ Entry = 'downsample_mip_cs'; Target = 'cs_5_0' },
            [pscustomobject]@{ Entry = 'visibility_gi_cs'; Target = 'cs_5_0' },
            [pscustomobject]@{ Entry = 'FullscreenVS'; Target = 'vs_5_0' },
            [pscustomobject]@{ Entry = 'CompositePS'; Target = 'ps_5_0' })) {
        $outputPath = Join-Path $shaderValidationRoot "gi-q$quality-$($job.Entry).cso"
        $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
            -Shader $giShader -EntryPoint $job.Entry -Target $job.Target `
            -Defines $common -ValidationOutputPath $outputPath `
            -CacheRelativePath "GI/$($job.Entry).$($job.Target).q$quality.half$halfResolution.cso"
        ++$shaderVariantCount
    }
    foreach ($horizontal in 0..1) {
        $filterDefines = @{
            SPATCH_GI_QUALITY = "$quality"
            SPATCH_GI_HALF_RES = $halfResolution
            SPATCH_GI_FILTER_HORIZONTAL = "$horizontal"
        }
        $outputPath = Join-Path $shaderValidationRoot "gi-q$quality-spatial-filter-h$horizontal.cso"
        $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
            -Shader $giShader -EntryPoint 'spatial_filter_cs' -Target 'cs_5_0' `
            -Defines $filterDefines -ValidationOutputPath $outputPath `
            -CacheRelativePath "GI/spatial_filter_cs.cs_5_0.q$quality.half$halfResolution.horizontal$horizontal.cso"
        ++$shaderVariantCount
    }
}

$outputPath = Join-Path $shaderValidationRoot 'sdao-prepare-depth.cso'
$runtimeShaderVariants += Invoke-RuntimeShaderVariant `
    -Shader $sdaoShader -EntryPoint 'prepare_depth_cs' -Target 'cs_5_0' `
    -Defines @{} -ValidationOutputPath $outputPath `
    -CacheRelativePath 'SDAO/prepare_depth_cs.cs_5_0.cso'
++$shaderVariantCount
$sdaoLayerCounts = @(1, 2, 2, 4, 4)
foreach ($quality in 0..4) {
    $captureOutputPath =
        Join-Path $shaderValidationRoot "sdao-q$quality-capture-depth.cso"
    $captureVariant = Invoke-RuntimeShaderVariant `
        -Shader $sdaoShader -EntryPoint 'capture_depth_ps' -Target 'ps_5_0' `
        -Defines @{ SD_SDAO_QUALITY = "$quality" } `
        -ValidationOutputPath $captureOutputPath `
        -CacheRelativePath "SDAO/capture_depth_ps.ps_5_0.q$quality.layers$($sdaoLayerCounts[$quality]).full1.cso"
    Assert-SdaoCaptureDonorBytecode -BytecodePath $captureOutputPath
    $runtimeShaderVariants += $captureVariant
    ++$shaderVariantCount
    $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
        -Shader $sdaoShader -EntryPoint 'main_pass_cs' -Target 'cs_5_0' `
        -Defines @{ SD_SDAO_QUALITY = "$quality" } `
        -ValidationOutputPath (Join-Path $shaderValidationRoot "sdao-q$quality-main.cso") `
        -CacheRelativePath "SDAO/main_pass_cs.cs_5_0.q$quality.cso"
    ++$shaderVariantCount
    $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
        -Shader $sdaoShader -EntryPoint 'spatial_filter_cs' -Target 'cs_5_0' `
        -Defines @{
            SD_SDAO_QUALITY = "$quality"
            SD_SDAO_FILTER_HORIZONTAL = '1'
            SD_SDAO_COLOR_OUTPUT = '0'
        } -ValidationOutputPath (Join-Path $shaderValidationRoot "sdao-q$quality-filter-h.cso") `
        -CacheRelativePath "SDAO/spatial_filter_cs.cs_5_0.q$quality.horizontal1.color0.cso"
    ++$shaderVariantCount
    $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
        -Shader $sdaoShader -EntryPoint 'spatial_filter_cs' -Target 'cs_5_0' `
        -Defines @{
            SD_SDAO_QUALITY = "$quality"
            SD_SDAO_FILTER_HORIZONTAL = '0'
            SD_SDAO_COLOR_OUTPUT = '1'
        } -ValidationOutputPath (Join-Path $shaderValidationRoot "sdao-q$quality-filter-v.cso") `
        -CacheRelativePath "SDAO/spatial_filter_cs.cs_5_0.q$quality.horizontal0.color1.cso"
    ++$shaderVariantCount
}
foreach ($quality in 0..4) {
    $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
        -Shader $sdaoShader -EntryPoint 'main_pass_cs' -Target 'cs_5_0' `
        -Defines @{
            SD_SDAO_QUALITY = "$quality"
            SD_GTAO_LITE = '1'
        } `
        -ValidationOutputPath (Join-Path $shaderValidationRoot "gtao-lite-q$quality-main.cso") `
        -CacheRelativePath "SDAO/main_pass_cs.cs_5_0.q$quality.gtaolite1.cso"
    ++$shaderVariantCount
}

$outputPath = Join-Path $shaderValidationRoot 'sss-fullscreen-vs.cso'
$runtimeShaderVariants += Invoke-RuntimeShaderVariant `
    -Shader $sssShader -EntryPoint 'FullscreenVS' -Target 'vs_5_0' `
    -Defines @{} -ValidationOutputPath $outputPath `
    -CacheRelativePath 'SSS/FullscreenVS.vs_5_0.cso'
++$shaderVariantCount
$waterCacheIdentities = Get-WaterCacheIdentityTable -SourcePath `
    (Join-Path $scriptRoot 'standalone\SPatchWater.cpp')
$compiledWaterVariants = @()
foreach ($entry in @(
        [pscustomobject]@{ Name = 'EyeMaskPS'; Target = 'ps_4_0' },
        [pscustomobject]@{ Name = 'HairCapturePS'; Target = 'ps_4_0' },
        [pscustomobject]@{ Name = 'FoliageCapturePS'; Target = 'ps_4_0' },
        [pscustomobject]@{ Name = 'FoliageTransmissionPS'; Target = 'ps_5_0' })) {
    $outputPath = Join-Path $shaderValidationRoot "sss-$($entry.Name).cso"
    $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
        -Shader $sssShader -EntryPoint $entry.Name -Target $entry.Target `
        -Defines @{} -ValidationOutputPath $outputPath `
        -CacheRelativePath "SSS/$($entry.Name).$($entry.Target).cso"
    ++$shaderVariantCount
}
foreach ($quality in 0..2) {
    foreach ($horizontal in 0..1) {
        foreach ($development in 0..1) {
            $outputPath = Join-Path $shaderValidationRoot "sss-q$quality-h$horizontal-dev$development.cso"
            $runtimeShaderVariants += Invoke-RuntimeShaderVariant `
                -Shader $sssShader -EntryPoint 'BlurPS' -Target 'ps_5_0' `
                -Defines @{
                    SPATCH_SSS_QUALITY = "$quality"
                    SPATCH_SSS_HORIZONTAL = "$horizontal"
                    SPATCH_SSS_DEVELOPMENT = "$development"
                } -ValidationOutputPath $outputPath `
                -CacheRelativePath "SSS/BlurPS.ps_5_0.q$quality.horizontal$horizontal.development$development.cso"
            ++$shaderVariantCount
        }
    }
}

foreach ($entry in @(
        [pscustomobject]@{
            Shader = $waterMainShader
            Name = 'WaterMain'
        },
        [pscustomobject]@{
            Shader = $waterSimpleShader
            Name = 'WaterSimple'
        },
        [pscustomobject]@{
            Shader = $waterBlendShader
            Name = 'WaterBlend'
        })) {
    $outputPath = Join-Path $shaderValidationRoot "water-$($entry.Name).cso"
    $compiledWaterVariant = Invoke-RuntimeShaderVariant `
        -Shader $entry.Shader -EntryPoint 'main' -Target 'ps_4_0' `
        -Defines @{} -ValidationOutputPath $outputPath `
        -CacheRelativePath "Water/$($entry.Name).ps_4_0.cso"
    $runtimeShaderVariants += $compiledWaterVariant
    $compiledWaterVariants += $compiledWaterVariant
    ++$shaderVariantCount
}

foreach ($variant in $compiledWaterVariants) {
    $cacheName = [IO.Path]::GetFileName($variant.CacheRelativePath)
    if (-not $waterCacheIdentities.ContainsKey($cacheName)) {
        throw "Compiled Water cache $cacheName is absent from the C++ identity table."
    }
    $bytes = [IO.File]::ReadAllBytes($variant.ValidationOutputPath)
    Assert-WaterCacheIdentity -Bytes $bytes `
        -Expected $waterCacheIdentities[$cacheName] `
        -Description "Compiled Water cache $cacheName"
}
if ($compiledWaterVariants.Count -ne 3) {
    throw "Compiled Water cache coverage drifted: expected 3 entries, found $($compiledWaterVariants.Count)."
}
$negativeControlVariant = $compiledWaterVariants[0]
$negativeControlBytes = [byte[]]([IO.File]::ReadAllBytes(
    $negativeControlVariant.ValidationOutputPath).Clone())
$negativeControlBytes[$negativeControlBytes.Length - 1] =
    $negativeControlBytes[$negativeControlBytes.Length - 1] -bxor 1
$negativeControlRejected = $false
try {
    $negativeControlName = [IO.Path]::GetFileName(
        $negativeControlVariant.CacheRelativePath)
    Assert-WaterCacheIdentity -Bytes $negativeControlBytes `
        -Expected $waterCacheIdentities[$negativeControlName] `
        -Description 'Tampered Water cache negative control'
} catch {
    if ($_.Exception.Message -match 'CRC32 drifted') {
        $negativeControlRejected = $true
    } else {
        throw
    }
}
if (-not $negativeControlRejected) {
    throw 'The tampered Water cache identity negative control was not rejected.'
}
Write-Host 'Validated all 3 compiled Water cache identities against the native CRC32, byte-size, and DXBC-checksum table; rejected the tampered-byte negative control.'

# Bind the CPU physical-invariant oracles above to the actual production HLSL.
# The validation entry points include the shipped PBR/helper sources directly,
# then this native harness executes their compiled bytecode through D3D11 WARP.
$semanticValidationRoot = Join-Path $scriptRoot 'validation'
$pbrSemanticShader = Join-Path $semanticValidationRoot `
    'SPatchPbrSemanticValidation.hlsl'
$waterSemanticShader = Join-Path $semanticValidationRoot `
    'SPatchWaterSemanticValidation.hlsl'
$pbrSemanticOutput = Join-Path $shaderValidationRoot 'pbr-semantic.cso'
$waterSemanticOutput = Join-Path $shaderValidationRoot 'water-semantic.cso'
$pbrSemanticNegativeOutput = Join-Path $shaderValidationRoot `
    'pbr-semantic-negative-control.cso'
$waterSemanticNegativeOutput = Join-Path $shaderValidationRoot `
    'water-semantic-negative-control.cso'
Invoke-FxcVariant -Shader $pbrSemanticShader `
    -EntryPoint 'SPatchPbrSemanticValidationMain' -Target 'ps_4_0' `
    -Defines @{ SPATCH_SEMANTIC_NEGATIVE_CONTROL = '0' } `
    -OutputPath $pbrSemanticOutput -IeeeStrictness
Invoke-FxcVariant -Shader $waterSemanticShader `
    -EntryPoint 'SPatchWaterSemanticValidationMain' -Target 'ps_4_0' `
    -Defines @{ SPATCH_SEMANTIC_NEGATIVE_CONTROL = '0' } `
    -OutputPath $waterSemanticOutput -IeeeStrictness
Invoke-FxcVariant -Shader $pbrSemanticShader `
    -EntryPoint 'SPatchPbrSemanticValidationMain' -Target 'ps_4_0' `
    -Defines @{ SPATCH_SEMANTIC_NEGATIVE_CONTROL = '1' } `
    -OutputPath $pbrSemanticNegativeOutput -IeeeStrictness
Invoke-FxcVariant -Shader $waterSemanticShader `
    -EntryPoint 'SPatchWaterSemanticValidationMain' -Target 'ps_4_0' `
    -Defines @{ SPATCH_SEMANTIC_NEGATIVE_CONTROL = '1' } `
    -OutputPath $waterSemanticNegativeOutput -IeeeStrictness

$semanticValidationProject = Join-Path $semanticValidationRoot `
    'SPatchShaderSemanticValidation.vcxproj'
$semanticValidationExe = Join-Path $repoRoot `
    'build\shader-semantic-validation\x64-Release\SPatchShaderSemanticValidation.exe'
$semanticBuildOutput = @(& $msbuild $semanticValidationProject /m /t:Rebuild `
    /p:Configuration=Release /p:Platform=x64 /v:minimal 2>&1)
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $semanticValidationExe -PathType Leaf)) {
    throw "PBR/Water WARP semantic harness build failed: $($semanticBuildOutput -join [Environment]::NewLine)"
}
$semanticOutput = @(& $semanticValidationExe `
    $pbrSemanticOutput $waterSemanticOutput 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "PBR/Water compiled-HLSL semantic validation failed: $($semanticOutput -join [Environment]::NewLine)"
}
Write-Host ($semanticOutput -join [Environment]::NewLine)

$negativePbrOutput = @(& $semanticValidationExe `
    $pbrSemanticNegativeOutput $waterSemanticOutput 2>&1)
if ($LASTEXITCODE -eq 0 -or
    ($negativePbrOutput -join [Environment]::NewLine) -notmatch
        'PBR compiled-HLSL mismatch') {
    throw 'The PBR compiled-HLSL semantic negative control was not rejected.'
}
$negativeWaterOutput = @(& $semanticValidationExe `
    $pbrSemanticOutput $waterSemanticNegativeOutput 2>&1)
if ($LASTEXITCODE -eq 0 -or
    ($negativeWaterOutput -join [Environment]::NewLine) -notmatch
        'Water compiled-HLSL mismatch') {
    throw 'The Water compiled-HLSL semantic negative control was not rejected.'
}
Write-Host 'Rejected the zero-output PBR and Water compiled-HLSL semantic negative controls.'

if ($shaderVariantCount -ne $expectedShaderVariantCount) {
    throw "Runtime shader coverage drifted: expected $expectedShaderVariantCount variants, validated $shaderVariantCount."
}
if ($runtimeShaderVariants.Count -ne $expectedCompiledShaderCacheVariantCount) {
    $message = "Runtime shader cache coverage drifted: expected $expectedCompiledShaderCacheVariantCount PBR/GI/SDAO/SSS/Water variants, compiled $($runtimeShaderVariants.Count)."
    if ($Configuration -eq 'Publishing-Release') {
        throw "Publishing requires a complete precompiled shader cache. $message"
    }
    throw $message
}
Assert-ShaderCacheFeatureCounts -Variants $runtimeShaderVariants `
    -ExpectedCounts $expectedCompiledShaderCacheFeatureCounts `
    -Description 'Compiled runtime shader cache'
Write-Host "Validated all $expectedShaderVariantCount runtime shader variants with FXC /WX /Ges; all SDAO capture donors used exact two-target MIN-blend DXBC-shape checks and all AgX variants also used /Gis and exact reflection checks."

$selectedSssDevelopment = if ($Configuration -eq 'Development-Release') {
    '1'
} else {
    '0'
}
$packageShaderVariants = @($runtimeShaderVariants | Where-Object {
        $_.Source -cne 'SPatchSSS.hlsl' -or
        $_.EntryPoint -cne 'BlurPS' -or
        @($_.Defines.Split(';')).Contains(
            "SPATCH_SSS_DEVELOPMENT=$selectedSssDevelopment")
    })
if ($packageShaderVariants.Count -ne $expectedPackagedShaderCacheVariantCount) {
    $message = "Configuration-exact shader cache selection drifted for $Configuration (SSS development=$selectedSssDevelopment): expected $expectedPackagedShaderCacheVariantCount entries, selected $($packageShaderVariants.Count)."
    if ($Configuration -eq 'Publishing-Release') {
        throw "Publishing requires a complete precompiled shader cache. $message"
    }
    throw $message
}
Assert-ShaderCacheFeatureCounts -Variants $packageShaderVariants `
    -ExpectedCounts $expectedPackagedShaderCacheFeatureCounts `
    -Description "$Configuration packaged shader cache"

$cacheRelativePaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$packagedCacheVariants = foreach ($variant in
    @($packageShaderVariants | Sort-Object CacheRelativePath)) {
    if (-not $cacheRelativePaths.Add($variant.CacheRelativePath)) {
        throw "Runtime shader cache contains a duplicate variant path: $($variant.CacheRelativePath)"
    }
    $destination = Get-CheckedChildPath `
        -Path (Join-Path $artifactShaderCacheRoot `
            $variant.CacheRelativePath.Replace('/', '\')) `
        -Parent $artifactShaderCacheRoot `
        -Description 'packaged runtime shader cache entry'
    Copy-Item -LiteralPath $variant.ValidationOutputPath -Destination $destination -Force
    $packagedFile = Get-Item -LiteralPath $destination
    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($packagedFile.Length -ne $variant.Length -or
        $packagedHash -cne $variant.Sha256) {
        throw "Packaged shader cache bytecode drifted from its validated FXC output: $($variant.CacheRelativePath)"
    }
    [pscustomobject]@{
        CacheRelativePath = $variant.CacheRelativePath
        Source = $variant.Source
        EntryPoint = $variant.EntryPoint
        Profile = $variant.Profile
        Defines = $variant.Defines
        Length = [long]$packagedFile.Length
        Sha256 = $packagedHash
    }
}

$artifactShaderCachePrefix =
    $artifactShaderCacheRoot.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
$actualCacheRelativePaths = @(
    Get-ChildItem -LiteralPath $artifactShaderCacheRoot -Filter '*.cso' -File -Recurse |
        ForEach-Object {
            $fullPath = [IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $artifactShaderCachePrefix,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "Shader cache entry escaped the staging root: $fullPath"
            }
            $fullPath.Substring($artifactShaderCachePrefix.Length).Replace('\', '/')
        } |
        Sort-Object)
$expectedCacheRelativePaths = @(
    $packagedCacheVariants | Select-Object -ExpandProperty CacheRelativePath |
        Sort-Object)
$cacheComplete =
    $actualCacheRelativePaths.Count -eq $expectedPackagedShaderCacheVariantCount -and
    @(Compare-Object -CaseSensitive -ReferenceObject $expectedCacheRelativePaths `
        -DifferenceObject $actualCacheRelativePaths).Count -eq 0
if (-not $cacheComplete) {
    $message = "Packaged shader cache is incomplete or unexpected for ${Configuration}: expected $expectedPackagedShaderCacheVariantCount exact entries, found $($actualCacheRelativePaths.Count)."
    if ($Configuration -eq 'Publishing-Release') {
        throw "Publishing requires a complete precompiled shader cache. $message"
    }
    throw $message
}

$cacheManifestLines = @(
    "Configuration`tSSSDevelopment`tSHA256`tBytes`tPath`tSource`tEntryPoint`tProfile`tDefines"
    foreach ($variant in $packagedCacheVariants) {
        "$Configuration`t$selectedSssDevelopment`t$($variant.Sha256)`t$($variant.Length)`t$($variant.CacheRelativePath)`t$($variant.Source)`t$($variant.EntryPoint)`t$($variant.Profile)`t$($variant.Defines)"
    }
)
$cacheManifestPath = Join-Path $artifactShaderCacheRoot 'manifest.tsv'
[IO.File]::WriteAllLines(
    $cacheManifestPath,
    $cacheManifestLines,
    [Text.UTF8Encoding]::new($false))
$writtenCacheManifestLines = @([IO.File]::ReadAllLines($cacheManifestPath))
if ($writtenCacheManifestLines.Count -ne $cacheManifestLines.Count) {
    throw "Shader cache manifest row count drifted. Expected $($cacheManifestLines.Count), found $($writtenCacheManifestLines.Count)."
}
for ($index = 0; $index -lt $cacheManifestLines.Count; ++$index) {
    if ($writtenCacheManifestLines[$index] -cne $cacheManifestLines[$index]) {
        throw "Shader cache manifest row drifted at index ${index}: $($writtenCacheManifestLines[$index])"
    }
}
Write-Host "Packaged $expectedPackagedShaderCacheVariantCount byte-identical, configuration-exact PBR/GI/SDAO/SSS/Water shader cache entries under ShenLong/ShaderCache/v1 for $Configuration (SSS development=$selectedSssDevelopment) with a hashed entry/profile/macro manifest."

if ($Configuration -eq 'Development-Release') {
    Copy-Item -LiteralPath (Join-Path $shaderSource 'SPatchGI.hlsl') `
        -Destination $artifactGameGiShaders -Force
    Copy-Item -LiteralPath (Join-Path $shaderSource 'SPatchGIShared.hlsli') `
        -Destination $artifactGameGiShaders -Force
    Copy-Item -LiteralPath (Join-Path $shaderSource 'SPatchPBR.hlsl') `
        -Destination $artifactGamePbrShaders -Force
    Copy-Item -LiteralPath (Join-Path $shaderSource 'Luma_SD_SDAO.hlsl') `
        -Destination (Join-Path $artifactGameSdaoShaders 'SPatchSDAO.hlsl') -Force
    Copy-Item -LiteralPath (Join-Path $shaderSource 'SPatchSSS.hlsl') `
        -Destination $artifactGameSssShaders -Force
    foreach ($waterSource in @(
            'SPatchWaterMain.hlsl',
            'SPatchWaterSimple.hlsl',
            'SPatchWaterBlend.hlsl',
            'SPatchWaterScattering.hlsli')) {
        Copy-Item -LiteralPath (Join-Path $shaderSource $waterSource) `
            -Destination $artifactGameWaterShaders -Force
    }
}
$expectedPackagedShaderSources = @()
if ($Configuration -eq 'Development-Release') {
    $expectedPackagedShaderSources = @(
        'ShenLong/Shaders/GI/SPatchGI.hlsl',
        'ShenLong/Shaders/GI/SPatchGIShared.hlsli',
        'ShenLong/Shaders/PBR/SPatchPBR.hlsl',
        'ShenLong/Shaders/SDAO/SPatchSDAO.hlsl',
        'ShenLong/Shaders/SSS/SPatchSSS.hlsl',
        'ShenLong/Shaders/Water/SPatchWaterBlend.hlsl',
        'ShenLong/Shaders/Water/SPatchWaterMain.hlsl',
        'ShenLong/Shaders/Water/SPatchWaterScattering.hlsli',
        'ShenLong/Shaders/Water/SPatchWaterSimple.hlsl')
}
$artifactSourcePrefix =
    $artifactRoot.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
$actualPackagedShaderSources = @(
    Get-ChildItem -LiteralPath $artifactRoot -File -Recurse |
        Where-Object { $_.Extension -in @('.hlsl', '.hlsli') } |
        ForEach-Object {
            $fullPath = [IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $artifactSourcePrefix,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "Packaged shader source escaped the staging root: $fullPath"
            }
            $fullPath.Substring($artifactSourcePrefix.Length).Replace('\', '/')
        } |
        Sort-Object)
$expectedPackagedShaderSources = @($expectedPackagedShaderSources | Sort-Object)
$sourcePolicyMatches =
    $actualPackagedShaderSources.Count -eq $expectedPackagedShaderSources.Count
if ($sourcePolicyMatches -and $actualPackagedShaderSources.Count -ne 0) {
    $sourcePolicyMatches = @(Compare-Object -CaseSensitive `
            -ReferenceObject $expectedPackagedShaderSources `
            -DifferenceObject $actualPackagedShaderSources).Count -eq 0
}
if (-not $sourcePolicyMatches) {
    throw "$Configuration packaged shader-source policy drifted. Expected [$($expectedPackagedShaderSources -join ', ')], found [$($actualPackagedShaderSources -join ', ')]."
}
Copy-Item -LiteralPath $reShadeConfigPath -Destination $artifactRoot -Force
Copy-Item -LiteralPath $shenLongConfigPath -Destination $artifactRoot -Force
Copy-Item -LiteralPath $graphicsInstallerPath `
    -Destination (Join-Path $artifactRoot 'Install-ShenLong.ps1') -Force
Copy-Item -LiteralPath $reShadeIniPolicyPath `
    -Destination (Join-Path $artifactRoot 'ReShadeIniPolicy.ps1') -Force
Copy-Item -LiteralPath (Join-Path $scriptRoot 'THIRD_PARTY_NOTICES.md') `
    -Destination (Join-Path $artifactRoot 'THIRD_PARTY_NOTICES.md') -Force
foreach ($license in $requiredLicenses) {
    $sourceLicense = Join-Path $licenseRoot $license.RelativePath
    $packagedLicense = Join-Path $artifactLicenseRoot $license.RelativePath
    Copy-Item -LiteralPath $sourceLicense -Destination $packagedLicense -Force
}
Copy-Item -LiteralPath (Join-Path $scriptRoot 'README.md') -Destination (Join-Path $artifactRoot 'SHENLONG-README.md') -Force

$packagedInstallerPath = Join-Path $artifactRoot 'Install-ShenLong.ps1'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $graphicsInstallerPath).Hash -cne
    (Get-FileHash -Algorithm SHA256 -LiteralPath $packagedInstallerPath).Hash) {
    throw 'Packaged graphics installer does not match the reviewed repository installer.'
}
$packagedReShadePolicyPath = Join-Path $artifactRoot 'ReShadeIniPolicy.ps1'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $reShadeIniPolicyPath).Hash -cne
    (Get-FileHash -Algorithm SHA256 -LiteralPath $packagedReShadePolicyPath).Hash) {
    throw 'Packaged ReShade INI policy does not match the reviewed repository policy.'
}

Assert-ExactLicenseFileSet -Path $artifactLicenseRoot `
    -ExpectedNames $requiredLicenseRelativePaths `
    -Description 'Packaged graphics license directory'
foreach ($license in $requiredLicenses) {
    $sourceLicense = Join-Path $licenseRoot $license.RelativePath
    $packagedLicense = Join-Path $artifactLicenseRoot $license.RelativePath
    if (-not (Test-Path -LiteralPath $packagedLicense -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceLicense).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $packagedLicense).Hash) {
        throw "Graphics package is missing the exact $($license.Name) license."
    }
}

$packagedBinaryPairs = @(
    [pscustomobject]@{
        Name = 'ShenLong ASI module'
        Source = $graphicsAddon
        Packaged = Join-Path $artifactRoot 'ShenLong.asi'
    },
    [pscustomobject]@{
        Name = 'ReShade runtime'
        Source = $reShadeRuntimePath
        Packaged = Join-Path $artifactRoot 'dxgi.dll'
    }
)
foreach ($pair in $packagedBinaryPairs) {
    if (-not (Test-Path -LiteralPath $pair.Packaged -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $pair.Source).Hash -cne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $pair.Packaged).Hash) {
        throw "The staged graphics package is missing the exact $($pair.Name)."
    }
}

$packagedAddonNames = @(Get-ChildItem -LiteralPath $artifactRoot -Filter '*.addon*' -File -Recurse |
    Sort-Object Name | Select-Object -ExpandProperty Name)
$packagedAsiNames = @(Get-ChildItem -LiteralPath $artifactRoot -Filter '*.asi' -File -Recurse |
    Sort-Object Name | Select-Object -ExpandProperty Name)
$expectedAsiNames = @('ShenLong.asi')
$forbiddenAddonNames = @(
    'SPatchGI.addon',
    'SPatchGTAO.addon',
    'SPatchPBR.addon',
    'SPatchSDAO.addon',
    'SPatchSSS.addon',
    'SPatchWater.addon')
if (@($packagedAddonNames | Where-Object { $_ -in $forbiddenAddonNames }).Count -ne 0 -or
    $packagedAddonNames.Count -ne 0 -or
    $packagedAsiNames.Count -ne $expectedAsiNames.Count -or
    @(Compare-Object -ReferenceObject $expectedAsiNames -DifferenceObject $packagedAsiNames).Count -ne 0 -or
    (Test-Path -LiteralPath (Join-Path $artifactRoot 'Luma')) -or
    (Test-Path -LiteralPath (Join-Path $artifactRoot 'SPatch'))) {
    throw 'Graphics package validation rejected stale or unexpected framework files.'
}

$artifactRootPrefix = $artifactRoot.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
$manifestLines = foreach ($file in Get-ChildItem -LiteralPath $artifactRoot -File -Recurse | Sort-Object FullName) {
    $fullPath = [IO.Path]::GetFullPath($file.FullName)
    if (-not $fullPath.StartsWith($artifactRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to manifest a file outside the graphics package: $fullPath"
    }
    $relativePath = $fullPath.Substring($artifactRootPrefix.Length).Replace('\', '/')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    "$hash *$relativePath"
}
$manifestPath = Join-Path $artifactRoot 'SHA256SUMS.txt'
[IO.File]::WriteAllLines(
    $manifestPath,
    $manifestLines,
    [Text.UTF8Encoding]::new($false))

$writtenManifestLines = @([IO.File]::ReadAllLines($manifestPath))
if ($writtenManifestLines.Count -ne $manifestLines.Count) {
    throw "Graphics package manifest entry count drifted. Expected $($manifestLines.Count), found $($writtenManifestLines.Count)."
}
$manifestPaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
for ($index = 0; $index -lt $manifestLines.Count; ++$index) {
    $line = $writtenManifestLines[$index]
    if ($line -cne $manifestLines[$index] -or
        $line -notmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>.+)$') {
        throw "Graphics package manifest entry is malformed or out of order: $line"
    }
    $expectedHash = $Matches['hash']
    $relativePath = $Matches['path']
    if (-not $manifestPaths.Add($relativePath)) {
        throw "Graphics package manifest contains a duplicate path: $relativePath"
    }
    $manifestedPath = Get-CheckedChildPath `
        -Path (Join-Path $artifactRoot $relativePath.Replace('/', '\')) `
        -Parent $artifactRoot -Description 'manifested graphics package file'
    if (-not (Test-Path -LiteralPath $manifestedPath -PathType Leaf)) {
        throw "Graphics package manifest references a missing file: $relativePath"
    }
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestedPath).Hash
    if ($actualHash -cne $expectedHash) {
        throw "Graphics package manifest hash mismatch for ${relativePath}: expected $expectedHash, found $actualHash."
    }
}

$hashRelativePaths = @(
    'ShenLong.asi',
    'ShenLong.ini',
    'dxgi.dll',
    'SHA256SUMS.txt'
)
$validatedHashes = foreach ($relativePath in $hashRelativePaths) {
    $stagedPath = Join-Path $artifactRoot $relativePath
    [pscustomobject]@{
        Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedPath).Hash
        RelativePath = $relativePath
    }
}

Publish-ValidatedDirectory -StagingPath $artifactStagingRoot -ActivePath $activeArtifactRoot `
    -Parent $allowedArtifactRoot

$releaseArchive = $null
if ($Configuration -eq 'Publishing-Release') {
    $releaseArchive = Publish-DeterministicZip `
        -SourceDirectory $activeArtifactRoot `
        -DestinationPath (Join-Path $allowedArtifactRoot 'ShenLong.zip') `
        -Parent $allowedArtifactRoot
}

foreach ($validatedHash in $validatedHashes) {
    [pscustomobject]@{
        Hash = $validatedHash.Hash
        Path = Join-Path (Join-Path $activeArtifactRoot 'ShenLong-Package') `
            $validatedHash.RelativePath
    }
}
if ($null -ne $releaseArchive) {
    $releaseArchive
}
} finally {
    if (Test-Path -LiteralPath $artifactStagingRoot) {
        try {
            Remove-CheckedDirectory -Path $artifactStagingRoot -Parent $allowedArtifactRoot `
                -Description 'graphics package staging directory'
        } catch {
            Write-Warning "Could not remove graphics package staging directory $artifactStagingRoot ($($_.Exception.Message))"
        }
    }
}
} finally {
    if ($ownsBuildMutex) {
        $buildMutex.ReleaseMutex()
    }
    $buildMutex.Dispose()
}
