# ShenLong

ShenLong is the optional graphics companion for Sleeping Dogs: Definitive
Edition. The renderer suite migrated to the separate `ShenLong.asi` module so
SPatch can remain focused on game fixes and bounded native tweaks. Native SMAA
and native texture filtering did not migrate: both remain in `SPatch.asi` and
`SPatch-Base.zip`, and ShenLong does not contain or configure either one.
ShenLong uses the pinned ReShade add-on runtime as its D3D11 event host; it does
not use ReShade `.fx` effects or Luma Core.

## Requirements

- Sleeping Dogs: Definitive Edition;
- a compatible x64 `dinput8.dll` ASI loader installed separately beside
  `sdhdship.exe`; neither the SPatch base package nor ShenLong bundles or
  replaces this loader;
- the package's pinned `dxgi.dll` ReShade add-on runtime.

The ShenLong installer validates the existing loader's PE architecture,
`DirectInput8Create` marker, and `.asi` loader marker. It never installs or
overwrites `dinput8.dll`.

## Supported executable identities

ShenLong authorizes renderer components only for these exact `sdhdship.exe`
SHA-256 identities:

- legacy researched build:
  `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`;
- latest Steam build:
  `2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA`.

The SHA-256 must match its complete PE profile before ShenLong registers its
renderer components or enables fixed-RVA hooks. If the executable cannot be
read or matched, `ConfigVersion` is unsupported, or the pre-device transaction
fails, ShenLong leaves the verified profile unpublished, keeps its custom
component registration uncommitted and any native detours transparent, and
retains native rendering. These identities describe the runtime authorization
contract; they do not by themselves claim a final-release live smoke.

## Graphics controls

All options live in `ShenLong.ini`. Restart the game after changing one.
`[ShenLong] Enabled=0` is the master disable and keeps every renderer path
native. `[ShenLong] ConfigVersion=1` is the runtime-validated package schema
marker, not a visual control; it must remain exactly `1`.

### Tonemapping

`AgX=1` replaces only the exact, known final pre-HUD scene tone-operator shader
with a full-RGB AgX transform. Unknown or near-match shaders remain native.
`AgXStrength` blends between the native curve and AgX, and `AgXExposure` scales
the input exposure.

`AgXLook=Neutral` uses the base AgX transform. `MediumHigh` is ShenLong's custom,
toe-preserving, visually stock-matched contrast and colour grade layered over
base AgX. It is not an official or standardized upstream AgX look transform.

### Shadow-map resolution

`ShadowResolution=0` keeps native shadow-map sizes. `2048` floors captured 512,
1024, and 1408 maps to 2048. `4096` also doubles captured native 2048 maps to
4096. Resource creation and shadow-pass viewport scaling are guarded by exact
map tracking and fail open to native behavior. ShenLong does not expose the
retired `ShadowFilterScale` or PCSS experiments.

### Ambient occlusion and global illumination

`AmbientOcclusion` accepts `Original`, `SDAO`, or `GTAOLite`. Original preserves
the game's renderer; `OriginalAOQuality=-1` follows the saved game setting, with
`0` and `1` selecting Normal and High. SDAO and GTAO Lite have independent
quality, radius, and strength controls. A failed custom-AO pass keeps native AO
for that frame.

`GlobalIllumination=1` adds screen-space diffuse bounce before tonemapping. It
does not choose the AO backend. GI quality levels 0 through 3 run at half
resolution; level 4 runs at full resolution and is intended for high-end GPUs
or screenshots.

### Exact material-lighting paths

`PhysicallyBasedRendering=1` replaces only the exact validated opaque,
vehicle-glass, and vehicle-paint shader identities with their GGX variants.
Eighteen exact identities are replaced; validated variants `0` and `13` remain
native. Water, hair, eyes, foliage, UI, post effects, and every unknown identity
retain their specialized native paths.

`SubsurfaceScattering=1` enables the exact skin scattering pass.
`StockHairBlur=0` suppresses the game's broad HairBlur stage, while
`StockHairBlur=1` retains it. Eye, hair, teeth, foliage, and water controls live
under `[MaterialScattering]` and are applied only to their exact supported
material families. `SSSStrength` and `SSSRadius` are shared master multipliers
for skin, eye, hair, teeth, and foliage; each non-skin profile adds a fixed
material-specific multiplier. `SSSStrength=0` disables those five profiles.
Water scattering is independent of the SSS strength/radius controls.

`WaterScattering=1` applies fixed-strength isotropic scattering to three exact
water permutations. It does not expose a water-anisotropy control. Unknown
water shaders and every failed validation retain native rendering.

### Diagnostics

`DumpShaders=1` writes developer captures under `ShenLong\ShaderDump`.
`CensusShadowConsumers=1` logs captured shadow producers and consumers. Both
default off in release packages. Runtime-generated `ShenLong\ShaderDump` and
`ShenLong\ReShadeCache` directories are preserved by upgrades.

## Install and upgrade

`SPatch-Base.zip` and `ShenLong.zip` are separate packages with separate
ownership and uninstall contracts. `SPatch-Base.zip` supplies the base fixes
and native SMAA and is extracted directly into the game directory. It is not
contained in, replaced by, or required by the optional ShenLong package.

