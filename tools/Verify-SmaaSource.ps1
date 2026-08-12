[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$upstreamRevision = '71c806a838bdd7d517df19192a20f0c61b3ca29d'
$expectedHashes = [ordered]@{
    'SMAA.hlsl'  = '4DB53D92EF0B45661F8450303589FFABCE93C352CAE4282A6F32DD2B80EFBEB9'
    'AreaTex.h'  = '1933BF43CE86BA71F7ED5F0FA94B148610C634DC3395B9BC67A817D51EE73AF2'
    'SearchTex.h' = '1FAC315AB5F87B60083C9B8387AF17F056204D3D1BF21500F2ACC5B3E8C1A37A'
    'LICENSE.txt' = '2B848B67BA50AC40B53E11CF1CCDAEE2A190CA83B4652CBA695A5F1CE81F52AE'
}

foreach ($entry in $expectedHashes.GetEnumerator()) {
    $path = Join-Path $root "third_party\smaa\$($entry.Key)"
    $stream = [IO.File]::OpenRead($path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            $actual = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    if ($actual -ne $entry.Value) {
        throw "SMAA vendor drift in $($entry.Key): expected upstream $upstreamRevision hash $($entry.Value), got $actual."
    }
}

$sourcePath = Join-Path $root 'third_party\smaa\SMAA.hlsl'
$embeddedPath = Join-Path $root 'src\SmaaShaderSource.inl'
$runtimePath = Join-Path $root 'src\SmaaRuntime.cpp'
$source = [Text.Encoding]::GetEncoding(1252).GetString(
    [IO.File]::ReadAllBytes($sourcePath))
$embeddedText = [IO.File]::ReadAllText($embeddedPath)
$chunkMatches = [regex]::Matches(
    $embeddedText,
    'R"__SPATCH_SMAA__\((?<chunk>.*?)\)__SPATCH_SMAA__"',
    [Text.RegularExpressions.RegexOptions]::Singleline)
if ($chunkMatches.Count -eq 0) {
    throw 'SmaaShaderSource.inl contains no embedded SMAA source chunks.'
}
if ($embeddedText.Contains([char]0xFFFD)) {
    throw 'SmaaShaderSource.inl contains a Unicode replacement character.'
}

function Normalize-SmaaText {
    param([Parameter(Mandatory = $true)][string]$Text)

    $normalized = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    $lines = $normalized.Split(
        [string[]]@("`n"), [StringSplitOptions]::None)
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        # The generated include deliberately removes only horizontal trailing
        # whitespace. Internal whitespace remains significant, so malformed
        # tokens such as '#defineSMAA_THRESHOLD' cannot pass this comparison.
        $lines[$index] = $lines[$index].TrimEnd([char[]]" `t")
    }
    return ([string]::Join("`n", $lines)).TrimEnd([char[]]"`n")
}

$canonicalBuilder = [Text.StringBuilder]::new()
$runtimeBuilder = [Text.StringBuilder]::new()
foreach ($match in $chunkMatches) {
    $chunk = $match.Groups['chunk'].Value.Replace("`r`n", "`n").Replace("`r", "`n")
    [void]$runtimeBuilder.Append($chunk)
    # Each raw-string delimiter contributes one formatting newline that is not
    # part of the vendored file. Remove exactly that newline and nothing else.
    if (-not $chunk.StartsWith("`n", [StringComparison]::Ordinal)) {
        throw 'An embedded SMAA chunk no longer starts at the reviewed raw-string boundary.'
    }
    [void]$canonicalBuilder.Append($chunk.Substring(1))
}
if ((Normalize-SmaaText $canonicalBuilder.ToString()) -cne
    (Normalize-SmaaText $source)) {
    throw 'SmaaShaderSource.inl no longer reconstructs third_party/smaa/SMAA.hlsl.'
}

$runtimeText = [IO.File]::ReadAllText($runtimePath)
$headerMatch = [regex]::Match(
    $runtimeText,
    'static const char\* kHeader = R"SMAAHLSL\((?<shader>[\s\S]*?)\)SMAAHLSL";')
$footerMatch = [regex]::Match(
    $runtimeText,
    'static const char\* kFooter = R"SMAAHLSL\((?<shader>[\s\S]*?)\)SMAAHLSL";')
if (-not $headerMatch.Success -or -not $footerMatch.Success) {
    throw 'The exact runtime SMAA header/footer could not be reconstructed.'
}

$presetContracts = [ordered]@{
    0 = [pscustomobject]@{
        Define = "#define SMAA_PRESET_LOW 1`n"
        Pattern = 'case\s+0:\s*return\s+"#define SMAA_PRESET_LOW 1\\n";'
    }
    1 = [pscustomobject]@{
        Define = "#define SMAA_PRESET_MEDIUM 1`n"
        Pattern = 'case\s+1:\s*return\s+"#define SMAA_PRESET_MEDIUM 1\\n";'
    }
    2 = [pscustomobject]@{
        Define = "#define SMAA_PRESET_HIGH 1`n"
        Pattern = 'case\s+2:\s*default:\s*return\s+"#define SMAA_PRESET_HIGH 1\\n";'
    }
    3 = [pscustomobject]@{
        Define = "#define SMAA_PRESET_ULTRA 1`n"
        Pattern = 'case\s+3:\s*return\s+"#define SMAA_PRESET_ULTRA 1\\n";'
    }
}
foreach ($contract in $presetContracts.Values) {
    if (-not [regex]::IsMatch($runtimeText, $contract.Pattern)) {
        throw 'The runtime SMAA preset mapping drifted from build validation.'
    }
}

$runtimeSamplerContracts = @(
    '(?s)PSSetShader\(g_pipeline\.edge_ps\.Get\(\), nullptr, 0\);\s*ID3D11SamplerState\* const sampler = g_pipeline\.point_sampler\.Get\(\);\s*context->PSSetSamplers\(0, 1, &sampler\);',
    '(?s)PSSetShader\(g_pipeline\.blend_ps\.Get\(\), nullptr, 0\);\s*ID3D11SamplerState\* const sampler = g_pipeline\.linear_sampler\.Get\(\);\s*context->PSSetSamplers\(0, 1, &sampler\);',
    '(?s)PSSetShader\(g_pipeline\.neighborhood_ps\.Get\(\), nullptr, 0\);\s*ID3D11SamplerState\* const sampler = g_pipeline\.linear_sampler\.Get\(\);\s*context->PSSetSamplers\(0, 1, &sampler\);'
)
foreach ($contract in $runtimeSamplerContracts) {
    if (-not [regex]::IsMatch($runtimeText, $contract)) {
        throw 'The runtime SMAA per-pass sampler binding drifted from build validation.'
    }
}

$runtimeResourceContracts = @(
    '(?s)ID3D11Buffer\* const cb = g_pipeline\.constant_buffer\.Get\(\);\s*context->VSSetConstantBuffers\(0, 1, &cb\);\s*context->PSSetConstantBuffers\(0, 1, &cb\);',
    '(?s)PSSetShader\(g_pipeline\.edge_ps\.Get\(\), nullptr, 0\);.*?ID3D11ShaderResourceView\* srvs\[8\] = \{\s*g_pipeline\.source_linear_srv\.Get\(\),\s*nullptr,\s*nullptr,\s*nullptr,\s*nullptr,\s*nullptr,\s*nullptr,\s*nullptr\};\s*context->PSSetShaderResources\(0, 8, srvs\);',
    '(?s)PSSetShader\(g_pipeline\.blend_ps\.Get\(\), nullptr, 0\);.*?ID3D11ShaderResourceView\* srvs\[8\] = \{\s*nullptr,\s*nullptr,\s*nullptr,\s*nullptr,\s*g_pipeline\.edges_srv\.Get\(\),\s*nullptr,\s*g_pipeline\.area_srv\.Get\(\),\s*g_pipeline\.search_srv\.Get\(\)\};\s*context->PSSetShaderResources\(0, 8, srvs\);',
    '(?s)PSSetShader\(g_pipeline\.neighborhood_ps\.Get\(\), nullptr, 0\);.*?ID3D11ShaderResourceView\* srvs\[8\] = \{\s*g_pipeline\.source_srgb_srv\.Get\(\),\s*nullptr,\s*nullptr,\s*nullptr,\s*nullptr,\s*g_pipeline\.blend_srv\.Get\(\),\s*nullptr,\s*nullptr\};\s*context->PSSetShaderResources\(0, 8, srvs\);'
)
foreach ($contract in $runtimeResourceContracts) {
    if (-not [regex]::IsMatch($runtimeText, $contract)) {
        throw 'The runtime SMAA constant-buffer or texture binding drifted from build validation.'
    }
}

$runtimePredicationContracts = @(
    '(?s)void Capture\(\).*?context->GetPredication\(predicate\.GetAddressOf\(\), &predicate_value\);',
    '(?s)void Restore\(\).*?context->SetPredication\(predicate\.Get\(\), predicate_value\);',
    'active_pass->context->SetPredication\(nullptr, FALSE\);',
    '(?s)bool RestoreTextureToBackbuffer\(.*?context->GetPredication\(&predicate, &predicate_value\);.*?context->SetPredication\(nullptr, FALSE\);.*?context->SetPredication\(predicate, predicate_value\);',
    '(?s)if \(!cleanup_ok && pass->source_snapshot_valid &&\s*pass->backbuffer_write_started\).*?RestoreTextureToBackbuffer\(pass->context\.Get\(\),\s*pass->backbuffer\.Get\(\),\s*pass->source_snapshot\.Get\(\)\)',
    '(?s)if \(apply_attempted && pass_completed &&\s*ShouldRestoreSmaaSource\(flags, result\)\).*?RestoreSmaaSourceAfterRejectedPresent\('
)
foreach ($contract in $runtimePredicationContracts) {
    if (-not [regex]::IsMatch($runtimeText, $contract)) {
        throw 'The runtime SMAA predication or failed-frame recovery contract drifted.'
    }
}

function Find-Fxc {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path -LiteralPath $kitsRoot)) {
        throw 'The Windows SDK shader compiler (fxc.exe) was not found.'
    }
    foreach ($version in Get-ChildItem -LiteralPath $kitsRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+' } |
        Sort-Object { [version]$_.Name } -Descending) {
        $candidate = Join-Path $version.FullName 'x64\fxc.exe'
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw 'The x64 Windows SDK shader compiler (fxc.exe) was not found.'
}

