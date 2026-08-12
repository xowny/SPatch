// SPatch screen-space indirect diffuse lighting.
// Clean-room implementation of the method described by Therrien, Levesque,
// and Gilet, "Screen Space Indirect Lighting with Visibility Bitmask" (2023),
// and the authors' implementation notes at cdrinmatane.github.io/posts/ssaovb-code/.
// Depth reconstruction and conservative sampling use independently written
// equivalents of concepts also used by Intel's MIT-licensed XeGTAO.

#include "SPatchGIShared.hlsli"

#ifndef SPATCH_GI_QUALITY
#define SPATCH_GI_QUALITY 2
#endif

#ifndef SPATCH_GI_HALF_RES
#define SPATCH_GI_HALF_RES 1
#endif

#ifndef SPATCH_GI_FILTER_HORIZONTAL
#define SPATCH_GI_FILTER_HORIZONTAL 0
#endif

#if SPATCH_GI_QUALITY == 0
#define SPATCH_GI_SLICE_COUNT 2
#define SPATCH_GI_STEP_COUNT 6
#define SPATCH_GI_FILTER_RADIUS 1
#elif SPATCH_GI_QUALITY == 1
#define SPATCH_GI_SLICE_COUNT 3
#define SPATCH_GI_STEP_COUNT 8
#define SPATCH_GI_FILTER_RADIUS 2
#elif SPATCH_GI_QUALITY == 2
#define SPATCH_GI_SLICE_COUNT 4
#define SPATCH_GI_STEP_COUNT 8
#define SPATCH_GI_FILTER_RADIUS 2
#elif SPATCH_GI_QUALITY == 3
#define SPATCH_GI_SLICE_COUNT 6
#define SPATCH_GI_STEP_COUNT 8
#define SPATCH_GI_FILTER_RADIUS 3
#elif SPATCH_GI_QUALITY == 4
#define SPATCH_GI_SLICE_COUNT 8
#define SPATCH_GI_STEP_COUNT 10
#define SPATCH_GI_FILTER_RADIUS 3
#else
#error SPATCH_GI_QUALITY must be 0, 1, 2, 3, or 4.
#endif

static const float SPATCH_PI = 3.14159265358979323846;
static const float SPATCH_HALF_PI = 1.57079632679489661923;
static const float SPATCH_FAR_DEPTH = 65504.0;
static const float SPATCH_MAX_SOURCE_LUMINANCE = 16.0;
static const float SPATCH_MAX_GI_LUMINANCE = 7.0;
static const uint SPATCH_SECTOR_COUNT = 32u;
// The reference SSRT3 implementation limits hierarchical tracing to mip 4.
// Coarser cells no longer represent a useful hit position for a short GI ray.
static const uint SPATCH_MAX_TRACE_MIP = SPATCH_GI_MAX_TRACE_MIP;

// Captured from the native AO pass. The final-composition matrices are not the
// camera projection, so view Z must be reconstructed as B / (A - deviceDepth).
cbuffer SleepingDogsAOProjection : register(b9)
{
    float4 GameValue0;
    float4 GameValue1;
    float4 GameValue2;
    float4 GameValue3;
};

// Exact byte layout: three 16-byte registers, 48 bytes total.
cbuffer SPatchGI : register(b11)
{
    float2 FullResolution;
    float2 GIResolution;

    float GIRadius;
    float GIStrength;
    float Thickness;
    float DepthFadeStart;
    float DepthFadeEnd;
    uint3 _Padding;
};

SamplerState PointClampSampler : register(s0);
SamplerState LinearClampSampler : register(s1);

// Each entry point has a different semantic contract for these slots. Using a
// common float4 SRV declaration keeps the register ABI stable; scalar R32F
// depth resources are read from X. FXC removes unused resources per entry.
Texture2D<float4> Input0 : register(t0);
Texture2D<float4> Input1 : register(t1);
Texture2D<float4> Input2 : register(t2);
Texture2D<float4> Input3 : register(t3);
Texture2D<float4> Input4 : register(t4);

RWTexture2D<float> DepthOutput : register(u0);
RWTexture2D<float2> NormalOutput : register(u0);
RWTexture2D<float4> ColorOutput : register(u0);
RWTexture2D<float4> AuxiliaryOutput : register(u1);

bool IsFiniteScalar(float value)
{
    return value == value && abs(value) <= SPATCH_FAR_DEPTH;
}

float SanitizePositiveScalar(float value)
{
    return value == value ? clamp(value, 0.0, SPATCH_FAR_DEPTH) : 0.0;
}

float3 SanitizePositive(float3 value)
{
    return float3(
        SanitizePositiveScalar(value.x),
        SanitizePositiveScalar(value.y),
        SanitizePositiveScalar(value.z));
}

float3 ClampLuminance(float3 value, float maximumLuminance)
{
    const float3 positive = SanitizePositive(value);
    const float luminance = dot(positive, float3(0.2126, 0.7152, 0.0722));
    return positive * min(1.0, maximumLuminance / max(luminance, 1e-5));
}

