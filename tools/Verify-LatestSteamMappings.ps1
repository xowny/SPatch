[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Executable,

    [string] $SourceRoot = (Split-Path -Parent $PSScriptRoot),

    [string] $OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FileSha256([string] $Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
}

function Get-BytesSha256([byte[]] $Bytes) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha256.ComputeHash($Bytes)).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
}

function Test-ByteArrayEqual([byte[]] $Left, [byte[]] $Right) {
    if ($Left.Length -ne $Right.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Left.Length; ++$index) {
        if ($Left[$index] -ne $Right[$index]) {
            return $false
        }
    }
    return $true
}

function Get-RelativePathUnderRoot([string] $Root, [string] $Path) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]] '\/')
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the source root: $fullPath"
    }
    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}

function Read-CppByteArray([string] $Text, [string] $Name) {
    $escaped = [Regex]::Escape($Name)
    $match = [Regex]::Match(
        $Text,
        "(?:inline\s+)?constexpr\s+std::array<std::uint8_t,\s*\d+>\s+$escaped\s*(?:=\s*)?\{(?<body>.*?)\};",
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success) {
        throw "C++ byte array was not found: $Name"
    }
    return [byte[]] @(
        [Regex]::Matches($match.Groups['body'].Value, '0x[0-9A-Fa-f]+') |
            ForEach-Object { [Convert]::ToByte($_.Value.Substring(2), 16) })
}

function Read-CppInteger([string] $Text, [string] $Name) {
    $escaped = [Regex]::Escape($Name)
    $match = [Regex]::Match(
        $Text,
        "(?:inline\s+)?constexpr\s+std::(?:u?int\d+_t|size_t|uintptr_t)\s+$escaped\s*=\s*(?<value>0x[0-9A-Fa-f]+|\d+)\s*;",
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success) {
        throw "C++ integer constant was not found: $Name"
    }
    $value = $match.Groups['value'].Value
    if ($value.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
        return [Convert]::ToUInt64($value.Substring(2), 16)
    }
    return [Convert]::ToUInt64($value, 10)
}

$executablePath = [IO.Path]::GetFullPath($Executable)
$sourceRootPath = [IO.Path]::GetFullPath($SourceRoot)
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Latest-Steam executable does not exist: $executablePath"
}
if (-not (Test-Path -LiteralPath $sourceRootPath -PathType Container)) {
    throw "Source root does not exist: $sourceRootPath"
}

$sourceFiles = [ordered] @{
    BuildInfo = Join-Path $sourceRootPath 'src\BuildInfo.h'
    BootstrapPolicy = Join-Path $sourceRootPath 'src\BootstrapPolicy.cpp'
    Hooks = Join-Path $sourceRootPath 'src\Hooks.cpp'
    EngineFixes = Join-Path $sourceRootPath 'src\EngineFixes.cpp'
    FogPolicy = Join-Path $sourceRootPath 'src\FogRestorationPolicy.h'
    VehicleCameraPolicy = Join-Path $sourceRootPath 'src\VehicleCameraPolicy.h'
}
foreach ($entry in $sourceFiles.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "Required source file does not exist: $($entry.Value)"
    }
}

$sourceText = @{}
foreach ($entry in $sourceFiles.GetEnumerator()) {
    $sourceText[$entry.Key] = [IO.File]::ReadAllText($entry.Value)
}

