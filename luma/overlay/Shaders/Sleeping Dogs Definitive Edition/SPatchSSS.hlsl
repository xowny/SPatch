// Jimenez separable subsurface scattering for SPatch.
//
// Uses Separable SSS. Copyright (C) 2012 by Jorge Jimenez and Diego Gutierrez.
// The original BSD-3-Clause notice is reproduced in THIRD_PARTY_NOTICES.md.

#ifndef SPATCH_SSS_QUALITY
#define SPATCH_SSS_QUALITY 2
#endif

#ifndef SPATCH_SSS_HORIZONTAL
#define SPATCH_SSS_HORIZONTAL 0
#endif

#ifndef SPATCH_SSS_DEVELOPMENT
#define SPATCH_SSS_DEVELOPMENT 0
#endif

cbuffer SPatchSSS : register(b0)
{
    float2 InvResolution;
    float Radius;
    float Strength;
    float2 Direction;
    float SpecularScale;
    float DebugView;
    uint MaterialProfile;
    float ProfileAnisotropy;
    float ProfileMaskScale;
    float ProfilePadding;
};

// Per-draw eye-island data. The centre and ellipse radii are in the eye
// material's periodic UV space; the inner/outer values are normalized radii.
cbuffer SPatchEyeMask : register(b5)
{
    float2 EyeCenter;
    float2 EyeRadius;
    float EyeIrisInner;
    float EyeIrisOuter;
    float2 EyeMaskPadding;
};

// This is the game's cbExternalViewTransform buffer. Its fifth vector is used
// by the stock deferred and final-composition shaders to reconstruct view depth.
cbuffer ExternalViewTransform : register(b1)
{
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
};

Texture2D<float4> SourceTexture : register(t0);
Texture2D<float4> OriginalTexture : register(t1);
Texture2D<float> SceneDepthTexture : register(t2);
Texture2D<float> SkinDepthTexture : register(t3);
// DXGI_FORMAT_X24_TYPELESS_G8_UINT exposes its stencil byte in G. Keep the
// vector type explicit: a scalar texture read selects the unused X24/R lane.
Texture2D<uint2> SkinStencilTexture : register(t4);
// Profile-specific replay data. Eye draws store sclera eligibility in R; hair
// draws store exact coverage in A with an invalid direction sentinel in RGB;
// foliage draws store captured back-light incident RGB and thickness in A.
Texture2D<float4> MaterialDataTexture : register(t5);

SamplerState LinearClampSampler : register(s0);
SamplerState FoliageDitherSampler : register(s1);
SamplerState FadeDitherSampler : register(s2);

static const uint SkinProfile = 1u;
static const uint EyeProfile = 2u;
static const uint HairProfile = 3u;
static const uint TeethProfile = 4u;
static const uint FoliageProfile = 5u;

struct FullscreenVertex
{
    float4 Position : SV_Position;
    float2 Texcoord : Texcoord0;
};

struct EyeMaskVertex
{
    float4 Position : SV_Position;
    float2 Texcoord : Texcoord0;
};

struct HairCaptureVertex
{
    float4 Position : SV_Position;
    float4 NormalAndU : Texcoord0;
    float4 TangentAndV : Texcoord1;
    float3 Bitangent : Texcoord2;
};

struct FoliageCaptureVertex
{
    float4 Position : SV_Position;
    float2 Texcoord : Texcoord0;
    float4 NormalAndMultiplier : Texcoord1;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.Texcoord = corner;
    return output;
}

float FadeDitherAt(float2 pixelPosition)
{
    // This exactly mirrors the native character shaders: the dither mask is
    // sampled at quarter horizontal and half vertical pixel frequency and a
    // value strictly below 0.1 is rejected.
    return SceneDepthTexture.Sample(
        FadeDitherSampler, pixelPosition * float2(0.25, 0.5));
}

