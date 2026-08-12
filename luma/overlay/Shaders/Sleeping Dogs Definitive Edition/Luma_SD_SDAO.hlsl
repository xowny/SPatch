// Full-resolution Stochastic-Depth Ambient Occlusion for Sleeping Dogs:
// Definitive Edition. The stochastic-depth acquisition and AO extension follow
// Vermeer, Scandolo, and Eisemann, "Stochastic-Depth Ambient Occlusion" (2021).
// The conventional horizon estimator is based on DiligentFX
// ScreenSpaceAmbientOcclusion (Apache-2.0) and XeGTAO (MIT).
// Modified for SPatch's D3D11 compute path and Sleeping Dogs resource layout.
// Native host: ShenLong.asi via the ReShade API.

#ifndef SD_SDAO_QUALITY
#define SD_SDAO_QUALITY 2
#endif

#ifndef SD_SDAO_COLOR_OUTPUT
#define SD_SDAO_COLOR_OUTPUT 0
#endif

#ifndef SD_SDAO_FILTER_HORIZONTAL
#define SD_SDAO_FILTER_HORIZONTAL 0
#endif

#ifndef SD_GTAO_LITE
#define SD_GTAO_LITE 0
#endif

#if SD_SDAO_QUALITY == 0
#define SDAO_SLICE_COUNT 2
#define SDAO_SAMPLES_PER_SLICE 2
#define SDAO_FILTER_RADIUS 1
#define SDAO_STOCHASTIC_LAYER_COUNT 1
#define SDAO_STOCHASTIC_SELECTION_CUTOFF 858993459u
#elif SD_SDAO_QUALITY == 1
#define SDAO_SLICE_COUNT 2
#define SDAO_SAMPLES_PER_SLICE 3
#define SDAO_FILTER_RADIUS 1
#define SDAO_STOCHASTIC_LAYER_COUNT 2
#define SDAO_STOCHASTIC_SELECTION_CUTOFF 1717986918u
#elif SD_SDAO_QUALITY == 2
#define SDAO_SLICE_COUNT 3
#define SDAO_SAMPLES_PER_SLICE 3
#define SDAO_FILTER_RADIUS 2
#define SDAO_STOCHASTIC_LAYER_COUNT 2
#define SDAO_STOCHASTIC_SELECTION_CUTOFF 1717986918u
#elif SD_SDAO_QUALITY == 3
#define SDAO_SLICE_COUNT 3
#define SDAO_SAMPLES_PER_SLICE 4
#define SDAO_FILTER_RADIUS 2
#define SDAO_STOCHASTIC_LAYER_COUNT 4
#define SDAO_STOCHASTIC_SELECTION_CUTOFF 3435973836u
#else
#define SDAO_SLICE_COUNT 4
#define SDAO_SAMPLES_PER_SLICE 4
#define SDAO_FILTER_RADIUS 2
#define SDAO_STOCHASTIC_LAYER_COUNT 4
#define SDAO_STOCHASTIC_SELECTION_CUTOFF 3435973836u
#endif

static const float SDAO_PI = 3.14159265358979323846;
static const float SDAO_HALF_PI = 1.57079632679489661923;
static const float SDAO_FAR_DEPTH = 65504.0;
static const float SDAO_MINIMUM_SAMPLE_DISTANCE_PIXELS = 1.3;
static const float SDAO_THIN_OCCLUDER_COMPENSATION = 1.0;
static const float SDAO_HIDDEN_RECEIVER_FADE_FRACTION = 0.35;
static const float SDAO_HORIZON_DECAY = 0.5;
static const uint SDAO_FAR_DEVICE_DEPTH_BITS = 0x3F7FFFEFu;
#if SDAO_SLICE_COUNT == 2
static const float SDAO_CHECKER_ROTATION_COS = 0.9238795325112867;
static const float SDAO_CHECKER_ROTATION_SIN = 0.3826834323650898;
#elif SDAO_SLICE_COUNT == 3
static const float SDAO_CHECKER_ROTATION_COS = 0.9659258262890683;
static const float SDAO_CHECKER_ROTATION_SIN = 0.2588190451025207;
#else
static const float SDAO_CHECKER_ROTATION_COS = 0.9807852804032304;
static const float SDAO_CHECKER_ROTATION_SIN = 0.1950903220161283;
#endif

