[CmdletBinding()]
param(
    [string] $RepoRoot = '',
    [string] $MinHookRoot = '',
    [switch] $Offline,
    [switch] $Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $scriptRoot
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot).TrimEnd([char[]]'\/')
if (-not (Test-Path -LiteralPath $RepoRoot -PathType Container)) {
    throw "Repository root is missing: $RepoRoot"
}

$minHookArtifactCommit = 'd5d8e1d67ddcea89fbef656b85052a5845dd34ee'
$sourceBase = "https://raw.githubusercontent.com/SDmodding/SDK/$minHookArtifactCommit/contrib"
if ([string]::IsNullOrWhiteSpace($MinHookRoot)) {
    $MinHookRoot = Join-Path $RepoRoot ".tmp\SDmodding-MinHook-$minHookArtifactCommit"
}
$MinHookRoot = [IO.Path]::GetFullPath($MinHookRoot).TrimEnd([char[]]'\/')

# SDmodding ships a modified x64 prebuilt with a reduced header. The SDK
# snapshot does not contain the matching fork source, so these exact public
# artifacts, rather than canonical MinHook, are the build authority.
$requiredFiles = @(
    [pscustomobject]@{
        Name = 'MinHook.h'
        Sha256 = 'F2642BB69230017E52F8FE2F1208F6FDEA146302CC670E2003D2A69B5AE860E8'
    },
    [pscustomobject]@{
        Name = 'MinHook.lib'
        Sha256 = 'DCF47C6ACDA033310E7C0FA3F7EE6E6C7F89AEA9F8C043D714A39FA01A5FECE2'
    })

function Get-ValidatedChildPath(
    [string] $Root,
    [string] $RelativePath,
    [string] $Label) {
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        @($RelativePath -split '\\' | Where-Object {
                $_ -ceq '..' -or $_ -ceq '.' -or
                [string]::IsNullOrWhiteSpace($_)
            }).Count -ne 0) {
        throw "$Label contains an unsafe relative path: $RelativePath"
    }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $path = [IO.Path]::GetFullPath((Join-Path $rootFull $RelativePath))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $path.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its root: $RelativePath"
    }
    return $path
}

function Get-Sha256([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    $stream = [IO.File]::OpenRead($Path)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $sha256.ComputeHash($stream))).Replace('-', '')
    } finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Assert-PinnedMinHookRoot([string] $Root) {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Pinned SDmodding MinHook root is missing: $Root"
    }
    foreach ($file in $requiredFiles) {
        $path = Get-ValidatedChildPath $Root $file.Name `
            'Pinned MinHook dependency'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Pinned MinHook dependency is missing: $path"
        }
        $actualHash = Get-Sha256 $path
        if ($actualHash -cne $file.Sha256) {
            throw ("Pinned MinHook dependency hash mismatch for " +
                   "$($file.Name). Expected $($file.Sha256), found $actualHash.")
        }
    }
}

$downloaded = $false
if (Test-Path -LiteralPath $MinHookRoot) {
    Assert-PinnedMinHookRoot $MinHookRoot
} else {
    if ($Offline) {
        throw "Pinned MinHook cache is unavailable in offline mode: $MinHookRoot"
    }
    $cacheParent = [IO.Path]::GetDirectoryName($MinHookRoot)
    if ([string]::IsNullOrWhiteSpace($cacheParent)) {
        throw "Could not resolve the pinned MinHook cache parent: $MinHookRoot"
    }
    [IO.Directory]::CreateDirectory($cacheParent) | Out-Null
    $stagingRoot = Join-Path $cacheParent (
        '.SDmodding-MinHook-download-{0}-{1}' -f
            $PID, [Guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
        Add-Type -AssemblyName System.Net.Http
        $http = [Net.Http.HttpClient]::new()
        try {
            $http.DefaultRequestHeaders.UserAgent.ParseAdd(
                'SPatch-pinned-dependency-resolver/1.0')
            foreach ($file in $requiredFiles) {
                $uri = "$sourceBase/$($file.Name)"
                try {
                    [byte[]] $bytes = $http.GetByteArrayAsync(
                        $uri).GetAwaiter().GetResult()
                } catch {
                    throw ("Failed to download pinned MinHook dependency " +
                           "$($file.Name): $($_.Exception.Message)")
                }
                $destination = Get-ValidatedChildPath `
                    $stagingRoot $file.Name 'MinHook download'
                [IO.File]::WriteAllBytes($destination, $bytes)
            }
        } finally {
            $http.Dispose()
        }

        Assert-PinnedMinHookRoot $stagingRoot
        if (Test-Path -LiteralPath $MinHookRoot) {
            Assert-PinnedMinHookRoot $MinHookRoot
        } else {
            try {
                [IO.Directory]::Move($stagingRoot, $MinHookRoot)
                $downloaded = $true
            } catch {
                $moveError = $_
                if (Test-Path -LiteralPath $MinHookRoot -PathType Container) {
                    # Another resolver may have won the atomic publication
                    # race after the existence check. Accept only the exact
                    # pinned payload it published.
                    Assert-PinnedMinHookRoot $MinHookRoot
                } else {
                    throw $moveError
                }
            }
        }
    } finally {
        if (Test-Path -LiteralPath $stagingRoot) {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force
        }
    }
    Assert-PinnedMinHookRoot $MinHookRoot
}

if (-not $Quiet) {
    [pscustomobject]@{
        MinHookArtifactCommit = $minHookArtifactCommit
        MinHookRoot = $MinHookRoot
        MinHookHeaderSha256 = $requiredFiles[0].Sha256
        MinHookLibrarySha256 = $requiredFiles[1].Sha256
        Downloaded = $downloaded
        Offline = [bool] $Offline
    }
}