$bytes = [IO.File]::ReadAllBytes($executablePath)
if ($bytes.Length -lt 0x100 -or
    [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) {
    throw 'Executable is not a valid DOS/PE image.'
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
if ($peOffset -lt 0 -or $peOffset + 24 -gt $bytes.Length -or
    [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
    throw 'Executable has an invalid PE header.'
}
$coffOffset = $peOffset + 4
$machine = [BitConverter]::ToUInt16($bytes, $coffOffset)
$sectionCount = [BitConverter]::ToUInt16($bytes, $coffOffset + 2)
$timeDateStamp = [BitConverter]::ToUInt32($bytes, $coffOffset + 4)
$optionalSize = [BitConverter]::ToUInt16($bytes, $coffOffset + 16)
$optionalOffset = $coffOffset + 20
if ($optionalOffset + $optionalSize -gt $bytes.Length -or
    [BitConverter]::ToUInt16($bytes, $optionalOffset) -ne 0x020B) {
    throw 'Executable is not a complete PE32+ image.'
}
$sizeOfImage = [BitConverter]::ToUInt32($bytes, $optionalOffset + 56)
$sectionOffset = $optionalOffset + $optionalSize
if ($sectionOffset + 40 * $sectionCount -gt $bytes.Length) {
    throw 'Executable has a truncated section table.'
}

$sections = @()
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $offset = $sectionOffset + 40 * $index
    $sections += [pscustomobject] [ordered] @{
        name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 8).Trim([char] 0)
        virtual_size = [BitConverter]::ToUInt32($bytes, $offset + 8)
        virtual_address = [BitConverter]::ToUInt32($bytes, $offset + 12)
        raw_size = [BitConverter]::ToUInt32($bytes, $offset + 16)
        raw_offset = [BitConverter]::ToUInt32($bytes, $offset + 20)
        characteristics = [BitConverter]::ToUInt32($bytes, $offset + 36)
    }
}

function Resolve-RvaOffset([uint32] $Rva, [int] $Length = 1) {
    foreach ($section in $sections) {
        if ($Rva -lt $section.virtual_address) {
            continue
        }
        $relative = [uint64] $Rva - [uint64] $section.virtual_address
        if ($relative + [uint64] $Length -gt [uint64] $section.raw_size) {
            continue
        }
        $fileOffset = [uint64] $section.raw_offset + $relative
        if ($fileOffset + [uint64] $Length -gt [uint64] $bytes.Length) {
            break
        }
        return [int] $fileOffset
    }
    throw ('RVA 0x{0:X8} length {1} is not file-backed.' -f $Rva, $Length)
}

$exceptionRva = [BitConverter]::ToUInt32($bytes, $optionalOffset + 112 + 8 * 3)
$exceptionSize = [BitConverter]::ToUInt32($bytes, $optionalOffset + 112 + 8 * 3 + 4)
$exceptionOffset = Resolve-RvaOffset $exceptionRva ([int] $exceptionSize)
$functionStarts = [Collections.Generic.HashSet[uint32]]::new()
for ($offset = $exceptionOffset;
     $offset + 12 -le $exceptionOffset + $exceptionSize;
     $offset += 12) {
    [void] $functionStarts.Add([BitConverter]::ToUInt32($bytes, $offset))
}

$expectedExecutableHash = [BitConverter]::ToString(
    (Read-CppByteArray $sourceText.BuildInfo 'kLatestSteamSha256')).Replace('-', '')
$expectedTimeDateStamp = [uint32] (Read-CppInteger $sourceText.BuildInfo 'kLatestSteamTimeDateStamp')
$expectedFileSize = [uint64] (Read-CppInteger $sourceText.BuildInfo 'kKnownFileSize')
$expectedImageSize = [uint32] (Read-CppInteger $sourceText.BuildInfo 'kKnownSizeOfImage')
$actualExecutableHash = Get-FileSha256 $executablePath
if ($actualExecutableHash -cne $expectedExecutableHash -or
    [uint64] $bytes.Length -ne $expectedFileSize -or
    $timeDateStamp -ne $expectedTimeDateStamp -or
    $sizeOfImage -ne $expectedImageSize -or
    $machine -ne 0x8664) {
    throw 'Executable identity does not match the current latest-Steam BuildInfo contract.'
}

$hookSites = @(
    @('pedestrian_spawn_update', 'Hooks', 'kLatestSteamPedestrianSpawnUpdateRva', 'Hooks', 'kPedestrianSpawnUpdateSignature'),
    @('pedestrian_frame_rate_throttle', 'Hooks', 'kLatestSteamPedestrianFrameRateThrottleRva', 'Hooks', 'kLatestPedestrianThrottleSignature'),
    @('average_window_initialize', 'Hooks', 'kLatestSteamAverageWindowInitializeRva', 'Hooks', 'kAverageWindowInitializeSignature'),
    @('material_on_load', 'Hooks', 'kLatestSteamMaterialOnLoadRva', 'Hooks', 'kMaterialOnLoadSignature'),
    @('ui_is_game_paused', 'Hooks', 'kLatestSteamUiIsGamePausedRva', 'Hooks', 'kUiIsGamePausedSignature'),
    @('pc_file_read', 'Hooks', 'kLatestSteamPcFileReadRva', 'Hooks', 'kPcFileReadSignature'),
    @('pc_file_seek', 'Hooks', 'kLatestSteamPcFileSeekRva', 'Hooks', 'kPcFileSeekSignature'),
    @('pc_file_tell', 'Hooks', 'kLatestSteamPcFileTellRva', 'Hooks', 'kPcFileTellAndSizeSignature'),
    @('pc_file_size', 'Hooks', 'kLatestSteamPcFileSizeRva', 'Hooks', 'kPcFileTellAndSizeSignature'),
    @('qfile_read_at', 'Hooks', 'kLatestSteamQFileReadAtRva', 'Hooks', 'kQFileReadAtSignature'),
    @('qfile_write_at', 'Hooks', 'kLatestSteamQFileWriteAtRva', 'Hooks', 'kQFileWriteAtSignature'),
    @('qfile_ready', 'Hooks', 'kLatestSteamQFileReadyRva', 'Hooks', 'kQFileReadySignature'),
    @('qcmp_decompress', 'Hooks', 'kLatestSteamQcmpDecompressRva', 'Hooks', 'kQcmpDecompressSignature'),
    @('stream_file_open', 'Hooks', 'kLatestSteamStreamFileOpenRva', 'Hooks', 'kStreamFileOpenSignature'),
    @('stream_file_close', 'Hooks', 'kLatestSteamStreamFileCloseRva', 'Hooks', 'kStreamFileCloseSignature'),
    @('resource_chunk_dispatch', 'Hooks', 'kLatestSteamResourceChunkDispatchRva', 'Hooks', 'kResourceChunkDispatchSignature'),
    @('volumetric_fog_setter', 'FogPolicy', 'kLatestSteamSetterRva', 'FogPolicy', 'kLatestSteamSetterSignature'),
    @('chase_camera_set_parameters', 'VehicleCameraPolicy', 'kLatestSteamChaseParameterSetterRva', 'VehicleCameraPolicy', 'kParameterSetterSignature'),
    @('chase_camera_update', 'VehicleCameraPolicy', 'kLatestSteamChaseUpdateRva', 'VehicleCameraPolicy', 'kChaseUpdateSignature'),
    @('game_camera_desired_pose', 'VehicleCameraPolicy', 'kLatestSteamDesiredEyeLookUpRva', 'VehicleCameraPolicy', 'kDesiredEyeLookUpSignature'),
    @('angular_approach', 'VehicleCameraPolicy', 'kLatestSteamAngularApproachRva', 'VehicleCameraPolicy', 'kLatestSteamAngularApproachSignature')
)

$engineSites = @(
    @('spherical_reflection', 'kLatestSphericalReflectionSetupRva', 'kStockSphericalReflectionSetup'),
    @('present_function', 'kLatestPresentFunctionRva', 'kPresentFunctionSignature'),
    @('hidden_120_fps_wait', 'kLatestHidden120FpsWaitBranchRva', 'kHidden120FpsWaitBranch'),
    @('save_header_load', 'kLatestSaveHeaderLoadRva', 'kSaveHeaderLoadSignature'),
    @('save_header_call', 'kLatestSaveHeaderCallRva', 'kLatestSaveHeaderCall'),
    @('save_parser_call_1', 'kLatestSaveParserCall1Rva', 'kLatestSaveParserCall1'),
    @('save_parser_call_2', 'kLatestSaveParserCall2Rva', 'kLatestSaveParserCall2'),
    @('task_create_thread', 'kLatestTaskCreateThreadCallRva', 'kLatestTaskCreateThreadCall'),
    @('generic_create_thread', 'kLatestGenericCreateThreadCallRva', 'kLatestGenericCreateThreadCall'),
    @('io_create_thread', 'kLatestIoCreateThreadCallRva', 'kLatestIoCreateThreadCall'),
    @('bank_create_thread', 'kLatestBankManagerCreateThreadCallRva', 'kLatestBankManagerCreateThreadCall'),
    @('bank_fence_create_event', 'kBankShutdownFenceCreateEventCallRva', 'kBankShutdownFenceCreateEventCall'),
    @('wwise_completion', 'kLatestWwiseBlockingCompletionRva', 'kLatestWwiseBlockingCompletion'),
    @('wwise_wait', 'kLatestWwiseBlockingWaitRva', 'kLatestWwiseBlockingWait'),
    @('first_run_resolution', 'kLatestFirstRunResolutionRva', 'kInvalidFirstRunResolution'),
    @('scaleform_qpc', 'kLatestScaleformQpcClockRva', 'kTruncatedScaleformQpcConversion'),
    @('timestamp_open', 'kLatestFileTimestampOpenModeRva', 'kTimestampOpenExisting'),
    @('audio_handle', 'kLatestAudioFileHandleTestRva', 'kNullFileHandleTest'),
    @('audio_mapping', 'kLatestAudioFileMappingArgumentRva', 'kMappingArgumentFromRax'),
    @('large_file_size', 'kLatestFileSizeCombineRva', 'kTruncatedFileSizeCombine'),
    @('contact_overflow', 'kLatestContactImageFormatCallRva', 'kLatestContactImageFormatCall'),
    @('benchmark_vram', 'kLatestBenchmarkVramReadRva', 'kLatestTruncatedBenchmarkVramRead'),
    @('vram_pool_lock', 'kLatestVramPoolFinalValidationCallRva', 'kLatestVramPoolFinalValidationCall'),
    @('character_surface', 'kLatestCharacterSurfaceCopyCallRva', 'kLatestCharacterSurfaceCopyCall'),
    @('loaded_chunk_error', 'kLatestLoadedChunkErrorLogCallRva', 'kLatestLoadedChunkErrorLogCall'),
    @('loaded_chunk_file_error', 'kLatestLoadedChunkFileErrorLogCallRva', 'kLatestLoadedChunkFileErrorLogCall'),
    @('loaded_chunk_file_size', 'kLatestLoadedChunkFileSizeCleanupCallRva', 'kLatestLoadedChunkFileSizeCleanupCall'),
    @('sync_resource_finalize', 'kLatestSynchronousResourceFinalizeCallRva', 'kLatestSynchronousResourceFinalizeCall'),
    @('sync_loose_open', 'kLatestSynchronousLooseOpenFailureBranchRva', 'kSynchronousLooseOpenFailureBranch'),
    @('sync_loose_finalize', 'kLatestSynchronousLooseFinalizeCallRva', 'kLatestSynchronousLooseFinalizeCall'),
    @('sync_loose_size', 'kLatestSynchronousLooseInvalidSizeStateRva', 'kSynchronousLooseInvalidSizeState'),
    @('qcmp_failure_copy', 'kLatestQcmpFailureCopyCallRva', 'kLatestQcmpFailureCopyCall'),
    @('compressed_xml_alloc', 'kLatestCompressedXmlAllocationCallRva', 'kLatestCompressedXmlAllocationCall'),
    @('compressed_xml_finalize', 'kLatestCompressedXmlFinalizeRva', 'kCompressedXmlFinalize')
)

$siteResults = [Collections.Generic.List[object]]::new()
foreach ($site in $hookSites) {
    $rva = [uint32] (Read-CppInteger $sourceText[$site[1]] $site[2])
    $expected = Read-CppByteArray $sourceText[$site[3]] $site[4]
    $offset = Resolve-RvaOffset $rva $expected.Length
    $actual = [byte[]] $bytes[$offset..($offset + $expected.Length - 1)]
    $siteResults.Add([pscustomobject] [ordered] @{
        group = 'hook_signature'
        name = $site[0]
        rva = '0x{0:X8}' -f $rva
        rva_constant = $site[2]
        signature = $site[4]
        length = $expected.Length
        expected_sha256 = Get-BytesSha256 $expected
        actual_sha256 = Get-BytesSha256 $actual
        matches = Test-ByteArrayEqual $expected $actual
    })
}
foreach ($site in $engineSites) {
    $rva = [uint32] (Read-CppInteger $sourceText.EngineFixes $site[1])
    $expected = Read-CppByteArray $sourceText.EngineFixes $site[2]
    $offset = Resolve-RvaOffset $rva $expected.Length
    $actual = [byte[]] $bytes[$offset..($offset + $expected.Length - 1)]
    $siteResults.Add([pscustomobject] [ordered] @{
        group = 'engine_patch_signature'
        name = $site[0]
        rva = '0x{0:X8}' -f $rva
        rva_constant = $site[1]
        signature = $site[2]
        length = $expected.Length
        expected_sha256 = Get-BytesSha256 $expected
        actual_sha256 = Get-BytesSha256 $actual
        matches = Test-ByteArrayEqual $expected $actual
    })
}

# The live angular-helper detour is shared by the executable. It is gated by
# the two exact Chase return addresses, so validate both rel32 call instructions
# as part of the fixed-site contract rather than trusting the RVAs alone.
$angularReturnSites = @(
    @('chase_camera_follow_yaw_return', 'kLatestSteamFollowYawReturnRva'),
    @('chase_camera_manual_yaw_recenter_return', 'kLatestSteamManualYawRecenterReturnRva')
)
$angularApproachRva = [uint32] (Read-CppInteger `
    $sourceText.VehicleCameraPolicy 'kLatestSteamAngularApproachRva')
foreach ($site in $angularReturnSites) {
    $returnRva = [uint32] (Read-CppInteger `
        $sourceText.VehicleCameraPolicy $site[1])
    if ($returnRva -lt 5) {
        throw "Latest-Steam angular return RVA cannot follow a rel32 call: $($site[1])"
    }
    $callRva = [uint32] ($returnRva - 5)
    $relativeTarget = [int64] $angularApproachRva - [int64] $returnRva
    if ($relativeTarget -lt [int32]::MinValue -or
        $relativeTarget -gt [int32]::MaxValue) {
        throw "Latest-Steam angular call target is outside rel32 range: $($site[0])"
    }
    $expected = [byte[]]::new(5)
    $expected[0] = 0xE8
    [BitConverter]::GetBytes([int32] $relativeTarget).CopyTo($expected, 1)
    $offset = Resolve-RvaOffset $callRva $expected.Length
    $actual = [byte[]] $bytes[$offset..($offset + $expected.Length - 1)]
    $actualRelativeTarget = if ($actual[0] -eq 0xE8) {
        [BitConverter]::ToInt32($actual, 1)
    } else {
        0
    }
    $actualTargetRva = [int64] $returnRva + $actualRelativeTarget
    $siteResults.Add([pscustomobject] [ordered] @{
        group = 'hook_return_callsite'
        name = $site[0]
        rva = '0x{0:X8}' -f $callRva
        return_rva = '0x{0:X8}' -f $returnRva
        rva_constant = $site[1]
        target_rva = '0x{0:X8}' -f $angularApproachRva
        target_rva_constant = 'kLatestSteamAngularApproachRva'
        signature = 'E8 rel32 to angular approach'
        length = $expected.Length
        expected_sha256 = Get-BytesSha256 $expected
        actual_sha256 = Get-BytesSha256 $actual
        actual_target_rva = '0x{0:X8}' -f $actualTargetRva
        matches = Test-ByteArrayEqual $expected $actual
    })
}
$expectedMappedSiteCount = 57
if ($siteResults.Count -ne $expectedMappedSiteCount -or
    @($siteResults | Where-Object { -not $_.matches }).Count -ne 0) {
    throw ('Latest-Steam mapped-site validation failed or coverage drifted ' +
           "from $expectedMappedSiteCount checks.")
}

