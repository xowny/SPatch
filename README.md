# SPatch

SPatch is a known-build `.asi` patch for **Sleeping Dogs: Definitive Edition**.
The release configuration keeps the normal game path quiet and leaves internal
reverse-engineering probes out of the shipped end-user configuration.
SPatch is process-lifetime software; hot unloading/reloading the ASI while the
game is running is unsupported.

SPatch owns executable fixes and non-renderer tweaks, including its native
texture-filtering controls. The optional AgX, shadow-map scaling, AO, GI, PBR,
SSS, material-scattering, and water renderer features are supplied by the
independent `ShenLong.asi` companion and configured in `ShenLong.ini`; they are
not SPatch hooks or `SPatch.ini` keys.

## Installation and uninstall

SPatch is a native x64 ASI and requires a compatible **x64** ASI loader. The
base release deliberately does not bundle or silently replace a loader. Install
an x64 `dinput8.dll` ASI loader beside `sdhdship.exe` first; an x86 loader cannot
load SPatch. The supported game executables have these SHA-256 identities:

- legacy/full-feature build:
  `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`;
- latest Steam build:
  `2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA`.

The legacy identity is the fully mapped, full-feature target. The latest-Steam
compatibility profile intentionally disables `RestoreSweat`,
`FixDuplicateCutsceneActorRestore`, the rumble override, SMAA, and internal
probes whose latest-build paths are not fully mapped. Latest-build support has
static identity/signature and unit coverage only in this release; no live
latest-build runtime smoke or full-feature parity is claimed.

Extract the contents of `SPatch-Base.zip` into the Sleeping Dogs: Definitive
Edition directory containing `sdhdship.exe`. Keep `SPatch.asi` and the supplied
configuration-v44 `SPatch.ini` beside the executable. `README.md`,
`THIRD_PARTY_NOTICES.md`, `licenses`, and `SHA256SUMS.txt` are release records
and may remain in that directory. When its renderer features are wanted, extract
the separately validated ShenLong archive outside the game directory and run
the installer from its `ShenLong-Package` folder.

To uninstall the base patch, remove `SPatch.asi`. Remove `SPatch.ini`,
`SPatch.log`, and `SPatch-*.dmp` only if their user configuration or diagnostics
are no longer wanted. Versioned migration backups are stored outside the game
directory under `%LOCALAPPDATA%\SPatch\ConfigBackups`; remove them separately
only when rollback is no longer wanted. Uninstall the separate
graphics package by removing `ShenLong.asi`, `ShenLong.ini`,
`ShenLong-SHA256SUMS.txt`, and the `ShenLong` directory. Delete the shared
`dxgi.dll`, `ReShade.ini`, or ReShade logs only when no other add-on uses them.
Do not delete a shared ASI loader unless no other installed ASI uses it.

Stable features include:

- the cutscene/NIS zero-tick fix, with state-aware scene-time recovery;
- arbitrary expected cutscene cadences (`30`, `60`, `90`, `120`, `144`, `165`,
  `240`, and other values accepted by the configuration);
- high-refresh-safe canonical SMAA 1x on the fully mapped legacy build and
  native-renderer fog guards;
- removal of the hidden 120 FPS wait while preserving explicit limiter and
  VSync modes, plus exact-output maximum-refresh selection;
- correction of the Definitive Edition's invalid `1920x1880` clean-profile
  default before its first-run display-mode parser sees it;
- full-width conversion of Scaleform's performance-counter clock, preventing a
  periodic backward time jump after long Windows uptimes;
- correct Win32 file-open handling for timestamp updates and missing MP3
  metadata paths, complete 64-bit file sizes for files at or above 4 GiB, and
  failure-safe seek/read/tell/size propagation for streamed files, plus
  rejection of archive entries whose reads exceed their `.big` files and
  retry-safe recovery from failed or short resource reads, including loose-file
  size/open failures that the engine otherwise mislabels or leaves pending;
- bounded traversal of loaded resource-chunk streams, including validation of
  BIG-file index counts, relative entry pointers, and sort order before an
  index can enter the engine's binary-search inventory;
- a single-pass bounded QCMP decoder that validates compressed input, output,
  and back-references before every access, plus a guard against the engine
  converting a decode failure into a 4 GiB buffer copy; compressed-XML loads
  also reject wrapped allocations, null allocations, and incomplete decodes
  before falling back to the engine's loose-file path;
- full-width DXGI dedicated-memory reporting on the PC benchmark screen,
  including GPUs with 4 GiB or more VRAM;
- balanced VRAM-pool locking when `ForceEmptyPool` runs, preventing its
  recursive critical-section acquisition from surviving pool shutdown;
- restoration of the original game's clearer atmosphere by suppressing the
  Definitive Edition-only volumetric-fog layer;
- restoration of Wei's original HD eye diffuse and reflection detail in
  cutscenes, using the texture already shipped by Definitive Edition;
- native shadow filtering; optional shadow-map scaling belongs to ShenLong;
- restoration of rain and post-swim wetness, with configurable full-wet and
  linear-fade durations, plus running- and combat-driven sweat;
- guarded rejection of truncated local saves before the stock checksum/table
  reader, and correct failure sentinels for the engine's thread wrappers;
- a 30 Hz pedestrian-density controller wrapper that preserves the stock
  response rate instead of running it once per rendered frame;
- forced use of the game's existing Windows Raw Input mouse path, optional
  removal of mouse-look/aim smoothing, and configurable controller deadzones;
- independent opt-in GTA IV-like road-vehicle and motorcycle/scooter cameras
  with a right-side driving position, looser follow yaw, delayed manual-yaw and
  controller-pitch recentering, native mouse-pitch mapping, and extra handbrake
  swing; specialized contexts remain native;
- resolution-aware spherical reflections in place of the engine's fixed
  `1280x640` target, with an automatic or user-selected width;
- native shadow filtering with the original game's quality selection;
- optional ShenLong renderer features with exact-profile, fail-open dispatch;
- versioned, SilentPatch-style `SPatch.ini` names with legacy snake-case/section
  compatibility and automatic backup during migration;
- known-build verification, fail-closed behavior gating, and transactional
  static-byte patching/restoration. Successfully created process-lifetime
  detours stay transparent after a late initialization failure.

## Executable fixes and diagnostics


The generated INI groups the supported executable corrections by purpose:

