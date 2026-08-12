# Nexus release notes

## What the two downloads contain

The release is split into two independent archives for Sleeping Dogs:
Definitive Edition:

- **`SPatch-Base.zip`** is the base patch. It owns verified executable fixes,
  high-FPS corrections, input and display tweaks, character restorations,
  configuration migration, crash diagnostics, and native SMAA. Extract this
  archive into the game directory containing `sdhdship.exe`.
- **`ShenLong.zip`** is the optional renderer package. It contains one
  `ShenLong-Package` directory; extract it into a fresh directory outside the
  game folder, then run its installer. ShenLong owns the renderer suite and all
  of its settings in `ShenLong.ini`.

The renderer suite migrated to the independent `ShenLong.asi` companion, but
native SMAA did not: SMAA remains part of `SPatch.asi` in `SPatch-Base.zip`.
Install ShenLong only when its graphics changes are wanted. SPatch does not
require ShenLong, and disabling or removing ShenLong does not disable SPatch's
fixes or native SMAA.

Both modules are native x64 ASIs and require a compatible x64 `dinput8.dll` ASI
loader installed separately beside `sdhdship.exe`. Neither archive bundles or
replaces that loader; an x86 loader cannot load these modules.

## SPatch features

- Corrects cutscene/NIS timing fallbacks at arbitrary expected cadences without
  acting as a global FPS limiter.
- Removes the hidden 120 FPS wait while preserving the game's explicit limiter
  and VSync modes, and can select the highest reported refresh rate.
- Keeps pedestrian density and verified camera/vehicle history windows stable
  at high frame rates.
- Corrects the invalid `1920x1880` first-run resolution before the stock display
  parser consumes it.
- Keeps Scaleform's performance-counter clock full width on long-running
  systems.
- Hardens verified file, archive, QCMP, compressed-XML, resource-stream, save,
  thread-wrapper, VRAM-pool-lock, and contact-list failure paths.
- Reports full dedicated VRAM on the benchmark screen and supports
  resolution-aware spherical reflections.
- Restores the original game's clearer atmosphere and focused neon, Wei's
  affected eye materials, rain/post-swim wetness, and exertion-driven sweat.
- Uses the game's existing Windows Raw Input mouse path, can remove stock mouse
  camera easing, and exposes radial controller deadzones.
- Adds an opt-in GTA IV-like road-vehicle camera with Wei's right-side driving
  position, looser follow yaw, delayed manual-yaw/controller-pitch recentering,
  native mouse-pitch mapping, and extra handbrake swing. Resources and the
  paired class/selector paths establish cars, limousines, exotics, vans, tall
  vans, SUVs, trucks, and buses as the eligible family; only truck heavy-vehicle
  aliasing is runtime-confirmed, and unverified live masks fail closed to native
  behavior. Actual Race, Hijack, Aim, Look, reverse, and special override
  handling also stays native. A separate default-off bike option covers the
  verified motorcycle/scooter Drive block through the same exact selector gate.
  Both options are enabled only on the fully traced legacy executable; latest
  Steam stays native until its gameplay path is observed.
- Provides known-build, fail-closed hook installation, opt-in `SPatch.log`, and
  independent crash dumps.
- Provides native SMAA on the fully mapped legacy executable. Unsupported or
  unready paths retain the game's native anti-aliasing.
- Provides native 4x, 8x, or 16x anisotropic filtering through the game's
  exact sampler builder and verified settings writer, plus an independently
  controlled promotion of only the exact stock trilinear filter-selector
  branch. LOD bias is built independently, so the former zero-bias claim was
  removed. The v42 defaults are `AnisotropicFiltering=16` and
  `ForceAnisotropicFiltering=1`.

SPatch supports only the executable identities documented in its packaged
README. The legacy executable is the fully mapped feature target. The newer
Steam profile intentionally disables features whose paths have not been fully
mapped, and live feature parity is not claimed.

## ShenLong graphics features

- Full-RGB AgX at the exact known final pre-HUD tone-operator shader.
  `AgXLook=MediumHigh` is ShenLong's custom, stock-matched, toe-preserving grade;
  it is not an official or standardized upstream AgX look preset. `Neutral`
  keeps the softer base transform.
- Fail-open shadow-target scaling for exact tracked shadow maps. The retired
  `ShadowFilterScale` and PCSS experiments are not features.
- A choice of native `Original` AO, full-resolution-capable `SDAO`, or
  front-depth `GTAOLite`, with independent settings and native fallback when a
  custom pass is unavailable.
- Screen-space diffuse global illumination before tonemapping. GI does not
  choose or replace the selected AO backend.
- Exact-profile GGX/PBR replacement for 18 validated shader identities.
  Variants `0` and `13` deliberately remain native.
