# Hair depth-dither and MSAA analysis

## Decision

Do not add an option described as “unconditionally dither hair depth when using
MSAA.” The researched hair shader already performs an unconditional
screen-space dither discard before its separate diffuse-alpha discard, and the
observed renderer path is single-sample rather than MSAA.

Adding the same discard again would be redundant. Advertising it as an MSAA
fix would also claim an operating path that has not been observed.

## Native shader evidence

The captured native pixel shader `0x0A6EDB3E`:

1. scales `SV_Position.xy` by `(0.25, 0.5)`;
2. samples the bound 4-by-2 fade-dither mask at `t2/s2`;
3. discards when that mask sample is below `0.1`;
4. samples biased diffuse alpha from `t0/s0`; and
5. independently discards when diffuse alpha is below `0.5`.

The canonical captured bytecode is
`artifacts/reverse-engineering/2026-07-22-sss-shader-capture/PS_0x0A6EDB3E.cso`,
SHA-256
`D3E5B2C389CE39DA5E2B2C70A02ACC9B92E640681E96EC55E4C1F253ED620B3F`.
Its human-readable assembly is
`artifacts/runtime-logs/sdao-depth-probe-20260728-011840-observational/active-shader-analysis/PS_0X0A6EDB3E.asm.txt`,
SHA-256
`7EFAAAA22CB1A1DF56CCF9A0DE29B17ACA68187C9F0C5F743EB9DF2ED1699A77`.

The exact draw records depth enabled with depth writes, blending disabled,
three G-buffer render targets, and the 4-by-2 dither texture bound at `t2`.
Those records are in
`artifacts/validation/sky-path-trace-20260729/SSSTrace.tsv`.

## ShenLong interaction

ShenLong's exact-profile hair capture deliberately reproduces both native
discards in `luma/overlay/Shaders/Sleeping Dogs Definitive Edition/SPatchSSS.hlsl`.
Its packaged `HairCapturePS` has SHA-256
`48B29EFD0FD89D2AB550EE8B74E247225C1A90D8973CA9B97461619255451917`.
Changing native coverage without changing this replay in lockstep would make
the scene depth and ShenLong hair mask disagree.

The captured G-buffer, depth resources, and ReShade swap chain all use
`SampleDesc.Count=1`. SPatch SMAA is a later, single-sample post-process; it is
not scene MSAA. ShenLong's hair/SSS replay also rejects multisampled G-buffer
resources and retains native rendering.

## What would justify a different experiment

An alpha-proportional coverage change would be a different feature. It would
need matched captures for every in-game anti-aliasing tier, including internal
resource dimensions, sample descriptions, alpha-to-coverage state, scene and
shadow hair permutations, and lockstep comparisons of native depth, G-buffer
coverage, ShenLong material masks, temporal shimmer, and performance. No such
evidence is currently available.