- `[Display]`: `FixFirstRunResolution`;
- `[Graphics]`: `FixVRAMReporting`;
- `[Stability]`: `FixScaleformTimerOverflow`, `FixFileTimestampUpdates`,
  `FixAudioFileOpen`, `FixLargeFileSizes`, `FixVRAMPoolLock`, and
  `FixResourceLoading`, `FixCorruptSaveCrash`, `FixThreadCreationFailure`, and
  `FixContactListOverflow`.

`FixResourceLoading` controls the complete resource-safety transaction: file
error propagation, failed-read recovery, archive-entry bounds, QCMP bounds,
compressed-XML recovery, resource-chunk traversal, and BIG-index validation.
Disabling it restores those stock paths together instead of leaving a partial
combination of dependent guards.

`FixContactListOverflow` removes a DE-only, unused `sprintf` that appends an
image suffix to a contact name in a fixed 64-byte stack buffer. The formatted
value is never consumed; the fix leaves the contact-list arguments and UI
behavior unchanged.

File logging is available in the end-user build but remains off by default:

```ini
[Debug]
Logging=0
```

Set it to `1` and restart the game to write `SPatch.log` beside `SPatch.asi`.
The one log is truncated in place after 8 MiB, so rotation does not leave an
extra `.old` file in the game directory. `WriteCrashDumps=1` is the shipped
default, is independent from logging, and is deliberately the final key in
`SPatch.ini`:

```ini
[Debug]
Logging=0

; Write a small diagnostic dump if the game crashes.
WriteCrashDumps=1
```

## Cutscene FPS setting

In `[Cutscenes]`, `CutsceneFPS=0` follows the cadence supplied by the game and
is the recommended setting. Every integer from `15` through `1000` is accepted;
a positive value tells the correction logic which external cadence the
game/display is already using, for example:

```ini
[Cutscenes]
FixCutsceneFPS=1
CutsceneFPS=144
```

This is a cutscene timing correction, not a renderer/physics FPS limiter. Use
the game's limiter, driver, or an external cap to select the actual refresh
rate. Auto mode preserves genuine measured hitches while repairing transient
legacy cadence steps against the same cutscene's stable rate. With an explicit
target, known 60/30 Hz fallbacks are repaired against that target. Sustained
rate changes are adopted instead of being forced indefinitely, preventing the
patch from changing playback speed when the game is genuinely busy.

## Input

Sleeping Dogs already registers the mouse through Windows Raw Input and reads
relative `WM_INPUT` deltas. `ForceRawMouseInput=1` forces the game's hidden
`PCMouseInputRaw` option and its current runtime state, so SPatch does not add a
second mouse device or duplicate the game's cursor/UI routing. Set it to `0` to
leave the hidden game option unchanged.

`DisableCameraSmoothing=1` removes the stock time-based FollowCamera drain from
mouse look and aiming. It does not alter camera collision, scripted camera
transitions, positional springs, or controller look. Keyboard gameplay input is
already handled as direct digital window messages and has no comparable
acceleration or smoothing stage, so there is deliberately no misleading raw
keyboard switch.

Controllers are already polled directly through XInput or DirectInput once per
active update. SPatch therefore exposes the radial stick filters instead of
adding another controller input stack. `-1` preserves the stock deadzone, `0`
is unfiltered, and `1` through `95` selects a custom percentage. An unfiltered
stick can drift on worn hardware.

`GTAIVCarCamera=1` applies only while the verified road-vehicle selector chooses
its exact Drive block or unique Flee block used during a police pursuit. This is
a class-and-profile check, not a short model-name allow-list. Game resources and
the paired mover/selector paths establish the eligible road family: cars,
limousines, exotic cars, vans, tall vans, SUVs, trucks, and buses. Runtime has
explicitly confirmed the truck's aliased Drive path; SPatch does not assume that
every other heavy-vehicle property set has the same alias layout. It replays the
game's branch predicates and accepts only a proven Drive/Flee slot mask, so an
unverified alias combination fails closed to native behavior. An actual Race or
HijackFront selection likewise stays native even if its slot shares a pointer
with Drive.

For an eligible Drive or Flee camera, SPatch applies the same `-0.35 m`
vehicle-local lateral delta to the three target and three eye ranges. During a
stock profile transition, only the incoming target's remaining share is added;
the outgoing source block retains its existing share. This keeps the lateral
offset continuous across the captured Drive-to-Flee switch.

The option also enables the GTA IV-like motion model on that eligible Chase
camera. Automatic follow yaw is slower, and slows further while the handbrake is
held so the vehicle can rotate beneath the camera. Manual yaw is not immediately
pulled back to center; after `0.75` seconds without input it recenters at a
reduced rate. Right-stick/controller vertical input drives a persistent,
clamped orbit pitch; after its own `0.75` seconds without controller input it
smoothly recenters. Mouse pitch is deliberately different: SPatch maps the
game's absolute `+0x4FC` mouse-look state to orbit pitch each frame instead of
creating a second persistent mouse accumulator. This preserves the game's mouse
sensitivity, inversion, clamp, one-frame timing, and native decay. Both pitch
paths preserve the current eye-to-look distance and pass through the game's
collision-aware finalizer.

`GTAIVBikeCamera=1` independently enables the same offset and motion model for
the context-3 Drive block used by verified motorcycles and scooters. A bike
property set may alias that block through Race/Drive or Drive/HijackFront slots;
the same exact Drive-branch replay is required. A distinct motorcycle Flee,
Aim, Look, passenger, or Hijack block remains native. The setting is off by
default and does not depend on `GTAIVCarCamera`.

Actual Race, HijackFront, Aim, Look, passenger, and other specialized parameter
blocks remain native. Look-back, aim/focus, eye/look locks, reverse handling,
and other special camera overrides also retain their stock behavior rather than
receiving the new yaw/pitch dynamics. Camera cycling and FOV remain controlled
by the game; invalid, unreadable, or ambiguous state fails open to native
behavior.

These boundaries come from paired disassembly and decompiled-code review of
both supported executable layouts plus captured legacy-build gameplay. Runtime
traces contain normal road-car Drive, a police-pursuit Drive-to-Flee transition,
motorcycle/scooter Drive aliases, and the Race/Drive/HijackFront alias used by a
truck. The other named road families are resource/class/path-proven rather than
individually runtime-confirmed, and unknown live alias masks remain native. The
normal road-car offset has received real-gameplay visual acceptance. The
corrected truck/heavy-vehicle eligibility and the new loose-follow,
manual-pitch, recenter, and handbrake behavior still need the next real-gameplay
A/B, as does the enabled bike path; the unattended benchmark has no drivable
vehicles and is only a load/crash check for this feature. The active camera
mutation remains limited to the fully traced legacy executable. The
latest-Steam profile and safe compatibility mode force both camera options off,
regardless of their requested INI values.

