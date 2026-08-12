[CmdletBinding()]
param(
    [ValidateSet('Development-Release', 'Publishing-Release')]
    [string] $Configuration = 'Publishing-Release',
    [string] $ReShadeRoot = '',
    [string] $ReShadeSetupPath = '',
    [string] $MinHookRoot = '',
    [switch] $OfflineDependencies
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$implementation = Join-Path $PSScriptRoot 'Build-Luma.ps1'
if (-not (Test-Path -LiteralPath $implementation -PathType Leaf)) {
    throw "ShenLong build implementation is missing: $implementation"
}
& $implementation @PSBoundParameters
