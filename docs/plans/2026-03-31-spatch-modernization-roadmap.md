# SPatch EXE Modernization Roadmap

> **For Hermes:** Use `subagent-driven-development` if this plan is executed as a multi-task implementation pass.

**Goal:** Turn the EXE audit into a ranked SPatch roadmap that modernizes the highest-value legacy game paths without widening hook risk blindly.

**Architecture:** Reuse SPatch's existing model: config-gated hooks in `src/Hooks.cpp`, compatibility shaping in `src/BootstrapPolicy.cpp`, XML-side stock setting fixes in `src/DisplaySettings.cpp`, and regression coverage in `tests/ConfigVersionTests.cpp`. Favor safe wins first, then add probe-only hooks for risky subsystems before shipping behavioral changes.

**Tech Stack:** C++20, MinHook, Win32, DXGI/D3D11, custom summary logging, custom console test runner.

---

## Ranked Opportunities

### Tier 1: Ship First

1. **Display / mode modernization**
   - EXE evidence: `FUN_14069EDB0` (`renderplat.cpp`) handles mode enumeration and filtering.
   - Why first: high user value, low regression risk, already adjacent to existing adapter and display work.
   - Best SPatch seam:
     - `src/Hooks.cpp` existing `kDxgiAdapterInitRva = 0x0069F270`
     - `src/DisplaySettings.cpp`
     - `src/Config.h` / `src/Config.cpp`
   - Expected outcomes:
     - prefer highest refresh mode
     - better duplicate mode filtering
     - saner fullscreen vs borderless defaults
     - explicit ultrawide-safe policy instead of stock fallback behavior

2. **NIS actor-state cleanup**
   - EXE evidence:
     - `FUN_1403E8050` (`nisnodes.cpp`) actor setup
     - `FUN_1403E7A80` (`nisnodes.cpp`) actor teardown / restore
   - Why first: SPatch already owns NIS timing and cutscene flow; this extends an existing subsystem instead of introducing a new one.
   - Best SPatch seam:
     - `src/Hooks.cpp` existing NIS hooks around `kNisBootstrapRva`, `kNisOwnerRva`, `kFrameFlowRva`
     - add new optional hook RVAs for `0x003E8050` and `0x003E7A80`
   - Expected outcomes:
     - reduce post-cutscene actor state leakage
     - stabilize visibility / effect / material restoration after NIS playback

3. **Twitch SDK kill-switch**
   - EXE evidence: `FUN_140081CB0` handles Twitch login failure via `TWITCHSDK_64_RELEASE.DLL` ordinals.
   - Why first: dead platform baggage is a clean modernization target.
   - Best SPatch seam:
     - `src/Hooks.cpp` new probe/hook group for Twitch callbacks / init path
     - `src/Config.h` / `src/Config.cpp` new user-facing kill-switch
     - `src/BootstrapPolicy.cpp` optional profile-level disable
   - Expected outcomes:
     - disable unused Twitch startup/state transitions
     - reduce compatibility risk from obsolete middleware

### Tier 2: Probe, Then Ship

4. **Input stack modernization**
   - EXE evidence:
     - `FUN_140A3BA70` mixed DirectInput + XInput bootstrap
     - `FUN_140A3E5C0` reconnect/update path
     - `FUN_140A3EA80` per-device poll / recover path
   - Why probe first: input regressions are expensive and visible.
   - Best SPatch seam:
     - `src/Hooks.cpp`
     - existing rumble override path around `kRumbleApplyHelperRva`
   - Expected outcomes after probe phase:
     - XInput-first policy
     - cleaner reconnect handling
     - optional DirectInput bypass for pads
     - better controller diagnostics

5. **VRAM allocator / pool modernization**
   - EXE evidence: `FUN_140173750` (`vramemorypool.cpp`) aligns and initializes pool ranges.
   - Why probe first: allocator mistakes cause hard crashes, not soft regressions.
   - Best SPatch seam:
     - `src/Hooks.cpp` existing adapter fix and VRAM summary plumbing
     - new probe-only hook group for `0x00173750`
   - Expected outcomes after probe phase:
     - expose stock alignment decisions
     - validate whether oversized allocations or pool bounds are the real pain point
     - only then add behavior changes