float4 EyeMaskPS(EyeMaskVertex input) : SV_Target
{
    clip(FadeDitherAt(input.Position.xy) - 0.1);

    // Eye UV islands can cross a wrap boundary, so measure the shortest
    // periodic displacement before applying the fitted per-material ellipse.
    const float2 delta = frac(input.Texcoord - EyeCenter + 0.5) - 0.5;
    const float2 safeRadius = max(abs(EyeRadius), float2(0.000001, 0.000001));
    const float normalizedRadius = length(delta / safeRadius);
    const float outerRadius = max(EyeIrisOuter, EyeIrisInner + 0.000001);
    const float scleraWeight = smoothstep(
        EyeIrisInner, outerRadius, normalizedRadius);
    return float4(scleraWeight, 0.0, 0.0, 1.0);
}

float4 HairCapturePS(HairCaptureVertex input) : SV_Target
{
    clip(FadeDitherAt(input.Position.xy) - 0.1);

    const float2 uv = float2(input.NormalAndU.w, input.TangentAndV.w);
    const float diffuseAlpha =
        SourceTexture.SampleBias(LinearClampSampler, uv, -2.0).a;
    clip(diffuseAlpha - 0.5);

    // The exact game VS exposes a transformed 3-D bitangent, not a proven
    // projected strand direction. Mark direction invalid so both blur passes
    // use their orthogonal X/Y directions instead of creating camera-dependent
    // anisotropy from an unprojected vector.
    return float4(0.5, 0.5, 0.0, diffuseAlpha);
}

float4 FoliageCapturePS(FoliageCaptureVertex input) : SV_Target
{
    // Dedicated 0x537E7246 replay: retain its exact screen-space dither and
    // biased diffuse-alpha cutout before writing any private material data.
    const float dither = OriginalTexture.Sample(
        FoliageDitherSampler,
        input.Position.xy * float2(0.25, 0.5)).r;
    clip(dither - 0.1);

    const float4 diffuse = SourceTexture.SampleBias(
        LinearClampSampler, input.Texcoord, -1.0);
    clip(diffuse.a - 0.5);

    const float3 normalSource = input.NormalAndMultiplier.xyz;
    const float normalLengthSquared = dot(normalSource, normalSource);
    const float3 normal = normalLengthSquared > 0.00000001
        ? normalSource * rsqrt(normalLengthSquared)
        : 0.0;
    // During this exact replay the game's original b0/b1 remain bound. In the
    // dedicated foliage shader b0 contains SunDir at offset 0 and SunColor at
    // offset 32, while b1 begins with ColourTint. Reinterpret those proven
    // slots through the declarations used by the later fullscreen pass.
    const float3 sunDirectionSource = float3(InvResolution, Radius);
    const float sunDirectionLengthSquared =
        dot(sunDirectionSource, sunDirectionSource);
    const float3 sunDirection = sunDirectionLengthSquared > 0.00000001
        ? sunDirectionSource * rsqrt(sunDirectionLengthSquared)
        : 0.0;
    const float backLighting = sunDirectionLengthSquared > 0.00000001
        ? saturate(-dot(normal, sunDirection))
        : 0.0;
    const float3 sunColor = max(
        float3(asfloat(MaterialProfile), ProfileAnisotropy, ProfileMaskScale),
        0.0);
    const float3 colourTint = max(WorldView[0].xyz, 0.0);
    const float vertexMultiplier = saturate(input.NormalAndMultiplier.w);
    const float3 nonnegativeDiffuse = max(diffuse.rgb, 0.0);
    // Exact 0x537E7246 behavior: the shipped foliage PS squares the sampled
    // diffuse before tinting, so the captured response stays in the same
    // linear-light domain as the R16G16B16A16 HDR lighting buffer.
    const float3 surfaceResponse = nonnegativeDiffuse * nonnegativeDiffuse *
        colourTint * vertexMultiplier;
    // Store only incident energy already present in the game's sunlight inputs.
    // The later pass can attenuate this value but cannot invent energy from
    // neighboring foliage or recolor unrelated local lights.
    const float3 incidentRadiance = min(
        surfaceResponse * sunColor * backLighting,
        65504.0);
    // Alpha is a coverage signal rather than a metric thickness. Remap only
    // its accepted half-range so the Beer-Lambert pass receives [0, 1].
    const float thickness = saturate((diffuse.a - 0.5) * 2.0);
    return float4(incidentRadiance, thickness);
}