function Find-PatternRvas([byte[]] $Pattern, [bool] $ExecutableSection) {
    $matches = [Collections.Generic.List[uint32]]::new()
    foreach ($section in $sections) {
        $isExecutable = ($section.characteristics -band 0x20000000) -ne 0
        if ($isExecutable -ne $ExecutableSection -or $section.raw_size -lt $Pattern.Length) {
            continue
        }
        $sectionStart = [int] $section.raw_offset
        $sectionEnd = $sectionStart + [int] $section.raw_size
        $position = $sectionStart
        while ($position -le $sectionEnd - $Pattern.Length) {
            $position = [Array]::IndexOf($bytes, $Pattern[0], $position)
            if ($position -lt 0 -or $position -gt $sectionEnd - $Pattern.Length) {
                break
            }
            $matched = $true
            for ($index = 1; $index -lt $Pattern.Length; ++$index) {
                if ($bytes[$position + $index] -ne $Pattern[$index]) {
                    $matched = $false
                    break
                }
            }
            if ($matched) {
                $rva = [uint32] ($section.virtual_address + $position - $sectionStart)
                $matches.Add($rva)
            }
            ++$position
        }
    }
    return @($matches)
}

$inputPatternDefinitions = @(
    @('raw_mouse_option', 'kRawMouseOptionSignature', $true),
    @('controller_response', 'kControllerResponseSignature', $false),
    @('mouse_camera_smoothing', 'kMouseCameraSignature', $false)
)
$inputPatternResults = [Collections.Generic.List[object]]::new()
foreach ($definition in $inputPatternDefinitions) {
    $pattern = Read-CppByteArray $sourceText.EngineFixes $definition[1]
    $matchRvas = @(Find-PatternRvas $pattern ([bool] $definition[2]))
    $inputPatternResults.Add([pscustomobject] [ordered] @{
        name = $definition[0]
        signature = $definition[1]
        memory_class = if ($definition[2]) { 'executable' } else { 'non_executable' }
        length = $pattern.Length
        occurrences = $matchRvas.Count
        match_rvas = @($matchRvas | ForEach-Object { '0x{0:X8}' -f $_ })
        unique = $matchRvas.Count -eq 1
    })
}
if (@($inputPatternResults | Where-Object { -not $_.unique }).Count -ne 0) {
    throw 'Latest-Steam input patterns are not unique in their runtime memory classes.'
}