// Activision GTAO approximation used by the SSRT3 reference implementation.
float FastAcos(float value)
{
    const float magnitude = saturate(abs(value));
    const float result =
        (-0.156583 * magnitude + SPATCH_HALF_PI) * sqrt(1.0 - magnitude);
    return value >= 0.0 ? result : SPATCH_PI - result;
}

float2 FastAcos(float2 value)
{
    const float2 magnitude = saturate(abs(value));
    const float2 result =
        (-0.156583 * magnitude + SPATCH_HALF_PI) * sqrt(1.0 - magnitude);
    return value >= 0.0 ? result : SPATCH_PI - result;
}

bool HasValidProjectionConstants()
{
    const float4 values = float4(
        GameValue1.x, GameValue2.x, abs(GameValue3.x), abs(GameValue3.y));
    return all(values == values) && all(values < 1e10) &&
        GameValue1.x > 1e-6 && GameValue2.x > 1e-6 &&
        all(abs(GameValue3.xy) > 1e-6) && all(abs(GameValue3.xy) < 10.0);
}

float LinearizeDeviceDepth(float deviceDepth)
{
    if (!HasValidProjectionConstants() || !IsFiniteScalar(deviceDepth) ||
        deviceDepth < 0.0 || deviceDepth >= 0.999999)
        return SPATCH_FAR_DEPTH;

    const float denominator = GameValue2.x - deviceDepth;
    if (denominator <= 1e-7)
        return SPATCH_FAR_DEPTH;

    const float viewDepth = GameValue1.x / denominator;
    return IsFiniteScalar(viewDepth) && viewDepth > 0.0
        ? min(viewDepth, SPATCH_FAR_DEPTH)
        : SPATCH_FAR_DEPTH;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return IsFiniteScalar(lengthSquared) && lengthSquared > 1e-12
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float3 ViewPosition(float2 uv, float viewDepth)
{
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc * max(abs(GameValue3.xy), 1e-5) * viewDepth, viewDepth);
}

float SpecularFraction(float4 lighting)
{
    // Stock deferred-light shaders store four times the RGB specular sum in A.
    const float totalMean = dot(SanitizePositive(lighting.rgb), 1.0 / 3.0);
    const float specularMean = SanitizePositiveScalar(lighting.a) * (1.0 / 12.0);
    return saturate(specularMean / max(totalMean, 1e-4));
}

float3 DiffuseOnlyRadiance(float4 lighting)
{
    const float3 positiveLighting = SanitizePositive(lighting.rgb);
    return positiveLighting * (1.0 - SpecularFraction(lighting));
}

float LoadLinearDepth(uint2 pixel)
{
    return LinearizeDeviceDepth(Input0.Load(int3(pixel, 0)).x);
}

[numthreads(8, 8, 1)]
void prepare_gbuffer_cs(uint2 pixel : SV_DispatchThreadID)
{
    uint outputWidth;
    uint outputHeight;
    DepthOutput.GetDimensions(outputWidth, outputHeight);
    if (any(pixel >= uint2(outputWidth, outputHeight)))
        return;

    uint fullWidth;
    uint fullHeight;
    Input0.GetDimensions(fullWidth, fullHeight);
    const uint2 fullSize = uint2(fullWidth, fullHeight);

#if SPATCH_GI_HALF_RES
    const uint2 footprintOrigin = pixel * 2u;
    const uint footprintExtent = 2u;
#else
    const uint2 footprintOrigin = pixel;
    const uint footprintExtent = 1u;
#endif

    float nearestDepth = SPATCH_FAR_DEPTH;
    uint2 nearestPixel = min(footprintOrigin, fullSize - 1u);
    bool foundSurface = false;

    [unroll]
    for (uint y = 0u; y < footprintExtent; ++y)
    {
        [unroll]
        for (uint x = 0u; x < footprintExtent; ++x)
        {
            const uint2 sourcePixel = footprintOrigin + uint2(x, y);
            if (any(sourcePixel >= fullSize))
                continue;

            const float depth = LoadLinearDepth(sourcePixel);
            if (depth < nearestDepth)
            {
                nearestDepth = depth;
                nearestPixel = sourcePixel;
                foundSurface = depth < SPATCH_FAR_DEPTH * 0.5;
            }
        }
    }

    if (!foundSurface)
    {
        DepthOutput[pixel] = SPATCH_FAR_DEPTH;
        AuxiliaryOutput[pixel] = 0.0;
        return;
    }

    // Do not mix foreground and background radiance at half-resolution edges.
    const float depthTolerance = max(0.05, nearestDepth * 0.01);
    float3 radianceSum = 0.0;
    float radianceCount = 0.0;
    [unroll]
    for (uint sampleY = 0u; sampleY < footprintExtent; ++sampleY)
    {
        [unroll]
        for (uint sampleX = 0u; sampleX < footprintExtent; ++sampleX)
        {
            const uint2 sourcePixel = footprintOrigin + uint2(sampleX, sampleY);
            if (any(sourcePixel >= fullSize))
                continue;

            const float depth = LoadLinearDepth(sourcePixel);
            const bool isNearest = all(sourcePixel == nearestPixel);
            if (depth < SPATCH_FAR_DEPTH * 0.5 &&
                (isNearest || abs(depth - nearestDepth) <= depthTolerance))
            {
                radianceSum += DiffuseOnlyRadiance(Input1.Load(int3(sourcePixel, 0)));
                radianceCount += 1.0;
            }
        }
    }

    DepthOutput[pixel] = nearestDepth;
    AuxiliaryOutput[pixel] = float4(
        radianceCount > 0.0 ? radianceSum / radianceCount : 0.0, 0.0);
}