- Exact-profile skin SSS and material-specific scattering for supported eyes,
  hair, teeth, foliage, and water while unknown profiles remain native.
- Three exact water permutations with fixed isotropic scattering strength.
  There is no separate water-anisotropy control.

ShenLong uses a pinned ReShade add-on runtime as its D3D11 event host. It is not
a ReShade `.fx` preset and does not use Luma Core. Exact shader, resource, and
map checks fail open to native rendering when the expected path is absent.

ShenLong authorizes its renderer components only for these exact
`sdhdship.exe` SHA-256 identities:

- legacy researched build:
  `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`;
- latest Steam build:
  `2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA`.

Before component registration or fixed-RVA hooks, ShenLong requires a complete
SHA-256 and PE-profile match. An unreadable or mismatched executable, an
unsupported `ShenLong.ini` schema, or a failed pre-device registration
transaction leaves the profile unpublished, disables ShenLong's custom
component registration, keeps any native detours transparent, and retains
native rendering.

## Changelog

### Configuration version 44

- Extends `[Input] GTAIVCarCamera` to the exact road-vehicle Drive branch and
  captured Flee profile used during the user's police pursuit. The
  resource data and paired mover/class/selector paths establish an eligible
  family of cars, limousines, exotics, vans, tall vans, SUVs, trucks, and buses.
  Only the captured truck's heavy-vehicle aliasing is runtime-confirmed. SPatch
  replays the engine's selector predicates and accepts only a proven live
  Drive/Flee mask, so unverified heavy aliases, actual Race, and actual Hijack
  paths remain native.
- The incoming Drive/Flee target receives only `1 - source_weight` of the
  `-0.35 m` lateral offset while the outgoing snapshot supplies the remaining
  share, preventing the offset from dropping at the transition boundary.
- Adds the full GTA IV-like dynamics candidate to eligible Chase cameras.
  Vehicle-follow yaw is looser and slows further under handbrake; manual yaw is
  held for `0.75 s` after input before a slower recenter; and right-stick/
  controller vertical input becomes a persistent clamped orbit pitch with its
  own `0.75 s` delay before smooth recentering. Mouse pitch instead maps the
  stock absolute `+0x4FC` state to orbit pitch, retaining the game's sensitivity,
  inversion, clamp, one-frame timing, and native decay. It does not receive
  mod-owned persistence or the controller idle delay. Both pitch paths preserve
  eye-to-look distance and still pass through the stock collision-aware
  finalizer.
- Adds independent `[Input] GTAIVBikeCamera=0`. When enabled, the same offset
  and dynamics apply to the verified motorcycle/scooter context-3 Drive block.
  Its Race/Drive or Drive/HijackFront pointer alias is accepted only while the
  exact Drive branch is selected; a distinct motorcycle Flee block stays
  native.
- Actual Race, HijackFront, Aim, Look, passenger, and other specialized blocks
  remain native. Look-back, aim/focus, eye/look locks, reverse handling, and
  other special overrides retain stock yaw/pitch behavior.
- Both settings are strict restart-time `0`/`1` switches and are independently
  class-gated. Latest Steam and safe/unverified compatibility force their
  effective values off without rewriting the user's requested values.
- Camera diagnostics are now `gtaiv_vehicle_camera`,
  `gtaiv_vehicle_camera_probe`, and the bounded Chase update/pose event
  `gtaiv_vehicle_camera_dynamic`. They report both requested classes, the
  replayed Drive-branch result, classified target, complete 13-slot pointer-match
  masks, and `applied_delta_m` (the detour-applied target share, not the
  already-offset outgoing source snapshot). Inactive or partial states are
  zeroed and tagged with active-field/source-weight validity instead of
  exposing the engine's allocator-fill bytes as camera input.
- Migrating v43 without a bike key writes `GTAIVBikeCamera=0`. Explicit
  canonical or sole legacy `gta_iv_bike_camera` values are preserved, the exact
  old bytes are backed up externally as `SPatch-pre-v44.ini`, and
  `WriteCrashDumps=1` remains the literal final INI key.
- Paired disassembly and decompiled-code analysis covers both executable
  layouts. Legacy gameplay has captured normal Drive, the police-pursuit Flee
  transition, motorcycle/scooter aliases, and the aliased truck Drive block.
  Other named road families are resource/class/path-proven rather than
  individually runtime-confirmed. Mutation remains legacy-only. The normal-car
  lateral placement has user gameplay acceptance; the corrected heavy-vehicle
  eligibility, full dynamics, and enabled bike path still require the next
  real-game A/B because the unattended benchmark has no drivable vehicle scene.

### Configuration version 43