// Native AO projection constants copied at the game's final AO dispatch. The
// final composition pass binds identity matrices, so its cbViewTransform is
// not a camera projection and must never be used for depth reconstruction.
cbuffer SleepingDogsAOProjection : register(b9)
{
   float4 GameValue0;
   float4 GameValue1;
   float4 GameValue2;
   float4 GameValue3;
}

cbuffer SPatchSDAO : register(b11)
{
   float SDAORadius;
   float SDAOStrength;
   float SDAOFalloffRange;
   float SDAORadiusMultiplier;
}

SamplerState PointClampSampler : register(s0);
Texture2D<float4> Input0 : register(t0);
Texture2D<float4> Input1 : register(t1);
Texture2DArray<uint2> StochasticDepth : register(t2);

#if SD_SDAO_COLOR_OUTPUT
RWTexture2D<unorm float4> Output0 : register(u0);
#else
RWTexture2D<float> Output0 : register(u0);
#endif

uint SdaoHash(uint value)
{
   // Integer finalizer with full-period input coverage. The donor intentionally
   // hashes only pixel position and device depth, matching the paper while
   // keeping a stable subset whenever those inputs are stable.
   value ^= value >> 16;
   value *= 0x7FEB352Du;
   value ^= value >> 15;
   value *= 0x846CA68Bu;
   value ^= value >> 16;
   return value;
}

struct StochasticCaptureOutput
{
   float2 Layer01 : SV_Target0;
   float2 Layer23 : SV_Target1;
};

StochasticCaptureOutput capture_depth_ps(float4 position : SV_Position)
{
   const uint2 pixel = uint2(position.xy);
   const float deviceDepth = saturate(position.z);
   const uint deviceDepthBits = asuint(deviceDepth);
   const uint selection = SdaoHash(
      pixel.x ^ SdaoHash(pixel.y ^ SdaoHash(deviceDepthBits)));
   const uint layer = SdaoHash(selection ^ 0x9E3779B9u) %
      SDAO_STOCHASTIC_LAYER_COUNT;

   // The capture-only derivative has already executed the native alpha discard.
   // Reject non-selected fragments, then let fixed-function MIN blending retain
   // the nearest selected device depth in each stochastic layer. Packing two
   // layers per RG32 target supports all one-to-four-layer quality levels.
   if (selection >= SDAO_STOCHASTIC_SELECTION_CUTOFF)
      discard;

   StochasticCaptureOutput output;
   output.Layer01 = float2(1.0, 1.0);
   output.Layer23 = float2(1.0, 1.0);
   if (layer == 0u)
      output.Layer01.x = deviceDepth;
   else if (layer == 1u)
      output.Layer01.y = deviceDepth;
   else if (layer == 2u)
      output.Layer23.x = deviceDepth;
   else
      output.Layer23.y = deviceDepth;
   return output;
}

bool HasValidProjectionConstants()
{
   const float4 values = float4(
      GameValue1.x, GameValue2.x, abs(GameValue3.x), abs(GameValue3.y));
   return all(values == values) &&
      all(values < 1e10) &&
      GameValue1.x > 1e-6 &&
      GameValue2.x > 1e-6 &&
      all(abs(GameValue3.xy) > 1e-6) &&
      all(abs(GameValue3.xy) < 10.0);
}

float LinearizeDeviceDepth(float deviceDepth)
{
   // The native AO pass supplies the exact D3D depth coefficients: viewZ is
   // B / (A - deviceDepth). Captured live values are approximately
   // B=0.33002594 and A=1.00007856 for the standard gameplay camera.
   const float denominator = max(GameValue2.x - deviceDepth, 1e-7);
   return max(GameValue1.x / denominator, 1e-5);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
   const float lengthSquared = dot(value, value);
   return lengthSquared > 1e-12 ? value * rsqrt(lengthSquared) : fallback;
}

float3 ViewPosition(float2 uv, float viewDepth)
{
   const float2 tanHalfFov = max(abs(GameValue3.xy), 1e-5);
   const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
   return float3(ndc * tanHalfFov * viewDepth, viewDepth);
}