$hookFunctionDefinitions = @(
    @('nis_set_play_time', 'Hooks', 'kLatestSteamNisSetPlayTimeRva'),
    @('nis_owner', 'Hooks', 'kLatestSteamNisOwnerRva'),
    @('frame_flow', 'Hooks', 'kLatestSteamFrameFlowRva'),
    @('cutscene_flow_owner', 'Hooks', 'kLatestSteamCutsceneFlowOwnerRva'),
    @('chase_camera_set_parameters', 'VehicleCameraPolicy', 'kLatestSteamChaseParameterSetterRva'),
    @('chase_camera_update', 'VehicleCameraPolicy', 'kLatestSteamChaseUpdateRva'),
    @('game_camera_desired_pose', 'VehicleCameraPolicy', 'kLatestSteamDesiredEyeLookUpRva'),
    @('angular_approach', 'VehicleCameraPolicy', 'kLatestSteamAngularApproachRva')
)
$hookFunctionStartResults = [Collections.Generic.List[object]]::new()
foreach ($definition in $hookFunctionDefinitions) {
    $rva = [uint32] (Read-CppInteger $sourceText[$definition[1]] $definition[2])
    $offset = Resolve-RvaOffset $rva 32
    $prologue = [byte[]] $bytes[$offset..($offset + 31)]
    $hookFunctionStartResults.Add([pscustomobject] [ordered] @{
        name = $definition[0]
        rva = '0x{0:X8}' -f $rva
        rva_constant = $definition[2]
        pe_exception_function_start = $functionStarts.Contains($rva)
        first_32_bytes_sha256 = Get-BytesSha256 $prologue
    })
}
$expectedHookFunctionStartCount = 8
if ($hookFunctionStartResults.Count -ne $expectedHookFunctionStartCount -or
    @($hookFunctionStartResults | Where-Object {
            -not $_.pe_exception_function_start }).Count -ne 0) {
    throw ('A latest-Steam hook mapping is not an x64 PE exception-table ' +
           'function start, or hook-start coverage drifted from ' +
           "$expectedHookFunctionStartCount checks.")
}

