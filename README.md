# SPatch

SPatch is a native x64 ASI patch for **Sleeping Dogs: Definitive Edition**.
It applies verified executable fixes, timing and input corrections, native
texture filtering, SMAA, diagnostics, and configuration migration.

This repository contains SPatch source code only.

- Nexus Mods: https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148?tab=description

## Install

SPatch requires a compatible x64 ASI loader beside `sdhdship.exe`. An x86
loader cannot load the patch. The supported executable identities are:

- legacy/full-feature build: `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`;
- latest Steam build: `2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA`.

Extract the contents of `SPatch-Base.zip` beside `sdhdship.exe`. Keep
`SPatch.asi` and `SPatch.ini` together. The latest Steam compatibility profile
disables features whose runtime paths are not fully mapped.

To uninstall SPatch, remove `SPatch.asi`. Remove `SPatch.ini`, `SPatch.log`,
and `SPatch-*.dmp` only when their settings and diagnostics are no longer
needed. Configuration migration backups live under
`%LOCALAPPDATA%\SPatch\ConfigBackups`.

## Included fixes

- known-build verification, fail-closed gating, and transactional patching;
- cutscene/NIS timing recovery for arbitrary configured cadences;
- display-setting persistence and removal of the hidden 120 FPS wait;
- native fog, eye-material, wetness, sweat, shadow-filter, and VRAM fixes;
- file, archive, QCMP, compressed-XML, resource-stream, and save-safety guards;
- native Windows Raw Input, controller deadzones, and opt-in vehicle cameras;
- native texture-filtering controls and canonical SMAA 1x;
- optional file logging and bounded crash minidumps.

## Configuration

`SPatch.ini` is generated beside `SPatch.asi` and uses grouped PascalCase
settings. Legacy snake-case names remain readable during migration. Common
sections include:

- `[Cutscenes]` for cadence and scene-time recovery;
- `[Display]` for first-run resolution and refresh behavior;
- `[Input]` for raw mouse input, controller deadzones, and vehicle cameras;
- `[TextureFiltering]` for native anisotropic filtering;
- `[AntiAliasing]` for SMAA;
- `[Stability]` for file, resource, save, and thread guards;
- `[Debug]` for local logging and crash dumps.

Set `[Debug] Logging=1` to write `SPatch.log`. Set
`[Debug] WriteCrashDumps=1` to write a small timestamped dump when the game
crashes. Diagnostics stay local; SPatch exports no telemetry.

## Build and test

From the repository root:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1

& $msbuild .\SPatchTests.vcxproj /t:Build /p:Configuration=Release `
    /p:Platform=x64 /p:FinalRelease=false
.\build\tests\Release\false\SPatchTests.exe

& $msbuild .\SPatch.vcxproj /t:Rebuild /p:Configuration=Release `
    /p:Platform=x64 /p:FinalRelease=true
```

The project validates the pinned MinHook and SMAA inputs during the build.
Normal builds write only under `build\`; release packaging writes under
`artifacts\`.

## Documentation

- [Texture-filtering paired analysis](docs/2026-08-10-texture-filtering-paired-analysis.md)
- [Main-pool crash analysis](docs/2026-08-08-main-pool-ringbuffer-crash-analysis.md)
- [Modernization roadmap](docs/plans/2026-03-31-spatch-modernization-roadmap.md)
- [Changelog](CHANGELOG.md)