float3 ReconstructViewNormal(Texture2D<float4> depthTexture, uint2 pixel)
{
   uint width;
   uint height;
   depthTexture.GetDimensions(width, height);
   const uint2 size = uint2(width, height);
   const uint2 leftPixel = uint2(pixel.x > 0 ? pixel.x - 1 : 0, pixel.y);
   const uint2 rightPixel = uint2(min(pixel.x + 1, width - 1), pixel.y);
   const uint2 topPixel = uint2(pixel.x, pixel.y > 0 ? pixel.y - 1 : 0);
   const uint2 bottomPixel = uint2(pixel.x, min(pixel.y + 1, height - 1));
   const float centerDepth = depthTexture.Load(int3(pixel, 0)).x;
   const float leftDepth = depthTexture.Load(int3(leftPixel, 0)).x;
   const float rightDepth = depthTexture.Load(int3(rightPixel, 0)).x;
   const float topDepth = depthTexture.Load(int3(topPixel, 0)).x;
   const float bottomDepth = depthTexture.Load(int3(bottomPixel, 0)).x;
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
   if (pixel.x == 0)
      horizontal = right - center;
   if (pixel.x + 1 >= width)
      horizontal = center - left;
   if (pixel.y == 0)
      vertical = bottom - center;
   if (pixel.y + 1 >= height)
      vertical = center - top;
   return SafeNormalize(cross(horizontal, vertical), float3(0.0, 0.0, -1.0));
}

float FastACos(float value)
{
   const float absoluteValue = abs(value);
   float result = -0.156583 * absoluteValue + SDAO_HALF_PI;
   result *= sqrt(saturate(1.0 - absoluteValue));
   return value >= 0.0 ? result : SDAO_PI - result;
}

float IntegrateCosineWeightedArc(
   float positiveHorizon, float negativeHorizon, float normalAngle, float cosNormal)
{
   const float h1 = positiveHorizon * 2.0;
   const float h2 = negativeHorizon * 2.0;
   const float sinNormal = sin(normalAngle);
   return 0.25 * (
      (-cos(h1 - normalAngle) + cosNormal + h1 * sinNormal) +
      (-cos(h2 - normalAngle) + cosNormal + h2 * sinNormal));
}

float2 ComputeSliceDirection(uint sliceIndex, uint checkerPhase)
{
   // The fixed midpoint direction remains compile-time foldable. Alternating a
   // precomputed quarter-stratum rotation breaks screen-wide rays without the
   // runtime trigonometry and cache scattering of per-pixel random directions.
   const float phi =
      (float(sliceIndex) + 0.5) * SDAO_PI / float(SDAO_SLICE_COUNT);
   const float2 baseDirection = float2(cos(phi), sin(phi));
   const float rotationSign = checkerPhase != 0u ? 1.0 : -1.0;
   const float rotationSin = SDAO_CHECKER_ROTATION_SIN * rotationSign;
   return float2(
      baseDirection.x * SDAO_CHECKER_ROTATION_COS -
         baseDirection.y * rotationSin,
      baseDirection.x * rotationSin +
         baseDirection.y * SDAO_CHECKER_ROTATION_COS);
}

float EvaluateHorizonCandidate(
   float3 difference,
   float distance,
   float3 viewDirection,
   float minimumHorizon,
   float falloffMultiplier,
   float falloffAdd)
{
   const float safeDistance = max(distance, 1e-5);
   const float cosHorizon = dot(difference / safeDistance, viewDirection);
   // XeGTAO's active thin-occluder heuristic shortens the lifetime of samples
   // separated in view depth while leaving coplanar/contact geometry unchanged.
   // This is distinct from the disabled, step-order-dependent horizon decay.
   // Exact at both coplanar and pure-depth extremes, while avoiding a second
   // square root for every horizon candidate. The small conservative bias for
   // mixed offsets rejects detached foreground silhouettes rather than contact.
   const float compensatedDistance = safeDistance +
      abs(difference.z) * SDAO_THIN_OCCLUDER_COMPENSATION;
   const float weight = saturate(
      compensatedDistance * falloffMultiplier + falloffAdd);
   return lerp(minimumHorizon, cosHorizon, weight);
}