#if SPATCH_SSS_QUALITY == 2
static const uint KernelSize = 25;
static const float4 Kernel[KernelSize] =
{
    float4(0.530605, 0.613514, 0.739601, 0.0),
    float4(0.000973794, 0.0000111862, 0.000000943437, -3.0),
    float4(0.00333804, 0.0000785437, 0.0000129415, -2.52083),
    float4(0.00500364, 0.00020094, 0.0000528848, -2.08333),
    float4(0.00700976, 0.00049366, 0.000151938, -1.6875),
    float4(0.0094389, 0.00139119, 0.000416598, -1.33333),
    float4(0.0128496, 0.00356329, 0.00132016, -1.02083),
    float4(0.017924, 0.00711691, 0.00347194, -0.75),
    float4(0.0263642, 0.0119715, 0.00684598, -0.520833),
    float4(0.0410172, 0.0199899, 0.0118481, -0.333333),
    float4(0.0493588, 0.0367726, 0.0219485, -0.1875),
    float4(0.0402784, 0.0657244, 0.04631, -0.0833333),
    float4(0.0211412, 0.0459286, 0.0378196, -0.0208333),
    float4(0.0211412, 0.0459286, 0.0378196, 0.0208333),
    float4(0.0402784, 0.0657244, 0.04631, 0.0833333),
    float4(0.0493588, 0.0367726, 0.0219485, 0.1875),
    float4(0.0410172, 0.0199899, 0.0118481, 0.333333),
    float4(0.0263642, 0.0119715, 0.00684598, 0.520833),
    float4(0.017924, 0.00711691, 0.00347194, 0.75),
    float4(0.0128496, 0.00356329, 0.00132016, 1.02083),
    float4(0.0094389, 0.00139119, 0.000416598, 1.33333),
    float4(0.00700976, 0.00049366, 0.000151938, 1.6875),
    float4(0.00500364, 0.00020094, 0.0000528848, 2.08333),
    float4(0.00333804, 0.0000785437, 0.0000129415, 2.52083),
    float4(0.000973794, 0.0000111862, 0.000000943437, 3.0),
};
#elif SPATCH_SSS_QUALITY == 1
static const uint KernelSize = 17;
static const float4 Kernel[KernelSize] =
{
    float4(0.536343, 0.624624, 0.748867, 0.0),
    float4(0.00317394, 0.000134823, 0.0000377269, -2.0),
    float4(0.0100386, 0.000914679, 0.000275702, -1.53125),
    float4(0.0144609, 0.00317269, 0.00106399, -1.125),
    float4(0.0216301, 0.00794618, 0.00376991, -0.78125),
    float4(0.0347317, 0.0151085, 0.00871983, -0.5),
    float4(0.0571056, 0.0287432, 0.0172844, -0.28125),
    float4(0.0582416, 0.0659959, 0.0411329, -0.125),
    float4(0.0324462, 0.0656718, 0.0532821, -0.03125),
    float4(0.0324462, 0.0656718, 0.0532821, 0.03125),
    float4(0.0582416, 0.0659959, 0.0411329, 0.125),
    float4(0.0571056, 0.0287432, 0.0172844, 0.28125),
    float4(0.0347317, 0.0151085, 0.00871983, 0.5),
    float4(0.0216301, 0.00794618, 0.00376991, 0.78125),
    float4(0.0144609, 0.00317269, 0.00106399, 1.125),
    float4(0.0100386, 0.000914679, 0.000275702, 1.53125),
    float4(0.00317394, 0.000134823, 0.0000377269, 2.0),
};
#elif SPATCH_SSS_QUALITY == 0
static const uint KernelSize = 11;
static const float4 Kernel[KernelSize] =
{
    float4(0.560479, 0.669086, 0.784728, 0.0),
    float4(0.00471691, 0.000184771, 0.0000507566, -2.0),
    float4(0.0192831, 0.00282018, 0.00084214, -1.28),
    float4(0.03639, 0.0130999, 0.00643685, -0.72),
    float4(0.0821904, 0.0358608, 0.0209261, -0.32),
    float4(0.0771802, 0.113491, 0.0793803, -0.08),
    float4(0.0771802, 0.113491, 0.0793803, 0.08),
    float4(0.0821904, 0.0358608, 0.0209261, 0.32),
    float4(0.03639, 0.0130999, 0.00643685, 0.72),
    float4(0.0192831, 0.00282018, 0.00084214, 1.28),
    float4(0.00471691, 0.000184771, 0.0000507565, 2.0),
};
#else
#error SPATCH_SSS_QUALITY must be 0, 1, or 2.
#endif