6. **Render-thread queue hardening**
   - EXE evidence: `FUN_140A17090` (`renderthreadplat.cpp`) allocates and enqueues render-thread commands.
   - Why probe first: touches queueing behavior near current render hooks.
   - Best SPatch seam:
     - `src/Hooks.cpp`
     - existing task/scenery summary counters
   - Expected outcomes after probe phase:
     - detect queue pressure / long waits during render hook activity
     - decide whether any compatibility hardening is justified

### Tier 3: Repo Housekeeping

7. **Config and test modernization**
   - Current issues:
     - `tests/ConfigVersionTests.cpp` is a monolith for all regression coverage.
     - `src/Config.cpp` still uses legacy INI APIs (`GetPrivateProfileIntW`, `GetPrivateProfileStringW`).
     - `README.md` is outdated and still describes the project as an initial probe.
   - Why do this: lowers friction for every future patch.

---

## Concrete Workstreams

### Task 1: Add modernization config groups

**Objective:** Create config switches that let new modernization work land in narrow, testable slices.

**Files:**
- Modify: `src/Config.h`
- Modify: `src/Config.cpp`
- Modify: `src/Main.cpp`
- Test: `tests/ConfigVersionTests.cpp`

**Add config fields**

```cpp
// Display modernization
bool hook_display_mode_filter = false;
int prefer_max_refresh_rate = 1;
int prefer_borderless_window = -1;

// NIS modernization
bool hook_nis_actor_state_guard = false;

// Twitch modernization
bool disable_twitch_sdk = false;

// Probe-first buckets
bool hook_input_probe = false;
bool hook_vram_pool_probe = false;
bool hook_renderthread_probe = false;
```

**Test additions**

Add config-default and config-parse coverage in `tests/ConfigVersionTests.cpp` for every new field before writing hook code.

**Verification**

Run:

```powershell
msbuild .\SPatchTests.vcxproj /p:Configuration=Release /p:Platform=x64
.\build\Release\SPatchTests.exe
```

Expected:
- `SPatch ConfigVersionTests passed`

### Task 2: Ship display / mode modernization

**Objective:** Improve display selection behavior without touching fragile render logic.

**Files:**
- Modify: `src/Hooks.cpp`
- Modify: `src/DisplaySettings.cpp`
- Modify: `src/HooksSummary.h`
- Modify: `tests/ConfigVersionTests.cpp`

**Hook targets**
- keep `kDxgiAdapterInitRva = 0x0069F270`
- add a new display-mode detour at `0x0069EDB0` only if the call boundary is mapped cleanly

**Implementation shape**

```cpp
void DetourDisplayModeEnumeration(...) {
    g_display_mode_probe_count.fetch_add(1);
    // Record stock mode list.
    // Sort or filter only when config says so.
    // Prefer refresh rate improvements over structural rewrites.
    g_display_mode_original(...);
}
```

**Rules**
- Do not invent aspect-ratio math in phase 1.
- Start with sorting/filtering and explicit logging of picked width/height/refresh.
- If the mode hook is not stable, fall back to `DisplaySettings.xml` policy only.

**Acceptance**
- chosen mode is logged with refresh rate
- duplicate / obviously bad modes are filtered
- stock behavior remains default when new flags are off

### Task 3: Extend NIS from time fixes into actor-state fixes

**Objective:** Use the current NIS patch cluster to fix actor setup/teardown leakage after cutscenes.

**Files:**
- Modify: `src/Hooks.cpp`
- Modify: `src/HooksSummary.h`
- Modify: `src/Config.h`
- Modify: `src/Config.cpp`
- Test: `tests/ConfigVersionTests.cpp`

**Hook targets**
- new NIS actor entry probe: `0x003E8050`
- new NIS actor restore probe: `0x003E7A80`

**Implementation shape**

```cpp
void DetourNisActorSetup(...) {
    g_nis_actor_setup_count.fetch_add(1);
    g_nis_actor_setup_original(...);
}

void DetourNisActorRestore(...) {
    g_nis_actor_restore_count.fetch_add(1);
    g_nis_actor_restore_original(...);
}
```

**Rules**
- First land probes and summary fields.
- Only add corrective writes after two or three reproducible traces identify a bad stock state transition.
- Keep this work under the existing NIS config family instead of inventing a second parallel system.

### Task 4: Add a Twitch SDK hard-disable path

**Objective:** Disable dead Twitch middleware cleanly instead of carrying an obsolete runtime path forever.