$sourceResults = @(
    foreach ($entry in $sourceFiles.GetEnumerator()) {
        [pscustomobject] [ordered] @{
            path = Get-RelativePathUnderRoot $sourceRootPath $entry.Value
            sha256 = Get-FileSha256 $entry.Value
        }
    })

$scriptPath = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    ''
} else {
    [IO.Path]::GetFullPath($OutputPath)
}
$command = 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Executable "{1}" -SourceRoot "{2}"' -f $scriptPath, $executablePath, $sourceRootPath
if (-not [string]::IsNullOrEmpty($resolvedOutputPath)) {
    $command += ' -OutputPath "{0}"' -f $resolvedOutputPath
}

$evidence = [pscustomobject] [ordered] @{
    schema_version = 1
    status = 'pass'
    generated_utc = [DateTime]::UtcNow.ToString('o')
    methodology = [pscustomobject] [ordered] @{
        verifier = Get-RelativePathUnderRoot $sourceRootPath $scriptPath
        verifier_sha256 = Get-FileSha256 $scriptPath
        powershell = $PSVersionTable.PSVersion.ToString()
        exact_command = $command
        mapped_sites = 'Parse current C++ RVA/signature constants, map each RVA through the PE section table, and compare exact file-backed bytes.'
        input_patterns = 'Count each current C++ signature only in file-backed PE sections whose execute classification matches the runtime scanner.'
        function_starts = 'Parse the x64 PE exception directory and require each selected latest-Steam hook target to equal a RuntimeFunction begin RVA.'
        limitation = 'This is static identity/mapping evidence, not a latest-Steam live gameplay or behavioral smoke.'
    }
    executable = [pscustomobject] [ordered] @{
        path = $executablePath
        sha256 = $actualExecutableHash
        file_size = $bytes.Length
        machine = '0x{0:X4}' -f $machine
        time_date_stamp = '0x{0:X8}' -f $timeDateStamp
        size_of_image = '0x{0:X8}' -f $sizeOfImage
        build_info_identity_match = $true
    }
    sources = $sourceResults
    mapped_site_summary = [pscustomobject] [ordered] @{
        checks = $siteResults.Count
        passed = @($siteResults | Where-Object { $_.matches }).Count
        failed = @($siteResults | Where-Object { -not $_.matches }).Count
    }
    mapped_sites = @($siteResults)
    input_patterns = @($inputPatternResults)
    hook_function_starts = @($hookFunctionStartResults)
}