[numthreads(8, 8, 1)]
void downsample_mip_cs(uint2 pixel : SV_DispatchThreadID)
{
    uint outputWidth;
    uint outputHeight;
    DepthOutput.GetDimensions(outputWidth, outputHeight);
    if (any(pixel >= uint2(outputWidth, outputHeight)))
        return;

    // The host binds SRVs restricted to the source mip. Mip zero is therefore
    // deliberately SRV-relative and never an absolute Load index.
    uint sourceWidth;
    uint sourceHeight;
    Input0.GetDimensions(sourceWidth, sourceHeight);
    const uint2 sourceSize = uint2(sourceWidth, sourceHeight);
    const uint2 footprintOrigin = pixel * 2u;

    float nearestDepth = SPATCH_FAR_DEPTH;
    uint2 nearestPixel = min(footprintOrigin, sourceSize - 1u);
    bool foundSurface = false;
    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 2u; ++x)
        {
            const uint2 sourcePixel = footprintOrigin + uint2(x, y);
            if (any(sourcePixel >= sourceSize))
                continue;
            const float depth = Input0.Load(int3(sourcePixel, 0)).x;
            if (IsFiniteScalar(depth) && depth > 0.0 && depth < nearestDepth)
            {
                nearestDepth = depth;
                nearestPixel = sourcePixel;
                foundSurface = depth < SPATCH_FAR_DEPTH * 0.5;
            }
        }
    }

    if (!foundSurface)
    {
        DepthOutput[pixel] = SPATCH_FAR_DEPTH;
        AuxiliaryOutput[pixel] = 0.0;
        return;
    }

    // Keep radiance tied to the selected min-depth surface. Exponentially
    // widening this tolerance mixed foreground depth with background lighting
    // at coarse mips and produced block-shaped leaks.
    const float depthTolerance =
        max(0.05, min(nearestDepth * 0.01, 0.25));
    float3 radianceSum = 0.0;
    float radianceCount = 0.0;
    [unroll]
    for (uint sampleY = 0u; sampleY < 2u; ++sampleY)
    {
        [unroll]
        for (uint sampleX = 0u; sampleX < 2u; ++sampleX)
        {
            const uint2 sourcePixel = footprintOrigin + uint2(sampleX, sampleY);
            if (any(sourcePixel >= sourceSize))
                continue;
            const float depth = Input0.Load(int3(sourcePixel, 0)).x;
            const bool isNearest = all(sourcePixel == nearestPixel);
            if (IsFiniteScalar(depth) && depth > 0.0 &&
                depth < SPATCH_FAR_DEPTH * 0.5 &&
                (isNearest || abs(depth - nearestDepth) <= depthTolerance))
            {
                radianceSum += SanitizePositive(Input1.Load(int3(sourcePixel, 0)).rgb);
                radianceCount += 1.0;
            }
        }
    }

    DepthOutput[pixel] = nearestDepth;
    AuxiliaryOutput[pixel] = float4(
        radianceCount > 0.0 ? radianceSum / radianceCount : 0.0, 0.0);
}