float LinearizeDepth(float depth)
{
    const float nearPlane = ViewScaleAndNearFar.z;
    const float farPlane = ViewScaleAndNearFar.w;
    const float denominator = farPlane - depth * (farPlane - nearPlane);
    return nearPlane * farPlane / max(abs(denominator), 0.000001);
}

int2 PixelFromUv(float2 uv)
{
    const int2 size = int2(round(1.0 / InvResolution));
    return clamp(int2(uv * size), int2(0, 0), size - 1);
}

uint MaterialStencilAt(int2 pixel)
{
    return SkinStencilTexture.Load(int3(pixel, 0)).y;
}

bool IsVisibleMaterialPixel(int2 pixel)
{
    // Every pass accepts only its exact replay profile. In particular, an eye,
    // tooth, or hair texel can never enter the skin footprint (or vice versa).
    if (MaterialStencilAt(pixel) != MaterialProfile)
        return false;

    const float sceneDepth = SceneDepthTexture.Load(int3(pixel, 0));
    const float skinDepth = SkinDepthTexture.Load(int3(pixel, 0));
    // Both surfaces use the same R24 depth format and projection. Comparing in
    // depth-buffer units is exact and avoids magnifying quantization error when
    // reconstructing distant view-space positions. Eight R24 units cover
    // rasterization roundoff while still rejecting hair, clothing, and scene
    // geometry in front of the replayed skin surface.
    const float tolerance = 8.0 / 16777215.0;
    return abs(sceneDepth - skinDepth) <= tolerance;
}

bool IsVisibleMaterial(float2 uv)
{
    return IsVisibleMaterialPixel(PixelFromUv(uv));
}

