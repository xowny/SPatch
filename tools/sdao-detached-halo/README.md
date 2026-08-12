# Detached-foreground and hidden-shell AO WARP fixture

`Test-SDAODetachedHalo.ps1` compiles and runs both `SD_GTAO_LITE=1` and full
SDAO quality 2 against two fixed 64x64 depth fields on D3D11 WARP:

1. `detached-front` places a silhouette at view depth 4.2 over a flat wall at
   4.4. Its 0.2 separation is inside the effective 0.7285 AO radius
   (`0.5 * 1.457`), while both stochastic layers stay at far depth.
2. `hidden-shell` keeps the same receiver wall, moves the regular silhouette
   front to 3.0, and puts a rear/interior shell at 4.2 in stochastic layer 0.
   The regular separation is 1.4 and therefore outside the AO radius, while the
   hidden depth separation is 0.2. With the fixed q2 sample directions, actual
   hidden texel-center distances are 0.216–0.646 and regular-front distances are
   1.402–1.600, so `SelectStepHorizon` takes the full-SDAO stochastic fallback
   and accepts hidden candidates. Such a layer is valid for the production
   capture: a rear fragment can survive MIN blending when the nearer fragment
   was not selected into that stochastic layer. The fixture directly seeds
   this reachable consumer state as a separately rasterizable, front-facing
   hidden surface—not an assumed culled backface. It does not reproduce the
   production hash distribution or test the capture/MIN-blend pass itself.
   Of 1,080 receiver-to-silhouette hits, 512 remain positively weighted after
   applying `distance + abs(depth separation)` compensation.

GTAO-lite binds no stochastic-depth SRV, matching production. Full SDAO binds
the controlled texture; stochastic layer 1 remains at far depth in both cases.

The runner executes the current shader twice per mode, requires byte-identical
raw and filtered output, and compares wall-receiver halo excess against the
permanent candidate-6 control fixture. Both raw R16 AO and final RGBA8 AO must
reduce the candidate-6 halo by at least 50 percent in the shared detached-front
path and the SDAO-only hidden-shell path. The hidden-shell GTAO output must
remain byte-identical across the compensation change and have no positive halo excess.
The gate also proves that the hidden-shell SDAO output differs from GTAO, that
SDAO lowers receiver visibility relative to GTAO, that far stochastic layers
preserve shared-path equivalence, and that the two modes emit distinct
main-pass bytecode.

`SdaoDetachedHaloControl.hlsl` is a byte-for-byte promotion of the control
shader originally recorded at
`artifacts/visual-validation/20260730-ao-ghosting/candidate-6-checker2phase-fastfalloff.hlsl`.
Its SHA-256 is
`01EB2A72C44D79E552A27C7DBB47021CC3BB5DC91B46EEAAD4914C1AB052A9F4`.
The dated path is provenance only; the zero-argument validator reads the
permanent fixture beside this README.

Run from the repository root:

```powershell
& .\tools\Test-SDAODetachedHalo.ps1
```

The generated binaries, bytecode, and readbacks stay under `.tmp`; the harness
source and project are permanent under `tools/sdao-detached-halo`.