uint Hash32(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float HashToUnitFloat(uint value)
{
    return float(Hash32(value) & 0x00ffffffu) * (1.0 / 16777216.0);
}

float2 SpatialRotation(uint2 pixel)
{
    const uint seed = pixel.x * 0x9e3779b9u ^ pixel.y * 0x85ebca6bu;
    return float2(HashToUnitFloat(seed), HashToUnitFloat(seed ^ 0xc2b2ae35u));
}

uint LowBitMask(uint count)
{
    if (count == 0u)
        return 0u;
    if (count >= SPATCH_SECTOR_COUNT)
        return 0xffffffffu;
    return (1u << count) - 1u;
}

uint SectorMask(float minimumHorizon, float maximumHorizon)
{
    const float lower = saturate(min(minimumHorizon, maximumHorizon));
    const float upper = saturate(max(minimumHorizon, maximumHorizon));
    // Rounding the endpoints activates sectors covered by at least one half.
    const uint first = min((uint)floor(lower * 32.0 + 0.5), 32u);
    const uint end = min((uint)floor(upper * 32.0 + 0.5), 32u);
    if (end <= first || first >= 32u)
        return 0u;
    const uint count = min(end - first, 32u - first);
    return LowBitMask(count) << first; // first is at most 31 when count is nonzero.
}

uint CoverageMask(
    float3 centerPosition,
    float3 samplePosition,
    float3 viewDirection,
    float normalAngle,
    float samplingDirection)
{
    const float3 front = samplePosition - centerPosition;
    const float3 back = front - viewDirection * max(Thickness, 0.0);
    const float3 frontDirection = SafeNormalize(front, viewDirection);
    const float3 backDirection = SafeNormalize(back, frontDirection);
    float2 angles = FastAcos(clamp(float2(
        dot(frontDirection, viewDirection),
        dot(backDirection, viewDirection)), -1.0, 1.0));
    float2 horizons = saturate(
        (samplingDirection * -angles - normalAngle + SPATCH_HALF_PI) / SPATCH_PI);
    horizons = samplingDirection >= 0.0 ? horizons.yx : horizons.xy;
    return SectorMask(horizons.x, horizons.y);
}

uint2 MipDimensions(uint mipLevel, uint2 baseSize)
{
    return max(baseSize >> min(mipLevel, 31u), 1u);
}

uint SelectMip(float pixelDistance, uint mipCount)
{
    const float estimatedFootprint = max(
        pixelDistance / float(SPATCH_GI_STEP_COUNT), 1.0);
    const uint requestedMip = (uint)max(floor(log2(estimatedFootprint)), 0.0);
    return min(
        min(requestedMip, max(mipCount, 1u) - 1u),
        SPATCH_MAX_TRACE_MIP);
}

bool IsInsideScreen(float2 uv)
{
    return all(uv >= 0.0) && all(uv < 1.0);
}

bool LoadPyramidHit(
    float2 requestedUv,
    uint mipLevel,
    uint2 baseSize,
    out float depth,
    out float2 snappedUv)
{
    if (!IsInsideScreen(requestedUv))
    {
        depth = SPATCH_FAR_DEPTH;
        snappedUv = 0.0;
        return false;
    }

    const uint2 size = MipDimensions(mipLevel, baseSize);
    const uint2 samplePixel = min(
        (uint2)(requestedUv * float2(size)), size - 1u);
    snappedUv = (float2(samplePixel) + 0.5) / float2(size);
    depth = Input0.SampleLevel(PointClampSampler, snappedUv, float(mipLevel)).x;
    return IsFiniteScalar(depth) && depth > 0.0 &&
        depth < SPATCH_FAR_DEPTH * 0.5;
}

float3 ReconstructNormalAtMip(uint2 pixel, uint mipLevel, uint2 baseSize)
{
    const uint2 size = MipDimensions(mipLevel, baseSize);
    const uint2 leftPixel = uint2(pixel.x > 0u ? pixel.x - 1u : 0u, pixel.y);
    const uint2 rightPixel = uint2(min(pixel.x + 1u, size.x - 1u), pixel.y);
    const uint2 topPixel = uint2(pixel.x, pixel.y > 0u ? pixel.y - 1u : 0u);
    const uint2 bottomPixel = uint2(pixel.x, min(pixel.y + 1u, size.y - 1u));

    const float centerDepth = Input0.Load(int3(pixel, mipLevel)).x;
    const float leftDepth = Input0.Load(int3(leftPixel, mipLevel)).x;
    const float rightDepth = Input0.Load(int3(rightPixel, mipLevel)).x;
    const float topDepth = Input0.Load(int3(topPixel, mipLevel)).x;
    const float bottomDepth = Input0.Load(int3(bottomPixel, mipLevel)).x;
    const float2 inverseSize = rcp(float2(size));

    const float3 center = ViewPosition((float2(pixel) + 0.5) * inverseSize, centerDepth);
    const float3 left = ViewPosition((float2(leftPixel) + 0.5) * inverseSize, leftDepth);
    const float3 right = ViewPosition((float2(rightPixel) + 0.5) * inverseSize, rightDepth);
    const float3 top = ViewPosition((float2(topPixel) + 0.5) * inverseSize, topDepth);
    const float3 bottom = ViewPosition((float2(bottomPixel) + 0.5) * inverseSize, bottomDepth);

    float3 horizontal = abs(rightDepth - centerDepth) < abs(centerDepth - leftDepth)
        ? right - center
        : center - left;
    float3 vertical = abs(bottomDepth - centerDepth) < abs(centerDepth - topDepth)
        ? bottom - center
        : center - top;
    if (pixel.x == 0u)
        horizontal = right - center;
    if (pixel.x + 1u >= size.x)
        horizontal = center - left;
    if (pixel.y == 0u)
        vertical = bottom - center;
    if (pixel.y + 1u >= size.y)
        vertical = center - top;
    return SafeNormalize(cross(horizontal, vertical), float3(0.0, 0.0, -1.0));
}

float2 SignNotZero(float2 value)
{
    return float2(value.x >= 0.0 ? 1.0 : -1.0,
        value.y >= 0.0 ? 1.0 : -1.0);
}

float2 EncodeOctahedralNormal(float3 normal)
{
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-6);
    return normal.z >= 0.0
        ? normal.xy
        : (1.0 - abs(normal.yx)) * SignNotZero(normal.xy);
}