// Reconstruct the hardware linear sample from only visible same-profile texels.
// This prevents other-material lighting from entering the horizontal pass without
// falling back to point sampling. The nearest-texel decision is derived from
// the same four checks, avoiding a fifth visibility/depth lookup per tap.
#if SPATCH_SSS_HORIZONTAL
bool SampleVisibleSourceBilinear(
    float2 uv, float4 fallbackValue, out float4 sampleValue, out int2 samplePixel)
{
    const int2 size = int2(round(1.0 / InvResolution));
    const float2 texelPosition = saturate(uv) * float2(size) - 0.5;
    const int2 basePixel = int2(floor(texelPosition));
    const float2 fraction = frac(texelPosition);
    const int2 maxPixel = size - 1;

    const int2 pixel00 = clamp(basePixel, int2(0, 0), maxPixel);
    const int2 pixel10 = clamp(basePixel + int2(1, 0), int2(0, 0), maxPixel);
    const int2 pixel01 = clamp(basePixel + int2(0, 1), int2(0, 0), maxPixel);
    const int2 pixel11 = clamp(basePixel + int2(1, 1), int2(0, 0), maxPixel);
    const float4 weights = float4(
        (1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y,
        fraction.x * fraction.y);

    const bool visible00 = weights.x > 0.0 && IsVisibleMaterialPixel(pixel00);
    const bool visible10 = weights.y > 0.0 && IsVisibleMaterialPixel(pixel10);
    const bool visible01 = weights.z > 0.0 && IsVisibleMaterialPixel(pixel01);
    const bool visible11 = weights.w > 0.0 && IsVisibleMaterialPixel(pixel11);
    const bool nearestVisible = fraction.y >= 0.5
        ? (fraction.x >= 0.5 ? visible11 : visible01)
        : (fraction.x >= 0.5 ? visible10 : visible00);
    samplePixel = PixelFromUv(uv);
    if (!nearestVisible)
    {
        sampleValue = fallbackValue;
        return false;
    }

    float4 value = 0.0;
    float totalWeight = 0.0;
    if (visible00)
    {
        value += weights.x * SourceTexture.Load(int3(pixel00, 0));
        totalWeight += weights.x;
    }
    if (visible10)
    {
        value += weights.y * SourceTexture.Load(int3(pixel10, 0));
        totalWeight += weights.y;
    }
    if (visible01)
    {
        value += weights.z * SourceTexture.Load(int3(pixel01, 0));
        totalWeight += weights.z;
    }
    if (visible11)
    {
        value += weights.w * SourceTexture.Load(int3(pixel11, 0));
        totalWeight += weights.w;
    }

    sampleValue = totalWeight > 0.000001
        ? value / totalWeight
        : fallbackValue;
    return true;
}
#endif

float SpecularFraction(float4 lighting)
{
    // Stock deferred-light shaders store four times the RGB specular sum in A.
    const float total = dot(max(lighting.rgb, 0.0), 1.0 / 3.0);
    const float specular = max(lighting.a, 0.0) * (1.0 / 12.0) * SpecularScale;
    return saturate(specular / max(total, 0.0001));
}

float3 DiffuseOnly(float4 lighting)
{
    return lighting.rgb * (1.0 - SpecularFraction(lighting));
}

float EyeWeightAt(int2 pixel)
{
    return saturate(MaterialDataTexture.Load(int3(pixel, 0)).r);
}

float2 ProfileDirectionAt(int2 pixel)
{
    if (MaterialProfile != HairProfile)
        return Direction;

    const float4 materialData = MaterialDataTexture.Load(int3(pixel, 0));
    const float2 decodedFibre = materialData.rg * 2.0 - 1.0;
    const float fibreLengthSquared = dot(decodedFibre, decodedFibre);
    if (materialData.b <= 0.5 || fibreLengthSquared <= 0.00000001)
        return Direction;

    const float2 fibreDirection = decodedFibre * rsqrt(fibreLengthSquared);
    const float anisotropy = max(ProfileAnisotropy, 1.0);

#if SPATCH_SSS_HORIZONTAL
    // First distribute light along the captured strand direction.
    return fibreDirection * anisotropy;
#else
    // Then use the energy-preserving minor axis perpendicular to the strand.
    return float2(-fibreDirection.y, fibreDirection.x) / anisotropy;
#endif
}

float4 FoliageTransmissionPS(FullscreenVertex input) : SV_Target
{
    const float2 uv = saturate(input.Texcoord);
    const int2 centerPixel = PixelFromUv(uv);
    const float4 sourceCenter = SourceTexture.Load(int3(centerPixel, 0));
    if (MaterialProfile != FoliageProfile ||
        !IsVisibleMaterialPixel(centerPixel))
        return sourceCenter;

    const float4 materialData =
        MaterialDataTexture.Load(int3(centerPixel, 0));
#if SPATCH_SSS_DEVELOPMENT
    if (DebugView > 3.5)
        return float4(
            materialData.rgb / (1.0 + materialData.rgb), sourceCenter.a);
#endif
    const float3 incidentRadiance = max(materialData.rgb, 0.0);
    const float opticalThickness = 1.5 * saturate(materialData.a);
    const float3 absorption = float3(1.35, 0.32, 1.05);
    const float3 beerLambert = exp(-absorption * opticalThickness);
    const float scatterProbability =
        1.0 - exp(-1.25 * opticalThickness);
    const float3 transmittedDiffuse = incidentRadiance * beerLambert;
    const float scatterWeight = saturate(
        scatterProbability * saturate(Strength));

    // Redistribute the native diffuse response instead of adding a second sun
    // response to an already-lit HDR buffer. This bounds the foliage pass to a
    // convex diffuse blend while preserving the exact native specular remainder
    // and alpha. No tone-map or HDR-range clamp is applied here.
    const float3 nativeDiffuse = DiffuseOnly(sourceCenter);
    const float3 nativeSpecular = sourceCenter.rgb - nativeDiffuse;
    const float3 scatteredDiffuse = lerp(
        nativeDiffuse, transmittedDiffuse, scatterWeight);
    return float4(scatteredDiffuse + nativeSpecular, sourceCenter.a);
}

float4 BlurPS(FullscreenVertex input) : SV_Target
{
    const float2 uv = saturate(input.Texcoord);
    const int2 centerPixel = PixelFromUv(uv);
    const float4 sourceCenter = SourceTexture.Load(int3(centerPixel, 0));
    const float4 originalCenter = OriginalTexture.Load(int3(centerPixel, 0));

#if SPATCH_SSS_DEVELOPMENT && !SPATCH_SSS_HORIZONTAL
    // The stencil test already limits this pass to the replayed profile. Keep
    // development mask visualization independent of the later occlusion check
    // so raw coverage and final-depth rejection can be diagnosed separately.
    if (DebugView > 0.5 && DebugView < 1.5)
        return float4(8.0, 0.0, 8.0, originalCenter.a);
    if (DebugView > 3.5)
    {
        const float4 materialData =
            MaterialDataTexture.Load(int3(centerPixel, 0));
        if (MaterialProfile == EyeProfile)
            return float4(materialData.rrr * 8.0, originalCenter.a);
        if (MaterialProfile == HairProfile)
            return float4(materialData.rg * 8.0, materialData.b * 8.0,
                          originalCenter.a);
        if (MaterialProfile == FoliageProfile)
            return float4(materialData.rgb * 8.0, originalCenter.a);
        return float4(0.0, 0.0, 0.0, originalCenter.a);
    }
    if (DebugView > 2.5)
    {
        // Green proves the shader-visible X24_G8 SRV observes the same exact
        // profile reference as the hardware stencil test for this draw.
        return MaterialStencilAt(centerPixel) == MaterialProfile
            ? float4(0.0, 8.0, 0.0, originalCenter.a)
            : float4(8.0, 0.0, 0.0, originalCenter.a);
    }
    if (DebugView > 1.5)
    {
        const float sceneDepth = SceneDepthTexture.Load(int3(centerPixel, 0));
        const float skinDepth = SkinDepthTexture.Load(int3(centerPixel, 0));
        const float difference = sceneDepth - skinDepth;
        const float tolerance = 8.0 / 16777215.0;
        if (abs(difference) <= tolerance)
            return float4(0.0, 8.0, 8.0, originalCenter.a);
        return difference < 0.0
            ? float4(8.0, 0.0, 0.0, originalCenter.a)
            : float4(0.0, 0.0, 8.0, originalCenter.a);
    }
#endif

    if (!IsVisibleMaterial(uv))
#if SPATCH_SSS_HORIZONTAL
        // The intermediate is cleared to zero. Preserve zero coverage for a
        // replayed profile that is occluded in the final scene depth.
        return 0.0;
#else
        return originalCenter;
#endif

    const float centerDepth = LinearizeDepth(
        SceneDepthTexture.Load(int3(centerPixel, 0)));
    const float2 projectionScale = 1.0 / max(abs(ViewScaleAndNearFar.xy), 0.0001);
    const float2 finalStep =
        Radius * projectionScale * ProfileDirectionAt(centerPixel) /
        max(centerDepth, 0.001) / 3.0;

#if SPATCH_SSS_HORIZONTAL
    float3 center = DiffuseOnly(sourceCenter);
#else
    float3 center = sourceCenter.rgb;
#endif
    float3 blurred = center * Kernel[0].rgb;
    float3 accumulatedKernelWeight = Kernel[0].rgb;

    [unroll]
    for (uint index = 1; index < KernelSize; ++index)
    {
        const float2 sampleUv = saturate(uv + Kernel[index].a * finalStep);
        float3 sampleColor = center;
#if SPATCH_SSS_HORIZONTAL
        float4 sampleValue;
        int2 samplePixel;
        if (SampleVisibleSourceBilinear(
                sampleUv, sourceCenter, sampleValue, samplePixel))
        {
            sampleColor = DiffuseOnly(sampleValue);
            const float sampleDepth = LinearizeDepth(
                SceneDepthTexture.Load(int3(samplePixel, 0)));
            const float edge = saturate(
                300.0 * projectionScale.y * Radius * abs(centerDepth - sampleDepth));
            sampleColor = lerp(sampleColor, center, edge);
            if (MaterialProfile == EyeProfile)
                sampleColor = lerp(center, sampleColor, EyeWeightAt(samplePixel));
        }
#else
        if (IsVisibleMaterial(sampleUv))
        {
            // Horizontal RGB is premultiplied by binary eligibility coverage
            // stored in A. Hardware bilinear filtering plus this division is
            // the exact normalized footprint and never consumes cleared or
            // stale texels outside the visible-skin mask.
            const float4 sampleValue =
                SourceTexture.SampleLevel(LinearClampSampler, sampleUv, 0.0);
            sampleColor = sampleValue.a > 0.000001
                ? sampleValue.rgb / sampleValue.a
                : center;
            const int2 samplePixel = PixelFromUv(sampleUv);
            const float sampleDepth = LinearizeDepth(
                SceneDepthTexture.Load(int3(samplePixel, 0)));
            const float edge = saturate(
                300.0 * projectionScale.y * Radius * abs(centerDepth - sampleDepth));
            sampleColor = lerp(sampleColor, center, edge);
            if (MaterialProfile == EyeProfile)
                sampleColor = lerp(center, sampleColor, EyeWeightAt(samplePixel));
        }
#endif
        blurred += Kernel[index].rgb * sampleColor;
        accumulatedKernelWeight += Kernel[index].rgb;
    }

    // The Jimenez kernels are already normalized for skin, eyes, and teeth.
    // Make the hair redistribution explicitly normalized to eliminate even
    // their tiny decimal-table roundoff without changing the skin path.
    if (MaterialProfile == HairProfile)
        blurred /= max(accumulatedKernelWeight, 0.000001);

#if SPATCH_SSS_HORIZONTAL
    // Alpha is free after specular removal, so use it as binary eligibility
    // coverage for edge-safe bilinear reconstruction in the vertical pass.
    return float4(blurred, 1.0);
#else
    // Only diffuse energy is redistributed. Adding the exact original
    // specular remainder before the strength lerp preserves that baseline.
    const float3 specular = originalCenter.rgb - DiffuseOnly(originalCenter);
    const float3 scattered = max(blurred + specular, 0.0);
    const float profileStrength = MaterialProfile == EyeProfile
        ? Strength * EyeWeightAt(centerPixel)
        : Strength;
    return float4(
        lerp(originalCenter.rgb, scattered, profileStrength),
        originalCenter.a);
#endif
}