With `[Debug] Logging=1`, the hook-install record is
`gtaiv_vehicle_camera`, sampled state-change records are
`gtaiv_vehicle_camera_probe`, and bounded Chase-update/pose samples are
`gtaiv_vehicle_camera_dynamic`. The records expose the selected Drive-branch
result and slot-match masks for alias diagnosis. `applied_delta_m` is the delta
added by the current setter call, including its transition share; it is not a
claim about the total already present in the outgoing live block. Inactive or
partially read camera state is marked unreadable and publishes zeroed values
with explicit `active_fields_readable` and `source_weight_valid` fields, so
allocator poison cannot masquerade as a look-back or transition state.

```ini
[Input]
ForceRawMouseInput=1
DisableCameraSmoothing=1
GTAIVCarCamera=0
GTAIVBikeCamera=0
LeftStickDeadzone=-1
RightStickDeadzone=-1
```

Input changes require a game restart.

## Wetness timing

`RestoreWetness=1` restores the missing rain and post-swim material wetness.
`WetnessFullTime` controls how many seconds Wei remains fully wet after leaving
water, and `WetnessFadeTime` controls the following linear fade. The defaults
are 30 and 270 seconds, for a total of five minutes. Both accept values from 0
through 3600 and require a game restart after editing. A zero full-wet time
starts fading immediately; a zero fade time removes the effect immediately
after the full-wet period.

```ini
[Graphics]
RestoreWetness=1
WetnessFullTime=30
WetnessFadeTime=270
RestoreSweat=1
SweatBuildTime=150
SweatFadeTime=120
SweatOnsetTime=30
SweatRunSpeed=2.5
SweatCombatTime=15
```

The sweat controls apply to Wei and NPCs. `SweatBuildTime` is the time of
continuous running or combat needed to reach full sweat after onset,
`SweatFadeTime` is the linear dry-down, and `SweatOnsetTime` is the minimum
continuous exertion before any visible amount is added. `SweatRunSpeed` is the
horizontal movement threshold.
`SweatCombatTime` keeps an NPC's sweat active briefly after a melee hit so
stationary attack animations are covered too. All five values are configurable
and clamped to safe ranges; the defaults are intentionally slower than the
immediate sheen caused by the stock 30-Hz fallback.

## Original Wei eye materials

`RestoreOriginalEyeReflections=1` repairs three affected Wei eye materials: the
HD cutscene head plus the standard and HD gang-head variants. Definitive Edition
points their diffuse slots at the blank 32x32 `DEFAULTDIFFUSE00` fallback even
though the matching standard and HD Wei head/iris textures remain installed.
SPatch validates each exact material UID, shader, resolution-specific bump map,
spherical reflection map, parameter layout, and fallback value before selecting
the matching standard or HD diffuse. Every other character material remains
untouched. Set it to `0` to keep the Definitive Edition materials.

```ini
[Graphics]
RestoreOriginalEyeReflections=1
```

## Original fog and neon

`RestoreOriginalFogAndNeon=1` disables the additional volumetric-fog layer
introduced by Definitive Edition. The original time-of-day, night-zone,
weather, sky fog, emissive materials, and bloom remain active. This removes the
washed-out veil and restores focused neon contrast without adding a separate
neon override. Set it to `0` to keep Definitive Edition's volumetric fog.
Changing it requires a game restart.

```ini
[Graphics]
RestoreOriginalFogAndNeon=1
```

## Anisotropic filtering

These native sampler tweaks belong to SPatch and live in `SPatch.ini`, not
`ShenLong.ini`.

The PC renderer already requests 4x anisotropic filtering for its native
anisotropic material samplers. `AnisotropicFiltering=-1` preserves that stock
value. Set it to `4`, `8`, or `16` to publish that value immediately before
each exact native sampler-builder invocation, then read it back before the
builder runs. The verified filtering-settings writer is also detoured so later
game setting commits use the same exponent and retain the game's own sampler
rebuild path. Any other present value, including `0`, `1`, or the
quality-reducing `2`, falls back to the transparent `-1` policy instead of
exposing a downgrade or off mode.

`ForceAnisotropicFiltering=1` separately changes only the verified engine
filter-selector branch that emits stock trilinear (`0x15`) to anisotropic
(`0x55`). Point, comparison/shadow, and already-anisotropic selector branches,
plus ShenLong-owned post-process samplers, remain unchanged. The builder
computes LOD bias independently, so SPatch does not claim an unproved zero-bias
restriction. Both controls require a game restart. Unknown executable builds
leave both paths untouched.

```ini
[TextureFiltering]
AnisotropicFiltering=16
ForceAnisotropicFiltering=1
```

## Shadows

The user release uses Stock Native D3D11 and keeps the game's native shadow
filtering. In `[Shadows]`, `OriginalShadowFilter=-1` follows the in-game
Normal/High selection; `0` forces Normal and `1` forces High.

`ShadowResolution` controls only ShenLong's captured shadow-map policy:

- `0` retains every captured native map size and its native filter constants;
- `2048` floors captured 512, 1024, and 1408 map classes to 2048;
- `4096` keeps those smaller classes at 2048, doubles captured native 2048 maps
  to 4096, and leaves native 4096 maps capped at 4096.

The base ASI does not claim a separate static/native shadow-resolution patch.
`RestoreCharacterShadows`, `CharacterShadowResolution`, and `ShadowFilterScale`
are retired and are removed during configuration-v39 migration. No PCSS add-on
or PCSS keys are shipped.

## Anti-aliasing

`SMAA=1` replaces the game's post-process AA with canonical three-pass
color-edge SMAA 1x from reviewed upstream revision
`71c806a838bdd7d517df19192a20f0c61b3ca29d`. Edge detection and blending-weight
calculation use non-sRGB views. The final neighborhood pass samples and writes
through sRGB views so interpolation occurs in linear light. If the backbuffer
cannot expose a compatible sRGB render-target view, SPatch uses a private
compatible target and copies the finished image back. The edge pass writes the
canonical stencil mask, so the expensive weight search runs only on detected
edge pixels without changing image quality.

```ini
[AntiAliasing]
SMAA=1
SMAAPreset=3
```