float3 DecodeOctahedralNormal(float2 encoded)
{
    float3 normal = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0.0)
        normal.xy = (1.0 - abs(normal.yx)) * SignNotZero(normal.xy);
    return SafeNormalize(normal, float3(0.0, 0.0, -1.0));
}

[numthreads(8, 8, 1)]
void prepare_normals_cs(uint2 pixel : SV_DispatchThreadID)
{
    uint width;
    uint height;
    NormalOutput.GetDimensions(width, height);
    const uint2 size = uint2(width, height);
    if (any(pixel >= size))
        return;

    const float depth = Input0.Load(int3(pixel, 0)).x;
    if (!IsFiniteScalar(depth) || depth <= 0.0 ||
        depth >= SPATCH_FAR_DEPTH * 0.5)
    {
        NormalOutput[pixel] = 0.0;
        return;
    }
    NormalOutput[pixel] = EncodeOctahedralNormal(
        ReconstructNormalAtMip(pixel, 0u, size));
}

void ProcessGIHit(
    float2 sampleUv,
    float pixelDistance,
    uint mipCount,
    uint2 baseSize,
    float samplingDirection,
    float3 centerPosition,
    float3 centerNormal,
    float3 viewDirection,
    float normalAngle,
    inout uint occlusionMask,
    inout float3 irradiance)
{
    const uint mipLevel = SelectMip(pixelDistance, mipCount);
    float sampleDepth;
    float2 snappedUv;
    if (!LoadPyramidHit(
        sampleUv, mipLevel, baseSize, sampleDepth, snappedUv))
        return;

    const float3 samplePosition = ViewPosition(snappedUv, sampleDepth);
    const float3 receiverToSample = samplePosition - centerPosition;
    const float distanceToSample = length(receiverToSample);
    if (!IsFiniteScalar(distanceToSample) || distanceToSample <= 1e-5 ||
        distanceToSample > max(GIRadius, 0.0))
        return;

    const uint sampleMask = CoverageMask(
        centerPosition, samplePosition, viewDirection, normalAngle, samplingDirection);
    const uint newlyVisibleMask = sampleMask & ~occlusionMask;
    occlusionMask |= sampleMask;
    const uint newlyVisibleSectors = countbits(newlyVisibleMask);
    if (newlyVisibleSectors == 0u)
        return;

    const float3 directionToSample = receiverToSample / distanceToSample;
    const float receiverCosine = saturate(dot(centerNormal, directionToSample));
    if (receiverCosine <= 0.0)
        return;

    const float3 sourceRadiance = ClampLuminance(
        Input1.SampleLevel(PointClampSampler, snappedUv, float(mipLevel)).rgb,
        SPATCH_MAX_SOURCE_LUMINANCE);
    if (max(sourceRadiance.r, max(sourceRadiance.g, sourceRadiance.b)) <= 1e-5)
        return;

    float3 sampleNormal = DecodeOctahedralNormal(
        Input2.SampleLevel(PointClampSampler, snappedUv, float(mipLevel)).xy);
    const float3 sampleViewDirection = SafeNormalize(
        -samplePosition, float3(0.0, 0.0, -1.0));
    if (dot(sampleNormal, sampleViewDirection) < 0.0)
        sampleNormal = -sampleNormal;
    const float emitterCosine = saturate(dot(sampleNormal, -directionToSample));
    if (emitterCosine <= 0.0)
        return;

    const float sectorWeight = float(newlyVisibleSectors) / 32.0;
    irradiance += sourceRadiance * receiverCosine * emitterCosine * sectorWeight;
}

float ComputeDepthFade(float depth)
{
    if (DepthFadeEnd <= DepthFadeStart + 1e-5)
        return 1.0;
    return 1.0 - smoothstep(DepthFadeStart, DepthFadeEnd, depth);
}

