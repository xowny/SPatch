# Crash analysis: Main Pool `RingBuffer` allocation failure

## Scope and conclusion

The submitted dump records the game deliberately stopping on an `INT 3`
breakpoint after its main allocator failed a 3,204-byte allocation tagged
`RingBuffer`. It is not an access violation, and the dump does not establish
that SPatch, LotusPatch, the Steam overlay, the NVIDIA driver, or any other
loaded module exhausted or corrupted the pool.

No allocator bypass, forced-null return, capacity clamp, or object-lifetime
guard is justified by this dump. The failing path constructs game-owned state
and its callers immediately depend on that state. Skipping it without a proven
fallback would replace a deterministic out-of-memory stop with undefined
behavior or a later, less diagnosable crash.

## Evidence identity

- Dump:
  `C:\Users\Admin\Downloads\crash sleeping dogs user on nexusmods\SPatch-20260808-182420-612.dmp`
- Size: `308,561` bytes
- SHA-256:
  `4290DB858E165248C21CFED757BC808BA28C697F4226438FEA098443AF499DB1`
- File timestamp observed during analysis: `2026-08-08 18:11:16 +02:00`
- Process: `sdhdship.exe`
- Game image base in the dump: `0x00007FF784780000`
- Local reference executable SHA-256 for the matching legacy build:
  `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`

The dump's module timestamp/size tuple matches that reference build, and the
captured 256-byte exception window is byte-identical. Because this is a small
dump rather than a complete executable image, it cannot prove the whole-file
SHA-256 of the crashed process.

The dump records process creation at `2026-08-08 14:17:47 UTC` and the dump at
`15:24:20 UTC`: an uptime of 3,993 seconds (`1:06:33`). This is consistent with
the reporter's “about one hour” observation, but one dump cannot establish
repeatability or attribution.

The dump was parsed read-only with `minidump` 0.0.24. The corresponding local
game executable was inspected read-only with `pefile` and LLVM
`llvm-objdump`. The allocator and RingBuffer functions were then rechecked as
paired Ghidra instruction listings and decompilations in
`artifacts/reverse-engineering/20260810-crash-paired-evidence.txt` (SHA-256
`6DEB13A0701D60DBD5D7EC15369BD51EC7DB853EE1B91D5B0A0929C1CEDC3B4C`).
Function names below remain neutral when no trustworthy symbol name is
available.

## Exception and allocator evidence

The exception stream identifies thread `0x51A8`, code `0x80000003`
(`EXCEPTION_BREAKPOINT`), and address `sdhdship.exe+0x166D0E`. The instruction
at that address is `INT 3`. It immediately follows the game's stock diagnostic
call at `+0x166D09`.

The diagnostic format embedded in the verified executable is:

```text
ERROR: *** Out of memory ***
ERROR: Pool name      = %s
ERROR: Requested size = %d [%s]
```

The exception context and referenced static data resolve the arguments as:

- pool: `Main Pool`;
- requested size: `0xC84`, or `3,204` bytes;
- allocation tag: `RingBuffer`.

This is an engine allocator failure reported through an intentional breakpoint.
The breakpoint is the failure-reporting mechanism, not the original reason the
pool could not satisfy the request.

The paired function at RVA `0x166B60` confirms both levels of evidence: its
assembly calls the stock diagnostic routine and executes `INT 3` only after the
primary and fallback allocation paths return zero, while the decompiler
recovers the same null checks, diagnostic arguments, and terminal software
interrupt. There is no recoverable object returned from that branch.

## Exact captured game stack

Unwinding the x64 exception context with the verified executable's `.pdata`
and unwind metadata produces the following game return chain. Addresses are
relative to the dumped `sdhdship.exe` image:

```text
sdhdship.exe+0x166D0E  allocator failure INT 3
sdhdship.exe+0x187C26  return from the main-pool allocation call
sdhdship.exe+0x642403  return from RingBuffer backing allocation
sdhdship.exe+0x642FE6
sdhdship.exe+0x6454BA
sdhdship.exe+0x65ADA2
sdhdship.exe+0x64A797
sdhdship.exe+0x6447B9
sdhdship.exe+0x65FCBA
sdhdship.exe+0x590FF8  per-frame flow, immediately after call +0x65FA60
SPatch.asi+0x12122
```

At `sdhdship.exe+0x6423B0`, the constructor calculates its backing allocation
as `32 * count + 4`, passes the literal tag `RingBuffer`, and then initializes
the returned storage. A 3,204-byte request therefore represents a capacity of
exactly 100 32-byte entries plus a 4-byte header:

```text
32 * 100 + 4 = 3,204
```

The paired assembly uses `MUL 0x20`, adds four with explicit overflow handling,
and calls the allocator. The decompiler independently recovers the same
`32 * count + 4` calculation, null check, header write, and element
initialization. It also shows that a failed allocation leaves the backing
pointer null; callers were not proven to tolerate a forced continuation.

The upper part of the chain is reached from the game's per-frame update at
`+0x590FF3`, which calls `+0x65FA60`; the captured return is `+0x590FF8`.
Call-site and adjacent vehicle/traffic state evidence make this consistent
with an ambient vehicle/traffic-system ring buffer. In this interpretation,
"traffic" names the game subsystem. The 3,204 bytes are a memory allocation,
not network traffic and not 3,204 bytes of user or save data.

## Attribution limits

`SPatch.asi` is present on the stack because its frame-flow detour calls the
game's original per-frame routine. Presence on that call chain does not prove
that SPatch caused the Main Pool allocation failure.

The dump records the captured SPatch image at base `0x00007FFCA9560000`, image
size `0xAC000`, and module timestamp `0x69BE3863`. No byte-identical local
SPatch image with that identity was available during this analysis. The current
installed and workspace builds have different image identities, so
`SPatch.asi+0x12122` cannot be responsibly symbolized or compared with current
source. The dump also contains `LotusPatch.asi`, the Steam overlay, ReShade is
not listed, and NVIDIA user-mode driver modules. None is singled out by the
failing allocator stack.

The small dump does not include enough heap/pool state to determine:

- whether Main Pool was genuinely full, fragmented, or previously damaged;
- which allocation history consumed the pool;
- whether the failure is repeatable at the same scene, save, uptime, or frame;
- whether a mod interaction changes the rate of pool use;
- why this fresh backing allocation for a newly constructed
  `RoadSpaceComponent` could not be satisfied, including possible prior
  leakage, fragmentation, or corruption.

## Safe next evidence

The next useful report should preserve the exact crashing binaries and include
`SPatch.log`, `SPatch.ini`, the game build hash, the loader and all loaded ASI
hashes, save/mission/location, time since launch, and repeatability. A matched
reproduction matrix should compare the same save and route with SPatch alone,
each additional ASI alone, and the combined set. Process commit, available
system memory, and pool telemetry immediately before the failure would
distinguish exhaustion from corruption far better than another small dump.

Until that operating path is reproduced and the pool's ownership history is
observed, the safe result is analysis only: retain crash dumps and diagnostics,
but add no allocator guard.
