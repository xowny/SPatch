## Run & Test
- Build from this directory via `SPatch.sln` / `SPatch.vcxproj`.
- Runtime artifacts are written next to the loaded module:
  - `SPatch.ini`
  - `SPatch.log`
  - `SPatch-<timestamp>.dmp`

## Scope Markers
- The core ASI covers bootstrap/build guards, configuration, diagnostics, targeted
  executable fixes, display-setting persistence, input/timing fixes, and the
  built-in SMAA and native texture-filtering paths. Versioned config backups
  live under `%LOCALAPPDATA%\SPatch\ConfigBackups`, not in the game directory.
- The separately packaged `ShenLong.asi` under `luma/` owns AgX, shadow-map
  scaling, AO/GI/PBR/SSS, material transmission, and water scattering. It uses
  ReShade's API but loads through the ASI loader before D3D11 device creation.
