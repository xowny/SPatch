[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ArtifactPath,
    [Parameter(Mandatory)]
    [string] $FinalReleaseIdentityPath,
    [Parameter(Mandatory)]
    [string] $GameRoot,
    [Parameter(Mandatory)]
    [string] $BuildInfoPath,
    [Parameter(Mandatory)]
    [string] $AsiLoaderPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-NoReparsePath(
    [string] $Path,
    [string] $Label) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $pathRoot = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot)) {
        throw "$Label has no filesystem root: $fullPath"
    }
    $cursor = $pathRoot
    foreach ($component in @($fullPath.Substring($pathRoot.Length) `
            -split '[\\/]' | Where-Object {
                -not [string]::IsNullOrEmpty($_)
            })) {
        $cursor = Join-Path $cursor $component
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -eq $item) {
            break
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Label contains a reparse point: $cursor"
        }
    }
    return $fullPath
}

function Assert-NoReparseTree([string] $Root, [string] $Label) {
    $rootFull = Assert-NoReparsePath $Root $Label
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) {
        throw "$Label is not a directory: $rootFull"
    }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootFull)
    while ($pending.Count -ne 0) {
        $directory = $pending.Pop()
        foreach ($entryPath in [IO.Directory]::EnumerateFileSystemEntries(
                $directory)) {
            $entry = Get-Item -LiteralPath $entryPath -Force
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Label contains a reparse point: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) {
                $pending.Push($entry.FullName)
            }
        }
    }
}

function Get-Sha256([string] $Path, [string] $Label = 'Required file') {
    [void](Assert-NoReparsePath $Path $Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
                $sha256.ComputeHash($stream))).Replace('-', '')
    } finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Get-ExactChildPath(
    [string] $Root,
    [string] $LeafName,
    [string] $Label) {
    if ([string]::IsNullOrWhiteSpace($LeafName) -or
        [IO.Path]::GetFileName($LeafName) -cne $LeafName) {
        throw "$Label is not an exact leaf name: $LeafName"
    }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $path = [IO.Path]::GetFullPath((Join-Path $rootFull $LeafName))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $path.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its root: $LeafName"
    }
    [void](Assert-NoReparsePath $path $Label)
    return $path
}

function Get-PeIdentity([string] $Path, [string] $Description) {
    [void](Assert-NoReparsePath $Path $Description)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x100 -or $bytes[0] -ne 0x4D -or
        $bytes[1] -ne 0x5A) {
        throw "$Description is not a valid PE image: $Path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 26 -gt $bytes.Length -or
        $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "$Description has an invalid PE header: $Path"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $optionalMagic -ne 0x20B) {
        throw "$Description must be a native x64 PE32+ image: $Path"
    }
    return [pscustomobject]@{
        Machine = ('{0:X4}' -f $machine)
        OptionalMagic = ('{0:X4}' -f $optionalMagic)
    }
}

function Read-StrictKeyValueFile(
    [string] $Path,
    [string[]] $ExpectedKeys,
    [string] $Description) {
    [void](Assert-NoReparsePath $Path $Description)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $values = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines($Path, $encoding)) {
        if ($line -cnotmatch '^(?<key>[A-Z0-9_]+)=(?<value>[^\r\n]+)$') {
            throw "Malformed $Description line: $line"
        }
        if ($values.ContainsKey($Matches['key'])) {
            throw "Duplicate $Description key: $($Matches['key'])"
        }
        $values.Add($Matches['key'], $Matches['value'])
    }
    if ($values.Count -ne $ExpectedKeys.Count) {
        throw "$Description must contain exactly $($ExpectedKeys.Count) keys."
    }
    foreach ($key in $ExpectedKeys) {
        if (-not $values.ContainsKey($key)) {
            throw "$Description omits $key."
        }
    }
    return $values
}

function Copy-FileDurable([string] $Source, [string] $Destination) {
    $sourceStream = [IO.File]::Open(
        $Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $destinationStream = [IO.File]::Open(
            $Destination,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try {
            $sourceStream.CopyTo($destinationStream)
            $destinationStream.Flush($true)
        } finally {
            $destinationStream.Dispose()
        }
    } finally {
        $sourceStream.Dispose()
    }
}

function Remove-SafeLeaf([string] $Path, [string] $Label) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    [void](Assert-NoReparsePath $Path $Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is not a file: $Path"
    }
    Remove-Item -LiteralPath $Path -Force
}

function Write-DurableJournal(
    [Collections.Generic.Dictionary[string, string]] $Values) {
    $journalLines = foreach ($key in @(
            'VERSION', 'TRANSACTION', 'STATE', 'STAGING', 'SOURCE_SHA256',
            'HAD_DESTINATION', 'OLD_DESTINATION_SHA256', 'HAD_PDB',
            'OLD_PDB_SHA256')) {
        "$key=$($Values[$key])"
    }
    $text = ($journalLines -join [Environment]::NewLine) +
            [Environment]::NewLine
    $temporaryJournal = Get-ExactChildPath $GameRoot `
        ".SPatch.deploy-journal.tmp-$($Values['TRANSACTION'])" `
        'Deployment journal staging path'
    Remove-SafeLeaf $temporaryJournal 'Deployment journal staging cleanup'
    $stream = [IO.File]::Open(
        $temporaryJournal,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    if (Test-Path -LiteralPath $journalPath -PathType Leaf) {
        $journalBackup = Get-ExactChildPath $GameRoot `
            ".SPatch.deploy-journal.replace-backup-$($Values['TRANSACTION'])" `
            'Deployment journal replacement backup'
        Remove-SafeLeaf $journalBackup `
            'Deployment journal replacement backup cleanup'
        [IO.File]::Replace(
            $temporaryJournal, $journalPath, $journalBackup, $true)
        Remove-SafeLeaf $journalBackup `
            'Deployment journal replacement backup cleanup'
    } elseif (Test-Path -LiteralPath $journalPath) {
        throw "Deployment journal path is not a file: $journalPath"
    } else {
        [IO.File]::Move($temporaryJournal, $journalPath)
    }
}

function Read-DeploymentJournal {
    $values = Read-StrictKeyValueFile -Path $journalPath -ExpectedKeys @(
        'VERSION', 'TRANSACTION', 'STATE', 'STAGING', 'SOURCE_SHA256',
        'HAD_DESTINATION', 'OLD_DESTINATION_SHA256', 'HAD_PDB',
        'OLD_PDB_SHA256') -Description 'Deployment recovery journal'
    if ($values['VERSION'] -cne '1' -or
        $values['TRANSACTION'] -cnotmatch '^[0-9a-f]{32}$' -or
        $values['STATE'] -cnotin @(
            'PREPARED', 'ASI_REPLACED', 'PDB_BACKED_UP', 'COMMITTED') -or
        $values['STAGING'] -cnotmatch
            '^\.SPatch\.asi\.deploy-staging-[0-9]+-[0-9a-f]{32}$' -or
        $values['SOURCE_SHA256'] -cnotmatch '^[0-9A-F]{64}$' -or
        $values['HAD_DESTINATION'] -cnotin @('0', '1') -or
        $values['HAD_PDB'] -cnotin @('0', '1') -or
        $values['OLD_DESTINATION_SHA256'] -cnotmatch '^(NONE|[0-9A-F]{64})$' -or
        $values['OLD_PDB_SHA256'] -cnotmatch '^(NONE|[0-9A-F]{64})$' -or
        (($values['HAD_DESTINATION'] -ceq '0') -ne
            ($values['OLD_DESTINATION_SHA256'] -ceq 'NONE')) -or
        (($values['HAD_PDB'] -ceq '0') -ne
            ($values['OLD_PDB_SHA256'] -ceq 'NONE'))) {
        throw 'Deployment recovery journal has an invalid contract.'
    }
    return $values
}

function Restore-ReplacedFile(
    [string] $Backup,
    [string] $Destination,
    [string] $ExpectedOldHash,
    [string] $RecoveryDiscard) {
    if (Test-Path -LiteralPath $Backup -PathType Leaf) {
        if (Test-Path -LiteralPath $Destination -PathType Leaf) {
            Remove-SafeLeaf $RecoveryDiscard 'Recovery discard cleanup'
            [IO.File]::Replace($Backup, $Destination, $RecoveryDiscard, $true)
            Remove-SafeLeaf $RecoveryDiscard 'Recovery discard cleanup'
        } elseif (Test-Path -LiteralPath $Destination) {
            throw "Recovery destination is not a file: $Destination"
        } else {
            [IO.File]::Move($Backup, $Destination)
        }
    }
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf) -or
        (Get-Sha256 $Destination 'Recovered deployment file') -cne
            $ExpectedOldHash) {
        throw "Could not recover the prior deployment file: $Destination"
    }
}

function Remove-JournalTemps {
    foreach ($item in @(Get-ChildItem -LiteralPath $GameRoot -Force -File |
            Where-Object {
                $_.Name -match
                    '^\.SPatch\.deploy-journal\.(?:tmp|replace-backup)-[0-9a-f]{32}$'
            })) {
        Remove-SafeLeaf $item.FullName `
            'Deployment journal staging/backup cleanup'
    }
}

