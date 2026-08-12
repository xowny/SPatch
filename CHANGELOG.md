# Changelog

## 2026-08-10 — Configuration v44 vehicle-camera release candidate

- Extended `[Input] GTAIVCarCamera` to the verified road-vehicle Drive and Flee
  selector paths. Game resources and the paired mover/class/selector paths prove
  the eligible family includes cars, limousines, exotic cars, vans, tall vans,
  SUVs, trucks, and buses. Only the aliased truck Drive path has been explicitly
  runtime-confirmed among the heavy variants. SPatch accepts another family
  member only when its exact live Drive/Flee mask matches the proven policy;
  unverified alias masks fail closed to native behavior. The selector replay
  fixes the captured truck whose Race, Drive, and HijackFront slots all point at
  one block while keeping actual Race and Hijack branches native.
- Drive and Flee carry the same `-0.35 m` lateral delta. Transition calls apply
  only the incoming target share so it combines continuously with the outgoing
  block's retained share across the observed police-pursuit transition.
- Added the full GTA IV-like motion candidate for eligible Chase cameras:
  looser automatic follow yaw, a stronger vehicle-under-camera swing while the
  handbrake is held, manual yaw with a `0.75 s` idle delay and slower recenter,
  and persistent clamped right-stick/controller orbit pitch with its own
  `0.75 s` idle delay and smooth recenter. Mouse pitch instead maps the game's
  absolute `+0x4FC` state to orbit pitch, preserving native sensitivity,
  inversion, clamp, one-frame timing, and native decay without adding mod-owned
  mouse persistence or the controller's idle-delay policy. Both paths preserve
  eye-to-look distance and continue through the game's collision-aware
  finalizer.
- Added independent, default-off `[Input] GTAIVBikeCamera=0`. On the traced
  legacy executable it applies the same offset and motion model to a motorcycle
  or scooter's context-3 Drive block, but only after the exact Drive-branch
  replay accepts its Race/Drive or Drive/HijackFront pointer alias. A distinct
  motorcycle Flee block remains native. Both camera settings require a restart.
- Actual Race, HijackFront, Aim, Look, passenger, and other specialized
  parameter blocks stay native. Look-back, aim/focus, eye/look locks, reverse
  handling, and other special overrides retain stock yaw/pitch behavior.
- Paired disassembly and decompiled-code review established the selector,
  Chase-update, angular-approach, input, pitch, and collision paths in both
  supported executable layouts. Captured legacy gameplay established the Flee
  transition, bike/scooter aliases, and truck alias shape. The remaining named
  road families are resource/class/path-proven, not individually runtime-
  confirmed. Latest Steam and safe compatibility mode force both requested
  camera options off. The normal-car lateral placement has user gameplay
  acceptance; the corrected heavy-vehicle gate, full dynamics candidate, and
  enabled bike behavior still require the next real-gameplay A/B. The
  unattended benchmark is load-only for this feature.
- Renamed camera diagnostics to the install event
  `gtaiv_vehicle_camera`, sampled state event `gtaiv_vehicle_camera_probe`, and
  bounded Chase update/pose event `gtaiv_vehicle_camera_dynamic`. The records
  include the replayed Drive-branch result and selected slot-match masks. The
  per-setter addition remains `applied_delta_m`, avoiding the earlier
  implication that it represented the outgoing block's total offset. Inactive
  or partially read camera state is now published atomically as unreadable,
  with zeroed flags/pointers and explicit active-field/source-weight validity;
  the trace analyzer reports any future inactive-state sanitization failure.
  The final-smoke parser now treats CRLF and LF logs identically while still
  requiring exactly one complete camera-hook installation record.
- Configuration-v43 files without a bike key migrate to
  `GTAIVBikeCamera=0`; explicit canonical and `gta_iv_bike_camera` values are
  preserved. The exact source INI is backed up outside the game directory as
  `%LOCALAPPDATA%\SPatch\ConfigBackups\SPatch-pre-v44.ini`, and
  `WriteCrashDumps` remains the literal final key with no trailing setting.

## 2026-08-10 — Configuration v43 audit and review release