- Adds `[Input] GTAIVCarCamera=0`. Enabling it shifts the normal road-car Drive
  camera laterally toward Wei's right-side seat while keeping stock distance,
  height, FOV, collision, springs, look input, and camera-range cycling.
- Paired disassembly/decompiler analysis maps both supported selector and
  setter implementations. A normal-gameplay calibration trace then proves the
  legacy road-car path uses Drive slot `+0x388` and independently proves the
  motorcycle mover exclusion; an earlier static `+0x3B0` interpretation was
  corrected before mutation was enabled.
- The implementation copies the complete `0x140`-byte parameter block with a
  guarded byte copy, changes only six target/eye lateral floats, weights the
  offset through the stock profile transition, validates every source/result
  float, and calls the stock setter exactly once. A shared/ambiguous Drive
  pointer is rejected. Null, unknown, invalid, non-Drive, motorcycle, and boat
  paths fail open to native behavior.
- Existing canonical and legacy snake-case choices migrate without changing
  the selected value. A present but malformed/empty canonical key safely wins
  over old aliases instead of accidentally enabling the feature. Older
  configurations default to the native camera, and `WriteCrashDumps=1` remains
  the literal final organized-INI key.
- The latest-Steam binary mappings are retained for future calibration, but its
  compatibility profile disables this mutation until a latest-build
  normal-gameplay trace proves the same operating path.

### Configuration version 42

- SPatch now owns only verified fixes and tweaks. ShenLong renderer features
  and their INI keys moved to the independent `ShenLong.asi` companion; native
  SPatch graphics tweaks, native texture filtering, `OriginalShadowFilter`, and
  SMAA remain in SPatch.
- `[TextureFiltering]` is owned by SPatch again. Its implementation uses the
  exact game sampler builder, filtering-settings writer, and verified trilinear
  sampler instruction; it is not a ReShade sampler interception.
- SPatch logging is the canonical `[Debug] Logging` option. The diagnostic log
  is capped in place instead of leaving an additional `.old` file.
- `WriteCrashDumps=1` is independent of logging and is literally the final key
  in the organized INI.
- Migration keeps the exact pre-v42 bytes under
  `%LOCALAPPDATA%\SPatch\ConfigBackups`, not beside the game. A legacy
  `SPatch.ini.previous.bak` is relocated there after byte verification. The
  organized `SPatch.ini` excludes ShenLong renderer keys while retaining
  SPatch-owned native graphics, texture-filtering, `OriginalShadowFilter`, and
  SMAA settings.
- Renderer-backend switches, PCSS experiments, and other unverified code or
  options without a demonstrated operating path were removed or retired rather
  than advertised as working guards.
- A live startup trace showed that the stock settings writer is not necessarily
  invoked during an unattended boot. SPatch now publishes and verifies the
  requested anisotropy immediately before every exact native sampler-builder
  call, while retaining the writer detour for later settings commits.
- ShenLong's public contract now matches the verified exact-profile paths:
  AgX, shadow-target scaling, Original/SDAO/GTAOLite AO, GI, 18-shader PBR,
  SSS/material scattering, and three water permutations.

### Audit and stability corrections

- Removed unverified code and retired options whose game paths were not
  demonstrated. Named examples include `RendererBackend` and its swap-chain
  controls, the frame-counted and divide-guard switches, `RestoreVisualDamage`,
  `RestoreCharacterShadows`, `CharacterShadowResolution`,
  `ShadowFilterScale`, and the PCSS experiments.
- ShenLong now completes executable authorization and registers every renderer
  component synchronously in ReShade's pre-D3D11-device event. This prevents
  partially registered components, missed device initialization, and callback
  vector mutation from a background thread.
- Native AO and HairBlur behavior remains closed until the whole component
  registration transaction succeeds. A failed transaction leaves the stock
  renderer active.
- Shadow teardown now closes admission and drains accepted callbacks before it
  releases tracked D3D state. GI now restores the compute-shader resource that
  it temporarily occupies instead of leaking the binding into later passes.
- PBR accounting and diagnostics now state the verified result: 18 replaced
  identities and two deliberate native passthrough variants.
- Water shaders retain the game's normalized Blinn/Fresnel response and add
  only the advertised isotropic Beer-Lambert scattering. Removed controls and
  documentation no longer imply an anisotropic water phase that was not
  implemented.
- Hair scattering is documented according to the captured data: it is a
  material-masked, alpha-preserving separable isotropic profile. The capture
  does not provide fiber-direction data, so no directional-hair claim remains.
- ShenLong validates `ConfigVersion=1` before authorization or hooks. A missing
  or unsupported schema disables the companion, while `Enabled=0` avoids the
  executable hashing and hook path entirely.