float SelectStepHorizon(
   int2 samplePixel,
   float2 sampleUv,
   float regularDepth,
   float3 centerPosition,
   float3 centerNormal,
   float3 viewDirection,
   float minimumHorizon,
   float effectRadius,
   float falloffMultiplier,
   float falloffAdd)
{
   const float3 regularPosition = ViewPosition(sampleUv, regularDepth);
   const float3 regularDifference = regularPosition - centerPosition;
   const float regularDistance = length(regularDifference);

   // Sampling-efficiency heuristic from the SDAO paper: the conventional
   // front-depth sample has the strongest unattenuated horizon when it is
   // already inside the AO radius, so hidden layers cannot materially improve
   // this step and t2 is not accessed.
   float strongestHorizon = minimumHorizon;
   [branch]
   if (regularDepth < SDAO_FAR_DEPTH * 0.5 && regularDistance < effectRadius)
   {
      strongestHorizon = EvaluateHorizonCandidate(
         regularDifference,
         regularDistance,
         viewDirection,
         minimumHorizon,
         falloffMultiplier,
         falloffAdd);
   }
#if !SD_GTAO_LITE
   else
   {
      [unroll]
      for (uint layer = 0; layer < SDAO_STOCHASTIC_LAYER_COUNT; ++layer)
      {
         const uint2 deviceDepthPair =
            StochasticDepth.Load(int4(samplePixel, int(layer >> 1), 0));
         const uint deviceDepthBits =
            (layer & 1u) == 0u ? deviceDepthPair.x : deviceDepthPair.y;
         if (deviceDepthBits >= SDAO_FAR_DEVICE_DEPTH_BITS)
            continue;

         const float deviceDepth = asfloat(deviceDepthBits);
         if (deviceDepth != deviceDepth)
            continue;
         const float sampleDepth = min(
            LinearizeDeviceDepth(deviceDepth), SDAO_FAR_DEPTH);
         const float3 samplePosition = ViewPosition(sampleUv, sampleDepth);
         const float3 difference = samplePosition - centerPosition;
         // Stochastic buckets are unordered fragment subsets. When the visible
         // front is outside the radius, a selected rear shell of that same
         // foreground object can otherwise fall inside it and project a second
         // silhouette onto a farther receiver. Fade only separation in front
         // of the receiver plane; coplanar and true contact geometry is kept.
         const float foregroundPlaneDistance =
            max(dot(difference, centerNormal), 0.0);
         const float hiddenFadeRange = max(
            effectRadius * SDAO_HIDDEN_RECEIVER_FADE_FRACTION, 1e-5);
         const float hiddenT = saturate(
            foregroundPlaneDistance / hiddenFadeRange);
         if (hiddenT >= 1.0)
            continue;

         const float distance = length(difference);
         if (distance >= effectRadius)
            continue;

         const float hiddenWeight =
            1.0 - hiddenT * hiddenT * (3.0 - 2.0 * hiddenT);
         const float hiddenHorizon = EvaluateHorizonCandidate(
            difference,
            distance,
            viewDirection,
            minimumHorizon,
            falloffMultiplier,
            falloffAdd);
         strongestHorizon = max(
            strongestHorizon,
            lerp(minimumHorizon, hiddenHorizon, hiddenWeight));
      }
   }
#endif
   return strongestHorizon;
}

[numthreads(8, 8, 1)]
void prepare_depth_cs(uint2 pixel : SV_DispatchThreadID)
{
   uint width;
   uint height;
   Input0.GetDimensions(width, height);
   if (any(pixel >= uint2(width, height)))
      return;

   const float deviceDepth = Input0.Load(int3(pixel, 0)).x;
   Output0[pixel] = !HasValidProjectionConstants() ||
      deviceDepth != deviceDepth || deviceDepth >= 0.999999
      ? SDAO_FAR_DEPTH
      : min(LinearizeDeviceDepth(deviceDepth), SDAO_FAR_DEPTH);
}