`SMAAPreset` accepts 0 through 3 (low, medium, high, and ultra); Ultra is the
default. SPatch suppresses stock AA only after the current swapchain and device
complete an SMAA pass and the frame is presented successfully. Resize, device,
format, resource, or presentation failures immediately restore the stock path.
Test-only probes are left untouched. Real nonblocking presents receive SMAA;
if DXGI rejects one as still drawing, SPatch restores the retained source image
before returning so a retry can never filter the same backbuffer twice.

SMAA is enabled only on the fully mapped legacy executable. The latest-Steam
executable keeps stock AA until its complete suppression path is independently
mapped, preventing the two implementations from being stacked.

## Ambient occlusion

This renderer feature belongs to ShenLong and is configured in
`ShenLong.ini`. Unknown or unready paths retain native AO.

`AmbientOcclusion` selects `Original`, `SDAO`, or `GTAOLite`. `Original` keeps
the game's AO renderer and its Normal/High quality setting. It is never replaced
merely because global illumination is enabled. `SDAO` creates a full-resolution
stochastic depth array that records randomly selected surfaces, including hidden
geometry that an ordinary visible-depth buffer misses. Each layer uses a
selection probability of `0.2`. Quality levels 0 through 4 use `1`, `2`, `2`,
`4`, and `4` layers respectively.

SDAO derives a capture-only pixel shader from each validated native geometry
shader. The derivative retains only the data-flow slice needed by the native
alpha discard, then writes packed stochastic depths to full-resolution RG32
targets using fixed-function MIN blending. The untouched native draw follows
with its original MRT, depth, blend, and sample-mask state, so full material
shading runs once and keeps the game's early-depth path. Nine exact reviewed
shader identities use a bounded contiguous alpha-prefix fallback; every other
unsupported transform fails open to native AO. This replaces the former UAV
atomic path without reducing the layer count, selection probability, AO
resolution, sample budget, or filter quality.
`GTAOLite` skips shader instrumentation and hidden-surface capture entirely. It
uses only the native front-depth buffer with the same reviewed horizon and
reconstruction stages. It allocates, captures, and samples no stochastic depth
layers; no comparative performance claim is made without matched measurements.

The stochastic layers feed a DiligentFX-derived, cosine-weighted horizon
evaluation at the final lighting resolution using projection constants captured
from the native AO pass. Geometric normals are reconstructed from the closest
visible-depth derivatives, avoiding material-normal detail and depth
discontinuities at character and object silhouettes. A thickness-aware horizon
update prevents an isolated foreground depth sample from being copied onto a
nearby background at every search radius. A deterministic cross-bilateral
reconstruction pass reduces residual noise without requiring unavailable motion
vectors or temporal history. No half-resolution AO upsample or coarse depth-mip
sampling is used.

The replacement is part of ShenLong, a native ReShade API add-on written
specifically for the game, not a screen-space `.fx` filter. It does not load
Luma Core or a general shader analyzer. The exact final-composition shader and
every required binding, format, sample count, and dimension are validated
before ShenLong substitutes its AO texture at the original final-composition
draw. Stock AO dispatches remain
scheduled as a same-frame fail-open fallback, but their texture is not
composited after a successful custom-AO replacement. Any capture, validation,
or rendering failure leaves the stock AO result at composition, so the results
are never stacked.

On the verified native scheduler path, ShenLong selects the engine's high-quality
AO stage in the live render context while either custom backend is active. At
startup it briefly stages High so the game loads that render path, then
atomically restores the saved `DisplaySettings.xml` value as soon as the AO
stage starts. Builds without that verified scheduler signal do not stage a
persisted override. This keeps the user's Normal/High choice intact when
returning to Original AO.

ShenLong resolves the settings file from the installed game layout: Steam uses
`data\DisplaySettings.xml`, while GOG uses `Save\DisplaySettings.xml`. The
resolved path is used only for crash-safe AO scheduler staging/restoration and
the explicit `OriginalAOQuality` override. Other display fixes remain SPatch
features and do not consume this ShenLong path.

The ReShade Home tab may say `No effect files found` when opened manually.
That is expected: the Home tab only lists `.fx` effects, while the graphics
modules are native add-ons. `ReShade.log` records the selected AO backend and
its quality. SDAO additionally records stochastic-capture activation after the
first successful instrumented draw.

```ini
[AmbientOcclusion]
AmbientOcclusion=SDAO
OriginalAOQuality=-1
SDAOQuality=2
SDAORadius=0.5
SDAOStrength=100
GTAOLiteQuality=2
GTAOLiteRadius=0.5
GTAOLiteStrength=100
```

`OriginalAOQuality=-1` follows the in-game Normal/High selection; `0` forces
Normal and `1` forces High. `SDAOQuality` accepts 0 through 4 (low through
ultra); every level remains full-resolution and uses `1`, `2`, `2`, `4`, or `4`
stochastic depth layers. `SDAORadius` is measured in metres and accepts 0.05
through 5.00.
The recommended default is 0.5; values above 1.0 intentionally create broader
and darker occlusion.
`SDAOStrength` is a percentage from 0 through 200. `GTAOLiteQuality`,
`GTAOLiteRadius`, and `GTAOLiteStrength` use the same documented ranges but tune
only GTAO Lite. Backend and quality changes require a game restart.

## Global illumination

This renderer feature belongs to ShenLong and is configured in
`ShenLong.ini`.

`GlobalIllumination=1` enables ShenLong's separate clean-room 2023
visibility-bitmask screen-space diffuse GI (VBAO) before tonemapping. GI adds
diffuse bounce lighting only. It neither selects nor replaces ambient occlusion:
`Original`, `SDAO`, and `GTAOLite` continue to own AO independently while GI is
enabled, and disabling GI does not change the selected AO backend.

The optimized path builds an octahedrally encoded `RG16F` normal pyramid once
per frame instead of reconstructing five depth neighbours at every GI hit. Its
final pass writes the indirect RGB term directly into the native HDR lighting
target with fixed-function additive blending. Native lighting alpha is
preserved, and the former 4K full-size staging target, source-lighting sample,
and copy-back are gone.

```ini
[GlobalIllumination]
GlobalIllumination=1
GIQuality=2
GIStrength=100
GIRadius=15
```

