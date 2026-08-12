# Third-party notices

ShenLong uses ReShade API headers from revision
`9fcd6ad935cfa19801e5e59a89a885dbdd6e731b` and the official ReShade full-add-on
runtime version `6.7.3.2148` as the D3D11 event host for every renderer
component. Its SDAO module also uses DiligentFX ScreenSpaceAmbientOcclusion
revision `eb616a8e30efa5193baba71ff1edae85bc6230a1`. Its skin-scattering module uses
Separable SSS revision `b2174689ab22d90647825fdbada7bbd08e7e4e49` and the
SDmodding x64 MinHook fork artifact recorded from SDK revision
`d5d8e1d67ddcea89fbef656b85052a5845dd34ee`. Its AgX tonemapper adapts
the permissively licensed real-time implementations in three.js revision
`1bc25777ac82579be2137f1f2e3f7d649595405f` and Filament revision
`5fb2a0ec8c588b84eb13b673290252c086b777c3`. The global-illumination implementation is
clean-room ShenLong code based on the method in
<https://arxiv.org/abs/2301.11376>; it does not incorporate third-party shader
source.

## Stochastic-Depth Ambient Occlusion

Reference: Jop Vermeer, Leonardo Scandolo, and Elmar Eisemann,
"Stochastic-Depth Ambient Occlusion," *Proceedings of the ACM on Computer
Graphics and Interactive Techniques*, volume 4, issue 1, 2021.

DOI: <https://doi.org/10.1145/3451268>

ShenLong independently implements the paper's stochastic hidden-surface depth
acquisition for the game's D3D11 geometry path. It uses a full-resolution depth
array, a per-layer selection probability of 0.2, and preserves each original
pixel shader's alpha-discard behavior. No paper source code or paper assets are
distributed.

## ReShade

Source: <https://github.com/crosire/reshade>

BSD 3-Clause License

Copyright 2014 Patrick Mours. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
- Neither the name of the copyright holder nor the names of its contributors
  may be used to endorse or promote products derived from this software
  without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## DiligentFX ScreenSpaceAmbientOcclusion

Source: <https://github.com/DiligentGraphics/DiligentFX>

Revision: `eb616a8e30efa5193baba71ff1edae85bc6230a1`

Licensed under the Apache License, Version 2.0. ShenLong retains DiligentFX's
cosine-weighted GTAO horizon evaluation and spatial-reconstruction design as the
AO estimator shared by SDAO and GTAO Lite. SDAO evaluates additional stochastic
hidden-surface depth layers; GTAO Lite evaluates the same estimator against
front depth only. The integration adapts that design to the game's D3D11
compute and resource layout. ShenLong reconstructs stable geometric normals
from full-resolution visible depth and uses deterministic spatial filtering
because the integration point does not expose the motion vectors and history
required by DiligentFX's temporal stages. For the same reason, horizon samples
stay at full resolution instead of using temporally stabilized coarse depth
mips. DiligentFX itself identifies Intel XeGTAO as the basis of its SSAO
implementation.

The complete license text is distributed as
`licenses/DiligentFX-Apache-2.0.txt`.

## Intel XeGTAO

Source: <https://github.com/GameTechDev/XeGTAO>

Copyright (C) 2016-2021, Intel Corporation

Licensed under the MIT License. ShenLong retains XeGTAO's texel-centre alignment
and thin-occluder falloff and horizon-decay heuristics in its DiligentFX-derived
non-temporal SDAO and GTAO Lite estimator. These prevent point-sampled depth
from being reconstructed at a different screen coordinate and reduce
foreground depth silhouettes on nearby background surfaces.

The complete license text is distributed as `licenses/XeGTAO-MIT.txt`.

## MinHook

Upstream project and license: <https://github.com/TsudaKageyu/minhook>

Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.

ShenLong links a modified SDmodding x64 prebuilt recorded from SDK revision
`d5d8e1d67ddcea89fbef656b85052a5845dd34ee`. It intercepts the two exact
character-submit functions needed to identify shipped skin materials and the
D3D11 shadow resource/context methods used for map-aware shadow scaling. It
also intercepts the exact AO-stage and HairBlur-submit functions used by
ShenLong's verified AO scheduler and stock HairBlur control. This fork's ABI
differs from canonical MinHook: `MH_CreateHook` installs a detour immediately,
and its public header exposes create/remove rather than the normal
initialize/enable sequence. The reviewed header SHA-256 is
`F2642BB69230017E52F8FE2F1208F6FDEA146302CC670E2003D2A69B5AE860E8`; the
reviewed library SHA-256 is
`DCF47C6ACDA033310E7C0FA3F7EE6E6C7F89AEA9F8C043D714A39FA01A5FECE2`.
The graphics build verifies both artifacts before linking.

The SDK snapshot does not contain corresponding fork source. The canonical
upstream repository above is the license origin and must not be read as the
exact source for this modified binary.

The complete BSD 2-Clause license text is distributed as
`licenses/MinHook-BSD-2-Clause.txt`.

## AgX real-time tonemapping

Sources:

- <https://github.com/mrdoob/three.js>
- <https://github.com/google/filament>

ShenLong's full-RGB shader uses three.js's explicit linear-sRGB/Rec.2020 color
conversion around the AgX inset/outset transform and Filament's current
default analytic contrast polynomial. The implementation is adapted to the
game's verified `0x67843125` final pre-HUD filmic boundary and preserves its
scene reconstruction, dynamic white normalization, bloom composition, and
final user-gamma adjustment. `MediumHigh` is ShenLong's custom, visually
stock-matched, toe-preserving analytic contrast grade around AgX middle gray;
it is not an official or standardized upstream AgX look transform. No Blender
LUT or Blender source code is included.

three.js is licensed under the MIT License. The complete license text is
distributed as `licenses/ThreeJS-MIT.txt`.

Filament is licensed under the Apache License, Version 2.0. The complete
Apache 2.0 text is distributed as `licenses/DiligentFX-Apache-2.0.txt`.

Copyright 2023 The Android Open Source Project.

## Opaque Cook-Torrance GGX lighting

Sources:

- <https://google.github.io/filament/main/filament.html>
- <https://disneyanimation.com/publications/physically-based-shading-at-disney/>

ShenLong's opaque-lighting replacements use the Filament-documented
Cook-Torrance microfacet structure with a GGX normal distribution,
height-correlated Smith visibility, and Schlick Fresnel. The implementation is
adapted to Sleeping Dogs' verified specular/gloss G-buffer encoding and native
light units; it preserves the game's diffuse calibration, light attenuation,
shadowing, auxiliary render targets, and HDR specular-alpha convention. No
Filament or Disney shader source is distributed.

Filament is licensed under the Apache License, Version 2.0. The complete Apache
2.0 text is distributed as `licenses/DiligentFX-Apache-2.0.txt`.

Copyright 2023 The Android Open Source Project.

## Separable SSS

Source: <https://github.com/iryoku/separable-sss>

Copyright (C) 2011 Jorge Jimenez (<jorge@iryoku.com>)
Copyright (C) 2011 Diego Gutierrez (<diegog@unizar.es>)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the following disclaimer in
   the documentation and/or other materials provided with the distribution:

   "Uses Separable SSS. Copyright (C) 2011 by Jorge Jimenez and Diego
   Gutierrez."

The distributed shader also retains the 2012 attribution embedded in the
upstream `SeparableSSS.h` implementation from which its kernel data is derived:
"Uses Separable SSS. Copyright (C) 2012 by Jorge Jimenez and Diego Gutierrez."

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are
those of the authors and should not be interpreted as representing official
policies, either expressed or implied, of the copyright holders.
