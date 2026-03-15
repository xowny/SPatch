# SPatch

`SPatch` is an `.asi` mod for `Sleeping Dogs: Definitive Edition`.

## Download

- Nexus Mods: https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148

## Working feature set

- automatic support for both the `1.0` executable and the latest Steam executable
- cutscene timing fixes for real `60 fps` scene playback
- custom `SMAA` runtime with stock AA bypass
- `ACES` tonemapping integration
- DXGI adapter / VRAM selection fix
- stock option bootstrap overrides:
  - low-resolution buffer
  - shadow filter
  - SSAO
  - FPS limiter
  - texture detail
  - world density
  - rumble
- bruising / blood restore for normal NPC melee hits
- build/version guard and minidump support

## Build

Requirements:

- Visual Studio 2022 Build Tools or newer
- Windows x64

This export vendors the required `MinHook` header/import library under `external/`, so it does not depend on the private `_research` tree.

Build example:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" .\SPatch.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output:

- `build\Release\SPatch.asi`
- `SPatch.ini` in the repo root is a sample public-facing config

## Notes

- `Release` defaults to the public/final build surface.