function Invoke-DeploymentRecovery {
    if (-not (Test-Path -LiteralPath $journalPath)) {
        foreach ($orphan in @($asiBackup, $pdbBackup)) {
            if (Test-Path -LiteralPath $orphan) {
                throw ('Deployment backup exists without its recovery journal: ' +
                       $orphan)
            }
        }
        Remove-JournalTemps
        return
    }
    $journal = Read-DeploymentJournal
    $staging = Get-ExactChildPath $GameRoot $journal['STAGING'] `
        'Journal deployment staging path'
    $discard = Get-ExactChildPath $GameRoot `
        ".SPatch.recovery-discard-$($journal['TRANSACTION'])" `
        'Deployment recovery discard path'

    if ($journal['STATE'] -ceq 'COMMITTED') {
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or
            (Get-Sha256 $destination 'Committed deployed SPatch.asi') -cne
                $journal['SOURCE_SHA256'] -or
            (Test-Path -LiteralPath $pdbDestination)) {
            throw 'Committed deployment recovery state no longer matches the game root.'
        }
    } else {
        if ($journal['HAD_DESTINATION'] -ceq '1') {
            Restore-ReplacedFile -Backup $asiBackup -Destination $destination `
                -ExpectedOldHash $journal['OLD_DESTINATION_SHA256'] `
                -RecoveryDiscard $discard
        } else {
            if (Test-Path -LiteralPath $asiBackup) {
                throw 'Unexpected ASI backup exists for a previously absent destination.'
            }
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                if ((Get-Sha256 $destination 'Uncommitted deployed SPatch.asi') `
                        -cne $journal['SOURCE_SHA256']) {
                    throw 'Uncommitted deployment destination has unknown bytes.'
                }
                Remove-SafeLeaf $destination 'Uncommitted deployment rollback'
            } elseif (Test-Path -LiteralPath $destination) {
                throw 'Uncommitted deployment destination is not a file.'
            }
        }

        if ($journal['HAD_PDB'] -ceq '1') {
            Restore-ReplacedFile -Backup $pdbBackup -Destination $pdbDestination `
                -ExpectedOldHash $journal['OLD_PDB_SHA256'] `
                -RecoveryDiscard $discard
        } elseif (Test-Path -LiteralPath $pdbBackup) {
            throw 'Unexpected PDB backup exists for a previously absent PDB.'
        }
    }

    Remove-SafeLeaf $staging 'Deployment staging recovery cleanup'
    Remove-SafeLeaf $asiBackup 'Deployment ASI backup cleanup'
    Remove-SafeLeaf $pdbBackup 'Deployment PDB backup cleanup'
    Remove-SafeLeaf $discard 'Deployment recovery discard cleanup'
    Remove-JournalTemps
    Remove-SafeLeaf $journalPath 'Deployment journal cleanup'
}