`GIQuality` accepts 0 through 4. Levels 0 through 3 run the low-frequency
diffuse signal at half resolution with depth-aware reconstruction; level 3 uses
six visibility slices and radius-3 bilateral filtering. Level 4 remains the
full-resolution, eight-slice Ultra mode for screenshots or exceptionally fast
GPUs. `GIStrength` accepts 0 through 200 percent. `GIRadius` is measured in
metres and accepts 0.25 through 30.0; the default is 15. AO quality, radius, and
strength settings do not affect GI, and GI settings do not affect AO. Changes
require a game restart.

## Physically based rendering

This renderer feature belongs to ShenLong and is configured in
`ShenLong.ini`.

`PhysicallyBasedRendering=1` recognizes 20 exact world-lighting identities:
17 known opaque-lighting shaders, live vehicle-glass pixel shader
`0x282EE2DC`, and vehicle-paint G-buffer writers `0x5DB1CB6E` and
`0xE611C192`. It compiles and validates 18 replacements: 15 direct-lit opaque
variants, the glass variant, and both paint variants. Recognized variants `0`
and `13` deliberately remain native and have no replacement cache. The
direct-light paths use an energy-conserving Cook-Torrance
microfacet BRDF.
The ambient-only opaque environment variant and six-axis irradiance-volume
variant `0xD71D285B` remain native. Mixed ambient/sun variants preserve their
native ambient contribution and replace only their direct-sun response. The
opaque replacements use the game's existing per-pixel base color/F0, normal,
and gloss G-buffer data, converting the stock gloss exponent directly to GGX
`alpha^2=2/(exponent+2)`. Replaced opaque families preserve their native
per-channel grazing reflectance `F90=min(1,50*F0)`, preventing zero-F0 foliage
and interiors from acquiring a false white grazing lobe. The unit-grazing
volume pass stays native because it exposes irradiance but no prefiltered
reflected-radiance input; replacing its energy split caused severe grazing-angle
interior darkening in clean 4K A/B captures.
Screen-space normal derivatives add geometric-variance specular antialiasing to
opaque lighting without clamping HDR output. The glass replacement preserves
its two-sided normal, alpha, fog, spherical-reflection coordinates, seven-mip
smoothness law, and output encoding. Its separable direct-sun lobe uses GGX
with the legitimate high-gloss exponent domain retained. Its spherical
environment reflection now uses the same unit-grazing dielectric Fresnel and
an exact reflection/transmission energy partition instead of the stock
smoothness-dependent grazing response and 50% transmission-loss cap. The
native spherical projection and prefiltered seven-mip radiance lookup remain
unchanged. The two vehicle-paint writers preserve their exact material,
damage, and dirt decode; fade dither; bump normal; MRT layout and encoding;
authored smoothness; single spherical sample; and native
`LOD=6*(1-smoothness^3)` law. They change only the stock `F90=smoothness` and
50% diffuse-loss cap to the zero-F0-safe `F90=min(1,50*F0)` contract and an
exact `F(NoV)` reflection / `1-F(NoV)` remainder partition. Direct lighting
still comes from the downstream verified GGX lighting variants. The feature
does not assign a global metalness or roughness value and does not guess from
broad material names.

```ini
[PhysicallyBasedRendering]
PhysicallyBasedRendering=1
```

The runtime replacements cover the verified one- and two-render-target sun,
point, spot, and tube/capsule direct-light families. The dedicated
irradiance-volume shader `0xD71D285B` remains native. The exact runtime-proven
`V_GLASS01` vehicle-glass sun path and both traced `CITTA01` vehicle-paint
environment writers are replaced. Exact CRC, byte size, DXBC checksum, shader
ABI, and precompiled cache data are required; unknown or mismatched shaders
remain native. Native light attenuation, shadows, cookies, volume integration,
glass alpha/fog and spherical environment lookup, auxiliary render targets,
and the HDR specular-alpha convention used by GI and SSS are preserved.

Opaque replacement is selected at the deferred lighting boundary, not from a
material-name guess, so opaque G-buffer pixels reached by those 15 exact shaders
receive the new direct-light response. The glass and two paint shaders were
added only after runtime PS/VS/material and resource traces proved each exact
operating path. Hair anisotropy, eye/cornea rendering, foliage transmission, skin/teeth
scattering, water Fresnel/scattering, all other transparent glass, emissives,
UI, particles, and post-processing keep their specialized passes. Exact pixel
shader `0x729711D6` also remains native: a live color-ID trace identified its
observed draw as a small illuminated street-sign/light-box inside a broad
forward specular/environment/emissive transparent family, not a mirror-specific
path. Applying mirror Fresnel there would affect unrelated emissive and
transparent draws. The sky/skydome also keeps its specialized native radiance
path. A live material/D3D trace identified pixel shader `0x91A46134`,
vertex shader `0x389B7B3D`, and the 36-index skybox draw: it samples the game's
3D atmosphere/fog radiance LUT and applies the native sun direction, sun color,
scatter colors, and sun-scatter parameters. That is the appropriate lighting
model for the sky; a surface Cook-Torrance/GGX BRDF has no material normal or
view-surface interface there and would introduce energy and clipping artifacts.
Changes require a game restart.

## Subsurface scattering

These renderer features belong to ShenLong and are configured in
`ShenLong.ini`.

`SubsurfaceScattering=1` adds Jimenez separable screen-space scattering to Wei
and NPC skin. ShenLong maps exact shipped skin-material index ranges, replays them
into a private depth/stencil mask, rejects pixels hidden by hair or clothing,
and filters only diffuse HDR lighting immediately before the game's final
composition. Specular highlights remain sharp. Eyes, hair, clothing, the
environment, sky, water, and the interface are not processed by the skin pass;
the separate material controls below operate only on their exact supported
material and shader paths.

```ini
[SubsurfaceScattering]
SubsurfaceScattering=1
StockHairBlur=0
SSSQuality=2
SSSStrength=100
SSSRadius=100
```

`SSSQuality` accepts 0 through 2 (low through high). `SSSStrength` accepts 0
through 100 percent. `SSSRadius` accepts 25 through 400 percent; 100 is the
natural-skin default. Strength and radius are shared master multipliers for the
skin, eye, hair, teeth, and foliage profiles; those non-skin profiles apply
additional fixed material-specific multipliers. `SSSStrength=0` therefore
disables all five scattering profiles even if a material toggle is enabled.
Water scattering is independent. `StockHairBlur=0` disables the game's broad HairBlur for
sharper hair; `1` keeps it whenever stock anti-aliasing runs. Once SMAA proves
the current render path ready, it bypasses the stock AA/HairBlur stage, so this
setting no longer affects that path. Changes require a game restart.