**Files:**
- Modify: `src/Hooks.cpp`
- Modify: `src/Config.h`
- Modify: `src/Config.cpp`
- Modify: `src/BootstrapPolicy.cpp`
- Test: `tests/ConfigVersionTests.cpp`

**Hook targets**
- callback already identified at `0x00081CB0`
- before shipping a full kill-switch, map its caller chain and the Twitch init owner

**Implementation shape**

```cpp
if (g_config.disable_twitch_sdk) {
    log::Info("twitch_sdk disabled by config");
    return;
}
```

**Rules**
- Do not ship a callback-only bypass if init still runs elsewhere.
- First add a probe mode that logs Twitch init/callback hits.
- After the caller chain is confirmed, short-circuit the highest stable init boundary.

### Task 5: Add input probe-only hooks

**Objective:** Modernize pad handling safely by observing the stock controller stack before changing behavior.

**Files:**
- Modify: `src/Hooks.cpp`
- Modify: `src/HooksSummary.h`
- Modify: `src/Config.h`
- Modify: `src/Config.cpp`
- Test: `tests/ConfigVersionTests.cpp`

**Hook targets**
- `0x00A3BA70`
- `0x00A3E5C0`
- `0x00A3EA80`

**Probe data to capture**
- connected slot count
- reconnect count
- DirectInput fallback count
- `ERROR_DEVICE_NOT_CONNECTED` count
- rumble override interactions

**Rules**
- Default off.
- No behavior changes until probe output proves where the stock stack actually fails.

### Task 6: Add VRAM pool probe-only hook

**Objective:** Close the gap between adapter selection fixes and actual allocator behavior.

**Files:**
- Modify: `src/Hooks.cpp`
- Modify: `src/HooksSummary.h`
- Modify: `src/Config.h`
- Modify: `src/Config.cpp`
- Test: `tests/ConfigVersionTests.cpp`

**Hook target**
- `0x00173750`

**Probe data to capture**
- requested size
- aligned size
- requested base
- aligned base
- pool/channel identity if recoverable

**Rules**
- Phase 1 is telemetry only.
- No allocator writes until traces show a stable, repeatable stock failure pattern.

### Task 7: Tighten repo maintenance surfaces

**Objective:** Reduce future patch cost by fixing the local maintenance bottlenecks.

**Files:**
- Modify: `README.md`
- Create: `tests/ConfigTests.cpp`
- Create: `tests/BootstrapPolicyTests.cpp`
- Create: `tests/HooksSummaryTests.cpp`
- Optionally create: `src/IniConfig.cpp` / `src/IniConfig.h`

**Work**
- split `tests/ConfigVersionTests.cpp` by subsystem
- refresh README to match current SPatch scope
- decide whether legacy INI APIs stay or get wrapped behind a small config layer

---

## Recommended Execution Order

1. Task 1: add config groups and tests
2. Task 2: ship display modernization
3. Task 3: ship NIS actor-state probes, then fixes
4. Task 4: map and disable Twitch cleanly
5. Task 5: land input probes
6. Task 6: land VRAM pool probes
7. Task 7: do repo cleanup after the first two shipped wins

---

## Build and Verification Commands

### Tests

```powershell
msbuild .\SPatchTests.vcxproj /p:Configuration=Release /p:Platform=x64
.\build\Release\SPatchTests.exe
```

### ASI build

```powershell
msbuild .\SPatch.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Game deployment build

```powershell
msbuild .\SPatch.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build
```

This historical command now builds only. Current deployment is deliberately
opt-in and accepts only a validated final artifact: pass both
`/p:FinalRelease=true` and `/p:DeployGameArtifacts=true` with an explicit
`/p:GameDir=...`.

---

## Notes

- The EXE still imports `WSOCK32.DLL`, `WININET.DLL`, `GetVersionExA`, `GetAdaptersInfo`, `CryptGenRandom`, `XINPUT9_1_0.DLL`, and `TWITCHSDK_64_RELEASE.DLL`. Those are real modernization leads, but not all of them deserve SPatch hooks immediately.
- `src/SmaaRuntime.cpp` already prefers `D3DCompiler_47.dll`, so the game's `D3DCompiler_46` import is not the best next SPatch target.
- The fastest safe wins are display behavior, NIS actor-state cleanup, and Twitch deprecation.