[numthreads(8, 8, 1)]
void main_pass_cs(uint2 pixel : SV_DispatchThreadID)
{
   uint width;
   uint height;
   Input0.GetDimensions(width, height);
   const uint2 size = uint2(width, height);
   if (any(pixel >= size))
      return;

   const float centerDepth = Input0.Load(int3(pixel, 0)).x;
   if (centerDepth >= SDAO_FAR_DEPTH * 0.5)
   {
      Output0[pixel] = 1.0;
      return;
   }

   const float2 inverseSize = rcp(float2(size));
   const float2 centerUv = (float2(pixel) + 0.5) * inverseSize;
   float3 centerPosition = ViewPosition(centerUv, centerDepth);
   const float3 viewDirection = SafeNormalize(-centerPosition, float3(0.0, 0.0, -1.0));
   float3 normal = ReconstructViewNormal(Input0, pixel);
   if (dot(normal, viewDirection) < 0.0)
      normal = -normal;
   centerPosition += normal * (1e-5 * centerDepth);

   const float effectRadius = SDAORadius * SDAORadiusMultiplier;
   const float falloffRange = max(SDAOFalloffRange * effectRadius, 1e-5);
   const float falloffFrom = effectRadius - falloffRange;
   const float falloffMultiplier = -1.0 / falloffRange;
   const float falloffAdd = falloffFrom / falloffRange + 1.0;
   const float2 projectionScale = rcp(max(abs(GameValue3.xy), 1e-5));
   const float2 sampleRadiusUv =
      0.5 * effectRadius * projectionScale / max(centerDepth, 1e-5);
   const uint checkerPhase = (pixel.x ^ pixel.y) & 1u;
   float visibility = 0.0;
   [unroll]
   for (uint sliceIndex = 0; sliceIndex < SDAO_SLICE_COUNT; ++sliceIndex)
   {
      const float2 omega = ComputeSliceDirection(sliceIndex, checkerPhase);
      const float3 sliceDirection = float3(omega, 0.0);
      const float3 orthogonalSliceDirection =
         sliceDirection - dot(sliceDirection, viewDirection) * viewDirection;
      const float3 axis = SafeNormalize(
         cross(sliceDirection, viewDirection), float3(0.0, 1.0, 0.0));
      const float3 projectedNormal = normal - axis * dot(normal, axis);
      const float projectedNormalLength = max(length(projectedNormal), 1e-5);
      const float cosNormal = saturate(
         dot(projectedNormal / projectedNormalLength, viewDirection));
      const float normalAngle = sign(dot(orthogonalSliceDirection, projectedNormal)) *
         FastACos(cosNormal);

      float2 minimumHorizons = float2(
         cos(normalAngle + SDAO_HALF_PI), cos(normalAngle - SDAO_HALF_PI));
      float2 maximumHorizons = minimumHorizons;
      const float2 sampleDirectionUv = float2(
         omega.x * sampleRadiusUv.x, -omega.y * sampleRadiusUv.y);
      const float2 sampleDirectionPixels = sampleDirectionUv * float2(size);
      const float screenSpaceRadius = max(length(sampleDirectionPixels), 1e-5);
      const float minimumSampleFraction =
         SDAO_MINIMUM_SAMPLE_DISTANCE_PIXELS / screenSpaceRadius;

      [unroll]
      for (uint sampleIndex = 0;
           sampleIndex < SDAO_SAMPLES_PER_SLICE;
           ++sampleIndex)
      {
         const float sampleFraction =
            (float(sampleIndex) + 0.5) / float(SDAO_SAMPLES_PER_SLICE);
         const float distributedFraction =
            sampleFraction * sampleFraction + minimumSampleFraction;
         // A point-sampled depth belongs to the selected texel centre, not the
         // continuously moving UV that selected it. Reconstructing position from
         // that unsnapped UV can create stepwise slope errors and block-shaped
         // duplicate silhouettes during camera motion.
         const float2 maximumOffset = float2(size - 1u);
         const int2 sampleOffsetPixels = int2(clamp(
            round(distributedFraction * sampleDirectionPixels),
            -maximumOffset,
            maximumOffset));
         const int2 lastPixel = int2(size) - 1;
         const int2 samplePixel0 = clamp(
            int2(pixel) + sampleOffsetPixels, int2(0, 0), lastPixel);
         const int2 samplePixel1 = clamp(
            int2(pixel) - sampleOffsetPixels, int2(0, 0), lastPixel);
         const float2 uv0 = (float2(samplePixel0) + 0.5) * inverseSize;
         const float2 uv1 = (float2(samplePixel1) + 0.5) * inverseSize;
         const float depth0 = Input0.Load(int3(samplePixel0, 0)).x;
         const float depth1 = Input0.Load(int3(samplePixel1, 0)).x;
         const float2 stepHorizons = float2(
            SelectStepHorizon(
               samplePixel0,
               uv0,
               depth0,
               centerPosition,
               normal,
               viewDirection,
               minimumHorizons.x,
               effectRadius,
               falloffMultiplier,
               falloffAdd),
            SelectStepHorizon(
               samplePixel1,
               uv1,
               depth1,
               centerPosition,
               normal,
               viewDirection,
               minimumHorizons.y,
               effectRadius,
               falloffMultiplier,
               falloffAdd));

         // Decay an older horizon only after selecting the strongest visible or
         // stochastic candidate for this step.
         maximumHorizons = lerp(
            max(maximumHorizons, stepHorizons),
            stepHorizons,
            SDAO_HORIZON_DECAY);
      }

      const float2 horizonAngles = float2(
         FastACos(maximumHorizons.x), -FastACos(maximumHorizons.y));
      visibility += projectedNormalLength * IntegrateCosineWeightedArc(
         horizonAngles.x, horizonAngles.y, normalAngle, cosNormal);
   }

   Output0[pixel] = saturate(visibility / float(SDAO_SLICE_COUNT));
}