- Both canonical ASIs use MSVC `/O2` and full link-time code generation. clang-cl
  was evaluated but was not substituted without an equivalent SEH, ABI,
  deterministic-package, and runtime-hook validation path.
- Investigated the proposed “unconditionally dither hair depth under MSAA”
  change and did not add a duplicate switch. The exact native hair shader
  already performs an unconditional dither discard on a depth-writing draw,
  ShenLong's hair capture mirrors it, and the verified operating path is
  single-sample with later post-process SMAA. See
  `docs/2026-08-09-hair-depth-dither-analysis.md`.

### Installation and troubleshooting

- SPatch and ShenLong packages have separate ownership and uninstall contracts.
  The ShenLong archive uses a single `ShenLong-Package` envelope so extracting
  it cannot overwrite files in the game directory before the installer runs.
- The ShenLong installer validates the existing x64 ASI loader, verifies its
  owned package manifest, rejects package roots inside the game directory, and
  migrates only recognized ShenLong renderer keys. A differing user-owned
  `dxgi.dll` is preserved by refusing the install rather than overwriting it.
- Installer backups are external to the game directory. TextureFiltering keys
  are never copied into ShenLong because they remain SPatch settings. Upgrading
  an existing v1 `ShenLong.ini` surgically removes that retired section and any
  stray filtering keys, preserves all unrelated bytes, and saves the original
  under `%LOCALAPPDATA%\ShenLong\ConfigBackups`.
- `[Debug] Logging=1` enables local `SPatch.log` diagnostics, including the
  loaded SPatch module path, size, and SHA-256 identity. Logging is disabled by
  default and does not export telemetry.

### Crash-report clarification

A submitted `SPatch-20260808-182420-612.dmp` records the game's Main Pool
failing a 3,204-byte, 100-entry allocation tagged `RingBuffer`, followed by the
stock allocator's deliberate `0x80000003` breakpoint. SPatch is on the stack
because its frame-flow detour invokes the original per-frame routine, but the
dump does not identify SPatch or another loaded module as the cause of pool
allocation failure. No speculative allocator bypass was added. See
`docs/2026-08-08-main-pool-ringbuffer-crash-analysis.md` for the evidence and
limits.

## Release-candidate verification

Completed on 2026-08-10 against the installed release-candidate artifacts,
apart from the real-gameplay camera checks listed below:

- `SPatch-Base.zip` SHA-256:
  `A0CCF4F5561D6F493AD772476F494B6840D65547FE338AB49FC201174C8A6BB5`
- `ShenLong.zip` SHA-256:
  `7B5479FB45F2C1CE1E18340FF3747905CAEA57C2E412DBE2626455061EA044BE`
- Build and package validation: the current SPatch FinalRelease passed
  `/W4 /WX`, `/O2`, `/GL`, full `/LTCG`, both normal and
  `SPATCH_FINAL_RELEASE` native tests, deterministic archive checks, package
  whitelist/manifest validation, identity attestation, and transactional
  deployment. Diagnostic and normal-test builds were independently confirmed
  to omit `/GL` and `/LTCG`. The installed, packaged, and build-tree SPatch ASI
  hashes are byte-identical at
  `EB7B6B4C070076FA5C82A41ADCE27F0ED71720D1D1382A88FEB6C4AC2DCF4B52`.
  The installed and packaged ShenLong ASI remains byte-identical at
  `3E7998FECF2177CEBD10334026829EE9048DAEBAF42335496DDDB97D1C34EE58`.
- Qlty 0.640.0: the combined ripgrep/comment and TruffleHog gates, plus the
  separate TruffleHog-only gate, reported no issue on a fresh validation
  mirror. Qlty's `radarlint-java` 2.0.0 plugin was unavailable because it
  requires Java class version 66 (Java 22) while this machine has Java 21;
  that plugin was disabled for the combined gate and is explicitly skipped.
  Cppcheck 2.20.0 reported no warning, performance, or portability issue in
  the changed C++ units and tests.
- Live load/crash smoke: the game started through the exact `Sleeping Dogs DE
  - Unattended Benchmark` shortcut, loaded SPatch hash `EB7B6B4C...DCF4B52`,
  committed all 30 hooks, exited naturally, produced a valid benchmark XML,
  logged no warning or error in the run window, and created no crash dump,
  backup, PDB, deployment journal, or temporary work file in the game folder.
  Evidence:
  `artifacts/runtime-logs/final-camera-load-smoke-20260810-201918/summary.md`.
- Camera visual behavior remains pending the user's real-gameplay A/B. The
  benchmark has no drivable-car scene, so it does not validate offset direction
  or magnitude, transitions, reversing, look/aim behavior, camera cycling, or
  vehicle exclusions.