```ini
[MaterialScattering]
EyeScattering=1
HairScattering=1
TeethScattering=1
FoliageTransmission=1
WaterScattering=1
```

`EyeScattering` diffuses sclera lighting while keeping the iris and corneal
reflections sharp. `HairScattering` uses a material-masked, alpha-preserving
separable isotropic profile; it does not re-enable the stock full-screen
HairBlur or claim fiber-direction data the captured material does not provide.
`TeethScattering` applies a short enamel/dentin profile
only to supported teeth. `FoliageTransmission` redistributes the native diffuse
response toward Beer-Lambert thin-sheet transmission with a bounded convex
blend. It preserves the exact native specular remainder, HDR range, alpha, and
cutout coverage instead of adding a second sun response. `WaterScattering` adds
fixed-strength isotropic Beer-Lambert volumetric in-scattering to three exact
supported water permutations without replacing their reflection, refraction,
foam, ripple, or depth behavior. There is no water-anisotropy control. Every
unsupported or failed exact-profile path leaves native rendering active.
Changes require a game restart.

## AgX tonemapping

This renderer feature belongs to ShenLong and is configured in
`ShenLong.ini`.

`AgX=1` replaces only the terminal curve in the exact known final-composition
pixel shader. The full-RGB path converts the game's linear-sRGB HDR scene to
Rec.2020, applies the AgX inset, analytic contrast, and outset transforms, then
returns display-ready sRGB to the game's existing display-gamma stage. All
other scene-composition math remains equivalent to the captured stock shader,
including its dynamic white normalization, and the later HUD is not tone
mapped. The exact shader size, DXBC checksum, and CRC are validated before
substitution; a mismatch leaves the stock shader active.

`AgXStrength=0` keeps the game's curve, while `100` applies the full AgX result.
`AgXExposure` is a percentage from `25` to `400`; `100` is a neutral exposure
multiplier. This SDR output path does not require Windows HDR or an HDR display.
`AgXLook=MediumHigh` is ShenLong's custom, stock-matched default, not an
official or standardized upstream AgX preset. It applies a smooth,
toe-preserving contrast grade around AgX middle gray before the analytic
sigmoid plus a luminance-aware chroma correction, restoring the game's authored
contrast and color separation without contaminating near-black detail or
overdriving saturated highlights. A hue-preserving scene-peak limiter contains
the final SDR result before the 8-bit target instead of clipping RGB channels
independently; the HUD remains untouched.
`Neutral` keeps the softer base AgX look.

```ini
[Tonemapping]
AgX=1
AgXLook=MediumHigh
AgXStrength=100
AgXExposure=100
```

## Files and diagnostics

Configuration and crash artifacts are written next to the loaded module:

- `SPatch.ini` — concise end-user configuration;
- `SPatch.log` — optional startup/build/hook diagnostics when
  `[Debug] Logging=1`; the end-user default is quiet;
- `SPatch-<timestamp>.dmp` — optional unhandled-exception minidumps.

An optional ShenLong install adds `ShenLong.asi`, `ShenLong.ini`,
`ShenLong-SHA256SUMS.txt`, its `ShenLong\ShaderCache\v1`, the pinned `dxgi.dll`
host, and `ReShade.ini`; `ReShade.log` is generated at runtime. See
`SHENLONG-README.md` in the separately downloaded ShenLong package for its
exact ownership and diagnostic contract.

Developer-only periodic summaries, verbose events, and callback-name retention
are intentionally omitted from the end-user INI and compiled out of final
release builds.

Configuration v6 organizes settings into `[Cutscenes]`, `[Graphics]`,
`[Display]`, `[Gameplay]`, `[Stability]`, `[AntiAliasing]`, and `[Tonemapping]`.
It retires the broken adapter-selection/VRAM controls along with raw `hook_*`,
probe, debug-key, and experimental AA controls. `TimeStepSmoothing` now
truthfully exposes the stock smoother as preserve/off/on instead of pretending
its value was an arbitrary frame count. The exact pre-migration file is
preserved as `SPatch.ini.previous.bak`; supported values are written with
canonical PascalCase names.

Configuration v8 adds `WetnessFullTime` and `WetnessFadeTime`. Migrating an
older file preserves existing settings, writes the new defaults, and keeps the
exact previous file as `SPatch.ini.previous.bak`.