`ShenLong.zip` contains exactly one top-level `ShenLong-Package` directory.
Extract the archive into a fresh directory outside the game folder, open
PowerShell inside `ShenLong-Package`, and run:

```powershell
.\Install-ShenLong.ps1 -GameRoot '<Sleeping Dogs Definitive Edition folder>'
```

The Publishing package is the end-user release. Development packages include
reviewable shader-source fallbacks under `ShenLong\Shaders`; Publishing packages
contain only validated precompiled shaders under `ShenLong\ShaderCache\v1`.

The installer verifies every adjacent package file against `SHA256SUMS.txt`,
validates the nested shader-cache manifest, rejects reparse points and redirected
ReShade add-on paths, takes the shared live-mutation mutex, and deploys only
runtime files. A compatible existing `ReShade.ini` is preserved by default. To
replace it explicitly, pass `-ReplaceReShadeIni`; an unowned original is retained
under `%LOCALAPPDATA%\ShenLong\ConfigBackups`, outside the game directory and
without a `.bak` extension.

`ShenLong.ini` is always user-owned. Ordinary upgrades preserve existing bytes
and timestamp. The backed-up exception is a one-time cleanup of retired
`[TextureFiltering]`, `AnisotropicFiltering`, and
`ForceAnisotropicFiltering` entries, which now belong to SPatch; the exact
pre-cleanup bytes are retained under `%LOCALAPPDATA%\ShenLong\ConfigBackups`.
When the INI is absent, the installer starts from the packaged default and
overlays recognized legacy graphics values. Current `SPatch.ini` has precedence,
followed by the exact external pre-v42 backup under
`%LOCALAPPDATA%\SPatch\ConfigBackups`, then the legacy in-game
`SPatch.ini.previous.bak` fallback. It migrates tonemapping, shadow resolution,
AO/GI/PBR/SSS/material-scattering, and shader-debug values. It never migrates
SPatch-owned texture filtering, retired `ShadowFilterScale`, or PCSS keys.

An old `SPatchGraphics.addon`, standalone SPatch/Luma graphics add-on, or old
`SPatch\ShaderCache` file is retired automatically only when
`SPatchGraphics-SHA256SUMS.txt` proves the exact installed bytes. Changed or
unowned legacy modules are rejected without mutation. Base `SPatch.asi`,
`SPatch.ini`, and unrelated files under `SPatch` are never package targets.

The installed runtime ownership list is `ShenLong-SHA256SUMS.txt`. It excludes
the user-owned `ShenLong.ini`, preserved external `dxgi.dll` or `ReShade.ini`,
and package-only documentation, notices, licenses, installer, and policy helper.

To uninstall ShenLong, remove `ShenLong.asi`, `ShenLong.ini`,
`ShenLong-SHA256SUMS.txt`, and the `ShenLong` directory from the game folder.
Remove `dxgi.dll`, `ReShade.ini`, or ReShade logs only when no other ReShade
add-on uses those shared files. SPatch itself and the shared ASI loader are not
ShenLong-owned uninstall targets.

## Build

The user-facing entry point is:

```powershell
.\luma\Build-ShenLong.ps1 -Configuration Publishing-Release
```

`Build-Luma.ps1` remains the implementation filename for historical developer
workflows. The build verifies pinned ReShade API/runtime, MinHook,
DiligentFX, XeGTAO, Separable SSS, three.js, and Filament inputs; builds
`ShenLong.asi`; compiles the configuration-exact shader cache; runs CPU physical
invariants and D3D11 WARP shader gates; stages an exact manifested payload under
`artifacts\shenlong\Publishing-Release\ShenLong-Package`; and creates the
deterministic end-user archive `artifacts\shenlong\ShenLong.zip`.

`Development-Release` publishes separately under
`artifacts\shenlong\Development-Release\ShenLong-Package` and does not replace
the end-user ZIP.
The exact development fallback contract is:

- `ShenLong\Shaders\GI`
- `ShenLong\Shaders\PBR`
- `ShenLong\Shaders\SDAO`
- `ShenLong\Shaders\SSS`
- `ShenLong\Shaders\Water`

The Publishing shader cache contract is `ShenLong\ShaderCache\v1`.

## Pinned inputs and licenses

- ReShade API revision `9fcd6ad935cfa19801e5e59a89a885dbdd6e731b`;
- ReShade full add-on runtime `6.7.3.2148`, SHA-256
  `EC9245D05C11751F2AC0D2256E6921AD8FB36BE9172EF6D587856591EB729A25`;
- ReShade setup SHA-256
  `C78DB69BD127E98054BD496FB422655F4A1CC664E28F8D12CE9835B2647BC571`;
- DiligentFX ScreenSpaceAmbientOcclusion revision
  `eb616a8e30efa5193baba71ff1edae85bc6230a1`;
- Separable SSS revision `b2174689ab22d90647825fdbada7bbd08e7e4e49`;
- three.js revision `1bc25777ac82579be2137f1f2e3f7d649595405f` and
  Filament revision `5fb2a0ec8c588b84eb13b673290252c086b777c3`
  for the reviewed AgX integration;
- modified SDmodding MinHook x64 prebuilt recorded from SDK revision
  `d5d8e1d67ddcea89fbef656b85052a5845dd34ee`.

Inside `ShenLong-Package`, see `THIRD_PARTY_NOTICES.md` and `licenses` for
complete attribution and license texts.