This release consolidates the audited configuration-v42 and configuration-v43
work, the SPatch/ShenLong ownership split, and the related stability review.

### Vehicle camera — configuration v43

- Added the opt-in `[Input] GTAIVCarCamera=0` setting. It is off by default and
  older configurations migrate to the native centered camera unless the user
  had already made an explicit canonical or legacy snake-case choice.
- On the fully traced legacy executable only, the active code applies a
  transition-weighted `-0.35 m` vehicle-local lateral delta to the six target
  and eye fields of the normal road-car Drive profile. The intent is to move
  the view toward Wei's right-side seat while leaving the game's distance,
  height, FOV, collision, springs, look input, and camera cycling in charge.
  Invalid or ambiguous data, non-Drive contexts, motorcycles, boats, aiming,
  looking back, and specialized profiles retain native behavior.
- Paired disassembly/decompiler work and an observe-only legacy gameplay trace
  confirmed the normal road-car Drive slot at `+0x388` and independently
  confirmed the motorcycle exclusion. The earlier static `+0x3B0`
  interpretation was corrected before active mutation was enabled.
- The latest-Steam compatibility profile keeps this feature disabled. Its
  static selector/setter mappings are retained, but its active gameplay path
  has not been traced.
- Fixed the audit findings found before release: a present-but-empty or
  malformed canonical value now wins over legacy aliases and safely disables
  the option; the Drive pointer must be unique across all 13 camera-profile
  slots; transition-local blocks use the engine's remaining-source weight so
  the offset blends instead of stepping; and every rejected read leaves the
  original parameter block untouched and calls the original setter exactly
  once.
- Camera diagnostics are local, sampled at most once per 100 ms, and capped at
  32 state-change records per run. They distinguish the canonical target from
  the transition-local block, never interpret that block's uninitialized
  identity header, and reject transition weights outside `[0,1]`. A startup
  record with `readable=0` and raw `0xDE` fill bytes is diagnostic-only: no
  vehicle exists yet and mutation has already failed closed.
- **At the v43 review point, visual validation was still pending:** the active
  camera's visible direction, magnitude, transitions, reversing, look/aim
  behavior, camera cycling, and vehicle exclusions required the user's
  real-gameplay A/B. The unattended benchmark contains no drivable car and is
  a load-only check; it can verify configuration parsing, signature acceptance,
  hook attachment, normal exit, and absence of a crash dump, but it cannot
  validate camera behavior.

### SPatch ownership and configuration — configuration v42

- Returned `[TextureFiltering]` to SPatch. Native 4x, 8x, and 16x anisotropy
  uses the game's exact sampler builder and settings writer, not ReShade
  sampler interception. The defaults are `AnisotropicFiltering=16` and
  `ForceAnisotropicFiltering=1`.
- A live startup trace showed that the stock settings writer is not guaranteed
  to run during an unchanged-settings boot. SPatch therefore publishes and
  verifies the requested anisotropy immediately before each exact native
  sampler-builder call while retaining the writer detour for later settings
  commits. Only the exact stock trilinear selector is promoted; point,
  comparison/shadow, and already-anisotropic paths remain native.
- Moved AgX, shadow-map scaling, AO, GI, PBR, SSS/material scattering, and water
  rendering to the independent `ShenLong.asi` companion and `ShenLong.ini`.
  SPatch retains its executable fixes, native graphics tweaks, native texture
  filtering, `OriginalShadowFilter`, SMAA, and diagnostics. Installing or
  removing ShenLong does not disable the base patch.
- Made `[Debug] Logging` the canonical, default-off local logging switch.
  `SPatch.log` is capped in place at 8 MiB instead of leaving a `.old` file,
  and no telemetry is exported.
- Kept crash dumps independent from logging. The organized INI generator writes
  `WriteCrashDumps=1` as the literal end-of-file content: no setting or trailing
  newline follows it.
- Moved exact pre-migration SPatch configuration backups out of the game
  directory to `%LOCALAPPDATA%\SPatch\ConfigBackups`. The organized SPatch INI
  excludes ShenLong renderer keys, and ShenLong migration does not absorb
  SPatch's texture-filtering settings.