$json = $evidence | ConvertTo-Json -Depth 8
if ([string]::IsNullOrEmpty($resolvedOutputPath)) {
    $json
} else {
    $outputDirectory = Split-Path -Parent $resolvedOutputPath
    if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
        [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    }
    $temporaryPath = $resolvedOutputPath + '.tmp-' + [Guid]::NewGuid().ToString('N')
    $replacementBackup = $resolvedOutputPath + '.bak-' + [Guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText(
            $temporaryPath, $json + [Environment]::NewLine,
            [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $resolvedOutputPath -PathType Leaf) {
            [IO.File]::Replace(
                $temporaryPath, $resolvedOutputPath, $replacementBackup)
        } else {
            [IO.File]::Move($temporaryPath, $resolvedOutputPath)
        }
    } finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            [IO.File]::Delete($temporaryPath)
        }
        if (Test-Path -LiteralPath $replacementBackup -PathType Leaf) {
            [IO.File]::Delete($replacementBackup)
        }
    }
    Write-Host (
        "Latest-Steam mapping audit PASS: $expectedMappedSiteCount/" +
        "$expectedMappedSiteCount sites, 3/3 unique input patterns, " +
        "$expectedHookFunctionStartCount/$expectedHookFunctionStartCount " +
        'hook function starts.')
    Write-Host "Evidence: $resolvedOutputPath"
}