float ComputeSpatialWeight(float distanceSquared, float sigma)
{
   return exp(-distanceSquared / max(2.0 * sigma * sigma, 1e-5));
}

float ComputeGeometryWeight(
   float3 centerPosition, float3 samplePosition, float3 centerNormal, float scale)
{
   return saturate(
      1.0 - abs(dot(samplePosition - centerPosition, centerNormal)) * scale);
}

float ComputeDepthWeight(float centerDepth, float sampleDepth)
{
   const float relativeDifference =
      abs(centerDepth - sampleDepth) / max(centerDepth, 1e-5);
   const float sigma = 0.0075;
   return exp(
      -(relativeDifference * relativeDifference) / (2.0 * sigma * sigma));
}

[numthreads(8, 8, 1)]
void spatial_filter_cs(uint2 pixel : SV_DispatchThreadID)
{
   uint width;
   uint height;
   Input0.GetDimensions(width, height);
   const uint2 size = uint2(width, height);
   if (any(pixel >= size))
      return;

   const float centerDepth = Input1.Load(int3(pixel, 0)).x;
   if (centerDepth >= SDAO_FAR_DEPTH * 0.5)
   {
#if SD_SDAO_COLOR_OUTPUT
      Output0[pixel] = float4(0.0, 0.0, 1.0, 1.0);
#else
      Output0[pixel] = 1.0;
#endif
      return;
   }

   const float2 inverseSize = rcp(float2(size));
   const float3 centerPosition = ViewPosition(
      (float2(pixel) + 0.5) * inverseSize, centerDepth);
   float3 centerNormal = ReconstructViewNormal(Input1, pixel);
   const float3 centerViewDirection = SafeNormalize(
      -centerPosition, float3(0.0, 0.0, -1.0));
   if (dot(centerNormal, centerViewDirection) < 0.0)
      centerNormal = -centerNormal;

   const float planeScale = 10.0 / (1.0 + centerDepth);
   const float spatialSigma = max(float(SDAO_FILTER_RADIUS) * 0.75, 0.75);
   float visibilitySum = 0.0;
   float weightSum = 0.0;
   [unroll]
   for (int tap = -SDAO_FILTER_RADIUS; tap <= SDAO_FILTER_RADIUS; ++tap)
   {
#if SD_SDAO_FILTER_HORIZONTAL
      const int2 offset = int2(tap, 0);
#else
      const int2 offset = int2(0, tap);
#endif
      const int2 samplePixel = clamp(
         int2(pixel) + offset, int2(0, 0), int2(size) - 1);
      const float sampleDepth = Input1.Load(int3(samplePixel, 0)).x;
      if (sampleDepth >= SDAO_FAR_DEPTH * 0.5)
         continue;

      const float3 samplePosition = ViewPosition(
         (float2(samplePixel) + 0.5) * inverseSize, sampleDepth);
      const float spatialWeight = ComputeSpatialWeight(
         float(tap * tap), spatialSigma);
      const float geometryWeight = ComputeGeometryWeight(
         centerPosition, samplePosition, centerNormal, planeScale);
      const float depthWeight = ComputeDepthWeight(centerDepth, sampleDepth);
      const float weight = spatialWeight * geometryWeight * depthWeight;
      visibilitySum += Input0.Load(int3(samplePixel, 0)).x * weight;
      weightSum += weight;
   }

   const float filteredVisibility = weightSum > 1e-5
      ? visibilitySum / weightSum
      : Input0.Load(int3(pixel, 0)).x;
#if SD_SDAO_COLOR_OUTPUT
   const float finalVisibility = saturate(
      1.0 - (1.0 - filteredVisibility) * SDAOStrength);
   Output0[pixel] = float4(0.0, 0.0, finalVisibility, 1.0);
#else
   Output0[pixel] = filteredVisibility;
#endif
}