$fxc = Find-Fxc
$validationParent = [IO.Path]::GetFullPath(
    (Join-Path $root 'build\smaa-shader-validation'))
$validationRoot = [IO.Path]::GetFullPath((Join-Path $validationParent (
            'run-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N'))))
if (-not $validationRoot.StartsWith(
        $validationParent + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe SMAA validation path: $validationRoot"
}
New-Item -ItemType Directory -Force -Path $validationRoot | Out-Null
try {
$entryPoints = @(
    [pscustomobject]@{
        Name = 'EdgeVS'; Profile = 'vs_5_0'
        Bindings = @('SMAAConstants:cbuffer:cb0:1')
    },
    [pscustomobject]@{
        Name = 'EdgePS'; Profile = 'ps_5_0'
        Bindings = @('PointSampler:sampler:s0:1', 'colorTex:texture:t0:1')
    },
    [pscustomobject]@{
        Name = 'BlendVS'; Profile = 'vs_5_0'
        Bindings = @('SMAAConstants:cbuffer:cb0:1')
    },
    [pscustomobject]@{
        Name = 'BlendPS'; Profile = 'ps_5_0'
        Bindings = @(
            'LinearSampler:sampler:s0:1',
            'edgesTex:texture:t4:1',
            'areaTex:texture:t6:1',
            'searchTex:texture:t7:1',
            'SMAAConstants:cbuffer:cb0:1')
    },
    [pscustomobject]@{
        Name = 'NeighborhoodVS'; Profile = 'vs_5_0'
        Bindings = @('SMAAConstants:cbuffer:cb0:1')
    },
    [pscustomobject]@{
        Name = 'NeighborhoodPS'; Profile = 'ps_5_0'
        Bindings = @(
            'LinearSampler:sampler:s0:1',
            'colorTex:texture:t0:1',
            'blendTex:texture:t5:1',
            'SMAAConstants:cbuffer:cb0:1')
    }
)
$compiled = 0
$reflectedShaders = 0
foreach ($preset in $presetContracts.GetEnumerator()) {
    $shaderPath = Join-Path $validationRoot "smaa-preset-$($preset.Key).hlsl"
    $shaderText = $headerMatch.Groups['shader'].Value +
        $preset.Value.Define + $runtimeBuilder.ToString() +
        $footerMatch.Groups['shader'].Value
    [IO.File]::WriteAllText($shaderPath, $shaderText, [Text.UTF8Encoding]::new($false))

    foreach ($entry in $entryPoints) {
        $outputPath = Join-Path $validationRoot `
            "smaa-preset-$($preset.Key)-$($entry.Name).cso"
        $arguments = @(
            '/nologo', '/WX', '/Ges', '/Gis', '/O3',
            '/T', $entry.Profile,
            '/E', $entry.Name,
            '/Fo', $outputPath,
            $shaderPath)
        $compilerOutput = @(& $fxc @arguments 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "FXC rejected SMAA preset $($preset.Key) $($entry.Name): $($compilerOutput -join [Environment]::NewLine)"
        }
        $dumpOutput = @(& $fxc /nologo /dumpbin $outputPath 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "FXC could not reflect SMAA preset $($preset.Key) $($entry.Name): $($dumpOutput -join [Environment]::NewLine)"
        }
        $bindingMatches = [regex]::Matches(
            ($dumpOutput -join [Environment]::NewLine),
            '(?m)^\s*//\s+(?<Name>[A-Za-z_][A-Za-z0-9_]*)\s+(?<Type>cbuffer|texture|sampler)\s+.*?\s+(?<Bind>cb\d+|[tsu]\d+)\s+(?<Count>\d+)\s*$')
        $actualBindings = @($bindingMatches | ForEach-Object {
            '{0}:{1}:{2}:{3}' -f $_.Groups['Name'].Value,
                $_.Groups['Type'].Value,
                $_.Groups['Bind'].Value,
                $_.Groups['Count'].Value
        } | Sort-Object)
        $expectedBindings = @($entry.Bindings | Sort-Object)
        if ([string]::Join('|', $actualBindings) -cne
            [string]::Join('|', $expectedBindings)) {
            throw "SMAA preset $($preset.Key) $($entry.Name) binding mismatch. Expected [$($expectedBindings -join ', ')], found [$($actualBindings -join ', ')]."
        }
        ++$reflectedShaders
        ++$compiled
    }
}
if ($compiled -ne 24) {
    throw "SMAA shader validation coverage drifted: expected 24 variants, compiled $compiled."
}
if ($reflectedShaders -ne 24) {
    throw "SMAA resource reflection coverage drifted: expected 24 shaders, reflected $reflectedShaders."
}

Write-Host "Validated SMAA vendor revision $upstreamRevision, exact embedded reconstruction, all 24 runtime shader variants, and every reflected cbuffer/texture/sampler binding."
} finally {
    if (Test-Path -LiteralPath $validationRoot) {
        $checkedValidationRoot = [IO.Path]::GetFullPath($validationRoot)
        if (-not $checkedValidationRoot.StartsWith(
                $validationParent + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean unsafe SMAA validation path: $checkedValidationRoot"
        }
        Remove-Item -LiteralPath $checkedValidationRoot -Recurse -Force
    }
}