Configuration v9 adds `RestoreSweat`. The Definitive Edition retains the
CharacterLook sweat material value and scripted action-track support, but
normal running and combat never raise it. SPatch supplements those stock
owners with a frame-rate-independent policy for Wei and NPCs. Configuration
v10 adds the initial sweat controls; configuration v11 adds the explicit
`SweatOnsetTime` delay and slower defaults without changing existing wetness
choices. The current default profile waits 30 seconds of continuous exertion,
then takes 150 seconds to reach full sweat; all values remain configurable.
Configuration v12 adds `RestoreOriginalFog`; migration enables the
original-equivalent value by default and preserves an explicit user choice.
Configuration v13 renamed that public option to `RestoreOriginalAtmosphere`.
Configuration v14 adds `RestoreOriginalEyeReflections` for the affected HD and
gang-head eye materials; older configurations migrate with the restoration
enabled by default. Configuration v15 renames the fog option to
`RestoreOriginalFogAndNeon`, making both visible results explicit. Both earlier
names remain readable and are rewritten automatically without changing the
selected value. Configuration v16 completely replaces the previous ACES
tonemapping feature and migrates `ACES`, `ACESStrength`, and `ACESExposure` to
`AgX`, `AgXStrength`, and `AgXExposure`, preserving every selected value.
Configuration v17 adds the `[AmbientOcclusion]` section and preserves `GTAO`,
`GTAOQuality`, `GTAORadius`, and `GTAOStrength` during later migrations.
Configuration v18 replaces the ambiguous `GTAO` switch with
`AmbientOcclusion=Original/GTAO`, moves stock `SSAO` quality to
`OriginalAOQuality`, and preserves both older values during automatic migration.
Configuration v19 adds the dedicated `[SubsurfaceScattering]` section and
preserves any SSS values already added manually to a v18 file.
Configuration v20 adds `[Input]`, forces the stock raw-mouse path by default,
removes mouse camera easing by default, and adds independently configurable
left/right radial controller deadzones. Existing settings remain unchanged
during migration. Configuration v21 formerly added the dedicated `[Shadows]`
section and migrated `RestoreCharacterShadows` and `ShadowFilter` without
changing their values. Configuration v22 removes the experimental PCSS keys
from the user template, restores native shadow filtering as the default, and migrates any old
PCSS selection back to the game's native renderer while preserving a backup.
Configuration v23 made the then-reviewed correction controls and the
default-off `EnableLogging` switch public in the organized end-user INI.
Configuration v24 adds `FixContactListOverflow` and keeps it enabled by default.
Configuration v25 adds the `[GlobalIllumination]` section. Migration preserves
existing settings, enables diffuse GI by default, and writes its quality,
strength, and radius controls with canonical SilentPatch-style names.
Configuration v26 adds canonical three-pass SMAA and full-RGB AgX controls.
Configuration v27 derives stock-AA suppression from the selected SMAA mode,
defaults SMAA to Ultra, and moves AgX to the verified `0x67843125` final
pre-HUD filmic boundary with a neutral `AgXExposure=100` default. Explicit user
quality, strength, and exposure values remain unchanged during migration.
Configuration v28 restores the final-composition white scale omitted by the
first full-RGB shader path and adds `AgXLook`. Existing AgX enable, strength,
and exposure choices remain unchanged; migrated files default to the
color-faithful `MediumHigh` look.
Configuration v29 refreshes existing v28 files into the complete current
end-user template, including all shipped stability controls, opt-in diagnostics,
and the native-only shadow section. User-selected values remain unchanged.
Configuration v30 replaces the standalone GTAO renderer with SDAO. Migration
maps `AmbientOcclusion=GTAO`, `GTAOQuality`, `GTAORadius`, and `GTAOStrength` to
the canonical SDAO renderer and keys without changing the selected values.
Configuration v31 adds `StockHairBlur` under `[SubsurfaceScattering]` and
defaults it to `0` for sharper hair. Migration preserves an existing
`StockHairBlur` or `stock_hair_blur` choice; the canonical grouped key wins when
both forms are present.
Configuration v32 adds `[MaterialScattering]` controls for supported eye, hair,
teeth, foliage, and water paths. Migration preserves any manually added
canonical or snake-case choices and enables each newly introduced path by
default when no prior choice exists.
Configuration v33 adds `[PhysicallyBasedRendering]` and enables its
exact-identity GGX path by default. Migration preserves an explicit canonical,
grouped, catch-all, snake-case, or short `pbr` opt-out; an absent choice defaults
on. Specialized paths remain native unless their own exact runtime and ABI
proof permits a bounded extension under the same switch; the current
`0x282EE2DC` vehicle-glass path and `0x5DB1CB6E` / `0xE611C192`
vehicle-paint environment paths are proven extensions, for 18 active
replacements out of 20 recognized exact identities. Variants `0` and `13`
remain native and do not have replacement caches.
Configuration v34 adds `GTAOLite` as a third ambient-occlusion backend with
independent quality, radius, and strength controls. Existing `Original` and
`SDAO` choices remain unchanged during migration. GI now contributes diffuse
bounce only and leaves the selected AO backend authoritative.
Configuration v35 adds `AnisotropicFiltering` and
`ForceAnisotropicFiltering`. Existing files migrate with the original game
value and sampler classification preserved unless an explicit supported level
or force flag was already present; canonical and snake-case names remain
readable.
Configuration v36 adds the `[Renderer]` section for `RendererBackend`,
`SwapChainFlipModel`, `SwapChainTearing`, and `SwapChainFrameLatency`.
Those keys are historical and no longer select a runtime backend.
Configuration v37 adds `CharacterShadowResolution` and `ShadowResolution`
under `[Shadows]`; only the ShenLong `ShadowResolution` policy remains current.
Configuration v38 adds the texture-detail and motion-blur overrides.
Configuration v39 retires the renderer/swap-chain controls, the two
frame-counted fix switches, thirteen unverified divide-guard switches,
`RestoreCharacterShadows`, `CharacterShadowResolution`, and `ShadowFilterScale`.
Migration ignores and removes those retired keys with an exact previous-file
backup. The current renderer contract is Stock Native D3D11 only.
Configuration v40 retires the unverified `RestoreVisualDamage` feature and
removes both its public and legacy snake-case keys during a one-time backed-up
migration. It also moves the public logging control to `[Debug] Logging`;
`[Diagnostics] EnableLogging` and legacy `enable_logging` values remain
readable only for migration, with the canonical key taking precedence.
Configuration v41 completes the ownership split: SPatch keeps verified fixes,
tweaks, native SMAA, and `[Debug] Logging`, while renderer features and their
keys move to independent ShenLong. Migration retains the byte-identical
`SPatch.ini.previous.bak` and rewrites `SPatch.ini` without ShenLong renderer
keys; SPatch's native `[Graphics]` tweaks and `OriginalShadowFilter` remain.
Configuration v42 corrects that boundary by returning the native
`[TextureFiltering]` controls to SPatch. It defaults to 16x anisotropy and the
verified narrow trilinear-class promotion, moves `WriteCrashDumps` to the end of
`[Debug]`, and stores the exact pre-migration INI outside the game directory in
`%LOCALAPPDATA%\SPatch\ConfigBackups`. A legacy in-game
`SPatch.ini.previous.bak` is copied byte-identically into that external backup
directory and removed before the organized v42 file is published.
Configuration v43 adds the opt-in `[Input] GTAIVCarCamera` tweak. Migration
preserves canonical and legacy snake-case choices, defaults older files to the
native camera, and keeps `WriteCrashDumps` as the literal final INI key.
Configuration v44 extends `GTAIVCarCamera` to the verified road-vehicle Drive
and Flee selector paths, including aliased truck/heavy-vehicle Drive blocks,
and adds loose-follow yaw, delayed manual-yaw and controller-pitch recentering,
native-state mouse-pitch mapping, and handbrake swing. It also adds the
independent, default-off `[Input] GTAIVBikeCamera` tweak. Migrating a v43 file
with no bike key writes `GTAIVBikeCamera=0`; an explicit canonical or legacy
`gta_iv_bike_camera` choice is preserved. The byte-identical pre-migration file
is stored outside the game directory as
`%LOCALAPPDATA%\SPatch\ConfigBackups\SPatch-pre-v44.ini`, and
`WriteCrashDumps` remains the literal final INI key with nothing after it.