$ArtifactPath = [IO.Path]::GetFullPath($ArtifactPath)
$FinalReleaseIdentityPath = [IO.Path]::GetFullPath($FinalReleaseIdentityPath)
$GameRoot = [IO.Path]::GetFullPath($GameRoot).TrimEnd([char[]]'\/')
$BuildInfoPath = [IO.Path]::GetFullPath($BuildInfoPath)
$AsiLoaderPath = [IO.Path]::GetFullPath($AsiLoaderPath)
if (-not (Test-Path -LiteralPath $GameRoot -PathType Container)) {
    throw "Deployment game root is missing: $GameRoot"
}
[void](Assert-NoReparsePath $GameRoot 'Deployment game root')

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $scriptRoot))
$expectedArtifactPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\Release\SPatch.asi'))
$expectedIdentityPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\Release\SPatch.final-release.sha256'))
$expectedBuildInfoPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'src\BuildInfo.h'))
if (-not $ArtifactPath.Equals(
        $expectedArtifactPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Deployment source is not the canonical Release artifact: $ArtifactPath"
}
if (-not $FinalReleaseIdentityPath.Equals(
        $expectedIdentityPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "FinalRelease identity is not the canonical Release receipt: $FinalReleaseIdentityPath"
}
if (-not $BuildInfoPath.Equals(
        $expectedBuildInfoPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Deployment BuildInfo path is not repository-owned: $BuildInfoPath"
}
$loaderParent = [IO.Path]::GetDirectoryName($AsiLoaderPath).TrimEnd([char[]]'\/')
if (-not $loaderParent.Equals(
        $GameRoot, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($AsiLoaderPath) -cne 'dinput8.dll') {
    throw "The ASI loader must be dinput8.dll directly inside the game root: $AsiLoaderPath"
}

$deployValidator = Join-Path $scriptRoot 'Test-SPatchDeployTarget.ps1'
$gameExe = Get-ExactChildPath $GameRoot 'sdhdship.exe' 'Game executable'
$destination = Get-ExactChildPath $GameRoot 'SPatch.asi' 'Deployment destination'
$pdbDestination = Get-ExactChildPath $GameRoot 'SPatch.pdb' 'PDB destination'
$journalPath = Get-ExactChildPath $GameRoot '.SPatch.deploy-journal' `
    'Deployment journal path'
$asiBackup = Get-ExactChildPath $GameRoot '.SPatch.asi.deploy-backup' `
    'Deployment ASI backup path'
$pdbBackup = Get-ExactChildPath $GameRoot '.SPatch.pdb.deploy-backup' `
    'Deployment PDB backup path'

$rootKeyAlgorithm = [Security.Cryptography.SHA256]::Create()
try {
    $rootKey = ([BitConverter]::ToString($rootKeyAlgorithm.ComputeHash(
        [Text.Encoding]::UTF8.GetBytes($GameRoot.ToUpperInvariant())))).Replace(
            '-', '').Substring(0, 24)
} finally {
    $rootKeyAlgorithm.Dispose()
}
$sharedMutex = [Threading.Mutex]::new(
    $false, 'Local\SPatch.LiveGraphicsHarness')
$rootMutex = [Threading.Mutex]::new(
    $false, "Local\SPatch.BaseDeploy.$rootKey")
$ownsSharedMutex = $false
$ownsRootMutex = $false
$primaryError = $null
$cleanupErrors = [Collections.Generic.List[Exception]]::new()
$result = $null
$transactionStarted = $false
$staging = $null

try {
    try {
        $ownsSharedMutex = $sharedMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $ownsSharedMutex = $true
    }
    if (-not $ownsSharedMutex) {
        throw 'Another SPatch live mutation already owns a game installation.'
    }
    try {
        $ownsRootMutex = $rootMutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $ownsRootMutex = $true
    }
    if (-not $ownsRootMutex) {
        throw 'Another SPatch base deployment already owns this game root.'
    }

    Invoke-DeploymentRecovery
    if (Get-Process -Name sdhdship -ErrorAction SilentlyContinue) {
        throw 'Sleeping Dogs is running; SPatch.asi was not deployed.'
    }

    foreach ($path in @(
            $ArtifactPath, $FinalReleaseIdentityPath, $gameExe, $BuildInfoPath,
            $AsiLoaderPath, $deployValidator)) {
        [void](Get-Sha256 $path 'Deployment prerequisite')
    }
    foreach ($path in @($destination, $pdbDestination)) {
        if (Test-Path -LiteralPath $path) {
            [void](Assert-NoReparsePath $path 'Deployment destination')
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "Deployment destination is not a file: $path"
            }
        }
    }

    $receiptKeys = @(
        'SPATCH_FINAL_RELEASE', 'CONFIGURATION', 'PLATFORM', 'PE_MACHINE',
        'PE_OPTIONAL_MAGIC', 'FINAL_POLICY_ATTESTED', 'SHA256', 'FILE',
        'TEST_MODES', 'PACKAGE_DIR', 'PACKAGE', 'PACKAGE_SHA256',
        'PACKAGE_ASI', 'PACKAGE_ASI_SHA256', 'DEFAULT_INI',
        'DEFAULT_INI_SHA256', 'MANIFEST', 'MANIFEST_SHA256')
    $receipt = Read-StrictKeyValueFile -Path $FinalReleaseIdentityPath `
        -ExpectedKeys $receiptKeys -Description 'FinalRelease identity'
    if ($receipt['SPATCH_FINAL_RELEASE'] -cne '1' -or
        $receipt['CONFIGURATION'] -cne 'Release' -or
        $receipt['PLATFORM'] -cne 'x64' -or
        $receipt['PE_MACHINE'] -cne '8664' -or
        $receipt['PE_OPTIONAL_MAGIC'] -cne '020B' -or
        $receipt['FINAL_POLICY_ATTESTED'] -cne '1' -or
        $receipt['FILE'] -cne 'SPatch.asi' -or
        $receipt['TEST_MODES'] -cne 'normal,SPATCH_FINAL_RELEASE' -or
        $receipt['PACKAGE_DIR'] -cne 'SPatch-Base' -or
        $receipt['PACKAGE'] -cne 'SPatch-Base.zip' -or
        $receipt['PACKAGE_ASI'] -cne 'SPatch-Base/SPatch.asi' -or
        $receipt['DEFAULT_INI'] -cne 'SPatch-Base/SPatch.ini' -or
        $receipt['MANIFEST'] -cne 'SPatch-Base/SHA256SUMS.txt') {
        throw 'FinalRelease identity has an invalid canonical deployment contract.'
    }
    foreach ($key in @(
            'SHA256', 'PACKAGE_SHA256', 'PACKAGE_ASI_SHA256',
            'DEFAULT_INI_SHA256', 'MANIFEST_SHA256')) {
        if ($receipt[$key] -cnotmatch '^[0-9A-F]{64}$') {
            throw "FinalRelease identity has an invalid $key value."
        }
    }
    if ($receipt['SHA256'] -cne $receipt['PACKAGE_ASI_SHA256']) {
        throw 'FinalRelease identity does not bind build and packaged ASI bytes.'
    }

    $packageRoot = [IO.Path]::GetFullPath(
        (Join-Path $repoRoot 'artifacts\release\SPatch-Base'))
    $packageArchive = [IO.Path]::GetFullPath(
        (Join-Path $repoRoot 'artifacts\release\SPatch-Base.zip'))
    $packageAsi = Join-Path $packageRoot 'SPatch.asi'
    $packageIni = Join-Path $packageRoot 'SPatch.ini'
    $packageManifest = Join-Path $packageRoot 'SHA256SUMS.txt'
    [void](Assert-NoReparsePath $packageRoot 'Canonical package directory')
    if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
        throw "Canonical package directory is missing: $packageRoot"
    }
    Assert-NoReparseTree $packageRoot 'Canonical package directory'
    $boundHashes = [ordered]@{
        $ArtifactPath = $receipt['SHA256']
        $packageArchive = $receipt['PACKAGE_SHA256']
        $packageAsi = $receipt['PACKAGE_ASI_SHA256']
        $packageIni = $receipt['DEFAULT_INI_SHA256']
        $packageManifest = $receipt['MANIFEST_SHA256']
    }
    foreach ($entry in $boundHashes.GetEnumerator()) {
        if ((Get-Sha256 $entry.Key 'Receipt-bound release asset') -cne
            $entry.Value) {
            throw "Receipt-bound release asset changed: $($entry.Key)"
        }
    }
    $expectedPackageFiles = @(
        'licenses/MinHook-BSD-2-Clause.txt', 'licenses/SMAA-MIT.txt',
        'README.md', 'SHA256SUMS.txt', 'SPatch.asi', 'SPatch.ini',
        'THIRD_PARTY_NOTICES.md')
    $packagePrefix = $packageRoot.TrimEnd([char[]]'\/') +
                     [IO.Path]::DirectorySeparatorChar
    $packageFiles = @(Get-ChildItem -LiteralPath $packageRoot -File -Recurse |
        ForEach-Object {
            $_.FullName.Substring($packagePrefix.Length).Replace('\', '/')
        } | Sort-Object)
    if ($packageFiles.Count -ne $expectedPackageFiles.Count -or
        @(Compare-Object -CaseSensitive -ReferenceObject $expectedPackageFiles `
            -DifferenceObject $packageFiles).Count -ne 0) {
        throw 'Canonical SPatch-Base directory has a missing or unexpected file.'
    }
    $manifestEntries = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines($packageManifest)) {
        if ($line -cnotmatch '^(?<hash>[0-9A-F]{64}) \*(?<path>[^\r\n]+)$' -or
            $manifestEntries.ContainsKey($Matches['path'])) {
            throw "Canonical package manifest has an invalid line: $line"
        }
        $manifestEntries.Add($Matches['path'], $Matches['hash'])
    }
    foreach ($relative in @($expectedPackageFiles | Where-Object {
                $_ -cne 'SHA256SUMS.txt'
            })) {
        if (-not $manifestEntries.ContainsKey($relative) -or
            (Get-Sha256 (Join-Path $packageRoot $relative.Replace('/', '\')) `
                'Manifested package asset') -cne $manifestEntries[$relative]) {
            throw "Canonical package manifest does not bind $relative."
        }
    }
    if ($manifestEntries.Count -ne ($expectedPackageFiles.Count - 1)) {
        throw 'Canonical package manifest contains an unexpected entry.'
    }

    $artifactIdentity = Get-PeIdentity $ArtifactPath 'FinalRelease SPatch.asi'
    $packageIdentity = Get-PeIdentity $packageAsi 'Packaged SPatch.asi'
    if ($artifactIdentity.Machine -cne $receipt['PE_MACHINE'] -or
        $artifactIdentity.OptionalMagic -cne $receipt['PE_OPTIONAL_MAGIC'] -or
        $packageIdentity.Machine -cne $artifactIdentity.Machine -or
        $packageIdentity.OptionalMagic -cne $artifactIdentity.OptionalMagic) {
        throw 'Receipt-bound SPatch PE identity changed.'
    }

    & $deployValidator -GameExecutablePath $gameExe `
        -BuildInfoPath $BuildInfoPath -AsiLoaderPath $AsiLoaderPath | Out-Null

    $sourceHash = Get-Sha256 $ArtifactPath 'FinalRelease SPatch.asi'
    $serial = '{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N')
    $transactionId = [Guid]::NewGuid().ToString('N')
    $staging = Get-ExactChildPath $GameRoot `
        ".SPatch.asi.deploy-staging-$serial" 'Deployment staging path'
    if ((Test-Path -LiteralPath $staging) -or
        (Test-Path -LiteralPath $asiBackup) -or
        (Test-Path -LiteralPath $pdbBackup) -or
        (Test-Path -LiteralPath $journalPath)) {
        throw 'Deployment transaction paths were not clean after recovery.'
    }
    Copy-FileDurable $ArtifactPath $staging
    if ((Get-Sha256 $staging 'Staged deployment artifact') -cne $sourceHash) {
        throw 'The staged deployment artifact changed during durable copy.'
    }

    $hadDestination = Test-Path -LiteralPath $destination -PathType Leaf
    $hadPdb = Test-Path -LiteralPath $pdbDestination -PathType Leaf
    $journalValues = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::Ordinal)
    $journalValues['VERSION'] = '1'
    $journalValues['TRANSACTION'] = $transactionId
    $journalValues['STATE'] = 'PREPARED'
    $journalValues['STAGING'] = [IO.Path]::GetFileName($staging)
    $journalValues['SOURCE_SHA256'] = $sourceHash
    $journalValues['HAD_DESTINATION'] = if ($hadDestination) { '1' } else { '0' }
    $journalValues['OLD_DESTINATION_SHA256'] = if ($hadDestination) {
        Get-Sha256 $destination 'Prior deployed SPatch.asi'
    } else { 'NONE' }
    $journalValues['HAD_PDB'] = if ($hadPdb) { '1' } else { '0' }
    $journalValues['OLD_PDB_SHA256'] = if ($hadPdb) {
        Get-Sha256 $pdbDestination 'Prior deployed SPatch.pdb'
    } else { 'NONE' }
    $transactionStarted = $true
    Write-DurableJournal $journalValues

    if ($hadDestination) {
        [IO.File]::Replace($staging, $destination, $asiBackup, $true)
    } else {
        [IO.File]::Move($staging, $destination)
    }
    $journalValues['STATE'] = 'ASI_REPLACED'
    Write-DurableJournal $journalValues
    if ($hadPdb) {
        [IO.File]::Move($pdbDestination, $pdbBackup)
    }
    $journalValues['STATE'] = 'PDB_BACKED_UP'
    Write-DurableJournal $journalValues

    if ((Get-Sha256 $destination 'Deployed SPatch.asi') -cne $sourceHash -or
        (Test-Path -LiteralPath $pdbDestination)) {
        throw 'Deployed outputs do not match the FinalRelease transaction.'
    }
    $journalValues['STATE'] = 'COMMITTED'
    Write-DurableJournal $journalValues
    Remove-SafeLeaf $asiBackup 'Committed ASI backup cleanup'
    Remove-SafeLeaf $pdbBackup 'Committed PDB backup cleanup'
    Remove-SafeLeaf $staging 'Committed staging cleanup'
    Remove-JournalTemps
    Remove-SafeLeaf $journalPath 'Committed deployment journal cleanup'
    $transactionStarted = $false

    $result = [pscustomobject]@{
        Source = $ArtifactPath
        Destination = $destination
        Sha256 = $sourceHash
        RemovedStalePdb = $hadPdb
        MutationPerformed = $true
        RecoveryJournal = $journalPath
    }
} catch {
    $primaryError = $_
    if ($ownsRootMutex -and $transactionStarted) {
        try {
            Invoke-DeploymentRecovery
            $transactionStarted = $false
        } catch {
            $cleanupErrors.Add([InvalidOperationException]::new(
                "Deployment recovery failed: $($_.Exception.Message)",
                $_.Exception))
        }
    }
    if ($ownsRootMutex -and $null -ne $staging -and
        -not (Test-Path -LiteralPath $journalPath)) {
        try {
            Remove-SafeLeaf $staging 'Failed deployment staging cleanup'
        } catch {
            $cleanupErrors.Add([InvalidOperationException]::new(
                "Deployment staging cleanup failed: $($_.Exception.Message)",
                $_.Exception))
        }
    }
} finally {
    if ($ownsRootMutex) {
        try { $rootMutex.ReleaseMutex() } catch { $cleanupErrors.Add($_.Exception) }
    }
    try { $rootMutex.Dispose() } catch { $cleanupErrors.Add($_.Exception) }
    if ($ownsSharedMutex) {
        try { $sharedMutex.ReleaseMutex() } catch { $cleanupErrors.Add($_.Exception) }
    }
    try { $sharedMutex.Dispose() } catch { $cleanupErrors.Add($_.Exception) }
}

if ($null -ne $primaryError) {
    if ($cleanupErrors.Count -eq 0) {
        throw $primaryError
    }
    $failures = [Collections.Generic.List[Exception]]::new()
    $failures.Add($primaryError.Exception)
    foreach ($cleanupError in $cleanupErrors) {
        $failures.Add($cleanupError)
    }
    throw [AggregateException]::new(
        "Deployment failed: $($primaryError.Exception.Message)", $failures)
}
if ($cleanupErrors.Count -ne 0) {
    throw [AggregateException]::new(
        'Deployment succeeded but lock cleanup failed.', $cleanupErrors)
}
$result