### Audit corrections and rejected graphics claims

- Removed or retired options whose real game paths were not demonstrated,
  including `RendererBackend` and its swap-chain controls, frame-counted and
  divide-guard switches, `RestoreVisualDamage`, `RestoreCharacterShadows`,
  `CharacterShadowResolution`, `ShadowFilterScale`, and the PCSS experiments.
  These are not advertised as working features or safety guards.
- Removed the unsupported texture-filtering claim that the promoted branch was
  necessarily zero-bias/full-LOD. Paired analysis proves only the exact stock
  trilinear selector; LOD bias is built independently.
- Corrected graphics accounting and descriptions to the observed paths: PBR
  replaces 18 exact shader identities while variants `0` and `13` deliberately
  stay native; water scattering is isotropic; and the captured hair data does
  not support directional or fiber-scattering claims.
- Hardened ShenLong's fail-open transaction. Executable and configuration
  authorization now completes before synchronous pre-device component
  registration; a failed transaction retains native rendering. Shadow
  teardown drains accepted callbacks before releasing tracked state, and GI
  restores the compute resource it temporarily occupies.
- Rejected the proposed “unconditionally dither hair depth under MSAA” option.
  The exact native hair shader already performs an unconditional screen-space
  dither discard on a depth-writing draw, ShenLong mirrors the two native
  discard tests, and the observed G-buffer/depth path is single-sample. SPatch
  SMAA runs later as a post-process and is not scene MSAA, so adding another
  switch would duplicate existing behavior and claim an unobserved path.

### Crash-dump investigation

- The submitted `SPatch-20260808-182420-612.dmp` records the game's Main Pool
  failing a 3,204-byte allocation for a 100-entry `RingBuffer`, followed by the
  stock allocator's deliberate `0x80000003` breakpoint. It is not an access
  violation.
- SPatch appears on the stack because its frame-flow detour calls the original
  game routine. The small dump does not attribute the pool exhaustion,
  fragmentation, or corruption to SPatch or to another loaded module.
- No speculative allocator bypass, forced-null continuation, capacity clamp,
  or object-lifetime guard was added. That path remains analysis-only until a
  matched reproduction can observe pool history and module/configuration state.

### Build and release hardening

- Canonical release profiles use MSVC `/O2` and full link-time code generation.
  SPatch now reserves `/GL` and explicit full `/LTCG` for `FinalRelease`; normal
  Release diagnostics omit both rather than using incremental LTCG. `clang-cl`
  was evaluated but not substituted without equivalent SEH, ABI,
  deterministic-package, and runtime-hook validation.
- Restricted SPatch `FinalRelease` to `Release|x64`, separated diagnostic and
  canonical output paths, serialized canonical builds with a repository-keyed
  machine lock, and bound verification, packaging, identity receipts, and
  optional deployment to the current successful `Build`/`Rebuild` invocation.
  Direct invocation of publication targets is rejected instead of reusing a
  stale binary.
- Final publication runs both normal and `SPATCH_FINAL_RELEASE` test modes,
  requires byte-identical generated configuration from both, validates the
  exact package whitelist and manifest, produces deterministic archives, and
  records hashes for the build, package, INI, manifest, and archive.
- Deployment hardening validates the supported game executable and x64 ASI
  loader, rejects unsafe reparse targets or a running game, uses shared and
  game-root mutation locks, and keeps a durably flushed rollback journal until
  the installed ASI is verified.

Detailed evidence and limits are retained in
`artifacts/reverse-engineering/20260810-pursuit-bike-profile-audit/audit.md`,
`artifacts/runtime-logs/camera-v43-user-gameplay-police-bike-20260810-204003/summary.md`,
`artifacts/reverse-engineering/20260810-gtaiv-camera-v43/analysis.md`,
`docs/2026-08-10-texture-filtering-paired-analysis.md`,
`docs/2026-08-09-hair-depth-dither-analysis.md`, and
`docs/2026-08-08-main-pool-ringbuffer-crash-analysis.md`.
