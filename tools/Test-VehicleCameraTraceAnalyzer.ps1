[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$analyzer = Join-Path $PSScriptRoot 'Analyze-VehicleCameraTrace.ps1'
$fixture = Join-Path $repoRoot 'tests\fixtures\vehicle-camera-trace.log'
foreach ($path in @($analyzer, $fixture)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Vehicle-camera analyzer test input is missing: $path"
    }
}

function Assert-Trace([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw "Vehicle-camera trace analyzer test failed: $Message"
    }
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'SPatch-VehicleCameraTraceAnalyzer-' + [Guid]::NewGuid().ToString('N'))
[void] [IO.Directory]::CreateDirectory($temporaryRoot)
try {
    $outputPath = Join-Path $temporaryRoot 'report.json'
    & $analyzer -LogPath $fixture -OutputPath $outputPath
    $report = [IO.File]::ReadAllText($outputPath) | ConvertFrom-Json

    Assert-Trace ($report.schema_version -eq 2) 'schema version'
    Assert-Trace (@($report.sessions).Count -eq 2) 'session count'

    $current = $report.sessions[0]
    Assert-Trace (
        $current.camera_hook_status.parameter_setter_installed -eq 1 -and
        $current.camera_hook_status.update_installed -eq 1 -and
        $current.camera_hook_status.desired_pose_installed -eq 1 -and
        $current.camera_hook_status.angular_approach_installed -eq 1 -and
        $current.camera_hook_status.required_dynamics_hooks_complete -eq 1 -and
        $current.camera_hook_status.dynamics_mutation -eq 1) `
        'required camera-hook status'
    Assert-Trace ($current.state_probe_capture.count -eq 3) `
        'state-probe count'
    Assert-Trace (
        @($current.state_probe_capture.base_drive_branch_readable).Count -eq 2 -and
        $current.state_probe_capture.base_drive_branch_readable -contains '0' -and
        $current.state_probe_capture.base_drive_branch_readable -contains '1') `
        'Drive-branch readability summary'
    Assert-Trace (
        @($current.state_probe_capture.base_drive_branch_selected).Count -eq 2) `
        'Drive-branch selection summary'
    Assert-Trace (
        @($current.state_probe_capture.active_fields_readable).Count -eq 2 -and
        $current.state_probe_capture.active_fields_readable -contains '0' -and
        $current.state_probe_capture.active_fields_readable -contains '1' -and
        @($current.state_probe_capture.source_weight_valid).Count -eq 2 -and
        $current.state_probe_capture.source_weight_valid -contains '0' -and
        $current.state_probe_capture.source_weight_valid -contains '1' -and
        @($current.state_probe_capture.inactive_state_sanitization_failures).Count -eq 0) `
        'inactive camera-state sanitization summary'
    Assert-Trace (
        @($current.dynamic_capture.state_transitions).Count -eq 2 -and
        $current.dynamic_capture.state_transitions[0].base_drive_branch_readable -eq 1 -and
        $current.dynamic_capture.state_transitions[0].base_drive_branch_selected -eq 1 -and
        $current.dynamic_capture.state_transitions[1].base_drive_branch_selected -eq 0) `
        'Drive-branch transitions'
    Assert-Trace (
        $current.dynamic_capture.active_counts.lateral_parameter_mutation -eq 1 -and
        $current.dynamic_capture.active_counts.dynamics_mutation -eq 1 -and
        $current.dynamic_capture.active_counts.desired_pose_mutation -eq 1) `
        'mutation counts'
    Assert-Trace (
        [Math]::Abs(
            $current.dynamic_capture.ranges.manual_pitch_offset.maximum -
            0.125) -lt 0.000001 -and
        [Math]::Abs(
            $current.dynamic_capture.ranges.stock_pose_pitch.maximum) -lt
            0.000001 -and
        [Math]::Abs(
            $current.dynamic_capture.ranges.pose_pitch.maximum - 0.125) -lt
            0.000001) `
        'stock and effective pitch ranges'
    Assert-Trace (
        @($current.dynamic_capture.missing_desired_pose_samples).Count -eq 1 -and
        $current.dynamic_capture.missing_desired_pose_samples[0] -eq 2) `
        'missing desired-pose sample tracking'

    $poisonedFixturePath = Join-Path $temporaryRoot 'poisoned-trace.log'
    $poisonedReportPath = Join-Path $temporaryRoot 'poisoned-report.json'
    $fixtureText = [IO.File]::ReadAllText($fixture)
    $poisonedFixtureText = $fixtureText.Replace(
        'active_fields_readable=0 source_weight_valid=0',
        'active_fields_readable=0 source_weight_valid=1')
    Assert-Trace ($poisonedFixtureText -cne $fixtureText) `
        'poisoned fixture mutation'
    [IO.File]::WriteAllText(
        $poisonedFixturePath,
        $poisonedFixtureText,
        [Text.UTF8Encoding]::new($false))
    & $analyzer -LogPath $poisonedFixturePath -OutputPath $poisonedReportPath
    $poisonedReport =
        [IO.File]::ReadAllText($poisonedReportPath) | ConvertFrom-Json
    $poisonedCapture = $poisonedReport.sessions[0].state_probe_capture
    $poisonedSanitizationFailures = @(
        $poisonedCapture.inactive_state_sanitization_failures)
    Assert-Trace (
        $poisonedSanitizationFailures.Count -eq 1) `
        'correlated inactive source-weight validity failure'

    $legacy = $report.sessions[1]
    Assert-Trace (
        $legacy.camera_hook_status.update_installed -eq 1 -and
        $legacy.camera_hook_status.desired_pose_installed -eq 1 -and
        $null -eq $legacy.camera_hook_status.angular_approach_installed -and
        $null -eq $legacy.camera_hook_status.required_dynamics_hooks_complete) `
        'legacy probe-field compatibility'
} finally {
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container) {
        [IO.Directory]::Delete($temporaryRoot, $true)
    }
}

Write-Output 'VEHICLE_CAMERA_TRACE_ANALYZER=PASS sessions=2 schema=2'
