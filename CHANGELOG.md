# Changelog

## 2026-08-10 — Configuration v44 vehicle-camera release candidate

- Extended `[Input] GTAIVCarCamera` to the verified road-vehicle Drive and
  Flee selector paths while keeping ambiguous and specialized profiles native.
- Added the independent, default-off `[Input] GTAIVBikeCamera=0` option for
  the verified legacy motorcycle and scooter Drive path.
- Added guarded follow-yaw, manual-yaw, controller-pitch, mouse-pitch, and
  handbrake motion policies for eligible Chase cameras.
- Kept both camera options disabled on the latest Steam compatibility profile.
- Renamed camera diagnostics and bounded their state records so unreadable
  startup state cannot be mistaken for an active vehicle.
- Preserved exact pre-migration configuration backups under
  `%LOCALAPPDATA%\SPatch\ConfigBackups`.

## 2026-08-10 — Configuration v42 and v43 review release

- Returned native texture-filtering controls to SPatch with exact sampler
  builder and settings-writer validation.
- Made `[Debug] Logging` canonical, capped the local log in place, and kept
  crash dumps independent from logging.
- Retired unverified renderer-backend, PCSS, divide-guard, and visual-damage
  options from the public configuration and migration path.
- Hardened final-release builds, deterministic packaging, executable identity
  checks, deployment preflight, and rollback journaling.

## 2026-08-08 — Crash-dump investigation

- Documented the Main Pool allocation failure and stock breakpoint from the
  submitted dump without adding a speculative allocator workaround.

## Earlier releases

Earlier configuration migrations remain documented in the source history and
the generated `SPatch.ini` template. Every migration preserves the exact
pre-migration file in the external SPatch backup directory before publishing
the organized configuration.
