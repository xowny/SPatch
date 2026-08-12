[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $LogPath,

    [string] $OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$invariant = [Globalization.CultureInfo]::InvariantCulture
$floatStyles = [Globalization.NumberStyles]::Float

function Read-Fields([string] $Message) {
    $fields = [ordered] @{}
    foreach ($match in [Regex]::Matches(
        $Message,
        '(?<key>[A-Za-z][A-Za-z0-9_]*)=(?<value>"[^"]*"|\S+)')) {
        $value = $match.Groups['value'].Value
        if ($value.Length -ge 2 -and $value[0] -eq '"' -and
            $value[$value.Length - 1] -eq '"') {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $fields[$match.Groups['key'].Value] = $value
    }
    return $fields
}

function Read-Double([object] $Value) {
    if ($null -eq $Value) {
        return $null
    }
    $parsed = 0.0
    if (-not [double]::TryParse(
        [string] $Value,
        $floatStyles,
        $invariant,
        [ref] $parsed)) {
        return $null
    }
    return $parsed
}

function Read-Integer([object] $Value) {
    if ($null -eq $Value) {
        return $null
    }
    $parsed = 0L
    if (-not [long]::TryParse(
        [string] $Value,
        [Globalization.NumberStyles]::Integer,
        $invariant,
        [ref] $parsed)) {
        return $null
    }
    return $parsed
}

function Get-Field(
    [AllowNull()][Collections.IDictionary] $Fields,
    [string] $Name) {
    if ($null -ne $Fields -and $Fields.Contains($Name)) {
        return $Fields[$Name]
    }
    return $null
}

function Get-FirstField(
    [AllowNull()][Collections.IDictionary] $Fields,
    [string[]] $Names) {
    foreach ($name in $Names) {
        $value = Get-Field $Fields $name
        if ($null -ne $value) {
            return $value
        }
    }
    return $null
}

function Get-CameraHookStatus(
    [AllowNull()][Collections.IDictionary] $Fields) {
    $updateInstalled = Read-Integer (Get-FirstField $Fields @(
        'update_installed',
        'update_probe_installed'))
    $desiredPoseInstalled = Read-Integer (Get-FirstField $Fields @(
        'desired_pose_installed',
        'desired_pose_probe_installed'))
    $angularApproachInstalled = Read-Integer (
        Get-Field $Fields 'angular_approach_installed')
    $requiredHookValues = @(
        $updateInstalled,
        $desiredPoseInstalled,
        $angularApproachInstalled)
    $complete = if (@($requiredHookValues | Where-Object {
                $null -eq $_ }).Count -ne 0) {
        $null
    } else {
        [int] (@($requiredHookValues | Where-Object { $_ -ne 0 }).Count -eq 3)
    }
    return [ordered] @{
        parameter_setter_installed =
            Read-Integer (Get-FirstField $Fields @(
                'setter_installed',
                'installed'))
        update_installed = $updateInstalled
        desired_pose_installed = $desiredPoseInstalled
        angular_approach_installed = $angularApproachInstalled
        required_dynamics_hooks_complete = $complete
        dynamics_mutation =
            Read-Integer (Get-Field $Fields 'dynamics_mutation')
    }
}

function Test-Finite([double] $Value) {
    return -not [double]::IsNaN($Value) -and
        -not [double]::IsInfinity($Value)
}

function Get-UniqueValues([object[]] $Records, [string] $Field) {
    return @(
        $Records |
            ForEach-Object { Get-Field $_.fields $Field } |
            Where-Object { $null -ne $_ -and [string] $_ -ne '' } |
            Sort-Object -Unique)
}

function Get-NumericRange([object[]] $Records, [string] $Field) {
    $values = @(
        $Records |
            ForEach-Object { Read-Double (Get-Field $_.fields $Field) } |
            Where-Object { $null -ne $_ -and (Test-Finite $_) })
    if ($values.Count -eq 0) {
        return $null
    }
    $measure = $values | Measure-Object -Minimum -Maximum -Average
    return [ordered] @{
        minimum = [double] $measure.Minimum
        maximum = [double] $measure.Maximum
        average = [double] $measure.Average
    }
}

function Get-VectorComponentRange(
    [object[]] $Records,
    [string] $Field,
    [int] $Index) {
    $values = [Collections.Generic.List[double]]::new()
    foreach ($record in $Records) {
        $raw = Get-Field $record.fields $Field
        if ($null -eq $raw) {
            continue
        }
        $parts = ([string] $raw).Split(',')
        if ($parts.Count -le $Index) {
            continue
        }
        $value = Read-Double $parts[$Index]
        if ($null -ne $value -and (Test-Finite $value)) {
            $values.Add($value)
        }
    }
    if ($values.Count -eq 0) {
        return $null
    }
    $measure = $values | Measure-Object -Minimum -Maximum -Average
    return [ordered] @{
        minimum = [double] $measure.Minimum
        maximum = [double] $measure.Maximum
        average = [double] $measure.Average
    }
}

function Get-TruthyCount([object[]] $Records, [string] $Field) {
    return @(
        $Records |
            Where-Object {
                $value = Read-Integer (Get-Field $_.fields $Field)
                $null -ne $value -and $value -ne 0
            }).Count
}

function Get-InactiveStateSanitizationFailures([object[]] $Records) {
    $failures = [Collections.Generic.List[int]]::new()
    foreach ($record in $Records) {
        $activeFieldsReadable = Read-Integer (
            Get-Field $record.fields 'active_fields_readable')
        if ($null -eq $activeFieldsReadable -or
            $activeFieldsReadable -ne 0) {
            continue
        }
        $valid = $true
        foreach ($field in @(
                'source_weight_valid',
                'source_weight',
                'update_eye',
                'looking_back',
                'state_b5a',
                'state_b5b',
                'state_b5c')) {
            $value = Read-Double (Get-Field $record.fields $field)
            if ($null -eq $value -or -not (Test-Finite $value) -or
                $value -ne 0.0) {
                $valid = $false
                break
            }
        }
        $targetParameters = [string] (
            Get-Field $record.fields 'target_parameters')
        if ($targetParameters -notmatch '^0x0+$') {
            $valid = $false
        }
        if (-not $valid) {
            $failures.Add([int] $record.line)
        }
    }
    return @($failures)
}

function Get-StateTransitions([object[]] $Records) {
    $transitions = [Collections.Generic.List[object]]::new()
    $previous = $null
    foreach ($record in $Records) {
        $fields = $record.fields
        $state = '{0}|{1}|{2}|{3}|{4}|{5}|{6}' -f
            (Get-Field $fields 'subject'),
            (Get-Field $fields 'vehicle_class'),
            (Get-Field $fields 'target_profile'),
            (Get-Field $fields 'target_context'),
            (Get-Field $fields 'target_symbol'),
            (Get-Field $fields 'base_drive_branch_readable'),
            (Get-Field $fields 'base_drive_branch_selected')
        if ($state -eq $previous) {
            continue
        }
        $transitions.Add([ordered] @{
            timestamp = $record.timestamp
            sample = Read-Integer (Get-Field $fields 'sample')
            subject = Get-Field $fields 'subject'
            vehicle_class = Get-Field $fields 'vehicle_class'
            target_profile = Get-Field $fields 'target_profile'
            target_context = Read-Integer (Get-Field $fields 'target_context')
            target_symbol = Get-Field $fields 'target_symbol'
            base_drive_branch_readable =
                Read-Integer (Get-Field $fields 'base_drive_branch_readable')
            base_drive_branch_selected =
                Read-Integer (Get-Field $fields 'base_drive_branch_selected')
        })
        $previous = $state
    }
    return @($transitions)
}

function New-TraceSession([int] $Index, [string] $Timestamp, [object] $Identity) {
    return [ordered] @{
        index = $Index
        started_at = $Timestamp
        identity = $Identity
        hook_install = $null
        hook_commit = $null
        state_probes = [Collections.Generic.List[object]]::new()
        updates = [Collections.Generic.List[object]]::new()
        poses = [Collections.Generic.List[object]]::new()
        warnings = [Collections.Generic.List[string]]::new()
        errors = [Collections.Generic.List[string]]::new()
    }
}

$resolvedLogPath = [IO.Path]::GetFullPath($LogPath)
if (-not (Test-Path -LiteralPath $resolvedLogPath -PathType Leaf)) {
    throw "Vehicle-camera trace log does not exist: $resolvedLogPath"
}

$sessions = [Collections.Generic.List[object]]::new()
$current = $null
$lineNumber = 0
foreach ($line in [IO.File]::ReadLines($resolvedLogPath)) {
    ++$lineNumber
    $parsed = [Regex]::Match(
        $line,
        '^\[(?<timestamp>[^\]]+)\] \[(?<level>[^\]]+)\] (?<message>.*)$')
    if (-not $parsed.Success) {
        continue
    }

    $timestamp = $parsed.Groups['timestamp'].Value
    $level = $parsed.Groups['level'].Value
    $message = $parsed.Groups['message'].Value
    if ($message.StartsWith('module_identity ', [StringComparison]::Ordinal)) {
        $current = New-TraceSession `
            -Index ($sessions.Count + 1) `
            -Timestamp $timestamp `
            -Identity (Read-Fields $message)
        $sessions.Add($current)
        continue
    }
    if ($null -eq $current) {
        continue
    }

    if ($level -eq 'WARN') {
        $current.warnings.Add($line)
    } elseif ($level -eq 'ERROR') {
        $current.errors.Add($line)
    }

    if ($message.StartsWith('gtaiv_vehicle_camera ', [StringComparison]::Ordinal)) {
        $current.hook_install = Read-Fields $message
        continue
    }
    if ($message.StartsWith('hook_transaction_committed ', [StringComparison]::Ordinal)) {
        $current.hook_commit = Read-Fields $message
        continue
    }
    if ($message.StartsWith(
        'gtaiv_vehicle_camera_probe event=state_change ',
        [StringComparison]::Ordinal)) {
        $current.state_probes.Add([ordered] @{
            timestamp = $timestamp
            line = $lineNumber
            fields = Read-Fields $message
        })
        continue
    }
    if ($message.StartsWith(
        'gtaiv_vehicle_camera_dynamic event=update ',
        [StringComparison]::Ordinal)) {
        $current.updates.Add([ordered] @{
            timestamp = $timestamp
            line = $lineNumber
            fields = Read-Fields $message
        })
        continue
    }
    if ($message.StartsWith(
        'gtaiv_vehicle_camera_dynamic event=desired_pose ',
        [StringComparison]::Ordinal)) {
        $current.poses.Add([ordered] @{
            timestamp = $timestamp
            line = $lineNumber
            fields = Read-Fields $message
        })
    }
}

if ($sessions.Count -eq 0) {
    throw 'No SPatch module sessions were found in the supplied log.'
}

$sessionSummaries = [Collections.Generic.List[object]]::new()
foreach ($session in $sessions) {
    $stateProbes = @($session.state_probes)
    $updates = @($session.updates)
    $poses = @($session.poses)
    $firstUpdate = if ($updates.Count -ne 0) { $updates[0].timestamp } else { $null }
    $lastUpdate = if ($updates.Count -ne 0) { $updates[-1].timestamp } else { $null }
    $updateSamples = @(
        $updates |
            ForEach-Object { Read-Integer (Get-Field $_.fields 'sample') } |
            Where-Object { $null -ne $_ })
    $poseSamples = @(
        $poses |
            ForEach-Object { Read-Integer (Get-Field $_.fields 'sample') } |
            Where-Object { $null -ne $_ })
    $missingPoseSamples = @(
        $updateSamples |
            Where-Object { $poseSamples -notcontains $_ })

    $sessionSummaries.Add([ordered] @{
        index = $session.index
        started_at = $session.started_at
        asi_sha256 = Get-Field $session.identity 'sha256'
        executable_profile = Get-Field $session.hook_install 'layout'
        hook_transaction = $session.hook_commit
        camera_hook = $session.hook_install
        camera_hook_status = Get-CameraHookStatus $session.hook_install
        warnings = @($session.warnings)
        errors = @($session.errors)
        state_probe_capture = [ordered] @{
            count = $stateProbes.Count
            components = @(Get-UniqueValues $stateProbes 'component')
            subjects = @(Get-UniqueValues $stateProbes 'subject')
            physics_movers = @(Get-UniqueValues $stateProbes 'physics_mover')
            mover_vtable_rvas = @(Get-UniqueValues $stateProbes 'mover_vtable_rva')
            vehicle_classes = @(Get-UniqueValues $stateProbes 'vehicle_class')
            target_profiles = @(Get-UniqueValues $stateProbes 'target_profile')
            target_contexts = @(Get-UniqueValues $stateProbes 'target_context')
            target_symbols = @(Get-UniqueValues $stateProbes 'target_symbol')
            readable = @(Get-UniqueValues $stateProbes 'readable')
            active_fields_readable = @(
                Get-UniqueValues $stateProbes 'active_fields_readable')
            source_weight_valid = @(
                Get-UniqueValues $stateProbes 'source_weight_valid')
            source_weights = @(
                Get-UniqueValues $stateProbes 'source_weight')
            inactive_state_sanitization_failures = @(
                Get-InactiveStateSanitizationFailures $stateProbes)
            base_drive_branch_readable = @(
                Get-UniqueValues $stateProbes 'base_drive_branch_readable')
            base_drive_branch_selected = @(
                Get-UniqueValues $stateProbes 'base_drive_branch_selected')
        }
        dynamic_capture = [ordered] @{
            update_count = $updates.Count
            desired_pose_count = $poses.Count
            first_update_at = $firstUpdate
            last_update_at = $lastUpdate
            missing_desired_pose_samples = $missingPoseSamples
            threads = @(Get-UniqueValues $updates 'tid')
            components = @(Get-UniqueValues $updates 'component')
            subjects = @(Get-UniqueValues $updates 'subject')
            physics_movers = @(Get-UniqueValues $updates 'physics_mover')
            mover_vtable_rvas = @(Get-UniqueValues $updates 'mover_vtable_rva')
            vehicle_classes = @(Get-UniqueValues $updates 'vehicle_class')
            target_profiles = @(Get-UniqueValues $updates 'target_profile')
            target_contexts = @(Get-UniqueValues $updates 'target_context')
            target_symbols = @(Get-UniqueValues $updates 'target_symbol')
            base_drive_branch_readable = @(
                Get-UniqueValues $updates 'base_drive_branch_readable')
            base_drive_branch_selected = @(
                Get-UniqueValues $updates 'base_drive_branch_selected')
            state_transitions = @(Get-StateTransitions $updates)
            active_counts = [ordered] @{
                lateral_parameter_mutation = Get-TruthyCount $updates 'mutation'
                dynamics_mutation = Get-TruthyCount $updates 'dynamics_mutation'
                desired_pose_mutation = Get-TruthyCount $poses 'mutation'
                manual_look = Get-TruthyCount $updates 'looking_around'
                look_behind = Get-TruthyCount $updates 'looking_back'
                aim_or_focus = Get-TruthyCount $updates 'aim_or_focus'
                reverse = Get-TruthyCount $updates 'in_reverse'
                handbrake = Get-TruthyCount $updates 'handbrake'
                eye_lock = Get-TruthyCount $updates 'eye_locked'
                look_lock = Get-TruthyCount $updates 'look_locked'
            }
            ranges = [ordered] @{
                dt = Get-NumericRange $updates 'dt'
                horizontal_input = Get-VectorComponentRange $updates 'input_post' 0
                vertical_input = Get-VectorComponentRange $updates 'input_post' 1
                mouse_vertical_accumulator = Get-VectorComponentRange $updates 'input_post' 2
                center_timer = Get-NumericRange $updates 'center_timer_post'
                manual_yaw = Get-NumericRange $updates 'manual_yaw_post'
                follow_yaw = Get-NumericRange $updates 'follow_yaw'
                desired_follow_yaw = Get-NumericRange $updates 'desired_yaw'
                average_yaw_rate = Get-NumericRange $updates 'yaw_rate_avg'
                automatic_pitch = Get-NumericRange $updates 'target_pitch'
                automatic_pitch_target = Get-NumericRange $updates 'pitch_target'
                manual_pitch_offset =
                    Get-NumericRange $updates 'manual_pitch_offset'
                signed_forward_speed_mps = Get-NumericRange $updates 'signed_forward_speed_mps'
                steering_input = Get-NumericRange $updates 'steering_input'
                desired_pose_manual_pitch_offset =
                    Get-NumericRange $poses 'manual_pitch_offset'
                stock_pose_distance = Get-NumericRange $poses 'stock_distance'
                stock_pose_yaw = Get-NumericRange $poses 'stock_yaw'
                stock_pose_pitch = Get-NumericRange $poses 'stock_pitch'
                pose_distance = Get-NumericRange $poses 'distance'
                pose_yaw = Get-NumericRange $poses 'yaw'
                pose_pitch = Get-NumericRange $poses 'pitch'
            }
        }
    })
}

$report = [ordered] @{
    schema_version = 2
    source_log = $resolvedLogPath
    source_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedLogPath).Hash
    sessions = @($sessionSummaries)
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $resolvedOutputPath = [IO.Path]::ChangeExtension(
        $resolvedLogPath,
        '.vehicle-camera-trace.json')
} else {
    $resolvedOutputPath = [IO.Path]::GetFullPath($OutputPath)
}
$outputDirectory = [IO.Path]::GetDirectoryName($resolvedOutputPath)
if (-not [string]::IsNullOrWhiteSpace($outputDirectory) -and
    -not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $outputDirectory
}

$json = $report | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText(
    $resolvedOutputPath,
    $json + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

foreach ($session in $sessionSummaries) {
    $capture = $session.dynamic_capture
    Write-Host (
        'Session {0} ({1}): {2} updates, {3} poses, {4} warnings, {5} errors' -f
            $session.index,
            $session.started_at,
            $capture.update_count,
            $capture.desired_pose_count,
            $session.warnings.Count,
            $session.errors.Count)
}
Write-Host "Vehicle-camera trace report: $resolvedOutputPath"