The project builds `SPatch.sln`/`SPatch.vcxproj` and the standalone
`SPatchTests.vcxproj`; tests cover configuration migration, cadence policy,
display settings, and build-identity checks. A normal test build runs from its
own output and intermediate directories:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
& $msbuild .\SPatchTests.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:FinalRelease=false
.\build\tests\Release\false\SPatchTests.exe
```

For unattended local benchmarks, use
`tools\Start-UnattendedBenchmark.ps1` or the desktop shortcut created from it.
The launcher supplies Steam app identity only to the child process and starts
`sdhdship.exe -benchmark -skipStartScreen` from the game directory. It does not
use Steam `-applaunch`, so Steam's Continue screen cannot block the benchmark:

```powershell
.\tools\Start-UnattendedBenchmark.ps1 -Wait
```

The unattended launcher, PBR benchmark, and live final smoke are Steam-only.
Their GOG `DisplaySettings.xml` resolver is covered statically, but no live GOG
launch or smoke is claimed without a GOG executable and client path to test.

For a matched three-pass PBR performance check, use
`tools\Invoke-PBRBenchmark.ps1 -Passes 3`. It alternates PBR on/off with the
same GI/AO, resolution, and direct launcher, records each XML/log pair under
`artifacts\benchmarks`, and restores `SPatch.ini`, `ShenLong.ini`,
`ReShade.ini`, and `DisplaySettings.xml` byte-for-byte when complete.

`tools\Test-PBRBRDF.ps1` is a CPU reference and algebra gate. It covers
660 BRDF reflectance cases, 54 vehicle-glass environment compositions, and 324
vehicle-paint environment compositions. The paint cases require finite,
non-negative output, exact Fresnel/remainder partitioning, and convex
composition. It does not by itself prove that compiled HLSL calls those helper
paths. The graphics build separately compiles the production helpers and runs
their bytecode on D3D11 WARP across 28 reference vectors; its negative controls
detect gross semantic failures.

A canonical base release is produced with:

```powershell
& $msbuild .\SPatch.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:FinalRelease=true
```

`FinalRelease=true` is rejected for every configuration except `Release|x64`.
Non-final project builds use `build\<Configuration>\diagnostic`; the canonical
`build\Release\SPatch.asi` and its identity receipt are reserved for final
release builds. Publication runs only after a successful `Build`/`Rebuild` in
the same MSBuild invocation; the old verification, assembly, identity, and
deployment targets reject direct invocation so they cannot reuse a stale
output. A repository-keyed machine lock serializes canonical final builds.
Before the identity receipt is written, the build compiles and runs both the
normal test mode and the separate `SPATCH_FINAL_RELEASE` test mode. It then
derives the current configuration version from `Config.h`, generates that
configuration through both test executables, requires exact byte identity, and
assembles `artifacts\release\SPatch-Base` plus
`SPatch-Base.zip`. The package whitelist is `SPatch.asi`, the generated default
`SPatch.ini`, this README, MinHook and SMAA licenses, third-party notices, and a
validated `SHA256SUMS.txt`; stale binaries, PDBs, or unlisted files fail the
gate. ZIP entries are written in ordinal order with fixed timestamps, and the
gate creates the archive twice from the same snapshot and requires identical
bytes and SHA-256 values.

Ordinary builds do not touch the game directory. Deployment additionally
requires `DeployGameArtifacts=true`, a supported `sdhdship.exe` SHA-256, and an
x64 `dinput8.dll` ASI-loader candidate beside it. The deploy preflight checks PE
architecture, the `DirectInput8Create` export, and a static `.asi` marker; only
the runtime smoke proves that the loader actually loaded SPatch. Deployment
also refuses a running game, verifies the final receipt immediately before
staging, rejects reparse roots or targets, and binds the canonical package
directory, ZIP, INI, manifest, ASI, x64 PE identity, and positive final-policy
attestation. It replaces an existing `SPatch.asi` with atomic `File.Replace`
under shared and game-root-keyed mutation locks. A durably flushed transaction
journal supports rollback after interruption and is removed only after the ASI
is verified and any stale PDB is removed:

```powershell
& $msbuild .\SPatch.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:FinalRelease=true /p:DeployGameArtifacts=true
```

A custom `GameDir` may be supplied with or without a trailing slash. Final
release links omit debug data, use reproducible-link metadata, and write
`build\Release\SPatch.final-release.sha256` only after both test modes and the
base-package validation pass. The receipt records Release|x64/final-policy and
x64 PE identity plus SHA-256 values for the build ASI, packaged ASI, generated
default INI, package manifest, and ZIP; identical inputs must produce the same
ZIP hash as well as the same `SPatch.asi` hash. Canonical final builds use
`/O2`, `/GL`, and explicit full `/LTCG`; non-final Release diagnostics omit
whole-program compilation and LTCG for faster diagnostic iteration.

All three native projects resolve the same modified SDmodding x64 MinHook
prebuilt from SDK commit `d5d8e1d67ddcea89fbef656b85052a5845dd34ee`.
Every build validates header SHA-256
`F2642BB69230017E52F8FE2F1208F6FDEA146302CC670E2003D2A69B5AE860E8`
and library SHA-256
`DCF47C6ACDA033310E7C0FA3F7EE6E6C7F89AEA9F8C043D714A39FA01A5FECE2`.
The reduced header and prebuilt library have no matching fork source in that
snapshot, so this dependency is artifact-pinned, not source-reproducible and
not interchangeable with canonical MinHook. The default cache is repository
local under `.tmp`; use `/p:MinHookRoot=<path>` for another exact cache or
`/p:MinHookOffline=true` to forbid downloads.

ShenLong has an independent pinned build and package contract. The user-facing
entry point is:

```powershell
.\luma\Build-ShenLong.ps1 -Configuration Publishing-Release
```

`luma\Build-Luma.ps1` remains the historical implementation filename. The
Publishing build validates the pinned ReShade runtime and headers, MinHook,
DiligentFX, XeGTAO, Separable SSS, three.js, Filament, shader cache, package
manifest, licenses, and deterministic `artifacts\shenlong\ShenLong.zip`.
ShenLong does not contain or validate SMAA; canonical SMAA remains part of the
SPatch base module.

The package-local `Install-ShenLong.ps1` validates and installs ShenLong
transactionally. Ordinary upgrades preserve user-owned `ShenLong.ini` exactly;
the one backed-up exception removes retired texture-filtering entries that now
belong to SPatch. The installer also preserves compatible external `ReShade.ini`
and `dxgi.dll` files, and runtime-generated
`ShenLong\ReShadeCache` data according to the ownership rules documented in
the package-local `SHENLONG-README.md`. No final build, package count, or
live-smoke result is claimed here until the current release gate records it.