[numthreads(8, 8, 1)]
void visibility_gi_cs(uint2 pixel : SV_DispatchThreadID)
{
    uint width;
    uint height;
    ColorOutput.GetDimensions(width, height);
    const uint2 size = uint2(width, height);
    if (any(pixel >= size))
        return;

    uint pyramidWidth;
    uint pyramidHeight;
    uint mipCount;
    Input0.GetDimensions(0, pyramidWidth, pyramidHeight, mipCount);
    const uint2 pyramidSize = uint2(pyramidWidth, pyramidHeight);
    if (any(pixel >= pyramidSize))
    {
        ColorOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const float centerDepth = Input0.Load(int3(pixel, 0)).x;
    if (!HasValidProjectionConstants() || !IsFiniteScalar(centerDepth) ||
        centerDepth <= 0.0 || centerDepth >= SPATCH_FAR_DEPTH * 0.5)
    {
        ColorOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const float2 inverseSize = rcp(float2(size));
    const float2 centerUv = (float2(pixel) + 0.5) * inverseSize;
    float3 centerPosition = ViewPosition(centerUv, centerDepth);
    const float3 viewDirection = SafeNormalize(
        -centerPosition, float3(0.0, 0.0, -1.0));
    float3 centerNormal = DecodeOctahedralNormal(Input2.Load(int3(pixel, 0)).xy);
    if (dot(centerNormal, viewDirection) < 0.0)
        centerNormal = -centerNormal;
    centerPosition += centerNormal * max(centerDepth * 1e-5, 1e-5);

    const float2 projectionScale = rcp(max(abs(GameValue3.xy), 1e-5));
    const float2 giRadiusPixels =
        0.5 * max(GIRadius, 0.0) * projectionScale * float2(size) / centerDepth;
    const float2 spatialRotation = SpatialRotation(pixel);
    float3 irradianceSum = 0.0;

    [loop]
    for (uint slice = 0u; slice < SPATCH_GI_SLICE_COUNT; ++slice)
    {
        const float phi =
            (float(slice) + spatialRotation.x) * SPATCH_PI /
            float(SPATCH_GI_SLICE_COUNT);
        const float2 sliceDirection = float2(cos(phi), sin(phi));
        const float3 sliceDirectionView = float3(sliceDirection, 0.0);
        const float3 sliceNormal = SafeNormalize(
            cross(sliceDirectionView, viewDirection), float3(0.0, 1.0, 0.0));
        const float3 projectedNormal =
            centerNormal - sliceNormal * dot(centerNormal, sliceNormal);
        const float projectedNormalLength = length(projectedNormal);
        const float3 normalizedProjectedNormal = projectedNormal /
            max(projectedNormalLength, 1e-5);
        const float cosineNormal = clamp(
            dot(normalizedProjectedNormal, viewDirection), -1.0, 1.0);
        const float3 sliceTangent = cross(viewDirection, sliceNormal);
        const float orientation = dot(projectedNormal, sliceTangent) >= 0.0 ? 1.0 : -1.0;
        const float normalAngle = -orientation * acos(cosineNormal);

        uint giMask = 0u;
        float3 sliceIrradiance = 0.0;
        const float stepRotation = frac(
            spatialRotation.y + float(slice) * 0.61803398875);

        [loop]
        for (uint step = 0u; step < SPATCH_GI_STEP_COUNT; ++step)
        {
            const bool giComplete =
                GIRadius <= 0.0 || GIStrength <= 0.0 || giMask == 0xffffffffu;
            if (giComplete)
                break;

            const float stepPosition =
                (float(step) + 0.35 + 0.30 * stepRotation) /
                float(SPATCH_GI_STEP_COUNT);
            const float distributedStep = saturate(stepPosition * stepPosition);

            if (GIRadius > 0.0 && GIStrength > 0.0)
            {
                const float2 signedPixels = float2(
                    sliceDirection.x * giRadiusPixels.x,
                    -sliceDirection.y * giRadiusPixels.y) * distributedStep;
                const float pixelDistance = length(signedPixels);
                if (pixelDistance >= 0.75)
                {
                    const float2 offsetUv = signedPixels * inverseSize;
                    ProcessGIHit(centerUv + offsetUv, pixelDistance,
                        mipCount, pyramidSize, 1.0,
                        centerPosition, centerNormal, viewDirection, normalAngle,
                        giMask, sliceIrradiance);
                    ProcessGIHit(centerUv - offsetUv, pixelDistance,
                        mipCount, pyramidSize, -1.0,
                        centerPosition, centerNormal, viewDirection, normalAngle,
                        giMask, sliceIrradiance);
                }
            }
        }

        irradianceSum += sliceIrradiance;
    }

    const float inverseSliceCount = 1.0 / float(SPATCH_GI_SLICE_COUNT);
    const float fade = ComputeDepthFade(centerDepth);
    const float3 irradiance = ClampLuminance(
        irradianceSum * inverseSliceCount * fade,
        SPATCH_MAX_GI_LUMINANCE);
    ColorOutput[pixel] = float4(irradiance, 1.0);
}

float3 PositionAtPixel(uint2 pixel, uint2 size, float depth)
{
    return ViewPosition((float2(pixel) + 0.5) / float2(size), depth);
}

[numthreads(8, 8, 1)]
void spatial_filter_cs(uint2 pixel : SV_DispatchThreadID)
{
    uint width;
    uint height;
    ColorOutput.GetDimensions(width, height);
    const uint2 size = uint2(width, height);
    if (any(pixel >= size))
        return;

    const float centerDepth = Input1.Load(int3(pixel, 0)).x;
    if (!IsFiniteScalar(centerDepth) || centerDepth <= 0.0 ||
        centerDepth >= SPATCH_FAR_DEPTH * 0.5)
    {
        ColorOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // During this pass depth is t1, so reconstruct its geometric normal here
    // instead of using the t0-based pyramid helper.
    const uint2 leftPixel = uint2(pixel.x > 0u ? pixel.x - 1u : 0u, pixel.y);
    const uint2 rightPixel = uint2(min(pixel.x + 1u, size.x - 1u), pixel.y);
    const uint2 topPixel = uint2(pixel.x, pixel.y > 0u ? pixel.y - 1u : 0u);
    const uint2 bottomPixel = uint2(pixel.x, min(pixel.y + 1u, size.y - 1u));
    const float leftDepth = Input1.Load(int3(leftPixel, 0)).x;
    const float rightDepth = Input1.Load(int3(rightPixel, 0)).x;
    const float topDepth = Input1.Load(int3(topPixel, 0)).x;
    const float bottomDepth = Input1.Load(int3(bottomPixel, 0)).x;
    const float3 centerPosition = PositionAtPixel(pixel, size, centerDepth);
    const float3 leftPosition = PositionAtPixel(leftPixel, size, leftDepth);
    const float3 rightPosition = PositionAtPixel(rightPixel, size, rightDepth);
    const float3 topPosition = PositionAtPixel(topPixel, size, topDepth);
    const float3 bottomPosition = PositionAtPixel(bottomPixel, size, bottomDepth);
    float3 horizontal = abs(rightDepth - centerDepth) < abs(centerDepth - leftDepth)
        ? rightPosition - centerPosition
        : centerPosition - leftPosition;
    float3 vertical = abs(bottomDepth - centerDepth) < abs(centerDepth - topDepth)
        ? bottomPosition - centerPosition
        : centerPosition - topPosition;
    const float3 centerViewDirection = SafeNormalize(
        -centerPosition, float3(0.0, 0.0, -1.0));
    float3 centerNormal = SafeNormalize(
        cross(horizontal, vertical), float3(0.0, 0.0, -1.0));
    if (dot(centerNormal, centerViewDirection) < 0.0)
        centerNormal = -centerNormal;

    const float3 centerResult = Input0.Load(int3(pixel, 0)).rgb;
    float3 radianceSum = 0.0;
    float weightSum = 0.0;
    const float spatialSigma = max(float(SPATCH_GI_FILTER_RADIUS) * 0.65, 0.75);
    const float planeScale = 8.0 / max(1.0, centerDepth);

    [unroll]
    for (int tap = -SPATCH_GI_FILTER_RADIUS; tap <= SPATCH_GI_FILTER_RADIUS; ++tap)
    {
#if SPATCH_GI_FILTER_HORIZONTAL
        const int2 offset = int2(tap, 0);
#else
        const int2 offset = int2(0, tap);
#endif
        const uint2 samplePixel = (uint2)clamp(
            int2(pixel) + offset, int2(0, 0), int2(size) - 1);
        const float sampleDepth = Input1.Load(int3(samplePixel, 0)).x;
        if (!IsFiniteScalar(sampleDepth) || sampleDepth <= 0.0 ||
            sampleDepth >= SPATCH_FAR_DEPTH * 0.5)
            continue;

        const float3 samplePosition = PositionAtPixel(samplePixel, size, sampleDepth);
        const float relativeDepth =
            abs(sampleDepth - centerDepth) / max(centerDepth, 0.1);
        const float depthWeight = exp(-relativeDepth * relativeDepth / (2.0 * 0.012 * 0.012));
        const float planeDistance = abs(dot(
            samplePosition - centerPosition, centerNormal));
        const float planeWeight = exp(-planeDistance * planeDistance * planeScale * planeScale);
        const float spatialWeight = exp(
            -float(tap * tap) / (2.0 * spatialSigma * spatialSigma));
        const float preliminaryWeight = spatialWeight * depthWeight * planeWeight;
        if (preliminaryWeight <= 1e-5)
            continue;

        const uint2 sampleLeft = uint2(
            samplePixel.x > 0u ? samplePixel.x - 1u : 0u, samplePixel.y);
        const uint2 sampleRight = uint2(
            min(samplePixel.x + 1u, size.x - 1u), samplePixel.y);
        const uint2 sampleTop = uint2(
            samplePixel.x, samplePixel.y > 0u ? samplePixel.y - 1u : 0u);
        const uint2 sampleBottom = uint2(
            samplePixel.x, min(samplePixel.y + 1u, size.y - 1u));
        const float3 sampleDx = PositionAtPixel(sampleRight, size,
            Input1.Load(int3(sampleRight, 0)).x) - PositionAtPixel(sampleLeft, size,
            Input1.Load(int3(sampleLeft, 0)).x);
        const float3 sampleDy = PositionAtPixel(sampleBottom, size,
            Input1.Load(int3(sampleBottom, 0)).x) - PositionAtPixel(sampleTop, size,
            Input1.Load(int3(sampleTop, 0)).x);
        float3 sampleNormal = SafeNormalize(
            cross(sampleDx, sampleDy), centerNormal);
        if (dot(sampleNormal, centerViewDirection) < 0.0)
            sampleNormal = -sampleNormal;
        const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
        const float weight = preliminaryWeight * normalWeight;
        const float3 sampleResult = Input0.Load(int3(samplePixel, 0)).rgb;
        radianceSum += SanitizePositive(sampleResult) * weight;
        weightSum += weight;
    }

    ColorOutput[pixel] = weightSum > 1e-5
        ? float4(radianceSum / weightSum, 1.0)
        : float4(SanitizePositive(centerResult), 1.0);
}

struct FullscreenVertex
{
    float4 Position : SV_Position;
    float2 Texcoord : Texcoord0;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(
        corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.Texcoord = corner;
    return output;
}

float3 LoadHalfResolutionGI(
    uint2 fullPixel,
    float fullDepth,
    uint2 fullSize)
{
    uint giWidth;
    uint giHeight;
    Input0.GetDimensions(giWidth, giHeight);
    const uint2 giSize = uint2(giWidth, giHeight);
    const float2 giPosition =
        (float2(fullPixel) + 0.5) * float2(giSize) / float2(fullSize) - 0.5;
    const int2 basePixel = int2(floor(giPosition));
    const float2 fraction = frac(giPosition);
    float3 radianceSum = 0.0;
    float weightSum = 0.0;
    float bestDifference = SPATCH_FAR_DEPTH;
    float3 bestResult = 0.0;

    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 2u; ++x)
        {
            const uint2 samplePixel = (uint2)clamp(
                basePixel + int2(x, y), int2(0, 0), int2(giSize) - 1);
            const float sampleDepth = Input1.Load(int3(samplePixel, 0)).x;
            const float3 sampleResult = Input0.Load(int3(samplePixel, 0)).rgb;
            if (!IsFiniteScalar(sampleDepth) || sampleDepth <= 0.0 ||
                sampleDepth >= SPATCH_FAR_DEPTH * 0.5)
                continue;

            const float depthDifference = abs(sampleDepth - fullDepth);
            if (depthDifference < bestDifference)
            {
                bestDifference = depthDifference;
                bestResult = sampleResult;
            }

            const float2 bilinearAxis = float2(
                x == 0u ? 1.0 - fraction.x : fraction.x,
                y == 0u ? 1.0 - fraction.y : fraction.y);
            const float bilinearWeight = bilinearAxis.x * bilinearAxis.y;
            const float relativeDepth = depthDifference / max(fullDepth, 0.1);
            const float depthWeight = exp(
                -relativeDepth * relativeDepth / (2.0 * 0.015 * 0.015));
            const float weight = bilinearWeight * depthWeight;
            radianceSum += SanitizePositive(sampleResult) * weight;
            weightSum += weight;
        }
    }

    if (weightSum > 1e-5)
    {
        return radianceSum / weightSum;
    }

    // The half-resolution footprint stores its nearest surface. Only reuse its
    // nearest result when it still belongs to this full-resolution surface;
    // otherwise a thin foreground object would donate GI to the background.
    const float bestRelativeDifference =
        bestDifference / max(fullDepth, 0.1);
    const bool matchedSurface = bestDifference < SPATCH_FAR_DEPTH * 0.5 &&
        bestRelativeDifference <= 0.03;
    return matchedSurface ? SanitizePositive(bestResult) : 0.0;
}

float4 CompositePS(FullscreenVertex input) : SV_Target0
{
    uint fullWidth;
    uint fullHeight;
    Input2.GetDimensions(fullWidth, fullHeight);
    const uint2 fullSize = uint2(fullWidth, fullHeight);
    const uint2 fullPixel = min((uint2)input.Position.xy, fullSize - 1u);
    const float fullDepth = LinearizeDeviceDepth(Input2.Load(int3(fullPixel, 0)).x);
    float3 gi = 0.0;

    if (fullDepth < SPATCH_FAR_DEPTH * 0.5)
    {
#if SPATCH_GI_HALF_RES
        gi = LoadHalfResolutionGI(fullPixel, fullDepth, fullSize);
#else
        gi = Input0.Load(int3(fullPixel, 0)).rgb;
#endif
    }

    const float3 linearAlbedo = pow(
        saturate(Input3.Load(int3(fullPixel, 0)).rgb), 2.0);
    const float3 indirectDiffuse =
        linearAlbedo * SanitizePositive(gi) * max(GIStrength, 0.0);
    // The host binds the native HDR lighting texture as an RGB-only additive
    // render target. Returning only the indirect term avoids a second full-size
    // HDR target and its copy-back while preserving the native alpha channel.
    return float4(SanitizePositive(indirectDiffuse), 0.0);
}
