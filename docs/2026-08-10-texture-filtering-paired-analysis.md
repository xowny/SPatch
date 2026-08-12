# Texture-filtering paired analysis (2026-08-10)

## Evidence contract

Every conclusion below was checked against both Ghidra decompilation and the
instruction listing exported from the same function body. The source
executables were copied to a disposable analysis directory and verified before
analysis; neither installed nor archived original was modified.

- legacy researched executable SHA-256:
  `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`
- newer Steam executable SHA-256:
  `2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA`
- paired legacy export:
  `artifacts/reverse-engineering/20260810-texture-filtering-v42/legacy-paired-evidence.txt`
  (SHA-256 `7625731BA9FF92BAFE844106C291F3E1BF608058F758E39E82C02010559BE8F0`)
- paired newer-Steam export:
  `artifacts/reverse-engineering/20260810-texture-filtering-v42/latest-paired-evidence.txt`
  (SHA-256 `C2FA006BE804259A5E841FF808ACF82CABF75FB1D523E3AA4B55916E1B281154`)
- paired legacy writer-caller export:
  `artifacts/reverse-engineering/20260810-texture-filtering-v42/legacy-writer-caller-paired-evidence.txt`
  (SHA-256 `50061CDDBCBCA8601607E064145933DCB82C9E9BE68413B2D30884FCF2D50F62`)
- paired newer-Steam writer-caller export:
  `artifacts/reverse-engineering/20260810-texture-filtering-v42/latest-writer-caller-paired-evidence.txt`
  (SHA-256 `B0A6B80CD01D7D85C5958A960CF30DE8968CC00A3CFF7CC229B9739BBB64AC61`)

The reproducible exporter is `tools/GhidraExportFunctionEvidence.java`.

## Filtering-settings writer

| Executable | Writer RVA | Builder RVA | Shared value RVA |
| --- | ---: | ---: | ---: |
| Legacy researched | `0xA21F00` | `0xA196E0` | `0x20F2A0C` |
| Newer Steam | `0xA21DD0` | `0xA195B0` | `0x20F2A0C` |

The two writer bodies have the same instruction shape. `ECX` is tested, `1` is
shifted left by `CL` for a nonzero input, and the resulting value is compared
with and written to the shared integer before existing sampler states are
rebuilt. Ghidra independently recovers the function as `void function(int)`
and the same `1 << param_1` behavior. This supports SPatch's writer detour ABI
and the `2`, `3`, and `4` exponents used for 4x, 8x, and 16x respectively.

The detour still validates the complete 19-byte writer prologue before hook
creation and reads the shared integer after the first active invocation to
confirm the requested anisotropy actually reached the engine.

The binaries each contain one direct writer call, inside instruction-identical
settings-commit functions at legacy RVA `0x69E7C0` and newer-Steam RVA
`0x69E790`. The call sites at `0x69E7F0` and `0x69E7C0` load the exponent from
the settings object at offset `0x60`, but only after the commit helper returns
one. Both Ghidra decompilations independently recover the same conditional call
and object field.

An exact-shortcut unattended benchmark then supplied the missing runtime fact:
the writer hook installed, but this unchanged-settings startup never invoked
it. A writer-only override therefore did not prove startup application. The
corrected path also hooks the exact two-argument sampler builder. After the
whole hook and byte-patch transaction commits, each builder invocation writes
and reads back the requested shared native anisotropy immediately before
calling the original builder. The writer detour remains in place so later game
settings commits keep the native rebuild behavior. Hooking the builder only
after validating its exact stock prologue, while behavior is still gated, lets
the selector byte patch commit last. The patcher may skip re-reading only the
entry bytes now owned by MinHook through an explicit flag produced after that
validated hook succeeds; it still requires the exact branch prefix, stock or
SPatch-owned instruction, and suffix.

## Exact trilinear selector branch

The sampler builders are instruction-identical apart from relocated calls and
data references. The patched instructions are:

- legacy `0xA19788`: `B8 15 00 00 00` (`MOV EAX,0x15`)
- newer Steam `0xA19658`: `B8 15 00 00 00` (`MOV EAX,0x15`)

The Windows SDK defines `0x15` as `D3D11_FILTER_MIN_MAG_MIP_LINEAR` and `0x55`
as `D3D11_FILTER_ANISOTROPIC`. The surrounding assembly proves this is one
exact selector case among separate point, comparison, trilinear, and already
anisotropic branches. SPatch changes only that five-byte instruction and
validates the full function prologue plus the immediately adjacent branch
prefix and suffix before doing so.

The paired analysis also disproves the former documentation's stronger
"zero-bias, full-LOD" wording. The builder calculates the descriptor's LOD bias
later from independent fields; the selected branch does not test the bias.
No caller-data invariant was proven that would make the branch zero-bias-only.
The shipped contract therefore says only what the instructions prove: it
promotes the exact stock trilinear filter-selector branch. Other selector
branches remain native.

## Result

The exact builder hook, native writer detour, and selector patch are supported
for both known executables. The earlier zero-bias/full-LOD marketing claim is
not supported and has been removed from the INI comments, README, and Nexus
release notes.
