## Run & Test

- Build from this directory via `SPatch.sln` / `SPatch.vcxproj`.
- Runtime artifacts are written next to the loaded module:
  - `SPatch.ini`
  - `SPatch.log`
  - `SPatch-<timestamp>.dmp`

## Scope Markers

- This repository owns the SPatch base ASI: bootstrap and build guards,
  configuration, diagnostics, executable fixes, display persistence,
  input/timing fixes, native SMAA, and native texture filtering.
- Keep unrelated renderer add-ons, package payloads, and their installers out
  of this repository.
