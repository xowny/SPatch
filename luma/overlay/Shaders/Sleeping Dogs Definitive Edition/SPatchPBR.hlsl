// SPatch opaque deferred-lighting PBR replacements.
//
// Build contract: main / ps_4_0, with exactly one SPATCH_PBR_VARIANT:
//   0  = 0x12489767 (compiled/ABI-validated, kept native at runtime)
//   1  = 0x223AA776
//   2  = 0x2AF235E8
//   3  = 0x2D062589
//   4  = 0x32E195A0
//   5  = 0x398DA3BF
//   6  = 0x386DA32C
//   7  = 0x5167FBBE
//   8  = 0x5EBBA455
//   9  = 0x66072A23
//   10 = 0x8A331B0F
//   11 = 0xA30CEF48
//   12 = 0xDCF9CD0C
//   13 = 0xD71D285B (compiled/ABI-validated, kept native at runtime)
//   14 = 0xE5E2CE1C
//   15 = 0xEFD8577D
//   16 = 0xF74BCE96
//   17 = 0x282EE2DC (runtime-proven vehicle glass)
//   18 = 0x5DB1CB6E (runtime-proven vehicle paint with damage)
//   19 = 0xE611C192 (runtime-proven vehicle paint)
//
// Each selected body retains the validated native shader's CB/resource ABI,
// reconstruction, attenuation, shadow/cookie/volume/area integration, and MRT
// contract. Direct-light normalized Blinn-Phong/Fresnel lobes are blended to
// derivative-stabilized GGX. Opaque synthetic ambient/probe lobes remain
// native; exact vehicle glass and vehicle-paint writers also receive physical
// spherical-environment Fresnel and energy partition on their existing
// seven-mip lookups.

#ifndef SPATCH_PBR_VARIANT
#error SPATCH_PBR_VARIANT must select one of the documented variants.
#endif

#ifndef SPATCH_PBR_STRENGTH
#define SPATCH_PBR_STRENGTH 1.0
#endif

static const float SPATCH_PBR_PI = 3.14159265358979323846;
static const float SPATCH_PBR_SPECULAR_AA_VARIANCE_CAP = 0.18;
static const float SPATCH_PBR_F0_MAX = 0.999;

float3 SPatchPBRSafeNormalize(float3 value)
{
  return value * rsqrt(max(dot(value, value), 1.0e-8));
}

float SPatchPBRBlendStrength()
{
  return saturate((float)SPATCH_PBR_STRENGTH);
}

float3 SPatchPBRHalfVector(float3 viewDirection, float3 lightDirection)
{
  float3 halfVector = viewDirection + lightDirection;
  return SPatchPBRSafeNormalize(halfVector);
}

float3 SPatchPBRBoundF0(float3 f0)
{
  return min(max(f0, 0.0), SPATCH_PBR_F0_MAX);
}

float3 SPatchPBRNativeOpaqueF90(float3 f0)
{
  // The stock opaque deferred families use min(1, 50 * F0), not a forced
  // white grazing response. This matters for foliage and other authored
  // zero-F0 materials: zero reflectance must stay zero at every angle.
  return min(50.0 * SPatchPBRBoundF0(f0), 1.0);
}

float3 SPatchPBRFresnelCosineWithF90(
  float cosine,
  float3 f0,
  float3 f90)
{
  // Texture-authored F0 can contain HDR or malformed values. A physical
  // interface remains bounded at one; clamping prevents invalid highlights
  // while the explicit F90 preserves each native shader family's contract.
  float3 boundedF0 = SPatchPBRBoundF0(f0);
  float3 boundedF90 = min(max(f90, boundedF0), 1.0);
  float oneMinusCosine = 1.0 - saturate(cosine);
  float oneMinusCosine2 = oneMinusCosine * oneMinusCosine;
  float oneMinusCosine5 = oneMinusCosine2 * oneMinusCosine2 * oneMinusCosine;
  return boundedF0 + (boundedF90 - boundedF0) * oneMinusCosine5;
}

float3 SPatchPBRFresnelCosine(float cosine, float3 f0)
{
  return SPatchPBRFresnelCosineWithF90(
    cosine, f0, SPatchPBRNativeOpaqueF90(f0));
}

float3 SPatchPBRFresnelCosineUnitGrazing(float cosine, float3 f0)
{
  return SPatchPBRFresnelCosineWithF90(
    cosine, f0, float3(1.0, 1.0, 1.0));
}

float3 SPatchPBRFresnel(float3 viewDirection, float3 lightDirection, float3 f0)
{
  float3 halfVector = SPatchPBRHalfVector(viewDirection, lightDirection);
  return SPatchPBRFresnelCosine(dot(viewDirection, halfVector), f0);
}

float3 SPatchPBRFresnelUnitGrazing(
  float3 viewDirection,
  float3 lightDirection,
  float3 f0)
{
  float3 halfVector = SPatchPBRHalfVector(viewDirection, lightDirection);
  return SPatchPBRFresnelCosineUnitGrazing(
    dot(viewDirection, halfVector), f0);
}

float SPatchPBRGeometricVariance(float3 normal)
{
  // Toksvig-style geometric specular AA, evaluated only by direct-light GGX
  // calls. Flat or smoothly sampled normals retain the authored roughness;
  // only normal variation inside the 2x2 pixel quad broadens the microfacet
  // distribution. The bounded variance prevents silhouette discontinuities
  // from turning the entire direct lobe diffuse.
  float3 normalDx = ddx(normal);
  float3 normalDy = ddy(normal);
  float normalVariance = 0.25 * (
    dot(normalDx, normalDx) + dot(normalDy, normalDy));
  return min(2.0 * max(normalVariance, 0.0),
    SPATCH_PBR_SPECULAR_AA_VARIANCE_CAP);
}

float3 SPatchPBRDirectSpecularWithF90(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float nativeExponent,
  float3 f0,
  float geometricVariance,
  float3 f90)
{
  float noV = saturate(dot(normal, viewDirection));
  float noL = saturate(dot(normal, lightDirection));
  float3 halfVector = SPatchPBRHalfVector(viewDirection, lightDirection);
  float noH = saturate(dot(normal, halfVector));

  // The native exponent-to-roughness chain reduces exactly to alpha^2 =
  // 2/(n+2). The exponent cap keeps the former 0.045 roughness floor
  // unreachable, so this removes pow and redundant multiplies without
  // changing the represented GGX distribution.
  float safeExponent = min(max(nativeExponent, 0.0), 4096.0);
  float alphaSquared = saturate(
    2.0 / (safeExponent + 2.0) + max(geometricVariance, 0.0));

  float distributionDenominator = max(
    noH * noH * (alphaSquared - 1.0) + 1.0, 1.0e-4);
  // The engine's Lambert light units multiply the physical GGX BRDF by PI;
  // cancel that PI against the NDF denominator before evaluation.
  float engineDistribution = alphaSquared /
    (distributionDenominator * distributionDenominator);

  // Height-correlated Smith-GGX visibility, expressed as G / (4 NoL NoV).
  float smithV = noL * sqrt(noV * noV * (1.0 - alphaSquared) + alphaSquared);
  float smithL = noV * sqrt(noL * noL * (1.0 - alphaSquared) + alphaSquared);
  float visibility = 0.5 / max(smithV + smithL, 1.0e-6);

  return engineDistribution * visibility *
    SPatchPBRFresnelCosineWithF90(
      dot(viewDirection, halfVector), f0, f90);
}

float3 SPatchPBRDirectSpecular(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float nativeExponent,
  float3 f0,
  float geometricVariance)
{
  return SPatchPBRDirectSpecularWithF90(
    normal, viewDirection, lightDirection, nativeExponent, f0,
    geometricVariance, SPatchPBRNativeOpaqueF90(f0));
}

float3 SPatchPBRDirectSpecularUnitGrazing(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float nativeExponent,
  float3 f0,
  float geometricVariance)
{
  return SPatchPBRDirectSpecularWithF90(
    normal, viewDirection, lightDirection, nativeExponent, f0,
    geometricVariance, float3(1.0, 1.0, 1.0));
}

float3 SPatchPBRGlassDirectSpecular(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float nativeExponent,
  float3 f0)
{
  float noV = saturate(dot(normal, viewDirection));
  float noL = saturate(dot(normal, lightDirection));
  float3 halfVector = SPatchPBRHalfVector(viewDirection, lightDirection);
  float noH = saturate(dot(normal, halfVector));

  // Vehicle glass legitimately reaches an exponent near 32768. Its 65536
  // safety cap also keeps the former 0.045 roughness floor unreachable, so
  // alpha^2 reduces exactly to 2/(n+2) without a pow.
  float safeExponent = min(max(nativeExponent, 0.0), 65536.0);
  float alphaSquared = saturate(2.0 / (safeExponent + 2.0));

  float distributionDenominator = max(
    noH * noH * (alphaSquared - 1.0) + 1.0, 1.0e-6);
  float engineDistribution = alphaSquared /
    (distributionDenominator * distributionDenominator);

  float smithV = noL * sqrt(noV * noV * (1.0 - alphaSquared) + alphaSquared);
  float smithL = noV * sqrt(noL * noL * (1.0 - alphaSquared) + alphaSquared);
  float visibility = 0.5 / max(smithV + smithL, 1.0e-6);

  return engineDistribution * visibility *
    SPatchPBRFresnelUnitGrazing(viewDirection, lightDirection, f0);
}

float3 SPatchPBRDiffuseWeightWithF90(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float3 f0,
  float metallic,
  float3 f90)
{
  // A diffuse ray crosses the microsurface interface twice. Applying the
  // same native-family Fresnel on both sides keeps the partition reciprocal.
  float3 viewTransmission = 1.0 - SPatchPBRFresnelCosineWithF90(
    dot(normal, viewDirection), f0, f90);
  float3 lightTransmission = 1.0 - SPatchPBRFresnelCosineWithF90(
    dot(normal, lightDirection), f0, f90);
  return (1.0 - saturate(metallic)) * viewTransmission * lightTransmission;
}

float3 SPatchPBRDiffuseWeight(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float3 f0,
  float metallic)
{
  return SPatchPBRDiffuseWeightWithF90(
    normal, viewDirection, lightDirection, f0, metallic,
    SPatchPBRNativeOpaqueF90(f0));
}

float3 SPatchPBRDiffuseWeightUnitGrazing(
  float3 normal,
  float3 viewDirection,
  float3 lightDirection,
  float3 f0,
  float metallic)
{
  return SPatchPBRDiffuseWeightWithF90(
    normal, viewDirection, lightDirection, f0, metallic,
    float3(1.0, 1.0, 1.0));
}

#if SPATCH_PBR_VARIANT == 0
// Native shader 0x12489767.

cbuffer cbExternalViewTransform : register(b0)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbShaderParams : register(b1)
{

  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texNormal : register(s1);
SamplerState _texDepth : register(s2);
SamplerState _texAmbient2 : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texNormal : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture2D<float4> texAmbient2 : register(t3);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r1.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.z = -r0.x;
  r0.x = saturate(r0.x / cbShaderParams.Value0.w);
  r0.x = 1 + -r0.x;
  r1.w = -r1.y;
  r0.y = dot(r1.xzw, r1.xzw);
  r0.y = rsqrt(r0.y);
  r2.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value0.xyz;
  r1.y = dot(r2.xyz, r2.xyz);
  r1.y = rsqrt(r1.y);
  r2.xyz = r2.xyz * r1.yyy;
  r3.xyz = r1.xwz * r0.yyy;
  r4.x = saturate(dot(-r3.xyz, r2.xyz));
  r5.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value1.xyz;
  r1.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value2.xyz;
  r0.y = dot(r5.xyz, r5.xyz);
  r0.y = rsqrt(r0.y);
  r5.xyz = r5.xyz * r0.yyy;
  r4.y = saturate(dot(-r3.xyz, r5.xyz));
  r0.y = dot(r1.xyz, r1.xyz);
  r0.y = rsqrt(r0.y);
  r1.xyz = r1.xyz * r0.yyy;
  r4.z = saturate(dot(-r3.xyz, r1.xyz));
  r3.xyz = float3(1,1,1) + -r4.xyz;
  r3.xyz = r3.xyz * r3.xyz;
  r3.xyz = r3.xyz * r3.xyz;
  r3.xyz = r3.xyz + r3.xyz;
  r4.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r0.y = cmp(r4.w == 1.000000);
  r4.xyz = r4.xyz * r4.xyz;
  r4.xyz = r0.yyy ? r4.xyz : r4.www;
  r6.xyz = float3(50,50,50) * r4.xyz;
  r6.xyz = min(float3(1,1,1), r6.xyz);
  r6.xyz = saturate(r6.xyz + -r4.xyz);
  r7.xyz = r6.xyz * r3.yyy + r4.xyz;
  r8.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r9.xyzw = texAmbient2.Sample(_texAmbient2, r0.zw).xyzw;
  r0.yzw = cbShaderParams.Value1.www * r9.xyz;
  r0.yzw = r0.yzw * r0.yzw;
  r8.xyz = r8.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r1.w = 7 * r8.w;
  r1.w = exp2(r1.w);
  r2.w = dot(r8.xyz, r8.xyz);
  r2.w = rsqrt(r2.w);
  r8.xyz = r8.xyz * r2.www;
  r2.x = saturate(dot(r8.xyz, r2.xyz));
  r2.x = log2(r2.x);
  r2.w = saturate(dot(r8.xyz, r5.xyz));
  r2.y = log2(r2.w);
  r1.x = saturate(dot(r8.xyz, r1.xyz));
  r2.z = log2(r1.x);
  r1.xyz = r2.xyz * r1.www;
  r1.w = -1 + r1.w;
  r1.w = 0.125 * r1.w;
  r1.xyz = exp2(r1.xyz);
  r2.xyz = r1.yyy * r7.xyz;
  r5.x = saturate(dot(r8.xyz, cbShaderParams.Value0.xyz));
  r5.y = saturate(dot(r8.xyz, cbShaderParams.Value1.xyz));
  r5.z = saturate(dot(r8.xyz, cbShaderParams.Value2.xyz));
  r0.yzw = r5.xyz * r0.yzw;
  r0.yzw = r0.yzw * r1.www;
  r0.yzw = float3(3,3,3) * r0.yzw;
  r2.xyz = r2.xyz * r0.zzz;
  r3.xyw = r6.xyz * r3.xxx + r4.xyz;
  r4.xyz = r6.xyz * r3.zzz + r4.xyz;
  r1.yzw = r4.xyz * r1.zzz;
  r3.xyz = r3.xyw * r1.xxx;
  r2.xyz = r3.xyz * r0.yyy + r2.xyz;
  r0.yzw = r1.yzw * r0.www + r2.xyz;
  o0.xyz = r0.yzw * r0.xxx;
  o0.w = 0;
  return;
}

#elif SPATCH_PBR_VARIANT == 1
// Native shader 0x223AA776.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    row_major float4x4 WorldProjection;
    row_major float4x4 WorldViewInv;
    float4 CameraOffset;
    float4 CameraPosition;
    float4 Target;
  } cbViewTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b2)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b3)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s1);
SamplerState _texDiffuse : register(s2);
SamplerState _texNormal : register(s3);
SamplerState _texNoise : register(s4);
SamplerState _texDepth : register(s5);
SamplerState _texDistAtten : register(s6);
SamplerComparisonState _texShadow : register(s0);
Texture2D<float4> texShadow : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);
Texture2D<float4> texDiffuse : register(t2);
Texture2D<float4> texNormal : register(t3);
Texture2D<float4> texNoise : register(t4);
Texture2D<float4> texDepth : register(t5);
Texture2D<float4> texDistAtten : register(t6);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  float3 v3 : TEXCOORD2,
  float3 v4 : TEXCOORD3,
  float3 v5 : TEXCOORD4,
  float3 v6 : TEXCOORD5,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.xyz = v6.zxy + -v5.zxy;
  r3.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v5.yzx;
  r2.w = -r2.y;
  r3.xyz = r3.xyz + -r2.wzx;
  r4.xyz = r3.xyz * r1.xyz;
  r3.xyz = r1.zxy * r3.yzx + -r4.xyz;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r3.xyz = r3.xyz * r0.xxx;
  r0.x = dot(r3.xzy, r2.xzw);
  r0.y = dot(r3.xyz, v4.xyz);
  r0.x = r0.y + -r0.x;
  r4.xyz = v5.zxy + -v4.zxy;
  r0.y = dot(r3.zxy, r4.xyz);
  r0.x = saturate(-r0.x / r0.y);
  r0.y = 1 + -r0.x;
  r3.xyz = v4.zxy + -v3.zxy;
  r5.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v3.yzx;
  r5.xyz = r5.xyz + -r2.wzx;
  r6.xyz = r5.xyz * r3.xyz;
  r5.xyz = r3.zxy * r5.yzx + -r6.xyz;
  r1.w = dot(r5.xyz, r5.xyz);
  r1.w = rsqrt(r1.w);
  r5.xyz = r5.xyz * r1.www;
  r1.w = dot(r5.xzy, r2.xzw);
  r3.w = dot(r5.xyz, v6.xyz);
  r1.w = r3.w + -r1.w;
  r6.xyz = -v6.zxy + v3.zxy;
  r3.w = dot(r5.zxy, r6.xyz);
  r1.w = saturate(-r1.w / r3.w);
  r0.y = max(r1.w, r0.y);
  r1.w = 1 + -r1.w;
  r0.x = max(r1.w, r0.x);
  r5.xyz = r6.xyz * r0.yyy + v6.zxy;
  r7.xyz = r6.zxy * r0.yyy + v5.yzx;
  r8.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v6.yzx;
  r8.xyz = r8.xyz + -r2.wzx;
  r9.xyz = r8.xyz * r6.xyz;
  r6.xyz = r6.zxy * r8.yzx + -r9.xyz;
  r0.y = dot(r6.xyz, r6.xyz);
  r0.y = rsqrt(r0.y);
  r6.xyz = r6.xyz * r0.yyy;
  r0.y = dot(r6.xzy, r2.xzw);
  r1.w = dot(r6.xyz, v5.xyz);
  r3.w = dot(r6.zxy, r1.xyz);
  r0.y = r1.w + -r0.y;
  r0.y = saturate(-r0.y / r3.w);
  r1.w = 1 + -r0.y;
  r6.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v4.yzx;
  r6.xyz = r6.xyz + -r2.wzx;
  r8.xyz = r6.xyz * r4.xyz;
  r6.xyz = r4.zxy * r6.yzx + -r8.xyz;
  r3.w = dot(r6.xyz, r6.xyz);
  r3.w = rsqrt(r3.w);
  r6.xyz = r6.xyz * r3.www;
  r3.w = dot(r6.xzy, r2.xzw);
  r4.w = dot(r6.xyz, v3.xyz);
  r5.w = dot(r6.zxy, r3.xyz);
  r3.w = r4.w + -r3.w;
  r3.w = saturate(-r3.w / r5.w);
  r1.w = max(r3.w, r1.w);
  r3.w = 1 + -r3.w;
  r0.y = max(r3.w, r0.y);
  r5.xyz = r3.xyz * r1.www + r5.xyz;
  r6.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r6.xyz = r6.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r3.w = 10 * r6.w;
  r3.w = exp2(r3.w);
  r4.w = dot(r6.xyz, r6.xyz);
  r4.w = rsqrt(r4.w);
  r6.xyz = r6.xyz * r4.www;
  r4.w = dot(r6.zxy, r5.xyz);
  r5.w = dot(r6.xzy, r2.xzw);
  r4.w = -r5.w + r4.w;
  r4.w = max(0, -r4.w);
  r5.xyz = r6.zxy * r4.www + r5.xyz;
  r5.xyz = r5.xyz + -r2.zxw;
  r4.w = dot(r5.xyz, r5.xyz);
  r4.w = rsqrt(r4.w);
  r5.xyz = r5.xyz * r4.www;
  r8.xyz = r4.xyz * r0.xxx + v3.zxy;
  r4.xyz = r4.xyz * r0.xxx + v4.zxy;
  r4.xyz = r1.xyz * r0.yyy + r4.xyz;
  r1.xyz = r1.zxy * r0.yyy + r7.xyz;
  r3.xyz = r3.xyz * r1.www + r8.xyz;
  r0.x = dot(r6.zxy, r3.xyz);
  r0.x = r0.x + -r5.w;
  r0.x = max(0, -r0.x);
  r3.xyz = r6.zxy * r0.xxx + r3.xyz;
  r3.xyz = r3.xyz + -r2.zxw;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r3.xyz = r3.xyz * r0.xxx;
  r7.xyz = r5.xyz * r3.zxy;
  r7.xyz = r5.zxy * r3.xyz + -r7.xyz;
  r7.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r7.xyz;
  r0.x = dot(r7.xyz, r7.xyz);
  r0.x = rsqrt(r0.x);
  r7.xyz = r7.xyz * r0.xxx;
  r0.x = dot(r5.xyz, r3.xyz);
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.w = r1.w * r0.y;
  r4.w = r4.w * -2 + 3.14159274;
  r0.x = r0.x ? r4.w : 0;
  r0.x = r0.y * r1.w + r0.x;
  r7.xyz = r0.xxx * r7.xyz;
  r0.x = dot(r6.yzx, r1.xyz);
  r0.x = r0.x + -r5.w;
  r0.x = max(0, -r0.x);
  r1.xyz = r6.yzx * r0.xxx + r1.xyz;
  r1.xyz = r1.xyz + -r2.wzx;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r1.xyz = r1.xyz * r0.xxx;
  r8.xyz = r1.yzx * r5.zxy;
  r8.xyz = r1.xyz * r5.xyz + -r8.xyz;
  r0.x = dot(r1.yzx, r5.xyz);
  r5.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r8.xyz;
  r0.y = dot(r5.xyz, r5.xyz);
  r0.y = rsqrt(r0.y);
  r5.xyz = r5.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.w = r1.w * r0.y;
  r4.w = r4.w * -2 + 3.14159274;
  r0.x = r0.x ? r4.w : 0;
  r0.x = r0.y * r1.w + r0.x;
  r5.xyz = r0.xxx * r5.xyz + r7.xyz;
  r0.x = dot(r6.zxy, r4.xyz);
  r0.x = r0.x + -r5.w;
  r0.x = max(0, -r0.x);
  r4.xyz = r6.zxy * r0.xxx + r4.xyz;
  r4.xyz = r4.xyz + -r2.zxw;
  r0.x = dot(r4.xyz, r4.xyz);
  r0.x = rsqrt(r0.x);
  r4.xyz = r4.xyz * r0.xxx;
  r7.xyz = r4.zxy * r3.xyz;
  r7.xyz = r3.zxy * r4.xyz + -r7.xyz;
  r0.x = dot(r3.xyz, r4.xyz);
  r3.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r7.xyz;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.w = r1.w * r0.y;
  r4.w = r4.w * -2 + 3.14159274;
  r0.x = r0.x ? r4.w : 0;
  r0.x = r0.y * r1.w + r0.x;
  r3.xyz = r0.xxx * r3.xyz + r5.xyz;
  r5.xyz = r4.xyz * r1.xyz;
  r5.xyz = r4.zxy * r1.yzx + -r5.xyz;
  r0.x = dot(r4.zxy, r1.xyz);
  r1.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r5.xyz;
  r0.y = dot(r1.xyz, r1.xyz);
  r0.y = rsqrt(r0.y);
  r1.xyz = r1.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.x = r1.w * r0.y;
  r4.x = r4.x * -2 + 3.14159274;
  r0.x = r0.x ? r4.x : 0;
  r0.x = r0.y * r1.w + r0.x;
  r1.xyz = r0.xxx * r1.xyz + r3.xyz;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r0.y = dot(-r2.xzw, -r2.xzw);
  r0.y = rsqrt(r0.y);
  r3.xyz = -r2.xwz * r0.yyy;
  r4.xyz = r1.xyz * r0.xxx + r3.xyz;
  r5.xyz = r1.xyz * r0.xxx;
  float3 spatchViewDirection = r3.xyz;
  float3 spatchLightDirection = r5.xyz;
  float spatchNativeExponent = r3.w;
  r0.x = dot(r1.xyz, r6.xyz);
  r0.x = max(0, r0.x);
  r0.y = dot(r4.xyz, r4.xyz);
  r0.y = rsqrt(r0.y);
  r1.xyz = r4.xyz * r0.yyy;
  r0.y = saturate(dot(r6.xyz, r1.xyz));
  r1.x = saturate(dot(r3.xyz, r1.xyz));
  r1.x = 1 + -r1.x;
  r0.y = log2(r0.y);
  r0.y = r3.w * r0.y;
  r1.y = -1 + r3.w;
  r0.y = exp2(r0.y);
  r0.y = r1.y * r0.y;
  r0.xy = float2(0.159154907,0.125) * r0.xy;
  r1.y = r1.x * r1.x;
  r1.y = r1.y * r1.y;
  r1.x = r1.x * r1.y;
  r3.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r1.y = cmp(r3.w == 1.000000);
  float spatchMetallic = r1.y ? 1.0 : 0.0;
  r3.xyz = r3.xyz * r3.xyz;
  r4.xyz = r3.xyz * r3.xyz;
  r1.yzw = r1.yyy ? r4.xyz : r3.www;
  float3 spatchF0 = spatchMetallic ? r3.xyz : r3.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r6.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchNativeExponent,
    spatchF0, spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r4.xyz = float3(50,50,50) * r1.yzw;
  r4.xyz = min(float3(1,1,1), r4.xyz);
  r4.xyz = saturate(r4.xyz + -r1.yzw);
  r1.xyz = r4.xyz * r1.xxx + r1.yzw;
  r1.xyz = r1.xyz * r0.yyy;
  r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
  r0.y = 1 / cbDeferredLight.ColourAndInvRadiusSqr.w;
  r0.y = -cbDeferredLight.WidthHeightNearFar.z + r0.y;
  r4.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r1.w = dot(r4.xyz, r4.xyz);
  r1.w = sqrt(r1.w);
  r3.w = -cbDeferredLight.WidthHeightNearFar.z + r1.w;
  r4.x = saturate(r3.w / r0.y);
  r4.y = saturate(cbDeferredLight.Fov.w);
  r4.xyzw = texDistAtten.Sample(_texDistAtten, r4.xy).xyzw;
  r0.y = log2(r1.w);
  r0.y = cbDeferredLight.PositionAndRadius.w * r0.y;
  r0.y = exp2(r0.y);
  r0.y = 1 / r0.y;
  r0.y = saturate(-r1.w * cbDeferredLight.ColourAndInvRadiusSqr.w + r0.y);
  r1.w = r4.x + -r0.y;
  r3.w = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
  r3.w = r3.w ? 1.000000 : 0;
  r0.y = r3.w * r1.w + r0.y;
  r1.w = 1 + -r0.y;
  r1.w = r1.w * r1.w;
  r1.w = -r1.w * r1.w + 1;
  r1.xyz = r1.xyz * r1.www;
  r1.xyz = r1.xyz * r0.xxx;
  r4.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xxx;
  r4.xyz = r4.xyz * r0.yyy;
  r3.xyz = r4.xyz * (r3.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength()));
  r1.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r1.xyz;
  r0.x = cbDeferredLight.WidthHeightNearFar.z + cbDeferredLight.WidthHeightNearFar.w;
  r4.xyz = v2.xyz * r0.xxx + cbDeferredLight.PositionAndRadius.xyz;
  r4.xyz = -r4.xyz + r2.xwz;
  r0.x = dot(r4.xyz, v2.xyz);
  r0.x = cmp(1.00000001e-007 >= r0.x);
  r4.xyz = v2.xyz * cbDeferredLight.WidthHeightNearFar.zzz + cbDeferredLight.PositionAndRadius.xyz;
  r4.xyz = -r4.xyz + r2.xwz;
  r0.y = dot(r4.xyz, v2.xyz);
  r0.y = cmp(r0.y >= 1.00000001e-007);
  r0.xy = r0.xy ? float2(1,1) : 0;
  r0.x = r0.x * r0.y;
  r4.xyzw = cbShadowTransform.ViewShadow[0]._m10_m11_m12_m13 * -r2.yyyy;
  r4.xyzw = r2.xxxx * cbShadowTransform.ViewShadow[0]._m00_m01_m02_m03 + r4.xyzw;
  r2.xyzw = r2.zzzz * cbShadowTransform.ViewShadow[0]._m20_m21_m22_m23 + r4.xyzw;
  r2.xyzw = cbShadowTransform.ViewShadow[0]._m30_m31_m32_m33 + r2.xyzw;
  r0.y = -9.99999975e-005 + r2.z;
  r0.y = r0.y / r2.w;
  r4.xy = r2.xy / r2.ww;
  r0.y = texShadow.SampleCmp(_texShadow, r4.xy, r0.y, int2(0, 0)).x;
  r4.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r4.xy, 0).xyzw;
  r4.xyz = r4.xyz * r4.xyz;
  r6.xy = float2(1,1) / cbViewTransform.Target.xy;
  r0.zw = r6.xy * r0.zw;
  r0.zw = float2(0.03125,0.03125) * r0.zw;
  r6.xyzw = texNoise.Sample(_texNoise, r0.zw).xyzw;
  r0.zw = float2(0.5,0.5) + r6.xy;
  r1.w = 0.00079999998 * r2.w;
  r2.xyzw = float4(0,0,-9.99999975e-005,0) + r2.xyzw;
  r6.xy = r1.ww * r0.zw;
  r7.xy = float2(-1,1) * r6.xy;
  r7.zw = float2(0,0);
  r7.xyzw = r7.xyzw + r2.xyzw;
  r7.xyz = r7.xyz / r7.www;
  r0.z = texShadow.SampleCmp(_texShadow, r7.xy, r7.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r6.zw = float2(0,0);
  r7.xyzw = r6.xyzw + r2.xyzw;
  r7.xyz = r7.xyz / r7.www;
  r0.z = texShadow.SampleCmp(_texShadow, r7.xy, r7.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r7.xy = float2(-1,-1) * r6.xy;
  r6.xy = float2(1,-1) * r6.xy;
  r7.zw = float2(0,0);
  r7.xyzw = r7.xyzw + r2.xyzw;
  r7.xyz = r7.xyz / r7.www;
  r0.z = texShadow.SampleCmp(_texShadow, r7.xy, r7.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r6.zw = float2(0,0);
  r2.xyzw = r6.xyzw + r2.xyzw;
  r2.xyz = r2.xyz / r2.www;
  r0.z = texShadow.SampleCmp(_texShadow, r2.xy, r2.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r0.z = 0.200000003 * r0.y;
  r0.y = -r0.y * 0.200000003 + 1;
  r0.w = saturate(cbDeferredLight.Fov.z);
  r0.y = r0.w * r0.y + r0.z;
  r0.x = r0.x * r0.y;
  r0.xyz = r0.xxx * r4.xyz;
  r2.xyz = r1.xyz * r0.xyz;
  o0.w = dot(r1.xyz, float3(4,4,4));
  r0.xyz = r3.xyz * r0.xyz + r2.xyz;
  o0.xyz = r0.xyz;
  r0.x = dot(r0.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = r5.xyz * r0.xxx;
  o1.w = r0.x;
  return;
}

#elif SPATCH_PBR_VARIANT == 2
// Native shader 0x2AF235E8.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b2)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s0);
SamplerState _texDiffuse : register(s1);
SamplerState _texNormal : register(s2);
SamplerState _texDepth : register(s3);
SamplerState _texDistAtten : register(s4);
Texture2D<float4> texDiffuse2 : register(t0);
Texture2D<float4> texDiffuse : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texDepth : register(t3);
Texture2D<float4> texDistAtten : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v1.xy / v1.ww;
  r1.xyzw = texNormal.Sample(_texNormal, r0.xy).xyzw;
  r0.zw = saturate(r0.xy);
  r2.xyzw = texDepth.Sample(_texDepth, r0.zw).xyzw;
  r0.z = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = -r2.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.z = r0.z / r0.w;
  r2.xy = r0.xy * float2(2,2) + float2(-1,-1);
  r2.xy = r2.xy * r0.zz;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r2.xy;
  r2.z = -r0.z;
  r2.w = -r2.y;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.z = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.z);
  r4.xyz = r3.xyz * r0.www;
  r3.w = dot(v2.xyz, r4.xyz);
  r3.w = -cbDeferredLight.Fov.y + r3.w;
  r4.w = cbDeferredLight.Fov.x + -cbDeferredLight.Fov.y;
  r3.w = saturate(r3.w / r4.w);
  r5.xyzw = texDiffuse.Sample(_texDiffuse, r0.xy).xyzw;
  r5.xyz = r5.xyz * r5.xyz;
  float3 spatchDiffuseScale = float3(1.0, 1.0, 1.0);
  r0.x = cmp(0 < r3.w);
  r0.y = sqrt(r0.z);
  r6.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.y;
  r6.y = cbDeferredLight.Fov.w;
  r6.xy = saturate(r6.xy);
  r6.xyzw = texDistAtten.Sample(_texDistAtten, r6.xy).xyzw;
  float3 spatchPbrNormal =
    r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  spatchPbrNormal = SPatchPBRSafeNormalize(spatchPbrNormal);
  float spatchPbrGeometricVariance =
    SPatchPBRGeometricVariance(spatchPbrNormal);
  [flatten]
  if (r0.x != 0) {
    r1.xyz = spatchPbrNormal;
    float3 spatchLightDirection = r4.xyz;
    r6.yzw = cbShadowTransform.ViewShadow[0]._m10_m11_m13 * -r2.yyy;
    r6.yzw = r2.xxx * cbShadowTransform.ViewShadow[0]._m00_m01_m03 + r6.yzw;
    r6.yzw = r2.zzz * cbShadowTransform.ViewShadow[0]._m20_m21_m23 + r6.yzw;
    r6.yzw = cbShadowTransform.ViewShadow[0]._m30_m31_m33 + r6.yzw;
    r0.x = cmp(r5.w == 1.000000);
    r7.xyz = r5.xyz * r5.xyz;
    r7.xyz = r0.xxx ? r7.xyz : r5.www;
    float3 spatchF0 = r0.xxx ? r5.xyz : r5.www;
    float spatchMetallic = r0.x ? 1.0 : 0.0;
    r0.x = log2(r0.y);
    r0.x = cbDeferredLight.PositionAndRadius.w * r0.x;
    r0.x = exp2(r0.x);
    r0.x = 1 / r0.x;
    r0.x = saturate(-r0.y * cbDeferredLight.ColourAndInvRadiusSqr.w + r0.x);
    r0.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
    r0.y = r0.y ? 1.000000 : 0;
    r0.z = r6.x + -r0.x;
    r0.x = r0.y * r0.z + r0.x;
    r0.y = saturate(dot(r1.xyz, r4.xyz));
    r4.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yyy;
    r4.xyz = r4.xyz * r0.xxx;
    r0.z = dot(-r2.xzw, -r2.xzw);
    r0.z = rsqrt(r0.z);
    r2.xyz = -r2.xwz * r0.zzz;
    r0.z = 10 * r1.w;
    r0.z = exp2(r0.z);
    float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
      r1.xyz, r2.xyz, spatchLightDirection, r0.z, spatchF0,
      spatchPbrGeometricVariance);
    float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
      r1.xyz, r2.xyz, spatchLightDirection, spatchF0, spatchMetallic);
    spatchDiffuseScale = lerp(
      float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength());
    r3.xyz = r3.xyz * r0.www + r2.xyz;
    r0.w = dot(r3.xyz, r3.xyz);
    r0.w = rsqrt(r0.w);
    r3.xyz = r3.xyz * r0.www;
    r0.w = saturate(dot(r1.xyz, r3.xyz));
    r0.w = log2(r0.w);
    r0.w = r0.z * r0.w;
    r0.w = exp2(r0.w);
    r0.z = -1 + r0.z;
    r0.z = r0.z * r0.w;
    r0.z = 0.125 * r0.z;
    r1.xyz = float3(50,50,50) * r7.xyz;
    r1.xyz = min(float3(1,1,1), r1.xyz);
    r1.xyz = saturate(r1.xyz + -r7.xyz);
    r0.w = saturate(dot(r2.xyz, r3.xyz));
    r0.w = 1 + -r0.w;
    r1.w = r0.w * r0.w;
    r1.w = r1.w * r1.w;
    r0.w = r1.w * r0.w;
    r1.xyz = r1.xyz * r0.www + r7.xyz;
    r1.xyz = r1.xyz * r0.zzz;
    r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
    r0.x = 1 + -r0.x;
    r0.x = r0.x * r0.x;
    r0.x = -r0.x * r0.x + 1;
    r0.xzw = r1.xyz * r0.xxx;
    r0.xyz = r0.xzw * r0.yyy;
    r0.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xyz;
    r1.xy = r6.yz / r6.ww;
    r1.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r1.xy, 0).xyzw;
    r1.xyz = r3.www * r1.xxx;
  } else {
    r1.xyz = float3(0,0,0);
    r4.xyz = float3(0,0,0);
    r0.xyz = float3(0,0,0);
  }
  r2.xyz = r4.xyz * (r5.xyz * spatchDiffuseScale);
  r3.xyz = r0.xyz * r3.www;
  r3.xyz = r3.xyz * r1.xyz;
  o0.xyz = r2.xyz * r1.xyz + r3.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  return;
}

#elif SPATCH_PBR_VARIANT == 3
// Native shader 0x2D062589.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b2)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s0);
SamplerState _texDiffuse : register(s1);
SamplerState _texNormal : register(s2);
SamplerState _texDepth : register(s3);
SamplerState _texDistAtten : register(s4);
Texture2D<float4> texDiffuse2 : register(t0);
Texture2D<float4> texDiffuse : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texDepth : register(t3);
Texture2D<float4> texDistAtten : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  float3 v3 : TEXCOORD2,
  float3 v4 : TEXCOORD3,
  float3 v5 : TEXCOORD4,
  float3 v6 : TEXCOORD5,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.xyz = v6.zxy + -v5.zxy;
  r3.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v5.yzx;
  r2.w = -r2.y;
  r3.xyz = r3.xyz + -r2.wzx;
  r4.xyz = r3.xyz * r1.xyz;
  r3.xyz = r1.zxy * r3.yzx + -r4.xyz;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r3.xyz = r3.xyz * r0.xxx;
  r0.x = dot(r3.xzy, r2.xzw);
  r0.y = dot(r3.xyz, v4.xyz);
  r0.x = r0.y + -r0.x;
  r4.xyz = v5.zxy + -v4.zxy;
  r0.y = dot(r3.zxy, r4.xyz);
  r0.x = saturate(-r0.x / r0.y);
  r0.y = 1 + -r0.x;
  r3.xyz = v4.zxy + -v3.zxy;
  r5.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v3.yzx;
  r5.xyz = r5.xyz + -r2.wzx;
  r6.xyz = r5.xyz * r3.xyz;
  r5.xyz = r3.zxy * r5.yzx + -r6.xyz;
  r1.w = dot(r5.xyz, r5.xyz);
  r1.w = rsqrt(r1.w);
  r5.xyz = r5.xyz * r1.www;
  r1.w = dot(r5.xzy, r2.xzw);
  r3.w = dot(r5.xyz, v6.xyz);
  r1.w = r3.w + -r1.w;
  r6.xyz = -v6.zxy + v3.zxy;
  r3.w = dot(r5.zxy, r6.xyz);
  r1.w = saturate(-r1.w / r3.w);
  r0.y = max(r1.w, r0.y);
  r1.w = 1 + -r1.w;
  r0.x = max(r1.w, r0.x);
  r5.xyz = r6.xyz * r0.yyy + v6.zxy;
  r7.xyz = r6.zxy * r0.yyy + v5.yzx;
  r8.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v6.yzx;
  r8.xyz = r8.xyz + -r2.wzx;
  r9.xyz = r8.xyz * r6.xyz;
  r6.xyz = r6.zxy * r8.yzx + -r9.xyz;
  r0.y = dot(r6.xyz, r6.xyz);
  r0.y = rsqrt(r0.y);
  r6.xyz = r6.xyz * r0.yyy;
  r0.y = dot(r6.xzy, r2.xzw);
  r1.w = dot(r6.xyz, v5.xyz);
  r3.w = dot(r6.zxy, r1.xyz);
  r0.y = r1.w + -r0.y;
  r0.y = saturate(-r0.y / r3.w);
  r1.w = 1 + -r0.y;
  r6.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v4.yzx;
  r6.xyz = r6.xyz + -r2.wzx;
  r8.xyz = r6.xyz * r4.xyz;
  r6.xyz = r4.zxy * r6.yzx + -r8.xyz;
  r3.w = dot(r6.xyz, r6.xyz);
  r3.w = rsqrt(r3.w);
  r6.xyz = r6.xyz * r3.www;
  r3.w = dot(r6.xzy, r2.xzw);
  r4.w = dot(r6.xyz, v3.xyz);
  r5.w = dot(r6.zxy, r3.xyz);
  r3.w = r4.w + -r3.w;
  r3.w = saturate(-r3.w / r5.w);
  r1.w = max(r3.w, r1.w);
  r3.w = 1 + -r3.w;
  r0.y = max(r3.w, r0.y);
  r5.xyz = r3.xyz * r1.www + r5.xyz;
  r6.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r8.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r6.xyz = r6.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.z = 10 * r6.w;
  r0.z = exp2(r0.z);
  r0.w = dot(r6.xyz, r6.xyz);
  r0.w = rsqrt(r0.w);
  r6.xyz = r6.xyz * r0.www;
  r0.w = dot(r6.zxy, r5.xyz);
  r3.w = dot(r6.xzy, r2.xzw);
  r0.w = -r3.w + r0.w;
  r0.w = max(0, -r0.w);
  r5.xyz = r6.zxy * r0.www + r5.xyz;
  r5.xyz = r5.xyz + -r2.zxw;
  r0.w = dot(r5.xyz, r5.xyz);
  r0.w = rsqrt(r0.w);
  r5.xyz = r5.xyz * r0.www;
  r9.xyz = r4.xyz * r0.xxx + v3.zxy;
  r4.xyz = r4.xyz * r0.xxx + v4.zxy;
  r4.xyz = r1.xyz * r0.yyy + r4.xyz;
  r0.xyw = r1.zxy * r0.yyy + r7.xyz;
  r1.xyz = r3.xyz * r1.www + r9.xyz;
  r1.w = dot(r6.zxy, r1.xyz);
  r1.w = r1.w + -r3.w;
  r1.w = max(0, -r1.w);
  r1.xyz = r6.zxy * r1.www + r1.xyz;
  r1.xyz = r1.xyz + -r2.zxw;
  r1.w = dot(r1.xyz, r1.xyz);
  r1.w = rsqrt(r1.w);
  r1.xyz = r1.xyz * r1.www;
  r3.xyz = r5.xyz * r1.zxy;
  r3.xyz = r5.zxy * r1.xyz + -r3.xyz;
  r3.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r3.xyz;
  r1.w = dot(r3.xyz, r3.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r3.xyz * r1.www;
  r1.w = dot(r5.xyz, r1.xyz);
  r4.w = abs(r1.w) * -0.0187292993 + 0.0742610022;
  r4.w = r4.w * abs(r1.w) + -0.212114394;
  r4.w = r4.w * abs(r1.w) + 1.57072878;
  r5.w = 1 + -abs(r1.w);
  r1.w = cmp(r1.w < -r1.w);
  r5.w = sqrt(r5.w);
  r6.w = r5.w * r4.w;
  r6.w = r6.w * -2 + 3.14159274;
  r1.w = r1.w ? r6.w : 0;
  r1.w = r4.w * r5.w + r1.w;
  r3.xyz = r1.www * r3.xyz;
  r1.w = dot(r6.yzx, r0.xyw);
  r1.w = r1.w + -r3.w;
  r1.w = max(0, -r1.w);
  r0.xyw = r6.yzx * r1.www + r0.xyw;
  r0.xyw = r0.xyw + -r2.wzx;
  r1.w = dot(r0.xyw, r0.xyw);
  r1.w = rsqrt(r1.w);
  r0.xyw = r1.www * r0.xyw;
  r7.xyz = r0.ywx * r5.zxy;
  r7.xyz = r0.xyw * r5.xyz + -r7.xyz;
  r1.w = dot(r0.ywx, r5.xyz);
  r5.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r7.xyz;
  r4.w = dot(r5.xyz, r5.xyz);
  r4.w = rsqrt(r4.w);
  r5.xyz = r5.xyz * r4.www;
  r4.w = abs(r1.w) * -0.0187292993 + 0.0742610022;
  r4.w = r4.w * abs(r1.w) + -0.212114394;
  r4.w = r4.w * abs(r1.w) + 1.57072878;
  r5.w = 1 + -abs(r1.w);
  r1.w = cmp(r1.w < -r1.w);
  r5.w = sqrt(r5.w);
  r6.w = r5.w * r4.w;
  r6.w = r6.w * -2 + 3.14159274;
  r1.w = r1.w ? r6.w : 0;
  r1.w = r4.w * r5.w + r1.w;
  r3.xyz = r1.www * r5.xyz + r3.xyz;
  r1.w = dot(r6.zxy, r4.xyz);
  r1.w = r1.w + -r3.w;
  r1.w = max(0, -r1.w);
  r4.xyz = r6.zxy * r1.www + r4.xyz;
  r4.xyz = r4.xyz + -r2.zxw;
  r1.w = dot(r4.xyz, r4.xyz);
  r1.w = rsqrt(r1.w);
  r4.xyz = r4.xyz * r1.www;
  r5.xyz = r4.zxy * r1.xyz;
  r5.xyz = r1.zxy * r4.xyz + -r5.xyz;
  r1.x = dot(r1.xyz, r4.xyz);
  r1.yzw = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r5.xyz;
  r3.w = dot(r1.yzw, r1.yzw);
  r3.w = rsqrt(r3.w);
  r1.yzw = r3.www * r1.yzw;
  r3.w = abs(r1.x) * -0.0187292993 + 0.0742610022;
  r3.w = r3.w * abs(r1.x) + -0.212114394;
  r3.w = r3.w * abs(r1.x) + 1.57072878;
  r4.w = 1 + -abs(r1.x);
  r1.x = cmp(r1.x < -r1.x);
  r4.w = sqrt(r4.w);
  r5.x = r4.w * r3.w;
  r5.x = r5.x * -2 + 3.14159274;
  r1.x = r1.x ? r5.x : 0;
  r1.x = r3.w * r4.w + r1.x;
  r1.xyz = r1.xxx * r1.yzw + r3.xyz;
  r3.xyz = r4.xyz * r0.xyw;
  r3.xyz = r4.zxy * r0.ywx + -r3.xyz;
  r0.x = dot(r4.zxy, r0.xyw);
  r3.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r3.xyz;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r0.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r0.w = sqrt(r0.w);
  r1.w = r0.y * r0.w;
  r1.w = r1.w * -2 + 3.14159274;
  r0.x = r0.x ? r1.w : 0;
  r0.x = r0.y * r0.w + r0.x;
  r0.xyw = r0.xxx * r3.xyz + r1.xyz;
  r1.x = dot(r0.xyw, r0.xyw);
  r1.x = rsqrt(r1.x);
  r1.y = dot(-r2.xzw, -r2.xzw);
  r1.y = rsqrt(r1.y);
  r1.yzw = -r2.xwz * r1.yyy;
  r3.xyz = r0.xyw * r1.xxx + r1.yzw;
  r4.xyz = r1.xxx * r0.xyw;
  float3 spatchViewDirection = r1.yzw;
  float3 spatchLightDirection = r4.xyz;
  float spatchNativeExponent = r0.z;
  r0.x = dot(r0.xyw, r6.xyz);
  r0.x = max(0, r0.x);
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = saturate(dot(r6.xyz, r3.xyz));
  r0.w = saturate(dot(r1.yzw, r3.xyz));
  r0.w = 1 + -r0.w;
  r0.y = log2(r0.y);
  r0.y = r0.z * r0.y;
  r0.z = -1 + r0.z;
  r0.y = exp2(r0.y);
  r0.y = r0.z * r0.y;
  r0.xy = float2(0.159154907,0.125) * r0.xy;
  r0.z = cmp(r8.w == 1.000000);
  r1.xyz = r8.xyz * r8.xyz;
  r3.xyz = r1.xyz * r1.xyz;
  r3.xyz = r0.zzz ? r3.xyz : r8.www;
  float3 spatchF0 = r0.zzz ? r1.xyz : r8.www;
  float spatchMetallic = r0.z ? 1.0 : 0.0;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r6.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchNativeExponent,
    spatchF0, spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r5.xyz = float3(50,50,50) * r3.xyz;
  r5.xyz = min(float3(1,1,1), r5.xyz);
  r5.xyz = saturate(r5.xyz + -r3.xyz);
  r0.z = r0.w * r0.w;
  r0.z = r0.z * r0.z;
  r0.z = r0.w * r0.z;
  r3.xyz = r5.xyz * r0.zzz + r3.xyz;
  r0.yzw = r3.xyz * r0.yyy;
  r0.yzw = lerp(r0.yzw, spatchPbrSpecular, SPatchPBRBlendStrength());
  r1.w = 1 / cbDeferredLight.ColourAndInvRadiusSqr.w;
  r1.w = -cbDeferredLight.WidthHeightNearFar.z + r1.w;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r3.x = dot(r3.xyz, r3.xyz);
  r3.x = sqrt(r3.x);
  r3.y = -cbDeferredLight.WidthHeightNearFar.z + r3.x;
  r5.x = saturate(r3.y / r1.w);
  r5.y = saturate(cbDeferredLight.Fov.w);
  r5.xyzw = texDistAtten.Sample(_texDistAtten, r5.xy).xyzw;
  r1.w = log2(r3.x);
  r1.w = cbDeferredLight.PositionAndRadius.w * r1.w;
  r1.w = exp2(r1.w);
  r1.w = 1 / r1.w;
  r1.w = saturate(-r3.x * cbDeferredLight.ColourAndInvRadiusSqr.w + r1.w);
  r3.x = r5.x + -r1.w;
  r3.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
  r3.y = r3.y ? 1.000000 : 0;
  r1.w = r3.y * r3.x + r1.w;
  r3.x = 1 + -r1.w;
  r3.x = r3.x * r3.x;
  r3.x = -r3.x * r3.x + 1;
  r0.yzw = r3.xxx * r0.yzw;
  r0.yzw = r0.yzw * r0.xxx;
  r3.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xxx;
  r3.xyz = r3.xyz * r1.www;
  r1.xyz = r3.xyz * (r1.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength()));
  r0.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yzw;
  r0.w = cbDeferredLight.WidthHeightNearFar.z + cbDeferredLight.WidthHeightNearFar.w;
  r3.xyz = v2.xyz * r0.www + cbDeferredLight.PositionAndRadius.xyz;
  r3.xyz = -r3.xyz + r2.xwz;
  r0.w = dot(r3.xyz, v2.xyz);
  r0.w = cmp(1.00000001e-007 >= r0.w);
  r0.w = r0.w ? 1.000000 : 0;
  r3.xyz = v2.xyz * cbDeferredLight.WidthHeightNearFar.zzz + cbDeferredLight.PositionAndRadius.xyz;
  r3.xyz = -r3.xyz + r2.xwz;
  r1.w = dot(r3.xyz, v2.xyz);
  r1.w = cmp(r1.w >= 1.00000001e-007);
  r1.w = r1.w ? 1.000000 : 0;
  r0.w = r1.w * r0.w;
  r3.xyz = cbShadowTransform.ViewShadow[0]._m10_m11_m13 * -r2.yyy;
  r2.xyw = r2.xxx * cbShadowTransform.ViewShadow[0]._m00_m01_m03 + r3.xyz;
  r2.xyz = r2.zzz * cbShadowTransform.ViewShadow[0]._m20_m21_m23 + r2.xyw;
  r2.xyz = cbShadowTransform.ViewShadow[0]._m30_m31_m33 + r2.xyz;
  r2.xy = r2.xy / r2.zz;
  r2.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r2.xy, 0).xyzw;
  r2.xyz = r2.xyz * r2.xyz;
  r2.xyz = r2.xyz * r0.www;
  r3.xyz = r2.xyz * r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  r0.xyz = r1.xyz * r2.xyz + r3.xyz;
  o0.xyz = r0.xyz;
  r0.x = dot(r0.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = r4.xyz * r0.xxx;
  o1.w = r0.x;
  return;
}

#elif SPATCH_PBR_VARIANT == 4
// Native shader 0x32E195A0.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b2)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s1);
SamplerState _texDiffuse : register(s2);
SamplerState _texNormal : register(s3);
SamplerState _texDepth : register(s4);
SamplerState _texDistAtten : register(s5);
SamplerComparisonState _texShadow : register(s0);
Texture2D<float4> texShadow : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);
Texture2D<float4> texDiffuse : register(t2);
Texture2D<float4> texNormal : register(t3);
Texture2D<float4> texDepth : register(t4);
Texture2D<float4> texDistAtten : register(t5);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v1.xy / v1.ww;
  r1.xyzw = texNormal.Sample(_texNormal, r0.xy).xyzw;
  r0.zw = saturate(r0.xy);
  r2.xyzw = texDepth.Sample(_texDepth, r0.zw).xyzw;
  r0.z = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = -r2.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.z = r0.z / r0.w;
  r2.xy = r0.xy * float2(2,2) + float2(-1,-1);
  r2.xy = r2.xy * r0.zz;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r2.xy;
  r2.z = -r0.z;
  r2.w = -r2.y;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.z = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.z);
  r4.xyz = r3.xyz * r0.www;
  r3.w = dot(v2.xyz, r4.xyz);
  r3.w = -cbDeferredLight.Fov.y + r3.w;
  r4.w = cbDeferredLight.Fov.x + -cbDeferredLight.Fov.y;
  r3.w = saturate(r3.w / r4.w);
  r5.xyzw = cbShadowTransform.ViewShadow[0]._m10_m11_m12_m13 * -r2.yyyy;
  r5.xyzw = r2.xxxx * cbShadowTransform.ViewShadow[0]._m00_m01_m02_m03 + r5.xyzw;
  r5.xyzw = r2.zzzz * cbShadowTransform.ViewShadow[0]._m20_m21_m22_m23 + r5.xyzw;
  r5.xyzw = cbShadowTransform.ViewShadow[0]._m30_m31_m32_m33 + r5.xyzw;
  r6.xyzw = texDiffuse.Sample(_texDiffuse, r0.xy).xyzw;
  r6.xyz = r6.xyz * r6.xyz;
  float3 spatchDiffuseScale = float3(1.0, 1.0, 1.0);
  r0.x = cmp(0 < r3.w);
  r0.y = sqrt(r0.z);
  r7.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.y;
  r7.y = cbDeferredLight.Fov.w;
  r7.xy = saturate(r7.xy);
  r7.xyzw = texDistAtten.Sample(_texDistAtten, r7.xy).xyzw;
  r0.z = -9.99999975e-005 + r5.z;
  r5.xy = r5.xy / r5.ww;
  r0.z = r0.z / r5.w;
  r0.z = texShadow.SampleCmp(_texShadow, r5.xy, r0.z, int2(0, 0)).x;
  float3 spatchPbrNormal =
    r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  spatchPbrNormal = SPatchPBRSafeNormalize(spatchPbrNormal);
  float spatchPbrGeometricVariance =
    SPatchPBRGeometricVariance(spatchPbrNormal);
  [flatten]
  if (r0.x != 0) {
    r1.xyz = spatchPbrNormal;
    float3 spatchLightDirection = r4.xyz;
    r0.x = cmp(r6.w == 1.000000);
    r7.yzw = r6.xyz * r6.xyz;
    r7.yzw = r0.xxx ? r7.yzw : r6.www;
    float3 spatchF0 = r0.xxx ? r6.xyz : r6.www;
    float spatchMetallic = r0.x ? 1.0 : 0.0;
    r0.x = log2(r0.y);
    r0.x = cbDeferredLight.PositionAndRadius.w * r0.x;
    r0.x = exp2(r0.x);
    r0.x = 1 / r0.x;
    r0.x = saturate(-r0.y * cbDeferredLight.ColourAndInvRadiusSqr.w + r0.x);
    r0.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
    r0.y = r0.y ? 1.000000 : 0;
    r2.y = r7.x + -r0.x;
    r0.x = r0.y * r2.y + r0.x;
    r0.y = saturate(dot(r1.xyz, r4.xyz));
    r4.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yyy;
    r4.xyz = r4.xyz * r0.xxx;
    r2.y = dot(-r2.xzw, -r2.xzw);
    r2.y = rsqrt(r2.y);
    r2.xyz = -r2.xwz * r2.yyy;
    r1.w = 10 * r1.w;
    r1.w = exp2(r1.w);
    float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
      r1.xyz, r2.xyz, spatchLightDirection, r1.w, spatchF0,
      spatchPbrGeometricVariance);
    float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
      r1.xyz, r2.xyz, spatchLightDirection, spatchF0, spatchMetallic);
    spatchDiffuseScale = lerp(
      float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength());
    r3.xyz = r3.xyz * r0.www + r2.xyz;
    r0.w = dot(r3.xyz, r3.xyz);
    r0.w = rsqrt(r0.w);
    r3.xyz = r3.xyz * r0.www;
    r0.w = saturate(dot(r1.xyz, r3.xyz));
    r0.w = log2(r0.w);
    r0.w = r1.w * r0.w;
    r0.w = exp2(r0.w);
    r1.x = -1 + r1.w;
    r0.w = r1.x * r0.w;
    r0.w = 0.125 * r0.w;
    r1.xyz = float3(50,50,50) * r7.yzw;
    r1.xyz = min(float3(1,1,1), r1.xyz);
    r1.xyz = saturate(r1.xyz + -r7.yzw);
    r1.w = saturate(dot(r2.xyz, r3.xyz));
    r1.w = 1 + -r1.w;
    r2.x = r1.w * r1.w;
    r2.x = r2.x * r2.x;
    r1.w = r2.x * r1.w;
    r1.xyz = r1.xyz * r1.www + r7.yzw;
    r1.xyz = r1.xyz * r0.www;
    r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
    r0.x = 1 + -r0.x;
    r0.x = r0.x * r0.x;
    r0.x = -r0.x * r0.x + 1;
    r1.xyz = r1.xyz * r0.xxx;
    r0.xyw = r1.xyz * r0.yyy;
    r0.xyw = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xyw;
    r1.x = saturate(cbDeferredLight.Fov.z);
    r1.y = 1 + -r0.z;
    r0.z = r1.x * r1.y + r0.z;
    r0.z = r3.w * r0.z;
    r1.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r5.xy, 0).xyzw;
    r1.xyz = r1.xxx * r0.zzz;
  } else {
    r1.xyz = float3(0,0,0);
    r4.xyz = float3(0,0,0);
    r0.xyw = float3(0,0,0);
  }
  r2.xyz = r4.xyz * (r6.xyz * spatchDiffuseScale);
  r3.xyz = r0.xyw * r3.www;
  r3.xyz = r3.xyz * r1.xyz;
  o0.xyz = r2.xyz * r1.xyz + r3.xyz;
  o0.w = dot(r0.xyw, float3(4,4,4));
  return;
}

#elif SPATCH_PBR_VARIANT == 5
// Native shader 0x398DA3BF.

cbuffer cbEnvironmentSettings : register(b0)
{

  struct
  {
    float4 SunDir;
    float4 SunDirWorld;
    float4 SunColor;
    float4 AmbientColorHorizon;
    float4 ScaleAndHeight;
    float4 ScatterZenithColor;
    float4 ScatterHorizonColor;
    float4 ScatterGroundColor;
    float4 ScatterSunColor;
    float4 CharacterParams;
    float4 FogStartStopSky;
    float4 WindDirAndMag;
    float4 DisplayDebug;
    float4 LitWindowTimeOn;
    float4 Lighting;
    float4 SunScatterParams;
  } cbEnvironmentSettings : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbShaderParams : register(b2)
{

  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texNormal : register(s1);
SamplerState _texDepth : register(s2);
SamplerState _texAmbient2 : register(s3);
SamplerState _texCollector : register(s4);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texNormal : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture2D<float4> texAmbient2 : register(t3);
Texture2D<float4> texCollector : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r1.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.z = -r0.x;
  r0.x = saturate(r0.x / cbShaderParams.Value0.w);
  r0.x = 1 + -r0.x;
  r1.w = -r1.y;
  r0.y = dot(r1.xzw, r1.xzw);
  r0.y = rsqrt(r0.y);
  r2.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value0.xyz;
  r1.y = dot(r2.xyz, r2.xyz);
  r1.y = rsqrt(r1.y);
  r2.xyz = r2.xyz * r1.yyy;
  r3.xyz = r1.xwz * r0.yyy;
  r4.x = saturate(dot(-r3.xyz, r2.xyz));
  r5.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value1.xyz;
  r1.y = dot(r5.xyz, r5.xyz);
  r1.y = rsqrt(r1.y);
  r5.xyz = r5.xyz * r1.yyy;
  r4.y = saturate(dot(-r3.xyz, r5.xyz));
  r6.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value2.xyz;
  r1.xyz = -r1.xwz * r0.yyy + v2.xyz;
  r0.y = dot(r6.xyz, r6.xyz);
  r0.y = rsqrt(r0.y);
  r6.xyz = r6.xyz * r0.yyy;
  r4.z = saturate(dot(-r3.xyz, r6.xyz));
  float3 spatchViewDirection = -r3.xyz;
  float3 spatchSunLight = v2.xyz;
  r4.xyz = float3(1,1,1) + -r4.xyz;
  r4.xyz = r4.xyz * r4.xyz;
  r4.xyz = r4.xyz * r4.xyz;
  r4.xyz = r4.xyz + r4.xyz;
  r7.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r0.y = cmp(r7.w == 1.000000);
  r7.xyz = r7.xyz * r7.xyz;
  r8.xyz = r0.yyy ? r7.xyz : r7.www;
  float3 spatchF0 = r8.xyz;
  float spatchMetallic = r0.y ? 1.0 : 0.0;
  r9.xyz = float3(50,50,50) * r8.xyz;
  r9.xyz = min(float3(1,1,1), r9.xyz);
  r9.xyz = saturate(r9.xyz + -r8.xyz);
  r10.xyz = r9.xyz * r4.yyy + r8.xyz;
  r11.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r11.xyz = r11.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.y = 7 * r11.w;
  r0.y = exp2(r0.y);
  r1.w = dot(r11.xyz, r11.xyz);
  r1.w = rsqrt(r1.w);
  r11.xyz = r11.xyz * r1.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r11.xyz);
  float3 spatchPbrSun = SPatchPBRDirectSpecular(
    r11.xyz, spatchViewDirection, spatchSunLight, r0.y, spatchF0,
    spatchPbrGeometricVariance);
  float3 spatchPbrSunDiffuseWeight = SPatchPBRDiffuseWeight(
    r11.xyz, spatchViewDirection, spatchSunLight, spatchF0, spatchMetallic);
  r1.w = saturate(dot(r11.xyz, r2.xyz));
  r2.x = log2(r1.w);
  r1.w = saturate(dot(r11.xyz, r5.xyz));
  r2.y = log2(r1.w);
  r1.w = saturate(dot(r11.xyz, r6.xyz));
  r2.z = log2(r1.w);
  r2.xyz = r2.xyz * r0.yyy;
  r2.xyz = exp2(r2.xyz);
  r5.xyz = r2.yyy * r10.xyz;
  r6.xyzw = texAmbient2.Sample(_texAmbient2, r0.zw).xyzw;
  r10.xyzw = texCollector.Sample(_texCollector, r0.zw).xyzw;
  r10.xyz = cbEnvironmentSettings.SunColor.xyz * r10.www;
  r6.xyz = cbShaderParams.Value1.www * r6.xyz;
  r6.xyz = r6.xyz * r6.xyz;
  r12.x = saturate(dot(r11.xyz, cbShaderParams.Value0.xyz));
  r12.y = saturate(dot(r11.xyz, cbShaderParams.Value1.xyz));
  r12.z = saturate(dot(r11.xyz, cbShaderParams.Value2.xyz));
  r6.xyz = r12.xyz * r6.xyz;
  r0.z = -1 + r0.y;
  r0.z = 0.125 * r0.z;
  r6.xyz = r6.xyz * r0.zzz;
  r6.xyz = float3(3,3,3) * r6.xyz;
  r5.xyz = r6.yyy * r5.xyz;
  r4.xyw = r9.xyz * r4.xxx + r8.xyz;
  r12.xyz = r9.xyz * r4.zzz + r8.xyz;
  r2.yzw = r12.xyz * r2.zzz;
  r4.xyz = r4.xyw * r2.xxx;
  r4.xyz = r4.xyz * r6.xxx + r5.xyz;
  r2.xyz = r2.yzw * r6.zzz + r4.xyz;
  r4.xyz = r2.xyz * r0.xxx;
  r0.w = saturate(dot(r11.xyz, v2.xyz));
  r5.xyz = r0.www * r10.xyz;
  r4.xyz = r5.xyz * (r7.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrSunDiffuseWeight, SPatchPBRBlendStrength())) + r4.xyz;
  r1.w = dot(r1.xyz, r1.xyz);
  r1.w = rsqrt(r1.w);
  r1.xyz = r1.xyz * r1.www;
  r1.w = saturate(dot(r11.xyz, r1.xyz));
  r1.x = saturate(dot(-r3.xyz, r1.xyz));
  r1.x = 1 + -r1.x;
  r1.y = log2(r1.w);
  r0.y = r1.y * r0.y;
  r0.y = exp2(r0.y);
  r0.y = r0.y * r0.z;
  r0.z = r1.x * r1.x;
  r0.z = r0.z * r0.z;
  r0.z = r1.x * r0.z;
  r1.xyz = r9.xyz * r0.zzz + r8.xyz;
  r1.xyz = r1.xyz * r0.yyy;
  r0.yzw = r1.xyz * r0.www;
  r0.yzw = lerp(
    r0.yzw, spatchPbrSun * r0.www, SPatchPBRBlendStrength());
  r1.xyz = r0.yzw * r10.xyz + r4.xyz;
  r0.yzw = r0.yzw * r10.xyz;
  o0.w = dot(r0.yzw, float3(4,4,4));
  o0.xyz = r1.xyz;
  r0.xyz = -r2.xyz * r0.xxx + r1.xyz;
  r0.x = dot(r0.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = v2.xyz * r0.xxx;
  o1.w = r0.x;
  return;
}

#elif SPATCH_PBR_VARIANT == 6
// Native shader 0x386DA32C.

cbuffer cbEnvironmentSettings : register(b0)
{

  struct
  {
    float4 SunDir;
    float4 SunDirWorld;
    float4 SunColor;
    float4 AmbientColorHorizon;
    float4 ScaleAndHeight;
    float4 ScatterZenithColor;
    float4 ScatterHorizonColor;
    float4 ScatterGroundColor;
    float4 ScatterSunColor;
    float4 CharacterParams;
    float4 FogStartStopSky;
    float4 WindDirAndMag;
    float4 DisplayDebug;
    float4 LitWindowTimeOn;
    float4 Lighting;
    float4 SunScatterParams;
  } cbEnvironmentSettings : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texNormal : register(s1);
SamplerState _texDepth : register(s2);
SamplerState _texCollector : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texNormal : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture2D<float4> texCollector : register(t3);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = saturate(v1.xy);
  r0.xyzw = texDepth.Sample(_texDepth, r0.xy).xyzw;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.x = -r0.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.x = r0.y / r0.x;
  r0.yz = v1.xy * float2(2,2) + float2(-1,-1);
  r0.yz = r0.yz * r0.xx;
  r1.z = -r0.x;
  r1.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r0.yz;
  r1.w = -r1.y;
  r0.x = dot(-r1.xzw, -r1.xzw);
  r0.x = rsqrt(r0.x);
  r0.yzw = -r1.xwz * r0.xxx;
  float3 spatchViewDirection = r0.yzw;
  r1.xyz = -r1.xwz * r0.xxx + v2.xyz;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r1.xyz = r1.xyz * r0.xxx;
  r0.x = saturate(dot(r0.yzw, r1.xyz));
  r0.x = 1 + -r0.x;
  r0.y = r0.x * r0.x;
  r0.y = r0.y * r0.y;
  r0.x = r0.x * r0.y;
  r2.xyzw = texDiffuse.Sample(_texDiffuse, v1.xy).xyzw;
  r0.y = cmp(r2.w == 1.000000);
  float spatchMetallic = r0.y ? 1.0 : 0.0;
  r2.xyz = r2.xyz * r2.xyz;
  r0.yzw = r0.yyy ? r2.xyz : r2.www;
  float3 spatchF0 = r0.yzw;
  r3.xyz = float3(50,50,50) * r0.yzw;
  r3.xyz = min(float3(1,1,1), r3.xyz);
  r3.xyz = saturate(r3.xyz + -r0.yzw);
  r0.xyz = r3.xyz * r0.xxx + r0.yzw;
  r3.xyzw = texNormal.Sample(_texNormal, v1.xy).xyzw;
  r3.xyz = r3.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.w = 10 * r3.w;
  r0.w = exp2(r0.w);
  r1.w = dot(r3.xyz, r3.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r3.xyz * r1.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r3.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r3.xyz, spatchViewDirection, v2.xyz, r0.w, spatchF0,
    spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r3.xyz, spatchViewDirection, v2.xyz, spatchF0, spatchMetallic);
  r1.x = saturate(dot(r3.xyz, r1.xyz));
  r1.y = saturate(dot(r3.xyz, v2.xyz));
  r1.x = log2(r1.x);
  r1.x = r1.x * r0.w;
  r0.w = -1 + r0.w;
  r1.x = exp2(r1.x);
  r0.w = r1.x * r0.w;
  r0.w = 0.125 * r0.w;
  r0.xyz = r0.www * r0.xyz;
  r0.xyz = lerp(r0.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
  r0.xyz = r0.xyz * r1.yyy;
  r3.xyzw = texCollector.Sample(_texCollector, v1.xy).xyzw;
  r1.xzw = cbEnvironmentSettings.SunColor.xyz * r3.www;
  r0.xyz = r1.xzw * r0.xyz;
  r1.xyz = r1.yyy * r1.xzw;
  o0.xyz = r1.xyz * (r2.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength())) + r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  return;
}

#elif SPATCH_PBR_VARIANT == 7
// Native shader 0x5167FBBE.

cbuffer cbEnvironmentSettings : register(b0)
{

  struct
  {
    float4 SunDir;
    float4 SunDirWorld;
    float4 SunColor;
    float4 AmbientColorHorizon;
    float4 ScaleAndHeight;
    float4 ScatterZenithColor;
    float4 ScatterHorizonColor;
    float4 ScatterGroundColor;
    float4 ScatterSunColor;
    float4 CharacterParams;
    float4 FogStartStopSky;
    float4 WindDirAndMag;
    float4 DisplayDebug;
    float4 LitWindowTimeOn;
    float4 Lighting;
    float4 SunScatterParams;
  } cbEnvironmentSettings : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texNormal : register(s1);
SamplerState _texDepth : register(s2);
SamplerState _texCollector : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texNormal : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture2D<float4> texCollector : register(t3);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = saturate(v1.xy);
  r0.xyzw = texDepth.Sample(_texDepth, r0.xy).xyzw;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.x = -r0.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.x = r0.y / r0.x;
  r0.yz = v1.xy * float2(2,2) + float2(-1,-1);
  r0.yz = r0.yz * r0.xx;
  r1.z = -r0.x;
  r1.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r0.yz;
  r1.w = -r1.y;
  r0.x = dot(-r1.xzw, -r1.xzw);
  r0.x = rsqrt(r0.x);
  r0.yzw = -r1.xwz * r0.xxx;
  float3 spatchViewDirection = r0.yzw;
  r1.xyz = -r1.xwz * r0.xxx + v2.xyz;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r1.xyz = r1.xyz * r0.xxx;
  r0.x = saturate(dot(r0.yzw, r1.xyz));
  r0.x = 1 + -r0.x;
  r0.y = r0.x * r0.x;
  r0.y = r0.y * r0.y;
  r0.x = r0.x * r0.y;
  r2.xyzw = texDiffuse.Sample(_texDiffuse, v1.xy).xyzw;
  r0.y = cmp(r2.w == 1.000000);
  float spatchMetallic = r0.y ? 1.0 : 0.0;
  r2.xyz = r2.xyz * r2.xyz;
  r0.yzw = r0.yyy ? r2.xyz : r2.www;
  float3 spatchF0 = r0.yzw;
  r3.xyz = float3(50,50,50) * r0.yzw;
  r3.xyz = min(float3(1,1,1), r3.xyz);
  r3.xyz = saturate(r3.xyz + -r0.yzw);
  r0.xyz = r3.xyz * r0.xxx + r0.yzw;
  r3.xyzw = texNormal.Sample(_texNormal, v1.xy).xyzw;
  r3.xyz = r3.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.w = 10 * r3.w;
  r0.w = exp2(r0.w);
  r1.w = dot(r3.xyz, r3.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r3.xyz * r1.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r3.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r3.xyz, spatchViewDirection, v2.xyz, r0.w, spatchF0,
    spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r3.xyz, spatchViewDirection, v2.xyz, spatchF0, spatchMetallic);
  r1.x = saturate(dot(r3.xyz, r1.xyz));
  r1.y = saturate(dot(r3.xyz, v2.xyz));
  r1.x = log2(r1.x);
  r1.x = r1.x * r0.w;
  r0.w = -1 + r0.w;
  r1.x = exp2(r1.x);
  r0.w = r1.x * r0.w;
  r0.w = 0.125 * r0.w;
  r0.xyz = r0.www * r0.xyz;
  r0.xyz = lerp(r0.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
  r0.xyz = r0.xyz * r1.yyy;
  r3.xyzw = texCollector.Sample(_texCollector, v1.xy).xyzw;
  r1.xzw = cbEnvironmentSettings.SunColor.xyz * r3.www;
  r0.xyz = r1.xzw * r0.xyz;
  r1.xyz = r1.yyy * r1.xzw;
  r1.xyz = r1.xyz * (r2.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength())) + r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  o0.xyz = r1.xyz;
  r0.x = dot(r1.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = v2.xyz * r0.xxx;
  o1.w = r0.x;
  return;
}

#elif SPATCH_PBR_VARIANT == 8
// Native shader 0x5EBBA455.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    row_major float4x4 WorldProjection;
    row_major float4x4 WorldViewInv;
    float4 CameraOffset;
    float4 CameraPosition;
    float4 Target;
  } cbViewTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b2)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b3)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s1);
SamplerState _texDiffuse : register(s2);
SamplerState _texNormal : register(s3);
SamplerState _texNoise : register(s4);
SamplerState _texDepth : register(s5);
SamplerState _texDistAtten : register(s6);
SamplerComparisonState _texShadow : register(s0);
Texture2D<float4> texShadow : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);
Texture2D<float4> texDiffuse : register(t2);
Texture2D<float4> texNormal : register(t3);
Texture2D<float4> texNoise : register(t4);
Texture2D<float4> texDepth : register(t5);
Texture2D<float4> texDistAtten : register(t6);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v1.xy / v1.ww;
  r1.xyzw = texNormal.Sample(_texNormal, r0.xy).xyzw;
  r0.zw = saturate(r0.xy);
  r2.xyzw = texDepth.Sample(_texDepth, r0.zw).xyzw;
  r0.z = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = -r2.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.z = r0.z / r0.w;
  r2.xy = r0.xy * float2(2,2) + float2(-1,-1);
  r2.xy = r2.xy * r0.zz;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r2.xy;
  r2.z = -r0.z;
  r2.w = -r2.y;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.z = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.z);
  r4.xyz = r3.xyz * r0.www;
  r3.w = dot(v2.xyz, r4.xyz);
  r3.w = -cbDeferredLight.Fov.y + r3.w;
  r4.w = cbDeferredLight.Fov.x + -cbDeferredLight.Fov.y;
  r3.w = saturate(r3.w / r4.w);
  r5.xyzw = cbShadowTransform.ViewShadow[0]._m10_m11_m12_m13 * -r2.yyyy;
  r5.xyzw = r2.xxxx * cbShadowTransform.ViewShadow[0]._m00_m01_m02_m03 + r5.xyzw;
  r5.xyzw = r2.zzzz * cbShadowTransform.ViewShadow[0]._m20_m21_m22_m23 + r5.xyzw;
  r5.xyzw = cbShadowTransform.ViewShadow[0]._m30_m31_m32_m33 + r5.xyzw;
  r6.xyzw = texDiffuse.Sample(_texDiffuse, r0.xy).xyzw;
  r6.xyz = r6.xyz * r6.xyz;
  float3 spatchDiffuseScale = float3(1.0, 1.0, 1.0);
  r2.y = cmp(0 < r3.w);
  r0.z = sqrt(r0.z);
  r7.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.z;
  r7.y = cbDeferredLight.Fov.w;
  r7.xy = saturate(r7.xy);
  r7.xyzw = texDistAtten.Sample(_texDistAtten, r7.xy).xyzw;
  r4.w = -9.99999975e-005 + r5.z;
  r7.yz = float2(1,1) / cbViewTransform.Target.xy;
  r0.xy = r7.yz * r0.xy;
  r0.xy = float2(0.03125,0.03125) * r0.xy;
  r8.xyzw = texNoise.Sample(_texNoise, r0.xy).xyzw;
  r0.xy = float2(0.5,0.5) + r8.xy;
  r7.y = 0.00079999998 * r5.w;
  r8.xy = r7.yy * r0.xy;
  r9.xy = float2(-1,1) * r8.xy;
  r9.zw = float2(0,0);
  r8.zw = float2(0,0);
  r10.xy = float2(-1,-1) * r8.xy;
  r10.zw = float2(0,0);
  r11.xy = float2(1,-1) * r8.xy;
  r11.zw = float2(0,0);
  r0.xy = r5.xy / r5.ww;
  r4.w = r4.w / r5.w;
  r4.w = texShadow.SampleCmp(_texShadow, r0.xy, r4.w, int2(0, 0)).x;
  r5.xyzw = float4(0,0,-9.99999975e-005,0) + r5.xyzw;
  r9.xyzw = r5.xyzw + r9.xyzw;
  r7.yzw = r9.xyz / r9.www;
  r7.y = texShadow.SampleCmp(_texShadow, r7.yz, r7.w, int2(0, 0)).x;
  r8.xyzw = r5.xyzw + r8.xyzw;
  r8.xyz = r8.xyz / r8.www;
  r7.z = texShadow.SampleCmp(_texShadow, r8.xy, r8.z, int2(0, 0)).x;
  r8.xyzw = r5.xyzw + r10.xyzw;
  r8.xyz = r8.xyz / r8.www;
  r7.w = texShadow.SampleCmp(_texShadow, r8.xy, r8.z, int2(0, 0)).x;
  r5.xyzw = r5.xyzw + r11.xyzw;
  r5.xyz = r5.xyz / r5.www;
  r5.x = texShadow.SampleCmp(_texShadow, r5.xy, r5.z, int2(0, 0)).x;
  float3 spatchPbrNormal =
    r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  spatchPbrNormal = SPatchPBRSafeNormalize(spatchPbrNormal);
  float spatchPbrGeometricVariance =
    SPatchPBRGeometricVariance(spatchPbrNormal);
  [flatten]
  if (r2.y != 0) {
    r1.xyz = spatchPbrNormal;
    float3 spatchLightDirection = r4.xyz;
    r2.y = cmp(r6.w == 1.000000);
    r5.yzw = r6.xyz * r6.xyz;
    r5.yzw = r2.yyy ? r5.yzw : r6.www;
    float3 spatchF0 = r2.yyy ? r6.xyz : r6.www;
    float spatchMetallic = r2.y ? 1.0 : 0.0;
    r2.y = log2(r0.z);
    r2.y = cbDeferredLight.PositionAndRadius.w * r2.y;
    r2.y = exp2(r2.y);
    r2.y = 1 / r2.y;
    r0.z = saturate(-r0.z * cbDeferredLight.ColourAndInvRadiusSqr.w + r2.y);
    r2.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
    r2.y = r2.y ? 1.000000 : 0;
    r6.w = r7.x + -r0.z;
    r0.z = r2.y * r6.w + r0.z;
    r2.y = saturate(dot(r1.xyz, r4.xyz));
    r4.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r2.yyy;
    r4.xyz = r4.xyz * r0.zzz;
    r6.w = dot(-r2.xzw, -r2.xzw);
    r6.w = rsqrt(r6.w);
    r2.xzw = r6.www * -r2.xwz;
    r1.w = 10 * r1.w;
    r1.w = exp2(r1.w);
    float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
      r1.xyz, r2.xzw, spatchLightDirection, r1.w, spatchF0,
      spatchPbrGeometricVariance);
    float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
      r1.xyz, r2.xzw, spatchLightDirection, spatchF0, spatchMetallic);
    spatchDiffuseScale = lerp(
      float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength());
    r3.xyz = r3.xyz * r0.www + r2.xzw;
    r0.w = dot(r3.xyz, r3.xyz);
    r0.w = rsqrt(r0.w);
    r3.xyz = r3.xyz * r0.www;
    r0.w = saturate(dot(r1.xyz, r3.xyz));
    r0.w = log2(r0.w);
    r0.w = r1.w * r0.w;
    r0.w = exp2(r0.w);
    r1.x = -1 + r1.w;
    r0.w = r1.x * r0.w;
    r0.w = 0.125 * r0.w;
    r1.xyz = float3(50,50,50) * r5.yzw;
    r1.xyz = min(float3(1,1,1), r1.xyz);
    r1.xyz = saturate(r1.xyz + -r5.yzw);
    r1.w = saturate(dot(r2.xzw, r3.xyz));
    r1.w = 1 + -r1.w;
    r2.x = r1.w * r1.w;
    r2.x = r2.x * r2.x;
    r1.w = r2.x * r1.w;
    r1.xyz = r1.xyz * r1.www + r5.yzw;
    r1.xyz = r1.xyz * r0.www;
    r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
    r0.z = 1 + -r0.z;
    r0.z = r0.z * r0.z;
    r0.z = -r0.z * r0.z + 1;
    r1.xyz = r1.xyz * r0.zzz;
    r1.xyz = r1.xyz * r2.yyy;
    r1.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r1.xyz;
    r0.z = r7.y + r4.w;
    r0.z = r0.z + r7.z;
    r0.z = r0.z + r7.w;
    r0.z = r0.z + r5.x;
    r0.w = 0.200000003 * r0.z;
    r1.w = saturate(cbDeferredLight.Fov.z);
    r0.z = -r0.z * 0.200000003 + 1;
    r0.z = r1.w * r0.z + r0.w;
    r0.z = r3.w * r0.z;
    r2.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r0.xy, 0).xyzw;
    r0.xyz = r2.xxx * r0.zzz;
  } else {
    r0.xyz = float3(0,0,0);
    r4.xyz = float3(0,0,0);
    r1.xyz = float3(0,0,0);
  }
  r2.xyz = r4.xyz * (r6.xyz * spatchDiffuseScale);
  r3.xyz = r1.xyz * r3.www;
  r3.xyz = r3.xyz * r0.xyz;
  o0.xyz = r2.xyz * r0.xyz + r3.xyz;
  o0.w = dot(r1.xyz, float3(4,4,4));
  return;
}

#elif SPATCH_PBR_VARIANT == 9
// Native shader 0x66072A23.

cbuffer cbExternalViewTransform : register(b0)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b1)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texDepth : register(s1);
SamplerState _texNormal : register(s2);
SamplerState _texDistAtten : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texDepth : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texDistAtten : register(t3);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float3 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.zz;
  r0.zw = float2(0.5,0.5) + r0.zw;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r2.w = -r2.y;
  r0.x = dot(-r2.xzw, -r2.xzw);
  r0.x = rsqrt(r0.x);
  r1.xyz = -r2.xwz * r0.xxx;
  float3 spatchViewDirection = r1.xyz;
  r2.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.x = dot(r2.xyz, r2.xyz);
  r0.y = rsqrt(r0.x);
  r0.x = sqrt(r0.x);
  r3.xyz = r2.xyz * r0.yyy + r1.xyz;
  r2.xyz = r2.xyz * r0.yyy;
  float3 spatchLightDirection = r2.xyz;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = saturate(dot(r1.xyz, r3.xyz));
  r0.y = 1 + -r0.y;
  r1.x = r0.y * r0.y;
  r1.x = r1.x * r1.x;
  r0.y = r1.x * r0.y;
  r1.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r4.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r0.z = cmp(r1.w == 1.000000);
  r1.xyz = r1.xyz * r1.xyz;
  r5.xyz = r0.zzz ? r1.xyz : r1.www;
  float3 spatchF0 = r5.xyz;
  float spatchMetallic = r0.z ? 1.0 : 0.0;
  r6.xyz = float3(50,50,50) * r5.xyz;
  r6.xyz = min(float3(1,1,1), r6.xyz);
  r6.xyz = saturate(r6.xyz + -r5.xyz);
  r0.yzw = r6.xyz * r0.yyy + r5.xyz;
  r4.xyz = r4.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r1.w = 10 * r4.w;
  r1.w = exp2(r1.w);
  r2.w = dot(r4.xyz, r4.xyz);
  r2.w = rsqrt(r2.w);
  r4.xyz = r4.xyz * r2.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r4.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r4.xyz, spatchViewDirection, spatchLightDirection, r1.w, spatchF0,
    spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r4.xyz, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r2.w = saturate(dot(r4.xyz, r3.xyz));
  r2.x = saturate(dot(r4.xyz, r2.xyz));
  r2.y = log2(r2.w);
  r2.y = r2.y * r1.w;
  r1.w = -1 + r1.w;
  r2.y = exp2(r2.y);
  r1.w = r2.y * r1.w;
  r1.w = 0.125 * r1.w;
  r0.yzw = r1.www * r0.yzw;
  r0.yzw = lerp(r0.yzw, spatchPbrSpecular, SPatchPBRBlendStrength());
  r1.w = log2(r0.x);
  r1.w = cbDeferredLight.PositionAndRadius.w * r1.w;
  r1.w = exp2(r1.w);
  r1.w = 1 / r1.w;
  r1.w = saturate(-r0.x * cbDeferredLight.ColourAndInvRadiusSqr.w + r1.w);
  r3.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.x;
  r3.y = cbDeferredLight.Fov.w;
  r3.xy = saturate(r3.xy);
  r3.xyzw = texDistAtten.Sample(_texDistAtten, r3.xy).xyzw;
  r0.x = r3.x + -r1.w;
  r2.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
  r2.y = r2.y ? 1.000000 : 0;
  r0.x = r2.y * r0.x + r1.w;
  r1.w = 1 + -r0.x;
  r1.w = r1.w * r1.w;
  r1.w = -r1.w * r1.w + 1;
  r0.yzw = r1.www * r0.yzw;
  r0.yzw = r0.yzw * r2.xxx;
  r2.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r2.xxx;
  r2.xyz = r2.xyz * r0.xxx;
  r0.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yzw;
  o0.xyz = r2.xyz * (r1.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength())) + r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  return;
}

#elif SPATCH_PBR_VARIANT == 10
// Native shader 0x8A331B0F.

cbuffer cbEnvironmentSettings : register(b0)
{

  struct
  {
    float4 SunDir;
    float4 SunDirWorld;
    float4 SunColor;
    float4 AmbientColorHorizon;
    float4 ScaleAndHeight;
    float4 ScatterZenithColor;
    float4 ScatterHorizonColor;
    float4 ScatterGroundColor;
    float4 ScatterSunColor;
    float4 CharacterParams;
    float4 FogStartStopSky;
    float4 WindDirAndMag;
    float4 DisplayDebug;
    float4 LitWindowTimeOn;
    float4 Lighting;
    float4 SunScatterParams;
  } cbEnvironmentSettings : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbShaderParams : register(b2)
{

  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texNormal : register(s1);
SamplerState _texDepth : register(s2);
SamplerState _texAmbient2 : register(s3);
SamplerState _texCollector : register(s4);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texNormal : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture2D<float4> texAmbient2 : register(t3);
Texture2D<float4> texCollector : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r1.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.z = -r0.x;
  r0.x = saturate(r0.x / cbShaderParams.Value0.w);
  r0.x = 1 + -r0.x;
  r1.w = -r1.y;
  r0.y = dot(r1.xzw, r1.xzw);
  r0.y = rsqrt(r0.y);
  r2.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value0.xyz;
  r1.y = dot(r2.xyz, r2.xyz);
  r1.y = rsqrt(r1.y);
  r2.xyz = r2.xyz * r1.yyy;
  r3.xyz = r1.xwz * r0.yyy;
  r4.x = saturate(dot(-r3.xyz, r2.xyz));
  r5.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value1.xyz;
  r1.y = dot(r5.xyz, r5.xyz);
  r1.y = rsqrt(r1.y);
  r5.xyz = r5.xyz * r1.yyy;
  r4.y = saturate(dot(-r3.xyz, r5.xyz));
  r6.xyz = -r1.xwz * r0.yyy + cbShaderParams.Value2.xyz;
  r1.xyz = -r1.xwz * r0.yyy + v2.xyz;
  r0.y = dot(r6.xyz, r6.xyz);
  r0.y = rsqrt(r0.y);
  r6.xyz = r6.xyz * r0.yyy;
  r4.z = saturate(dot(-r3.xyz, r6.xyz));
  float3 spatchViewDirection = -r3.xyz;
  float3 spatchSunLight = v2.xyz;
  r4.xyz = float3(1,1,1) + -r4.xyz;
  r4.xyz = r4.xyz * r4.xyz;
  r4.xyz = r4.xyz * r4.xyz;
  r4.xyz = r4.xyz + r4.xyz;
  r7.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r0.y = cmp(r7.w == 1.000000);
  r7.xyz = r7.xyz * r7.xyz;
  r8.xyz = r0.yyy ? r7.xyz : r7.www;
  float3 spatchF0 = r8.xyz;
  float spatchMetallic = r0.y ? 1.0 : 0.0;
  r9.xyz = float3(50,50,50) * r8.xyz;
  r9.xyz = min(float3(1,1,1), r9.xyz);
  r9.xyz = saturate(r9.xyz + -r8.xyz);
  r10.xyz = r9.xyz * r4.yyy + r8.xyz;
  r11.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r11.xyz = r11.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.y = 7 * r11.w;
  r0.y = exp2(r0.y);
  r1.w = dot(r11.xyz, r11.xyz);
  r1.w = rsqrt(r1.w);
  r11.xyz = r11.xyz * r1.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r11.xyz);
  float3 spatchPbrSun = SPatchPBRDirectSpecular(
    r11.xyz, spatchViewDirection, spatchSunLight, r0.y, spatchF0,
    spatchPbrGeometricVariance);
  float3 spatchPbrSunDiffuseWeight = SPatchPBRDiffuseWeight(
    r11.xyz, spatchViewDirection, spatchSunLight, spatchF0, spatchMetallic);
  r1.w = saturate(dot(r11.xyz, r2.xyz));
  r2.x = log2(r1.w);
  r1.w = saturate(dot(r11.xyz, r5.xyz));
  r2.y = log2(r1.w);
  r1.w = saturate(dot(r11.xyz, r6.xyz));
  r2.z = log2(r1.w);
  r2.xyz = r2.xyz * r0.yyy;
  r2.xyz = exp2(r2.xyz);
  r5.xyz = r2.yyy * r10.xyz;
  r6.xyzw = texAmbient2.Sample(_texAmbient2, r0.zw).xyzw;
  r10.xyzw = texCollector.Sample(_texCollector, r0.zw).xyzw;
  r10.xyz = cbEnvironmentSettings.SunColor.xyz * r10.www;
  r6.xyz = cbShaderParams.Value1.www * r6.xyz;
  r6.xyz = r6.xyz * r6.xyz;
  r12.x = saturate(dot(r11.xyz, cbShaderParams.Value0.xyz));
  r12.y = saturate(dot(r11.xyz, cbShaderParams.Value1.xyz));
  r12.z = saturate(dot(r11.xyz, cbShaderParams.Value2.xyz));
  r6.xyz = r12.xyz * r6.xyz;
  r0.z = -1 + r0.y;
  r0.z = 0.125 * r0.z;
  r6.xyz = r6.xyz * r0.zzz;
  r6.xyz = float3(3,3,3) * r6.xyz;
  r5.xyz = r6.yyy * r5.xyz;
  r4.xyw = r9.xyz * r4.xxx + r8.xyz;
  r12.xyz = r9.xyz * r4.zzz + r8.xyz;
  r2.yzw = r12.xyz * r2.zzz;
  r4.xyz = r4.xyw * r2.xxx;
  r4.xyz = r4.xyz * r6.xxx + r5.xyz;
  r2.xyz = r2.yzw * r6.zzz + r4.xyz;
  r0.w = saturate(dot(r11.xyz, v2.xyz));
  r4.xyz = r0.www * r10.xyz;
  r4.xyz = r4.xyz * (r7.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrSunDiffuseWeight, SPatchPBRBlendStrength()));
  r2.xyz = r2.xyz * r0.xxx + r4.xyz;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r1.xyz = r1.xyz * r0.xxx;
  r0.x = saturate(dot(r11.xyz, r1.xyz));
  r1.x = saturate(dot(-r3.xyz, r1.xyz));
  r1.x = 1 + -r1.x;
  r0.x = log2(r0.x);
  r0.x = r0.y * r0.x;
  r0.x = exp2(r0.x);
  r0.y = r1.x * r1.x;
  r0.xy = r0.xy * r0.zy;
  r0.y = r1.x * r0.y;
  r1.xyz = r9.xyz * r0.yyy + r8.xyz;
  r0.xyz = r1.xyz * r0.xxx;
  r0.xyz = r0.xyz * r0.www;
  r0.xyz = lerp(r0.xyz, spatchPbrSun * r0.www, SPatchPBRBlendStrength());
  o0.xyz = r0.xyz * r10.xyz + r2.xyz;
  r0.xyz = r0.xyz * r10.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  return;
}

#elif SPATCH_PBR_VARIANT == 11
// Native shader 0xA30CEF48.

cbuffer cbExternalViewTransform : register(b0)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b1)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texDepth : register(s1);
SamplerState _texNormal : register(s2);
SamplerState _texDistAtten : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texDepth : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texDistAtten : register(t3);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float3 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.zz;
  r0.zw = float2(0.5,0.5) + r0.zw;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r2.w = -r2.y;
  r0.x = dot(-r2.xzw, -r2.xzw);
  r0.x = rsqrt(r0.x);
  r1.xyz = -r2.xwz * r0.xxx;
  float3 spatchViewDirection = r1.xyz;
  r2.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.x = dot(r2.xyz, r2.xyz);
  r0.y = rsqrt(r0.x);
  r0.x = sqrt(r0.x);
  r3.xyz = r2.xyz * r0.yyy + r1.xyz;
  r2.xyz = r2.xyz * r0.yyy;
  float3 spatchLightDirection = r2.xyz;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = saturate(dot(r1.xyz, r3.xyz));
  r0.y = 1 + -r0.y;
  r1.x = r0.y * r0.y;
  r1.x = r1.x * r1.x;
  r0.y = r1.x * r0.y;
  r1.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r4.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r0.z = cmp(r1.w == 1.000000);
  r1.xyz = r1.xyz * r1.xyz;
  r5.xyz = r0.zzz ? r1.xyz : r1.www;
  float3 spatchF0 = r5.xyz;
  float spatchMetallic = r0.z ? 1.0 : 0.0;
  r6.xyz = float3(50,50,50) * r5.xyz;
  r6.xyz = min(float3(1,1,1), r6.xyz);
  r6.xyz = saturate(r6.xyz + -r5.xyz);
  r0.yzw = r6.xyz * r0.yyy + r5.xyz;
  r4.xyz = r4.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r1.w = 10 * r4.w;
  r1.w = exp2(r1.w);
  r2.w = dot(r4.xyz, r4.xyz);
  r2.w = rsqrt(r2.w);
  r4.xyz = r4.xyz * r2.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r4.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r4.xyz, spatchViewDirection, spatchLightDirection, r1.w, spatchF0,
    spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r4.xyz, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r2.w = saturate(dot(r4.xyz, r3.xyz));
  r3.x = saturate(dot(r4.xyz, r2.xyz));
  r2.w = log2(r2.w);
  r2.w = r2.w * r1.w;
  r1.w = -1 + r1.w;
  r2.w = exp2(r2.w);
  r1.w = r2.w * r1.w;
  r1.w = 0.125 * r1.w;
  r0.yzw = r1.www * r0.yzw;
  r0.yzw = lerp(r0.yzw, spatchPbrSpecular, SPatchPBRBlendStrength());
  r1.w = log2(r0.x);
  r1.w = cbDeferredLight.PositionAndRadius.w * r1.w;
  r1.w = exp2(r1.w);
  r1.w = 1 / r1.w;
  r1.w = saturate(-r0.x * cbDeferredLight.ColourAndInvRadiusSqr.w + r1.w);
  r4.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.x;
  r4.y = cbDeferredLight.Fov.w;
  r4.xy = saturate(r4.xy);
  r4.xyzw = texDistAtten.Sample(_texDistAtten, r4.xy).xyzw;
  r0.x = r4.x + -r1.w;
  r2.w = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
  r2.w = r2.w ? 1.000000 : 0;
  r0.x = r2.w * r0.x + r1.w;
  r1.w = 1 + -r0.x;
  r1.w = r1.w * r1.w;
  r1.w = -r1.w * r1.w + 1;
  r0.yzw = r1.www * r0.yzw;
  r0.yzw = r0.yzw * r3.xxx;
  r3.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r3.xxx;
  r3.xyz = r3.xyz * r0.xxx;
  r0.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yzw;
  r1.xyz = r3.xyz * (r1.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength())) + r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  o0.xyz = r1.xyz;
  r0.x = dot(r1.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = r2.xyz * r0.xxx;
  o1.w = r0.x;
  return;
}

#elif SPATCH_PBR_VARIANT == 12
// Native shader 0xDCF9CD0C.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    row_major float4x4 WorldProjection;
    row_major float4x4 WorldViewInv;
    float4 CameraOffset;
    float4 CameraPosition;
    float4 Target;
  } cbViewTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b2)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b3)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s1);
SamplerState _texDiffuse : register(s2);
SamplerState _texNormal : register(s3);
SamplerState _texNoise : register(s4);
SamplerState _texDepth : register(s5);
SamplerState _texDistAtten : register(s6);
SamplerComparisonState _texShadow : register(s0);
Texture2D<float4> texShadow : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);
Texture2D<float4> texDiffuse : register(t2);
Texture2D<float4> texNormal : register(t3);
Texture2D<float4> texNoise : register(t4);
Texture2D<float4> texDepth : register(t5);
Texture2D<float4> texDistAtten : register(t6);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  float3 v3 : TEXCOORD2,
  float3 v4 : TEXCOORD3,
  float3 v5 : TEXCOORD4,
  float3 v6 : TEXCOORD5,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.xyz = v6.zxy + -v5.zxy;
  r3.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v5.yzx;
  r2.w = -r2.y;
  r3.xyz = r3.xyz + -r2.wzx;
  r4.xyz = r3.xyz * r1.xyz;
  r3.xyz = r1.zxy * r3.yzx + -r4.xyz;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r3.xyz = r3.xyz * r0.xxx;
  r0.x = dot(r3.xzy, r2.xzw);
  r0.y = dot(r3.xyz, v4.xyz);
  r0.x = r0.y + -r0.x;
  r4.xyz = v5.zxy + -v4.zxy;
  r0.y = dot(r3.zxy, r4.xyz);
  r0.x = saturate(-r0.x / r0.y);
  r0.y = 1 + -r0.x;
  r3.xyz = v4.zxy + -v3.zxy;
  r5.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v3.yzx;
  r5.xyz = r5.xyz + -r2.wzx;
  r6.xyz = r5.xyz * r3.xyz;
  r5.xyz = r3.zxy * r5.yzx + -r6.xyz;
  r1.w = dot(r5.xyz, r5.xyz);
  r1.w = rsqrt(r1.w);
  r5.xyz = r5.xyz * r1.www;
  r1.w = dot(r5.xzy, r2.xzw);
  r3.w = dot(r5.xyz, v6.xyz);
  r1.w = r3.w + -r1.w;
  r6.xyz = -v6.zxy + v3.zxy;
  r3.w = dot(r5.zxy, r6.xyz);
  r1.w = saturate(-r1.w / r3.w);
  r0.y = max(r1.w, r0.y);
  r1.w = 1 + -r1.w;
  r0.x = max(r1.w, r0.x);
  r5.xyz = r6.xyz * r0.yyy + v6.zxy;
  r7.xyz = r6.zxy * r0.yyy + v5.yzx;
  r8.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v6.yzx;
  r8.xyz = r8.xyz + -r2.wzx;
  r9.xyz = r8.xyz * r6.xyz;
  r6.xyz = r6.zxy * r8.yzx + -r9.xyz;
  r0.y = dot(r6.xyz, r6.xyz);
  r0.y = rsqrt(r0.y);
  r6.xyz = r6.xyz * r0.yyy;
  r0.y = dot(r6.xzy, r2.xzw);
  r1.w = dot(r6.xyz, v5.xyz);
  r3.w = dot(r6.zxy, r1.xyz);
  r0.y = r1.w + -r0.y;
  r0.y = saturate(-r0.y / r3.w);
  r1.w = 1 + -r0.y;
  r6.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v4.yzx;
  r6.xyz = r6.xyz + -r2.wzx;
  r8.xyz = r6.xyz * r4.xyz;
  r6.xyz = r4.zxy * r6.yzx + -r8.xyz;
  r3.w = dot(r6.xyz, r6.xyz);
  r3.w = rsqrt(r3.w);
  r6.xyz = r6.xyz * r3.www;
  r3.w = dot(r6.xzy, r2.xzw);
  r4.w = dot(r6.xyz, v3.xyz);
  r5.w = dot(r6.zxy, r3.xyz);
  r3.w = r4.w + -r3.w;
  r3.w = saturate(-r3.w / r5.w);
  r1.w = max(r3.w, r1.w);
  r3.w = 1 + -r3.w;
  r0.y = max(r3.w, r0.y);
  r5.xyz = r3.xyz * r1.www + r5.xyz;
  r6.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r6.xyz = r6.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r3.w = 10 * r6.w;
  r3.w = exp2(r3.w);
  r4.w = dot(r6.xyz, r6.xyz);
  r4.w = rsqrt(r4.w);
  r6.xyz = r6.xyz * r4.www;
  r4.w = dot(r6.zxy, r5.xyz);
  r5.w = dot(r6.xzy, r2.xzw);
  r4.w = -r5.w + r4.w;
  r4.w = max(0, -r4.w);
  r5.xyz = r6.zxy * r4.www + r5.xyz;
  r5.xyz = r5.xyz + -r2.zxw;
  r4.w = dot(r5.xyz, r5.xyz);
  r4.w = rsqrt(r4.w);
  r5.xyz = r5.xyz * r4.www;
  r8.xyz = r4.xyz * r0.xxx + v3.zxy;
  r4.xyz = r4.xyz * r0.xxx + v4.zxy;
  r4.xyz = r1.xyz * r0.yyy + r4.xyz;
  r1.xyz = r1.zxy * r0.yyy + r7.xyz;
  r3.xyz = r3.xyz * r1.www + r8.xyz;
  r0.x = dot(r6.zxy, r3.xyz);
  r0.x = r0.x + -r5.w;
  r0.x = max(0, -r0.x);
  r3.xyz = r6.zxy * r0.xxx + r3.xyz;
  r3.xyz = r3.xyz + -r2.zxw;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r3.xyz = r3.xyz * r0.xxx;
  r7.xyz = r5.xyz * r3.zxy;
  r7.xyz = r5.zxy * r3.xyz + -r7.xyz;
  r7.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r7.xyz;
  r0.x = dot(r7.xyz, r7.xyz);
  r0.x = rsqrt(r0.x);
  r7.xyz = r7.xyz * r0.xxx;
  r0.x = dot(r5.xyz, r3.xyz);
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.w = r1.w * r0.y;
  r4.w = r4.w * -2 + 3.14159274;
  r0.x = r0.x ? r4.w : 0;
  r0.x = r0.y * r1.w + r0.x;
  r7.xyz = r0.xxx * r7.xyz;
  r0.x = dot(r6.yzx, r1.xyz);
  r0.x = r0.x + -r5.w;
  r0.x = max(0, -r0.x);
  r1.xyz = r6.yzx * r0.xxx + r1.xyz;
  r1.xyz = r1.xyz + -r2.wzx;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r1.xyz = r1.xyz * r0.xxx;
  r8.xyz = r1.yzx * r5.zxy;
  r8.xyz = r1.xyz * r5.xyz + -r8.xyz;
  r0.x = dot(r1.yzx, r5.xyz);
  r5.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r8.xyz;
  r0.y = dot(r5.xyz, r5.xyz);
  r0.y = rsqrt(r0.y);
  r5.xyz = r5.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.w = r1.w * r0.y;
  r4.w = r4.w * -2 + 3.14159274;
  r0.x = r0.x ? r4.w : 0;
  r0.x = r0.y * r1.w + r0.x;
  r5.xyz = r0.xxx * r5.xyz + r7.xyz;
  r0.x = dot(r6.zxy, r4.xyz);
  r0.x = r0.x + -r5.w;
  r0.x = max(0, -r0.x);
  r4.xyz = r6.zxy * r0.xxx + r4.xyz;
  r4.xyz = r4.xyz + -r2.zxw;
  r0.x = dot(r4.xyz, r4.xyz);
  r0.x = rsqrt(r0.x);
  r4.xyz = r4.xyz * r0.xxx;
  r7.xyz = r4.zxy * r3.xyz;
  r7.xyz = r3.zxy * r4.xyz + -r7.xyz;
  r0.x = dot(r3.xyz, r4.xyz);
  r3.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r7.xyz;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.w = r1.w * r0.y;
  r4.w = r4.w * -2 + 3.14159274;
  r0.x = r0.x ? r4.w : 0;
  r0.x = r0.y * r1.w + r0.x;
  r3.xyz = r0.xxx * r3.xyz + r5.xyz;
  r5.xyz = r4.xyz * r1.xyz;
  r5.xyz = r4.zxy * r1.yzx + -r5.xyz;
  r0.x = dot(r4.zxy, r1.xyz);
  r1.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r5.xyz;
  r0.y = dot(r1.xyz, r1.xyz);
  r0.y = rsqrt(r0.y);
  r1.xyz = r1.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r1.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r1.w = sqrt(r1.w);
  r4.x = r1.w * r0.y;
  r4.x = r4.x * -2 + 3.14159274;
  r0.x = r0.x ? r4.x : 0;
  r0.x = r0.y * r1.w + r0.x;
  r1.xyz = r0.xxx * r1.xyz + r3.xyz;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r0.y = dot(-r2.xzw, -r2.xzw);
  r0.y = rsqrt(r0.y);
  r3.xyz = -r2.xwz * r0.yyy;
  float3 spatchViewDirection = r3.xyz;
  float3 spatchLightDirection = r1.xyz * r0.xxx;
  float spatchNativeExponent = r3.w;
  r4.xyz = r1.xyz * r0.xxx + r3.xyz;
  r0.x = dot(r1.xyz, r6.xyz);
  r0.x = max(0, r0.x);
  r0.y = dot(r4.xyz, r4.xyz);
  r0.y = rsqrt(r0.y);
  r1.xyz = r4.xyz * r0.yyy;
  r0.y = saturate(dot(r6.xyz, r1.xyz));
  r1.x = saturate(dot(r3.xyz, r1.xyz));
  r1.x = 1 + -r1.x;
  r0.y = log2(r0.y);
  r0.y = r3.w * r0.y;
  r1.y = -1 + r3.w;
  r0.y = exp2(r0.y);
  r0.y = r1.y * r0.y;
  r0.xy = float2(0.159154907,0.125) * r0.xy;
  r1.y = r1.x * r1.x;
  r1.y = r1.y * r1.y;
  r1.x = r1.x * r1.y;
  r3.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r1.y = cmp(r3.w == 1.000000);
  float spatchMetallic = r1.y ? 1.0 : 0.0;
  r3.xyz = r3.xyz * r3.xyz;
  r4.xyz = r3.xyz * r3.xyz;
  r1.yzw = r1.yyy ? r4.xyz : r3.www;
  float3 spatchF0 = spatchMetallic ? r3.xyz : r3.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r6.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchNativeExponent,
    spatchF0, spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r4.xyz = float3(50,50,50) * r1.yzw;
  r4.xyz = min(float3(1,1,1), r4.xyz);
  r4.xyz = saturate(r4.xyz + -r1.yzw);
  r1.xyz = r4.xyz * r1.xxx + r1.yzw;
  r1.xyz = r1.xyz * r0.yyy;
  r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
  r0.y = 1 / cbDeferredLight.ColourAndInvRadiusSqr.w;
  r0.y = -cbDeferredLight.WidthHeightNearFar.z + r0.y;
  r4.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r1.w = dot(r4.xyz, r4.xyz);
  r1.w = sqrt(r1.w);
  r3.w = -cbDeferredLight.WidthHeightNearFar.z + r1.w;
  r4.x = saturate(r3.w / r0.y);
  r4.y = saturate(cbDeferredLight.Fov.w);
  r4.xyzw = texDistAtten.Sample(_texDistAtten, r4.xy).xyzw;
  r0.y = log2(r1.w);
  r0.y = cbDeferredLight.PositionAndRadius.w * r0.y;
  r0.y = exp2(r0.y);
  r0.y = 1 / r0.y;
  r0.y = saturate(-r1.w * cbDeferredLight.ColourAndInvRadiusSqr.w + r0.y);
  r1.w = r4.x + -r0.y;
  r3.w = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
  r3.w = r3.w ? 1.000000 : 0;
  r0.y = r3.w * r1.w + r0.y;
  r1.w = 1 + -r0.y;
  r1.w = r1.w * r1.w;
  r1.w = -r1.w * r1.w + 1;
  r1.xyz = r1.xyz * r1.www;
  r1.xyz = r1.xyz * r0.xxx;
  r4.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xxx;
  r4.xyz = r4.xyz * r0.yyy;
  r3.xyz = r4.xyz * (r3.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength()));
  r1.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r1.xyz;
  r0.x = cbDeferredLight.WidthHeightNearFar.z + cbDeferredLight.WidthHeightNearFar.w;
  r4.xyz = v2.xyz * r0.xxx + cbDeferredLight.PositionAndRadius.xyz;
  r4.xyz = -r4.xyz + r2.xwz;
  r0.x = dot(r4.xyz, v2.xyz);
  r0.x = cmp(1.00000001e-007 >= r0.x);
  r4.xyz = v2.xyz * cbDeferredLight.WidthHeightNearFar.zzz + cbDeferredLight.PositionAndRadius.xyz;
  r4.xyz = -r4.xyz + r2.xwz;
  r0.y = dot(r4.xyz, v2.xyz);
  r0.y = cmp(r0.y >= 1.00000001e-007);
  r0.xy = r0.xy ? float2(1,1) : 0;
  r0.x = r0.x * r0.y;
  r4.xyzw = cbShadowTransform.ViewShadow[0]._m10_m11_m12_m13 * -r2.yyyy;
  r4.xyzw = r2.xxxx * cbShadowTransform.ViewShadow[0]._m00_m01_m02_m03 + r4.xyzw;
  r2.xyzw = r2.zzzz * cbShadowTransform.ViewShadow[0]._m20_m21_m22_m23 + r4.xyzw;
  r2.xyzw = cbShadowTransform.ViewShadow[0]._m30_m31_m32_m33 + r2.xyzw;
  r0.y = -9.99999975e-005 + r2.z;
  r0.y = r0.y / r2.w;
  r4.xy = r2.xy / r2.ww;
  r0.y = texShadow.SampleCmp(_texShadow, r4.xy, r0.y, int2(0, 0)).x;
  r4.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r4.xy, 0).xyzw;
  r4.xyz = r4.xyz * r4.xyz;
  r5.xy = float2(1,1) / cbViewTransform.Target.xy;
  r0.zw = r5.xy * r0.zw;
  r0.zw = float2(0.03125,0.03125) * r0.zw;
  r5.xyzw = texNoise.Sample(_texNoise, r0.zw).xyzw;
  r0.zw = float2(0.5,0.5) + r5.xy;
  r1.w = 0.00079999998 * r2.w;
  r2.xyzw = float4(0,0,-9.99999975e-005,0) + r2.xyzw;
  r5.xy = r1.ww * r0.zw;
  r6.xy = float2(-1,1) * r5.xy;
  r6.zw = float2(0,0);
  r6.xyzw = r6.xyzw + r2.xyzw;
  r6.xyz = r6.xyz / r6.www;
  r0.z = texShadow.SampleCmp(_texShadow, r6.xy, r6.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r5.zw = float2(0,0);
  r6.xyzw = r5.xyzw + r2.xyzw;
  r6.xyz = r6.xyz / r6.www;
  r0.z = texShadow.SampleCmp(_texShadow, r6.xy, r6.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r6.xy = float2(-1,-1) * r5.xy;
  r5.xy = float2(1,-1) * r5.xy;
  r6.zw = float2(0,0);
  r6.xyzw = r6.xyzw + r2.xyzw;
  r6.xyz = r6.xyz / r6.www;
  r0.z = texShadow.SampleCmp(_texShadow, r6.xy, r6.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r5.zw = float2(0,0);
  r2.xyzw = r5.xyzw + r2.xyzw;
  r2.xyz = r2.xyz / r2.www;
  r0.z = texShadow.SampleCmp(_texShadow, r2.xy, r2.z, int2(0, 0)).x;
  r0.y = r0.y + r0.z;
  r0.z = 0.200000003 * r0.y;
  r0.y = -r0.y * 0.200000003 + 1;
  r0.w = saturate(cbDeferredLight.Fov.z);
  r0.y = r0.w * r0.y + r0.z;
  r0.x = r0.x * r0.y;
  r0.xyz = r0.xxx * r4.xyz;
  r2.xyz = r1.xyz * r0.xyz;
  o0.w = dot(r1.xyz, float3(4,4,4));
  o0.xyz = r3.xyz * r0.xyz + r2.xyz;
  return;
}

#elif SPATCH_PBR_VARIANT == 13
// Native shader 0xD71D285B.

cbuffer cbEnvironmentSettings : register(b0)
{

  struct
  {
    float4 SunDir;
    float4 SunDirWorld;
    float4 SunColor;
    float4 AmbientColorHorizon;
    float4 ScaleAndHeight;
    float4 ScatterZenithColor;
    float4 ScatterHorizonColor;
    float4 ScatterGroundColor;
    float4 ScatterSunColor;
    float4 CharacterParams;
    float4 FogStartStopSky;
    float4 WindDirAndMag;
    float4 DisplayDebug;
    float4 LitWindowTimeOn;
    float4 Lighting;
    float4 SunScatterParams;
  } cbEnvironmentSettings : packoffset(c0);

}

cbuffer cbShadowTransform : register(b1)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

SamplerState _texDiffuse : register(s0);
SamplerState _texNormal : register(s1);
SamplerState _texDepth : register(s2);
SamplerState _texVolume0 : register(s3);
SamplerState _texVolume1 : register(s4);
SamplerState _texVolume2 : register(s5);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texNormal : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture3D<float4> texVolume0 : register(t3);
Texture3D<float4> texVolume1 : register(t4);
Texture3D<float4> texVolume2 : register(t5);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = v2.w * v2.z;
  r0.y = v2.w + -v2.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + v2.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = v2.xy * r1.xy;
  r1.xyz = cbShadowTransform.ViewShadow[0]._m10_m11_m12 * -r2.yyy;
  r1.xyz = r2.xxx * cbShadowTransform.ViewShadow[0]._m00_m01_m02 + r1.xyz;
  r1.xyz = r2.zzz * cbShadowTransform.ViewShadow[0]._m20_m21_m22 + r1.xyz;
  r1.xyz = cbShadowTransform.ViewShadow[0]._m30_m31_m32 + r1.xyz;
  r3.xyz = cbShadowTransform.ViewShadow[0]._m30_m31_m32 + -r1.xyz;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r4.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r5.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r0.yzw = r4.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r1.w = 10 * r4.w;
  r1.w = exp2(r1.w);
  r3.w = dot(r0.yzw, r0.yzw);
  r3.w = rsqrt(r3.w);
  r0.yzw = r3.www * r0.yzw;
  r4.xyz = cbShadowTransform.ViewShadow[1]._m10_m11_m12 * r0.zzz;
  r4.xyz = r0.yyy * cbShadowTransform.ViewShadow[1]._m00_m01_m02 + r4.xyz;
  r4.xyz = r0.www * cbShadowTransform.ViewShadow[1]._m20_m21_m22 + r4.xyz;
  r3.w = dot(r4.xyz, r4.xyz);
  r3.w = rsqrt(r3.w);
  r4.xyz = r4.xyz * r3.www;
  r1.xyz = r4.xyz * cbShadowTransform.Biases.xyz + r1.xyz;
  r6.xyzw = texVolume1.Sample(_texVolume1, r1.xyz).xyzw;
  r7.xyzw = texVolume2.Sample(_texVolume2, r1.xyz).xyzw;
  r8.xyzw = texVolume0.Sample(_texVolume0, r1.xyz).xyzw;
  r1.xyz = r8.xyz * r8.xyz;
  r1.xyz = cbEnvironmentSettings.DisplayDebug.yyy * r1.xyz;
  r8.xyz = -r7.xyz + r6.xyz;
  r3.w = dot(r8.xyz, r8.xyz);
  r3.w = rsqrt(r3.w);
  r8.xyz = r8.xyz * r3.www;
  float3 spatchNormal = SPatchPBRSafeNormalize(r4.xyz);
  float3 spatchViewDirection = r3.xyz * r0.xxx;
  float3 spatchLightDirection = r8.xyz;
  float3 spatchLightColor = r1.xyz;
  r3.xyz = r3.xyz * r0.xxx + r8.xyz;
  r0.x = saturate(dot(r8.xyz, r4.xyz));
  float spatchNoL = r0.x;
  r3.w = dot(r3.xyz, r3.xyz);
  r3.w = rsqrt(r3.w);
  r3.xyz = r3.xyz * r3.www;
  r3.x = saturate(dot(r3.xyz, r4.xyz));
  r3.x = log2(r3.x);
  r3.y = 0.25 * r1.w;
  // This volume-light shader raises NoH to n/4, unlike the other direct
  // families. Its proven effective exponent is therefore the roughness input.
  float spatchNativeExponent = r3.y;
  r1.w = r1.w * 0.25 + 2;
  r3.x = r3.y * r3.x;
  r3.x = exp2(r3.x);
  r1.w = r3.x * r1.w;
  r1.w = 0.125 * r1.w;
  r3.xyz = r1.xyz * r1.www;
  r3.xyz = r3.xyz * r0.xxx;
  r2.w = -r2.y;
  r0.x = dot(-r2.xzw, -r2.xzw);
  r0.x = rsqrt(r0.x);
  r2.xyz = -r2.xwz * r0.xxx;
  r0.x = saturate(dot(r2.xyz, r0.yzw));
  r0.x = 1 + -r0.x;
  r0.y = r0.x * r0.x;
  r0.y = r0.y * r0.y;
  r0.x = r0.x * r0.y;
  r0.y = cmp(r5.w == 1.000000);
  float spatchMetallic = r0.y ? 1.0 : 0.0;
  r2.xyz = r5.xyz * r5.xyz;
  r5.xyz = r2.xyz * r2.xyz;
  r0.yzw = r0.yyy ? r5.xyz : r5.www;
  float3 spatchF0 = spatchMetallic ? r2.xyz : r5.www;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(spatchNormal);
  float3 spatchPbrLitSpecular = spatchLightColor * spatchNoL * SPatchPBRDirectSpecularUnitGrazing(
    spatchNormal, spatchViewDirection, spatchLightDirection, spatchNativeExponent,
    spatchF0, spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeightUnitGrazing(
    spatchNormal, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r5.xyz = float3(1,1,1) + -r0.yzw;
  r0.xyz = r5.xyz * r0.xxx + r0.yzw;
  r0.xyz = r3.xyz * r0.xyz;
  r0.xyz = lerp(r0.xyz, spatchPbrLitSpecular, SPatchPBRBlendStrength());
  r3.xyz = cmp(float3(0,0,0) < r4.xyz);
  r5.xyz = cmp(r4.xyz < float3(0,0,0));
  r4.xyz = r4.xyz * r4.xyz;
  r3.xyz = (int3)-r3.xyz + (int3)r5.xyz;
  r3.xyz = (int3)r3.xyz;
  r3.xyz = r4.xyz * r3.xyz;
  r4.xyz = saturate(r3.xyz);
  r3.xyz = saturate(-r3.xyz);
  r0.w = dot(r7.xyz, r3.xyz);
  r1.w = dot(r6.xyz, r4.xyz);
  r0.w = r1.w + r0.w;
  r1.xyz = r1.xyz * r0.www;
  r1.xyz = r1.xyz * (r2.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength())) + r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  r0.xyz = cbEnvironmentSettings.Lighting.xxx * r1.xyz;
  o0.xyz = float3(1.5,1.5,1.5) * r0.xyz;
  return;
}

#elif SPATCH_PBR_VARIANT == 14
// Native shader 0xE5E2CE1C.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    row_major float4x4 WorldProjection;
    row_major float4x4 WorldViewInv;
    float4 CameraOffset;
    float4 CameraPosition;
    float4 Target;
  } cbViewTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b2)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b3)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s1);
SamplerState _texDiffuse : register(s2);
SamplerState _texNormal : register(s3);
SamplerState _texNoise : register(s4);
SamplerState _texDepth : register(s5);
SamplerState _texDistAtten : register(s6);
SamplerComparisonState _texShadow : register(s0);
Texture2D<float4> texShadow : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);
Texture2D<float4> texDiffuse : register(t2);
Texture2D<float4> texNormal : register(t3);
Texture2D<float4> texNoise : register(t4);
Texture2D<float4> texDepth : register(t5);
Texture2D<float4> texDistAtten : register(t6);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v1.xy / v1.ww;
  r1.xyzw = texNormal.Sample(_texNormal, r0.xy).xyzw;
  r0.zw = saturate(r0.xy);
  r2.xyzw = texDepth.Sample(_texDepth, r0.zw).xyzw;
  r0.z = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = -r2.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.z = r0.z / r0.w;
  r2.xy = r0.xy * float2(2,2) + float2(-1,-1);
  r2.xy = r2.xy * r0.zz;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r2.xy;
  r2.z = -r0.z;
  r2.w = -r2.y;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.z = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.z);
  r4.xyz = r3.xyz * r0.www;
  r3.w = dot(v2.xyz, r4.xyz);
  r3.w = -cbDeferredLight.Fov.y + r3.w;
  r4.w = cbDeferredLight.Fov.x + -cbDeferredLight.Fov.y;
  r3.w = saturate(r3.w / r4.w);
  r5.xyzw = cbShadowTransform.ViewShadow[0]._m10_m11_m12_m13 * -r2.yyyy;
  r5.xyzw = r2.xxxx * cbShadowTransform.ViewShadow[0]._m00_m01_m02_m03 + r5.xyzw;
  r5.xyzw = r2.zzzz * cbShadowTransform.ViewShadow[0]._m20_m21_m22_m23 + r5.xyzw;
  r5.xyzw = cbShadowTransform.ViewShadow[0]._m30_m31_m32_m33 + r5.xyzw;
  r6.xyzw = texDiffuse.Sample(_texDiffuse, r0.xy).xyzw;
  r6.xyz = r6.xyz * r6.xyz;
  float3 spatchDiffuseScale = float3(1.0, 1.0, 1.0);
  r2.y = cmp(0 < r3.w);
  r0.z = sqrt(r0.z);
  r7.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.z;
  r7.y = cbDeferredLight.Fov.w;
  r7.xy = saturate(r7.xy);
  r7.xyzw = texDistAtten.Sample(_texDistAtten, r7.xy).xyzw;
  r4.w = -9.99999975e-005 + r5.z;
  r7.yz = float2(1,1) / cbViewTransform.Target.xy;
  r0.xy = r7.yz * r0.xy;
  r0.xy = float2(0.03125,0.03125) * r0.xy;
  r8.xyzw = texNoise.Sample(_texNoise, r0.xy).xyzw;
  r0.xy = float2(0.5,0.5) + r8.xy;
  r7.y = 0.00079999998 * r5.w;
  r8.xy = r7.yy * r0.xy;
  r9.xy = float2(-1,1) * r8.xy;
  r9.zw = float2(0,0);
  r8.zw = float2(0,0);
  r10.xy = float2(-1,-1) * r8.xy;
  r10.zw = float2(0,0);
  r11.xy = float2(1,-1) * r8.xy;
  r11.zw = float2(0,0);
  r0.xy = r5.xy / r5.ww;
  r4.w = r4.w / r5.w;
  r4.w = texShadow.SampleCmp(_texShadow, r0.xy, r4.w, int2(0, 0)).x;
  r5.xyzw = float4(0,0,-9.99999975e-005,0) + r5.xyzw;
  r9.xyzw = r5.xyzw + r9.xyzw;
  r7.yzw = r9.xyz / r9.www;
  r7.y = texShadow.SampleCmp(_texShadow, r7.yz, r7.w, int2(0, 0)).x;
  r8.xyzw = r5.xyzw + r8.xyzw;
  r8.xyz = r8.xyz / r8.www;
  r7.z = texShadow.SampleCmp(_texShadow, r8.xy, r8.z, int2(0, 0)).x;
  r8.xyzw = r5.xyzw + r10.xyzw;
  r8.xyz = r8.xyz / r8.www;
  r7.w = texShadow.SampleCmp(_texShadow, r8.xy, r8.z, int2(0, 0)).x;
  r5.xyzw = r5.xyzw + r11.xyzw;
  r5.xyz = r5.xyz / r5.www;
  r5.x = texShadow.SampleCmp(_texShadow, r5.xy, r5.z, int2(0, 0)).x;
  float3 spatchPbrNormal =
    r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  spatchPbrNormal = SPatchPBRSafeNormalize(spatchPbrNormal);
  float spatchPbrGeometricVariance =
    SPatchPBRGeometricVariance(spatchPbrNormal);
  [flatten]
  if (r2.y != 0) {
    r1.xyz = spatchPbrNormal;
    float3 spatchLightDirection = r4.xyz;
    r2.y = cmp(r6.w == 1.000000);
    r5.yzw = r6.xyz * r6.xyz;
    r5.yzw = r2.yyy ? r5.yzw : r6.www;
    float3 spatchF0 = r2.yyy ? r6.xyz : r6.www;
    float spatchMetallic = r2.y ? 1.0 : 0.0;
    r2.y = log2(r0.z);
    r2.y = cbDeferredLight.PositionAndRadius.w * r2.y;
    r2.y = exp2(r2.y);
    r2.y = 1 / r2.y;
    r0.z = saturate(-r0.z * cbDeferredLight.ColourAndInvRadiusSqr.w + r2.y);
    r2.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
    r2.y = r2.y ? 1.000000 : 0;
    r6.w = r7.x + -r0.z;
    r0.z = r2.y * r6.w + r0.z;
    r2.y = saturate(dot(r1.xyz, r4.xyz));
    r8.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r2.yyy;
    r8.xyz = r8.xyz * r0.zzz;
    r6.w = dot(-r2.xzw, -r2.xzw);
    r6.w = rsqrt(r6.w);
    r2.xzw = r6.www * -r2.xwz;
    r1.w = 10 * r1.w;
    r1.w = exp2(r1.w);
    float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
      r1.xyz, r2.xzw, spatchLightDirection, r1.w, spatchF0,
      spatchPbrGeometricVariance);
    float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
      r1.xyz, r2.xzw, spatchLightDirection, spatchF0, spatchMetallic);
    spatchDiffuseScale = lerp(
      float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength());
    r3.xyz = r3.xyz * r0.www + r2.xzw;
    r0.w = dot(r3.xyz, r3.xyz);
    r0.w = rsqrt(r0.w);
    r3.xyz = r3.xyz * r0.www;
    r0.w = saturate(dot(r1.xyz, r3.xyz));
    r0.w = log2(r0.w);
    r0.w = r1.w * r0.w;
    r0.w = exp2(r0.w);
    r1.x = -1 + r1.w;
    r0.w = r1.x * r0.w;
    r0.w = 0.125 * r0.w;
    r1.xyz = float3(50,50,50) * r5.yzw;
    r1.xyz = min(float3(1,1,1), r1.xyz);
    r1.xyz = saturate(r1.xyz + -r5.yzw);
    r1.w = saturate(dot(r2.xzw, r3.xyz));
    r1.w = 1 + -r1.w;
    r2.x = r1.w * r1.w;
    r2.x = r2.x * r2.x;
    r1.w = r2.x * r1.w;
    r1.xyz = r1.xyz * r1.www + r5.yzw;
    r1.xyz = r1.xyz * r0.www;
    r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
    r0.z = 1 + -r0.z;
    r0.z = r0.z * r0.z;
    r0.z = -r0.z * r0.z + 1;
    r1.xyz = r1.xyz * r0.zzz;
    r1.xyz = r1.xyz * r2.yyy;
    r1.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r1.xyz;
    r0.z = r7.y + r4.w;
    r0.z = r0.z + r7.z;
    r0.z = r0.z + r7.w;
    r0.z = r0.z + r5.x;
    r0.w = 0.200000003 * r0.z;
    r1.w = saturate(cbDeferredLight.Fov.z);
    r0.z = -r0.z * 0.200000003 + 1;
    r0.z = r1.w * r0.z + r0.w;
    r0.z = r3.w * r0.z;
    r2.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r0.xy, 0).xyzw;
    r0.xyz = r2.xxx * r0.zzz;
  } else {
    r0.xyz = float3(0,0,0);
    r8.xyz = float3(0,0,0);
    r1.xyz = float3(0,0,0);
    r4.xyz = float3(0,0,0);
  }
  r2.xyz = r8.xyz * (r6.xyz * spatchDiffuseScale);
  r3.xyz = r1.xyz * r3.www;
  r3.xyz = r3.xyz * r0.xyz;
  r0.xyz = r2.xyz * r0.xyz + r3.xyz;
  o0.w = dot(r1.xyz, float3(4,4,4));
  r0.w = dot(r0.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = r4.xyz * r0.www;
  o0.xyz = r0.xyz;
  o1.w = r0.w;
  return;
}

#elif SPATCH_PBR_VARIANT == 15
// Native shader 0xEFD8577D.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b2)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s0);
SamplerState _texDiffuse : register(s1);
SamplerState _texNormal : register(s2);
SamplerState _texDepth : register(s3);
SamplerState _texDistAtten : register(s4);
Texture2D<float4> texDiffuse2 : register(t0);
Texture2D<float4> texDiffuse : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texDepth : register(t3);
Texture2D<float4> texDistAtten : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  float3 v3 : TEXCOORD2,
  float3 v4 : TEXCOORD3,
  float3 v5 : TEXCOORD4,
  float3 v6 : TEXCOORD5,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.y = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.zw = v1.xy / v1.ww;
  r1.xy = saturate(r0.zw);
  r1.xyzw = texDepth.Sample(_texDepth, r1.xy).xyzw;
  r0.y = -r1.x * r0.y + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.x = r0.x / r0.y;
  r1.xy = r0.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r0.xx;
  r2.z = -r0.x;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r1.xy;
  r1.xyz = v6.zxy + -v5.zxy;
  r3.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v5.yzx;
  r2.w = -r2.y;
  r3.xyz = r3.xyz + -r2.wzx;
  r4.xyz = r3.xyz * r1.xyz;
  r3.xyz = r1.zxy * r3.yzx + -r4.xyz;
  r0.x = dot(r3.xyz, r3.xyz);
  r0.x = rsqrt(r0.x);
  r3.xyz = r3.xyz * r0.xxx;
  r0.x = dot(r3.xzy, r2.xzw);
  r0.y = dot(r3.xyz, v4.xyz);
  r0.x = r0.y + -r0.x;
  r4.xyz = v5.zxy + -v4.zxy;
  r0.y = dot(r3.zxy, r4.xyz);
  r0.x = saturate(-r0.x / r0.y);
  r0.y = 1 + -r0.x;
  r3.xyz = v4.zxy + -v3.zxy;
  r5.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v3.yzx;
  r5.xyz = r5.xyz + -r2.wzx;
  r6.xyz = r5.xyz * r3.xyz;
  r5.xyz = r3.zxy * r5.yzx + -r6.xyz;
  r1.w = dot(r5.xyz, r5.xyz);
  r1.w = rsqrt(r1.w);
  r5.xyz = r5.xyz * r1.www;
  r1.w = dot(r5.xzy, r2.xzw);
  r3.w = dot(r5.xyz, v6.xyz);
  r1.w = r3.w + -r1.w;
  r6.xyz = -v6.zxy + v3.zxy;
  r3.w = dot(r5.zxy, r6.xyz);
  r1.w = saturate(-r1.w / r3.w);
  r0.y = max(r1.w, r0.y);
  r1.w = 1 + -r1.w;
  r0.x = max(r1.w, r0.x);
  r5.xyz = r6.xyz * r0.yyy + v6.zxy;
  r7.xyz = r6.zxy * r0.yyy + v5.yzx;
  r8.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v6.yzx;
  r8.xyz = r8.xyz + -r2.wzx;
  r9.xyz = r8.xyz * r6.xyz;
  r6.xyz = r6.zxy * r8.yzx + -r9.xyz;
  r0.y = dot(r6.xyz, r6.xyz);
  r0.y = rsqrt(r0.y);
  r6.xyz = r6.xyz * r0.yyy;
  r0.y = dot(r6.xzy, r2.xzw);
  r1.w = dot(r6.xyz, v5.xyz);
  r3.w = dot(r6.zxy, r1.xyz);
  r0.y = r1.w + -r0.y;
  r0.y = saturate(-r0.y / r3.w);
  r1.w = 1 + -r0.y;
  r6.xyz = v2.yzx * cbDeferredLight.WidthHeightNearFar.zzz + v4.yzx;
  r6.xyz = r6.xyz + -r2.wzx;
  r8.xyz = r6.xyz * r4.xyz;
  r6.xyz = r4.zxy * r6.yzx + -r8.xyz;
  r3.w = dot(r6.xyz, r6.xyz);
  r3.w = rsqrt(r3.w);
  r6.xyz = r6.xyz * r3.www;
  r3.w = dot(r6.xzy, r2.xzw);
  r4.w = dot(r6.xyz, v3.xyz);
  r5.w = dot(r6.zxy, r3.xyz);
  r3.w = r4.w + -r3.w;
  r3.w = saturate(-r3.w / r5.w);
  r1.w = max(r3.w, r1.w);
  r3.w = 1 + -r3.w;
  r0.y = max(r3.w, r0.y);
  r5.xyz = r3.xyz * r1.www + r5.xyz;
  r6.xyzw = texNormal.Sample(_texNormal, r0.zw).xyzw;
  r8.xyzw = texDiffuse.Sample(_texDiffuse, r0.zw).xyzw;
  r6.xyz = r6.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.z = 10 * r6.w;
  r0.z = exp2(r0.z);
  r0.w = dot(r6.xyz, r6.xyz);
  r0.w = rsqrt(r0.w);
  r6.xyz = r6.xyz * r0.www;
  r0.w = dot(r6.zxy, r5.xyz);
  r3.w = dot(r6.xzy, r2.xzw);
  r0.w = -r3.w + r0.w;
  r0.w = max(0, -r0.w);
  r5.xyz = r6.zxy * r0.www + r5.xyz;
  r5.xyz = r5.xyz + -r2.zxw;
  r0.w = dot(r5.xyz, r5.xyz);
  r0.w = rsqrt(r0.w);
  r5.xyz = r5.xyz * r0.www;
  r9.xyz = r4.xyz * r0.xxx + v3.zxy;
  r4.xyz = r4.xyz * r0.xxx + v4.zxy;
  r4.xyz = r1.xyz * r0.yyy + r4.xyz;
  r0.xyw = r1.zxy * r0.yyy + r7.xyz;
  r1.xyz = r3.xyz * r1.www + r9.xyz;
  r1.w = dot(r6.zxy, r1.xyz);
  r1.w = r1.w + -r3.w;
  r1.w = max(0, -r1.w);
  r1.xyz = r6.zxy * r1.www + r1.xyz;
  r1.xyz = r1.xyz + -r2.zxw;
  r1.w = dot(r1.xyz, r1.xyz);
  r1.w = rsqrt(r1.w);
  r1.xyz = r1.xyz * r1.www;
  r3.xyz = r5.xyz * r1.zxy;
  r3.xyz = r5.zxy * r1.xyz + -r3.xyz;
  r3.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r3.xyz;
  r1.w = dot(r3.xyz, r3.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r3.xyz * r1.www;
  r1.w = dot(r5.xyz, r1.xyz);
  r4.w = abs(r1.w) * -0.0187292993 + 0.0742610022;
  r4.w = r4.w * abs(r1.w) + -0.212114394;
  r4.w = r4.w * abs(r1.w) + 1.57072878;
  r5.w = 1 + -abs(r1.w);
  r1.w = cmp(r1.w < -r1.w);
  r5.w = sqrt(r5.w);
  r6.w = r5.w * r4.w;
  r6.w = r6.w * -2 + 3.14159274;
  r1.w = r1.w ? r6.w : 0;
  r1.w = r4.w * r5.w + r1.w;
  r3.xyz = r1.www * r3.xyz;
  r1.w = dot(r6.yzx, r0.xyw);
  r1.w = r1.w + -r3.w;
  r1.w = max(0, -r1.w);
  r0.xyw = r6.yzx * r1.www + r0.xyw;
  r0.xyw = r0.xyw + -r2.wzx;
  r1.w = dot(r0.xyw, r0.xyw);
  r1.w = rsqrt(r1.w);
  r0.xyw = r1.www * r0.xyw;
  r7.xyz = r0.ywx * r5.zxy;
  r7.xyz = r0.xyw * r5.xyz + -r7.xyz;
  r1.w = dot(r0.ywx, r5.xyz);
  r5.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r7.xyz;
  r4.w = dot(r5.xyz, r5.xyz);
  r4.w = rsqrt(r4.w);
  r5.xyz = r5.xyz * r4.www;
  r4.w = abs(r1.w) * -0.0187292993 + 0.0742610022;
  r4.w = r4.w * abs(r1.w) + -0.212114394;
  r4.w = r4.w * abs(r1.w) + 1.57072878;
  r5.w = 1 + -abs(r1.w);
  r1.w = cmp(r1.w < -r1.w);
  r5.w = sqrt(r5.w);
  r6.w = r5.w * r4.w;
  r6.w = r6.w * -2 + 3.14159274;
  r1.w = r1.w ? r6.w : 0;
  r1.w = r4.w * r5.w + r1.w;
  r3.xyz = r1.www * r5.xyz + r3.xyz;
  r1.w = dot(r6.zxy, r4.xyz);
  r1.w = r1.w + -r3.w;
  r1.w = max(0, -r1.w);
  r4.xyz = r6.zxy * r1.www + r4.xyz;
  r4.xyz = r4.xyz + -r2.zxw;
  r1.w = dot(r4.xyz, r4.xyz);
  r1.w = rsqrt(r1.w);
  r4.xyz = r4.xyz * r1.www;
  r5.xyz = r4.zxy * r1.xyz;
  r5.xyz = r1.zxy * r4.xyz + -r5.xyz;
  r1.x = dot(r1.xyz, r4.xyz);
  r1.yzw = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r5.xyz;
  r3.w = dot(r1.yzw, r1.yzw);
  r3.w = rsqrt(r3.w);
  r1.yzw = r3.www * r1.yzw;
  r3.w = abs(r1.x) * -0.0187292993 + 0.0742610022;
  r3.w = r3.w * abs(r1.x) + -0.212114394;
  r3.w = r3.w * abs(r1.x) + 1.57072878;
  r4.w = 1 + -abs(r1.x);
  r1.x = cmp(r1.x < -r1.x);
  r4.w = sqrt(r4.w);
  r5.x = r4.w * r3.w;
  r5.x = r5.x * -2 + 3.14159274;
  r1.x = r1.x ? r5.x : 0;
  r1.x = r3.w * r4.w + r1.x;
  r1.xyz = r1.xxx * r1.yzw + r3.xyz;
  r3.xyz = r4.xyz * r0.xyw;
  r3.xyz = r4.zxy * r0.ywx + -r3.xyz;
  r0.x = dot(r4.zxy, r0.xyw);
  r3.xyz = -r6.xyz * float3(9.99999975e-006,9.99999975e-006,9.99999975e-006) + r3.xyz;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = abs(r0.x) * -0.0187292993 + 0.0742610022;
  r0.y = r0.y * abs(r0.x) + -0.212114394;
  r0.y = r0.y * abs(r0.x) + 1.57072878;
  r0.w = 1 + -abs(r0.x);
  r0.x = cmp(r0.x < -r0.x);
  r0.w = sqrt(r0.w);
  r1.w = r0.y * r0.w;
  r1.w = r1.w * -2 + 3.14159274;
  r0.x = r0.x ? r1.w : 0;
  r0.x = r0.y * r0.w + r0.x;
  r0.xyw = r0.xxx * r3.xyz + r1.xyz;
  r1.x = dot(r0.xyw, r0.xyw);
  r1.x = rsqrt(r1.x);
  r1.y = dot(-r2.xzw, -r2.xzw);
  r1.y = rsqrt(r1.y);
  r1.yzw = -r2.xwz * r1.yyy;
  float3 spatchViewDirection = r1.yzw;
  float3 spatchLightDirection = r0.xyw * r1.xxx;
  float spatchNativeExponent = r0.z;
  r3.xyz = r0.xyw * r1.xxx + r1.yzw;
  r0.x = dot(r0.xyw, r6.xyz);
  r0.x = max(0, r0.x);
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r3.xyz = r3.xyz * r0.yyy;
  r0.y = saturate(dot(r6.xyz, r3.xyz));
  r0.w = saturate(dot(r1.yzw, r3.xyz));
  r0.w = 1 + -r0.w;
  r0.y = log2(r0.y);
  r0.y = r0.z * r0.y;
  r0.z = -1 + r0.z;
  r0.y = exp2(r0.y);
  r0.y = r0.z * r0.y;
  r0.xy = float2(0.159154907,0.125) * r0.xy;
  r0.z = cmp(r8.w == 1.000000);
  r1.xyz = r8.xyz * r8.xyz;
  r3.xyz = r1.xyz * r1.xyz;
  r3.xyz = r0.zzz ? r3.xyz : r8.www;
  float3 spatchF0 = r0.zzz ? r1.xyz : r8.www;
  float spatchMetallic = r0.z ? 1.0 : 0.0;
  float spatchPbrGeometricVariance = SPatchPBRGeometricVariance(r6.xyz);
  float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchNativeExponent,
    spatchF0, spatchPbrGeometricVariance);
  float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
    r6.xyz, spatchViewDirection, spatchLightDirection, spatchF0, spatchMetallic);
  r4.xyz = float3(50,50,50) * r3.xyz;
  r4.xyz = min(float3(1,1,1), r4.xyz);
  r4.xyz = saturate(r4.xyz + -r3.xyz);
  r0.z = r0.w * r0.w;
  r0.z = r0.z * r0.z;
  r0.z = r0.w * r0.z;
  r3.xyz = r4.xyz * r0.zzz + r3.xyz;
  r0.yzw = r3.xyz * r0.yyy;
  r0.yzw = lerp(r0.yzw, spatchPbrSpecular, SPatchPBRBlendStrength());
  r1.w = 1 / cbDeferredLight.ColourAndInvRadiusSqr.w;
  r1.w = -cbDeferredLight.WidthHeightNearFar.z + r1.w;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r3.x = dot(r3.xyz, r3.xyz);
  r3.x = sqrt(r3.x);
  r3.y = -cbDeferredLight.WidthHeightNearFar.z + r3.x;
  r4.x = saturate(r3.y / r1.w);
  r4.y = saturate(cbDeferredLight.Fov.w);
  r4.xyzw = texDistAtten.Sample(_texDistAtten, r4.xy).xyzw;
  r1.w = log2(r3.x);
  r1.w = cbDeferredLight.PositionAndRadius.w * r1.w;
  r1.w = exp2(r1.w);
  r1.w = 1 / r1.w;
  r1.w = saturate(-r3.x * cbDeferredLight.ColourAndInvRadiusSqr.w + r1.w);
  r3.x = r4.x + -r1.w;
  r3.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
  r3.y = r3.y ? 1.000000 : 0;
  r1.w = r3.y * r3.x + r1.w;
  r3.x = 1 + -r1.w;
  r3.x = r3.x * r3.x;
  r3.x = -r3.x * r3.x + 1;
  r0.yzw = r3.xxx * r0.yzw;
  r0.yzw = r0.yzw * r0.xxx;
  r3.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xxx;
  r3.xyz = r3.xyz * r1.www;
  r1.xyz = r3.xyz * (r1.xyz * lerp(
    float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength()));
  r0.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yzw;
  r0.w = cbDeferredLight.WidthHeightNearFar.z + cbDeferredLight.WidthHeightNearFar.w;
  r3.xyz = v2.xyz * r0.www + cbDeferredLight.PositionAndRadius.xyz;
  r3.xyz = -r3.xyz + r2.xwz;
  r0.w = dot(r3.xyz, v2.xyz);
  r0.w = cmp(1.00000001e-007 >= r0.w);
  r0.w = r0.w ? 1.000000 : 0;
  r3.xyz = v2.xyz * cbDeferredLight.WidthHeightNearFar.zzz + cbDeferredLight.PositionAndRadius.xyz;
  r3.xyz = -r3.xyz + r2.xwz;
  r1.w = dot(r3.xyz, v2.xyz);
  r1.w = cmp(r1.w >= 1.00000001e-007);
  r1.w = r1.w ? 1.000000 : 0;
  r0.w = r1.w * r0.w;
  r3.xyz = cbShadowTransform.ViewShadow[0]._m10_m11_m13 * -r2.yyy;
  r2.xyw = r2.xxx * cbShadowTransform.ViewShadow[0]._m00_m01_m03 + r3.xyz;
  r2.xyz = r2.zzz * cbShadowTransform.ViewShadow[0]._m20_m21_m23 + r2.xyw;
  r2.xyz = cbShadowTransform.ViewShadow[0]._m30_m31_m33 + r2.xyz;
  r2.xy = r2.xy / r2.zz;
  r2.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r2.xy, 0).xyzw;
  r2.xyz = r2.xyz * r2.xyz;
  r2.xyz = r2.xyz * r0.www;
  r3.xyz = r2.xyz * r0.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  o0.xyz = r1.xyz * r2.xyz + r3.xyz;
  return;
}

#elif SPATCH_PBR_VARIANT == 16
// Native shader 0xF74BCE96.

cbuffer cbShadowTransform : register(b0)
{

  struct
  {
    row_major float4x4 ViewShadow[4];
    float4 CutDepths;
    float4 Biases;
  } cbShadowTransform : packoffset(c0);

}

cbuffer cbExternalViewTransform : register(b1)
{

  struct
  {
    row_major float4x4 WorldView;
    float4 ViewScaleAndNearFar;
    float4 SkyFogDir;
    float4 NorthFogDir;
    float4 EastFogDir;
  } cbExternalViewTransform : packoffset(c0);

}

cbuffer cbDeferredLight : register(b2)
{

  struct
  {
    float4 PositionAndRadius;
    float4 ColourAndInvRadiusSqr;
    float4 Fov;
    float4 WidthHeightNearFar;
  } cbDeferredLight : packoffset(c0);

}

SamplerState _texDiffuse2 : register(s0);
SamplerState _texDiffuse : register(s1);
SamplerState _texNormal : register(s2);
SamplerState _texDepth : register(s3);
SamplerState _texDistAtten : register(s4);
Texture2D<float4> texDiffuse2 : register(t0);
Texture2D<float4> texDiffuse : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texDepth : register(t3);
Texture2D<float4> texDistAtten : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v1.xy / v1.ww;
  r1.xyzw = texNormal.Sample(_texNormal, r0.xy).xyzw;
  r0.zw = saturate(r0.xy);
  r2.xyzw = texDepth.Sample(_texDepth, r0.zw).xyzw;
  r0.z = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r0.w = -r2.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.z = r0.z / r0.w;
  r2.xy = r0.xy * float2(2,2) + float2(-1,-1);
  r2.xy = r2.xy * r0.zz;
  r2.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r2.xy;
  r2.z = -r0.z;
  r2.w = -r2.y;
  r3.xyz = cbDeferredLight.PositionAndRadius.xyz + -r2.xwz;
  r0.z = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.z);
  r4.xyz = r3.xyz * r0.www;
  r3.w = dot(v2.xyz, r4.xyz);
  r3.w = -cbDeferredLight.Fov.y + r3.w;
  r4.w = cbDeferredLight.Fov.x + -cbDeferredLight.Fov.y;
  r3.w = saturate(r3.w / r4.w);
  r5.xyzw = texDiffuse.Sample(_texDiffuse, r0.xy).xyzw;
  r5.xyz = r5.xyz * r5.xyz;
  float3 spatchDiffuseScale = float3(1.0, 1.0, 1.0);
  r0.x = cmp(0 < r3.w);
  r0.y = sqrt(r0.z);
  r6.x = cbDeferredLight.ColourAndInvRadiusSqr.w * r0.y;
  r6.y = cbDeferredLight.Fov.w;
  r6.xy = saturate(r6.xy);
  r6.xyzw = texDistAtten.Sample(_texDistAtten, r6.xy).xyzw;
  float3 spatchPbrNormal =
    r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  spatchPbrNormal = SPatchPBRSafeNormalize(spatchPbrNormal);
  float spatchPbrGeometricVariance =
    SPatchPBRGeometricVariance(spatchPbrNormal);
  [flatten]
  if (r0.x != 0) {
    r1.xyz = spatchPbrNormal;
    float3 spatchLightDirection = r4.xyz;
    r6.yzw = cbShadowTransform.ViewShadow[0]._m10_m11_m13 * -r2.yyy;
    r6.yzw = r2.xxx * cbShadowTransform.ViewShadow[0]._m00_m01_m03 + r6.yzw;
    r6.yzw = r2.zzz * cbShadowTransform.ViewShadow[0]._m20_m21_m23 + r6.yzw;
    r6.yzw = cbShadowTransform.ViewShadow[0]._m30_m31_m33 + r6.yzw;
    r0.x = cmp(r5.w == 1.000000);
    r7.xyz = r5.xyz * r5.xyz;
    r7.xyz = r0.xxx ? r7.xyz : r5.www;
    float3 spatchF0 = r0.xxx ? r5.xyz : r5.www;
    float spatchMetallic = r0.x ? 1.0 : 0.0;
    r0.x = log2(r0.y);
    r0.x = cbDeferredLight.PositionAndRadius.w * r0.x;
    r0.x = exp2(r0.x);
    r0.x = 1 / r0.x;
    r0.x = saturate(-r0.y * cbDeferredLight.ColourAndInvRadiusSqr.w + r0.x);
    r0.y = cmp(cbDeferredLight.Fov.w >= -0.00100000005);
    r0.y = r0.y ? 1.000000 : 0;
    r0.z = r6.x + -r0.x;
    r0.x = r0.y * r0.z + r0.x;
    r0.y = saturate(dot(r1.xyz, r4.xyz));
    r8.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.yyy;
    r8.xyz = r8.xyz * r0.xxx;
    r0.z = dot(-r2.xzw, -r2.xzw);
    r0.z = rsqrt(r0.z);
    r2.xyz = -r2.xwz * r0.zzz;
    r0.z = 10 * r1.w;
    r0.z = exp2(r0.z);
    float3 spatchPbrSpecular = SPatchPBRDirectSpecular(
      r1.xyz, r2.xyz, spatchLightDirection, r0.z, spatchF0,
      spatchPbrGeometricVariance);
    float3 spatchPbrDiffuseWeight = SPatchPBRDiffuseWeight(
      r1.xyz, r2.xyz, spatchLightDirection, spatchF0, spatchMetallic);
    spatchDiffuseScale = lerp(
      float3(1.0, 1.0, 1.0), spatchPbrDiffuseWeight, SPatchPBRBlendStrength());
    r3.xyz = r3.xyz * r0.www + r2.xyz;
    r0.w = dot(r3.xyz, r3.xyz);
    r0.w = rsqrt(r0.w);
    r3.xyz = r3.xyz * r0.www;
    r0.w = saturate(dot(r1.xyz, r3.xyz));
    r0.w = log2(r0.w);
    r0.w = r0.z * r0.w;
    r0.w = exp2(r0.w);
    r0.z = -1 + r0.z;
    r0.z = r0.z * r0.w;
    r0.z = 0.125 * r0.z;
    r1.xyz = float3(50,50,50) * r7.xyz;
    r1.xyz = min(float3(1,1,1), r1.xyz);
    r1.xyz = saturate(r1.xyz + -r7.xyz);
    r0.w = saturate(dot(r2.xyz, r3.xyz));

    r0.w = 1 + -r0.w;
    r1.w = r0.w * r0.w;
    r1.w = r1.w * r1.w;
    r0.w = r1.w * r0.w;
    r1.xyz = r1.xyz * r0.www + r7.xyz;
    r1.xyz = r1.xyz * r0.zzz;
    r1.xyz = lerp(r1.xyz, spatchPbrSpecular, SPatchPBRBlendStrength());
    r0.x = 1 + -r0.x;
    r0.x = r0.x * r0.x;
    r0.x = -r0.x * r0.x + 1;
    r0.xzw = r1.xyz * r0.xxx;
    r0.xyz = r0.xzw * r0.yyy;
    r0.xyz = cbDeferredLight.ColourAndInvRadiusSqr.xyz * r0.xyz;
    r1.xy = r6.yz / r6.ww;
    r1.xyzw = texDiffuse2.SampleLevel(_texDiffuse2, r1.xy, 0).xyzw;
    r1.xyz = r3.www * r1.xxx;
  } else {
    r1.xyz = float3(0,0,0);
    r8.xyz = float3(0,0,0);
    r0.xyz = float3(0,0,0);
    r4.xyz = float3(0,0,0);
  }
  r2.xyz = r8.xyz * (r5.xyz * spatchDiffuseScale);
  r3.xyz = r0.xyz * r3.www;
  r3.xyz = r3.xyz * r1.xyz;
  r1.xyz = r2.xyz * r1.xyz + r3.xyz;
  o0.w = dot(r0.xyz, float3(4,4,4));
  r0.x = dot(r1.xyz, float3(0.333330005,0.333330005,0.333330005));
  o1.xyz = r4.xyz * r0.xxx;
  o0.xyz = r1.xyz;
  o1.w = r0.x;
  return;
}

#elif SPATCH_PBR_VARIANT == 17
// Native shader 0x282EE2DC: runtime-proven vehicle glass.

cbuffer cbEnvironmentSettings : register(b0)
{

  struct
  {
    float4 SunDir;
    float4 SunDirWorld;
    float4 SunColor;
    float4 AmbientColorHorizon;
    float4 ScaleAndHeight;
    float4 ScatterZenithColor;
    float4 ScatterHorizonColor;
    float4 ScatterGroundColor;
    float4 ScatterSunColor;
    float4 CharacterParams;
    float4 FogStartStopSky;
    float4 WindDirAndMag;
    float4 DisplayDebug;
    float4 LitWindowTimeOn;
    float4 Lighting;
    float4 SunScatterParams;
  } cbEnvironmentSettings : packoffset(c0);

}

cbuffer sbVehicleLook : register(b1)
{

  struct
  {
    float4 DiffuseTint1;
    float4 DiffuseTint2;
    float4 SpecularLook;
    float4 DirtColour;
    float4 ExtraInfo;
  } sbVehicleLook : packoffset(c0);

}

cbuffer cbSceneryInstance : register(b2)
{

  struct
  {
    float4 ColourTint;
    float4 SIColourTint;
    float4 Mask;
    float4 Value0;
  } cbSceneryInstance : packoffset(c0);

}

SamplerState _texFogCube : register(s0);
SamplerState _texDiffuse : register(s1);
SamplerState _texSphericalMap : register(s2);
Texture3D<float4> texFogCube : register(t0);
Texture2D<float4> texDiffuse : register(t1);
Texture2D<float4> texSphericalMap : register(t2);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : COLOR0,
  float4 v2 : COLOR1,
  float3 v3 : TEXCOORD0,
  float4 v4 : TEXCOORD1,
  float3 v5 : TEXCOORD2,
  float3 v6 : TEXCOORD3,
  float3 v7 : TEXCOORD6,
  uint v8 : SV_IsFrontFace0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyzw = texDiffuse.Sample(_texDiffuse, v4.zw).xyzw;
  r0.x = r0.y * r0.y;
  // The native instruction reads c0.y (reflected as ColourTint.y).
  r0.x = r0.x * cbSceneryInstance.ColourTint.y;
  r0.y = r0.x * 0.400000006;
  r0.x = r0.x * -0.119999997 + 1;
  r0.z = cmp(v2.w == 1.000000);
  r0.w = v2.w * 0.800000012;
  r1.w = r0.z ? 1.000000 : r0.w;
  r1.xyz = v2.xyz * float3(0.100000001,0.100000001,0.100000001);
  r2.xyzw = float4(0.25,0.25,0.25,1) + -r1.xyzw;
  r1.xyzw = r0.yyyy * r2.xyzw + r1.xyzw;
  r0.yz = v4.xy + float2(0.5,0);
  r2.xyzw = texDiffuse.Sample(_texDiffuse, r0.yz).xyzw;
  r3.w = max(r1.w, r2.w);
  r3.xyz = r2.www;
  r0.y = saturate(r2.w + -0.300000012);
  r2.xyzw = -r1.xyzw + r3.xyzw;
  r2.xyzw = r0.yyyy * r2.wxyz + r1.wxyz;
  r3.xyzw = texDiffuse.Sample(_texDiffuse, v4.xy).xyzw;
  r0.y = saturate(r3.w * 6);
  r3.x = r0.y * r2.x;
  r3.y = r2.x * r0.y + -0.100000001;
  r0.y = v1.w >= 2.000000 ? v1.w + -2.000000 : v1.w;
  r0.z = cmp(r0.y >= 0.330000013);
  r0.w = cmp(r0.y >= 0.660000026);
  r3.z = cmp(r0.y < 0.330000013);
  r3.w = cmp(r0.y < 0.660000026);
  r3.xy = r0.w ? r3.xy : float2(0,1);
  r0.y = (r0.z && r3.w) ? 1.000000 : 0;
  r0.z = (r0.w || r0.y) ? 1.000000 : 0;
  r4.xyz = r0.z ? r2.yzw : float3(0,0,0);
  r1.xyz = r3.z ? r1.xyz : r4.xyz;
  r2.z = r1.w;
  r1.xyz = r1.xyz * r1.xyz;
  r2.yw = float2(0,1);
  r0.yz = r0.y ? r2.xy : r3.xy;
  r0.yz = r3.z ? r2.zw : r0.yz;
  r0.z = cmp(r0.z < 0.000000);
  r1.w = r0.y;
  if (r0.z != 0) discard;
  r2.xyzw = float4(0,0,0,0.949999988) + -r1.xyzw;
  r1.xyzw = sbVehicleLook.ExtraInfo.yyyy * r2.xyzw + r1.xyzw;

  r0.yzw = normalize(v5.xyz);
  r0.yzw = v8 ? r0.yzw : -r0.yzw;
  r2.x = saturate(dot(r0.yzw, cbEnvironmentSettings.SunDirWorld.xyz));
  float spatchGlassNoL = r2.x;
  r2.yzw = cbEnvironmentSettings.SunColor.xyz * float3(0.5,0.5,0.5);
  r3.xyz = r2.xxx * r2.yzw + v1.xyz;
  r1.xyz = r1.xyz * r3.xyz;

  r3.x = r0.x * 15;
  r0.x = saturate(r0.x + sbVehicleLook.ExtraInfo.x);
  r3.x = exp2(r3.x);
  float spatchGlassNativeExponent = max(r3.x - 0.999, 0.0);
  r3.xy = r3.xx + float2(-0.999000013,-1.99899995);
  r5.xyz = normalize(v6.xyz);
  r4.xyz = r5.xyz + cbEnvironmentSettings.SunDirWorld.xyz;
  r4.xyz = normalize(r4.xyz);
  r3.z = saturate(dot(r0.yzw, r4.xyz));
  r3.w = 1 + -saturate(dot(r5.xyz, r4.xyz));
  r3.z = log2(r3.z);
  r3.x = r3.z * r3.x;
  r3.x = exp2(r3.x);
  r3.x = r3.x * r3.y;
  r3.x = r3.x * 0.125;
  r3.y = r3.w * r3.w;
  r3.y = r3.y * r3.y;
  r3.y = r3.y * r3.w;
  r3.y = r3.y * 0.949999988 + 0.0500000007;
  r3.x = r3.y * r3.x;
  r2.x = r2.x * r3.x;
  r2.xyz = r2.yzw * r2.xxx;

  float3 spatchGlassPbrDirectSun =
    cbEnvironmentSettings.SunColor.xyz * 0.5 * spatchGlassNoL *
    SPatchPBRGlassDirectSpecular(
      r0.yzw, r5.xyz, cbEnvironmentSettings.SunDirWorld.xyz,
      spatchGlassNativeExponent, float3(0.05,0.05,0.05));
  r2.xyz = lerp(r2.xyz, spatchGlassPbrDirectSun, SPatchPBRBlendStrength());
  r1.xyz = r1.xyz * r1.www + r2.xyz;

  r2.x = dot(r5.xyz, r0.yzw);
  float spatchGlassNoV = saturate(r2.x);
  r2.y = r2.x + r2.x;
  r2.x = 1 + -saturate(r2.x);
  r0.yzw = r0.yzw * -r2.yyy + r5.xyz;
  r0.yzw = r5.xyz * float3(0.5,0.5,0.5) + r0.yzw;
  r0.yzw = normalize(r0.yzw);
  r2.y = max(abs(r0.z), abs(r0.y));
  r2.y = 1 / r2.y;
  r2.z = min(abs(r0.z), abs(r0.y));
  r2.y = r2.y * r2.z;
  r2.z = r2.y * r2.y;
  r2.w = r2.z * 0.0208350997 + -0.0851330012;
  r2.w = r2.z * r2.w + 0.180141002;
  r2.w = r2.z * r2.w + -0.330299497;
  r2.z = r2.z * r2.w + 0.999866009;
  r2.w = r2.z * r2.y;
  r2.w = r2.w * -2 + 1.57079637;
  r3.x = cmp(abs(r0.z) < abs(r0.y));
  r2.w = r3.x ? r2.w : 0;
  r2.y = r2.y * r2.z + r2.w;
  r2.z = cmp(r0.z < -r0.z);
  r2.z = r2.z ? -3.14159274 : 0;
  r2.y = r2.z + r2.y;
  r2.z = min(r0.z, r0.y);
  r0.y = max(r0.z, r0.y);
  r3.y = r0.w * 0.5 + 0.5;
  r0.y = cmp(r0.y >= -r0.y);
  r2.z = cmp(r2.z < -r2.z);
  r0.y = r0.y ? r2.z : 0;
  r0.y = r0.y ? -r2.y : r2.y;
  r3.x = r0.y * 0.159154937 + 0.5;
  r0.y = r0.x * r0.x;
  r0.y = -r0.y * r0.x + 1;
  r0.x = max(r0.x, 0.0500000007);
  r0.x = r0.x + -0.0500000007;
  r0.y = r0.y * 6;
  r3.xyzw = texSphericalMap.SampleLevel(
    _texSphericalMap, r3.xy, r0.y).xyzw;
  r0.yzw = r3.xyz + float3(-1.03999996,-1.03999996,-1.03999996);
  r0.yzw = r3.xyz / r0.yzw;
  r0.yzw = r0.yzw * float3(-0.200000003,-0.200000003,-0.200000003);
  float3 spatchGlassEnvironmentRadiance = r0.yzw;
  r2.y = r2.x * r2.x;
  r2.y = r2.y * r2.y;
  r2.x = r2.y * r2.x;
  r0.x = r0.x * r2.x + 0.0500000007;
  r0.yzw = r0.yzw * r0.xxx;
  r0.x = min(r0.x, 0.5);
  r0.x = 1 + -r0.x;
  r0.xyz = r1.xyz * r0.xxx + r0.yzw;
  float3 spatchGlassNativeEnvironmentComposite = r0.xyz;

  // Keep the native spherical projection and the authored seven-mip
  // smoothness law, but replace its non-physical F90=smoothness response and
  // 50% diffuse-loss cap. The same unit-grazing dielectric Fresnel used by
  // direct-sun GGX now partitions the environment reflection and transmitted
  // body lighting at every view angle.
  float3 spatchGlassPbrEnvironmentFresnel =
    SPatchPBRFresnelCosineUnitGrazing(
      spatchGlassNoV, float3(0.05,0.05,0.05));
  float3 spatchGlassPbrEnvironmentComposite =
    r1.xyz * (1.0 - spatchGlassPbrEnvironmentFresnel) +
    spatchGlassEnvironmentRadiance * spatchGlassPbrEnvironmentFresnel;
  r0.xyz = lerp(
    spatchGlassNativeEnvironmentComposite,
    spatchGlassPbrEnvironmentComposite,
    SPatchPBRBlendStrength());

  r2.xyzw = texFogCube.Sample(_texFogCube, v7.xyz).xyzw;
  r1.xyz = r1.www * r2.xyz;
  r0.w = -r2.w * r1.w + 1;
  o0.w = r1.w;
  r0.xyz = r0.xyz * r0.www + r1.xyz;
  r1.xyz = r0.xyz * float3(1.03999996,1.03999996,1.03999996);
  r0.xyz = r0.xyz + float3(0.200000003,0.200000003,0.200000003);
  o0.xyz = r1.xyz / r0.xyz;
  return;
}

#elif SPATCH_PBR_VARIANT == 18
// Native shader 0x5DB1CB6E: runtime-proven damaged vehicle paint.

cbuffer cbViewTransform : register(b0)
{
  struct
  {
    row_major float4x4 WorldView;
    row_major float4x4 WorldProjection;
    row_major float4x4 WorldViewInv;
    float4 CameraOffset;
    float4 CameraPosition;
    float4 Target;
  } cbViewTransform : packoffset(c0);
}

cbuffer cbSceneryInstance : register(b1)
{
  struct
  {
    float4 ColourTint;
    float4 SIColourTint;
    float4 Mask;
    float4 Value0;
  } cbSceneryInstance : packoffset(c0);
}

cbuffer sbVehicleLook : register(b2)
{
  struct
  {
    float4 DiffuseTint1;
    float4 DiffuseTint2;
    float4 SpecularLook;
    float4 DirtColour;
    float4 ExtraInfo;
  } sbVehicleLook : packoffset(c0);
}

SamplerState _texDiffuse : register(s0);
SamplerState _texDamage : register(s1);
SamplerState _texSpecular : register(s2);
SamplerState _texBump : register(s3);
SamplerState _texSphericalMap : register(s4);
SamplerState _texFadeDitherMask : register(s5);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texDamage : register(t1);
Texture2D<float4> texSpecular : register(t2);
Texture2D<float4> texBump : register(t3);
Texture2D<float4> texSphericalMap : register(t4);
Texture2D<float4> texFadeDitherMask : register(t5);

// 3Dmigoto declarations
#define cmp -

void main(
  float4 v0 : SV_Position0,
  float2 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  float3 v3 : TEXCOORD2,
  float3 v4 : TEXCOORD3,
  float3 v5 : TEXCOORD4,
  float3 v6 : TEXCOORD5,
  float3 v7 : TEXCOORD6,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1,r2,r3,r4;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = float2(0.25,0.5) * v0.xy;
  r0.xyzw = texFadeDitherMask.Sample(_texFadeDitherMask, r0.xy).xyzw;
  r0.x = -0.100000001 + r0.x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.xyz = sbVehicleLook.DiffuseTint2.xyz * sbVehicleLook.DiffuseTint2.xyz;
  r1.xyz = sbVehicleLook.DiffuseTint1.xyz * sbVehicleLook.DiffuseTint1.xyz + -r0.xyz;
  r2.xyzw = texDiffuse.SampleBias(_texDiffuse, v1.xy, -1).xyzw;
  r0.xyz = r2.www * r1.xyz + r0.xyz;
  r1.xyz = r2.xyz * r2.xyz;
  r2.xyz = r1.xyz * r0.xyz;
  r0.xyz = -r1.xyz * r0.xyz + float3(0.300000012,0.300000012,0.300000012);
  r1.xyzw = texDamage.Sample(_texDamage, v1.xy).xyzw;
  r0.w = -0.200000003 + r1.y;
  r0.w = saturate(1.66666663 * r0.w);
  r0.xyz = r0.www * r0.xyz;
  r3.xyzw = sbVehicleLook.SpecularLook.xyzw * r0.wwww;
  r3.xyzw = float4(3,0,-0.980000019,0) * r3.xyzw;
  r0.w = cmp(0.300000012 < v7.x);
  r0.w = r0.w ? 1.000000 : 0;
  r0.xyz = r0.www * r0.xyz + r2.xyz;
  r2.xyzw = r0.wwww * r3.xyzw + sbVehicleLook.SpecularLook.xyzw;
  r1.yz = v7.xx * r1.yz;
  r0.w = saturate(sbVehicleLook.DirtColour.w * r1.x);
  r0.w = 1 + r0.w;
  r0.w = saturate(v7.y * r0.w);
  r1.x = -r1.y * 0.800000012 + 1;
  r0.xyz = r1.xxx * r0.xyz;
  r1.xyw = sbVehicleLook.DirtColour.xyz * sbVehicleLook.DirtColour.xyz + -r0.xyz;
  r3.xy = v1.xy + v1.xy;
  r3.xyzw = texDamage.Sample(_texDamage, r3.xy).xyzw;
  r3.x = v7.z * r3.x;
  r3.x = cbSceneryInstance.ColourTint.y * r3.x;
  r0.w = cbSceneryInstance.ColourTint.y * r0.w + r3.x;
  r0.xyz = r0.www * r1.xyw + r0.xyz;
  r0.w = saturate(-r0.w * 1.5 + 0.800000012);
  r1.xyw = r0.xyz * r1.zzz;
  r0.xyz = -r1.xyw * cbSceneryInstance.ColourTint.xxx + r0.xyz;
  r0.xyz = sqrt(r0.xyz);
  r1.x = cbSceneryInstance.ColourTint.x * r1.z;
  r1.x = 0.699999988 * r1.x;
  r1.xy = -r2.xz * r1.xx + r2.xz;
  r1.z = r2.w * r0.w;
  r3.xyzw = texSpecular.Sample(_texSpecular, v1.xy).xyzw;
  r1.xy = -r3.wy * r3.wy + r1.xy;
  r2.xz = r3.wy * r3.wy;
  r1.y = r1.z * r1.y + r2.z;
  r1.x = r2.y * r1.x + r2.x;
  float spatchVehiclePaintF0 = r1.x;
  r0.w = saturate(sbVehicleLook.ExtraInfo.x * r0.w + r1.y);
  o1.w = r1.y;
  r1.y = max(r0.w, r1.x);
  r1.y = saturate(r1.y + -r1.x);
  r2.xyzw = texBump.Sample(_texBump, v1.xy).xyzw;
  r1.zw = float2(-0.5,-0.5) + r2.xy;
  r1.zw = r1.zw + r1.zw;
  r2.xyz = v4.xyz * -r1.www;
  r2.xyz = r1.zzz * v3.xyz + r2.xyz;
  r1.z = dot(r1.zw, r1.zw);
  r1.z = 1 + -r1.z;
  r1.z = sqrt(abs(r1.z));
  r2.xyz = r1.zzz * v2.xyz + r2.xyz;
  r1.z = dot(r2.xyz, r2.xyz);
  r1.z = rsqrt(r1.z);
  r3.xyz = r2.xyz * r1.zzz;
  r2.xyz = r2.xyz * r1.zzz + -v2.xyz;
  r2.xyz = v7.xxx * r2.xyz + v2.xyz;
  r4.xyz = cbViewTransform.WorldView._m10_m11_m12 * r3.yyy;
  r3.xyw = r3.xxx * cbViewTransform.WorldView._m00_m01_m02 + r4.xyz;
  r3.xyz = r3.zzz * cbViewTransform.WorldView._m20_m21_m22 + r3.xyw;
  r4.xy = v5.xy + r3.xy;
  r4.z = v5.z * r3.z;
  r3.xyz = -v5.xyz + r4.xyz;
  r3.xyz = v7.xxx * r3.xyz + v5.xyz;
  r1.z = dot(r3.xyz, r3.xyz);
  r1.z = rsqrt(r1.z);
  r3.xyz = r3.xyz * r1.zzz;
  r1.z = dot(v6.xyz, v6.xyz);
  r1.z = rsqrt(r1.z);
  r4.xyz = v6.xyz * r1.zzz;
  r1.z = dot(r4.xyz, r3.xyz);
  r1.w = saturate(r1.z);
  float spatchVehiclePaintNoV = r1.w;
  r1.z = r1.z + r1.z;
  r3.xyz = r3.xyz * -r1.zzz + r4.xyz;
  r3.xyz = r4.xyz * float3(0.5,0.5,0.5) + r3.xyz;
  r1.z = 1 + -r1.w;
  r1.w = r1.z * r1.z;
  r1.w = r1.w * r1.w;
  r1.z = r1.z * r1.w;
  r1.y = r1.y * r1.z + r1.x;
  float spatchVehiclePaintNativeFresnel = r1.y;
  float spatchVehiclePaintPbrFresnel = SPatchPBRFresnelCosine(
    spatchVehiclePaintNoV,
    float3(spatchVehiclePaintF0, spatchVehiclePaintF0,
      spatchVehiclePaintF0)).x;
  float spatchVehiclePaintEnvironmentFresnel = lerp(
    spatchVehiclePaintNativeFresnel,
    spatchVehiclePaintPbrFresnel,
    SPatchPBRBlendStrength());
  o0.w = r1.x;
  float spatchVehiclePaintNativeDiffuseWeight =
    1.0 - min(0.5, spatchVehiclePaintNativeFresnel);
  float spatchVehiclePaintPbrDiffuseWeight =
    1.0 - spatchVehiclePaintPbrFresnel;
  r1.x = lerp(
    spatchVehiclePaintNativeDiffuseWeight,
    spatchVehiclePaintPbrDiffuseWeight,
    SPatchPBRBlendStrength());
  o0.xyz = r1.xxx * r0.xyz;
  r0.x = dot(r2.xyz, r2.xyz);
  r0.x = rsqrt(r0.x);
  r0.xyz = r2.xyz * r0.xxx;
  o1.xyz = r0.xyz * float3(0.5,0.5,0.5) + float3(0.5,0.5,0.5);
  r0.x = r0.w * r0.w;
  r0.x = -r0.x * r0.w + 1;
  r0.x = 6 * r0.x;
  r0.y = dot(r3.xyz, r3.xyz);
  r0.y = rsqrt(r0.y);
  r0.yzw = r3.xyz * r0.yyy;
  r1.x = max(abs(r0.y), abs(r0.z));
  r1.x = 1 / r1.x;
  r1.z = min(abs(r0.y), abs(r0.z));
  r1.x = r1.z * r1.x;
  r1.z = r1.x * r1.x;
  r1.w = r1.z * 0.0208350997 + -0.0851330012;
  r1.w = r1.z * r1.w + 0.180141002;
  r1.w = r1.z * r1.w + -0.330299497;
  r1.z = r1.z * r1.w + 0.999866009;
  r1.w = r1.x * r1.z;
  r1.w = r1.w * -2 + 1.57079637;
  r2.x = cmp(abs(r0.z) < abs(r0.y));
  r1.w = r2.x ? r1.w : 0;
  r1.x = r1.x * r1.z + r1.w;
  r1.z = cmp(r0.z < -r0.z);
  r1.z = r1.z ? -3.141593 : 0;
  r1.x = r1.x + r1.z;
  r1.z = min(r0.y, r0.z);
  r1.z = cmp(r1.z < -r1.z);
  r0.y = max(r0.y, r0.z);
  r2.y = r0.w * 0.5 + 0.5;
  r0.y = cmp(r0.y >= -r0.y);
  r0.y = r0.y ? r1.z : 0;
  r0.y = r0.y ? -r1.x : r1.x;
  r2.x = r0.y * 0.159154907 + 0.5;
  r0.xyzw = texSphericalMap.SampleLevel(
    _texSphericalMap, r2.xy, r0.x).xyzw;
  r1.xzw = float3(-1.03999996,-1.03999996,-1.03999996) + r0.xyz;
  r0.xyz = r0.xyz / r1.xzw;
  r0.xyz = float3(-0.200000003,-0.200000003,-0.200000003) * r0.xyz;
  r1.xzw = spatchVehiclePaintEnvironmentFresnel * r0.xyz +
    float3(0.200000003,0.200000003,0.200000003);
  r0.xyz = spatchVehiclePaintEnvironmentFresnel * r0.xyz;
  r0.xyz = float3(1.03999996,1.03999996,1.03999996) * r0.xyz;
  o2.xyz = r0.xyz / r1.xzw;
  o2.w = 0;
  return;
}

#elif SPATCH_PBR_VARIANT == 19
// Native shader 0xE611C192: runtime-proven vehicle paint.

cbuffer cbViewTransform : register(b0)
{
  struct
  {
    row_major float4x4 WorldView;
    row_major float4x4 WorldProjection;
    row_major float4x4 WorldViewInv;
    float4 CameraOffset;
    float4 CameraPosition;
    float4 Target;
  } cbViewTransform : packoffset(c0);
}

cbuffer sbVehicleLook : register(b1)
{
  struct
  {
    float4 DiffuseTint1;
    float4 DiffuseTint2;
    float4 SpecularLook;
    float4 DirtColour;
    float4 ExtraInfo;
  } sbVehicleLook : packoffset(c0);
}

SamplerState _texDiffuse : register(s0);
SamplerState _texSpecular : register(s1);
SamplerState _texBump : register(s2);
SamplerState _texSphericalMap : register(s3);
SamplerState _texFadeDitherMask : register(s4);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texSpecular : register(t1);
Texture2D<float4> texBump : register(t2);
Texture2D<float4> texSphericalMap : register(t3);
Texture2D<float4> texFadeDitherMask : register(t4);

// 3Dmigoto declarations
#define cmp -

void main(
  float4 v0 : SV_Position0,
  float2 v1 : TEXCOORD0,
  float3 v2 : TEXCOORD1,
  float3 v3 : TEXCOORD2,
  float3 v4 : TEXCOORD3,
  float3 v5 : TEXCOORD5,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = float2(0.25,0.5) * v0.xy;
  r0.xyzw = texFadeDitherMask.Sample(_texFadeDitherMask, r0.xy).xyzw;
  r0.x = -0.100000001 + r0.x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.xyz = sbVehicleLook.DiffuseTint2.xyz * sbVehicleLook.DiffuseTint2.xyz;
  r1.xyz = sbVehicleLook.DiffuseTint1.xyz * sbVehicleLook.DiffuseTint1.xyz + -r0.xyz;
  r2.xyzw = texDiffuse.SampleBias(_texDiffuse, v1.xy, -1).xyzw;
  r0.xyz = r2.www * r1.xyz + r0.xyz;
  r1.xyz = r2.xyz * r2.xyz;
  r0.xyz = r1.xyz * r0.xyz;
  r0.xyz = sqrt(r0.xyz);
  r1.xyzw = texBump.Sample(_texBump, v1.xy).xyzw;
  r1.xy = float2(-0.5,-0.5) + r1.xy;
  r1.xy = r1.xy + r1.xy;
  r2.xyz = v4.xyz * -r1.yyy;
  r2.xyz = r1.xxx * v3.xyz + r2.xyz;
  r0.w = dot(r1.xy, r1.xy);
  r0.w = 1 + -r0.w;
  r0.w = sqrt(abs(r0.w));
  r1.xyz = r0.www * v2.xyz + r2.xyz;
  r0.w = dot(r1.xyz, r1.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = r1.xyz * r0.www;
  r2.xyz = cbViewTransform.WorldView._m10_m11_m12 * r1.yyy;
  r2.xyz = r1.xxx * cbViewTransform.WorldView._m00_m01_m02 + r2.xyz;
  r2.xyz = r1.zzz * cbViewTransform.WorldView._m20_m21_m22 + r2.xyz;
  o1.xyz = r1.xyz * float3(0.5,0.5,0.5) + float3(0.5,0.5,0.5);
  r0.w = dot(r2.xyz, r2.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = r2.xyz * r0.www;
  r0.w = dot(v5.xyz, v5.xyz);
  r0.w = rsqrt(r0.w);
  r2.xyz = v5.xyz * r0.www;
  r0.w = dot(r2.xyz, r1.xyz);
  r1.w = saturate(r0.w);
  float spatchVehiclePaintNoV = r1.w;
  r0.w = r0.w + r0.w;
  r1.xyz = r1.xyz * -r0.www + r2.xyz;
  r1.xyz = r2.xyz * float3(0.5,0.5,0.5) + r1.xyz;
  r0.w = 1 + -r1.w;
  r1.w = r0.w * r0.w;
  r1.w = r1.w * r1.w;
  r0.w = r1.w * r0.w;
  r2.xyzw = texSpecular.Sample(_texSpecular, v1.xy).xyzw;
  r2.xz = r2.wy * r2.wy;
  r2.yw = -r2.wy * r2.wy + sbVehicleLook.SpecularLook.xz;
  r2.xy = sbVehicleLook.SpecularLook.yw * r2.yw + r2.xz;
  float spatchVehiclePaintF0 = r2.x;
  r1.w = saturate(sbVehicleLook.ExtraInfo.x + r2.y);
  r2.z = max(r1.w, r2.x);
  r2.z = saturate(r2.z + -r2.x);
  r0.w = r2.z * r0.w + r2.x;
  float spatchVehiclePaintNativeFresnel = r0.w;
  float spatchVehiclePaintPbrFresnel = SPatchPBRFresnelCosine(
    spatchVehiclePaintNoV,
    float3(spatchVehiclePaintF0, spatchVehiclePaintF0,
      spatchVehiclePaintF0)).x;
  float spatchVehiclePaintEnvironmentFresnel = lerp(
    spatchVehiclePaintNativeFresnel,
    spatchVehiclePaintPbrFresnel,
    SPatchPBRBlendStrength());
  float spatchVehiclePaintNativeDiffuseWeight =
    1.0 - min(0.5, spatchVehiclePaintNativeFresnel);
  float spatchVehiclePaintPbrDiffuseWeight =
    1.0 - spatchVehiclePaintPbrFresnel;
  r2.z = lerp(
    spatchVehiclePaintNativeDiffuseWeight,
    spatchVehiclePaintPbrDiffuseWeight,
    SPatchPBRBlendStrength());
  o0.xyz = r2.zzz * r0.xyz;
  o0.w = r2.x;
  o1.w = r2.y;
  r0.x = r1.w * r1.w;
  r0.x = -r0.x * r1.w + 1;
  r0.x = 6 * r0.x;
  r0.y = dot(r1.xyz, r1.xyz);
  r0.y = rsqrt(r0.y);
  r1.xyz = r1.xyz * r0.yyy;
  r0.y = max(abs(r1.x), abs(r1.y));
  r0.y = 1 / r0.y;
  r0.z = min(abs(r1.x), abs(r1.y));
  r0.y = r0.z * r0.y;
  r0.z = r0.y * r0.y;
  r1.w = r0.z * 0.0208350997 + -0.0851330012;
  r1.w = r0.z * r1.w + 0.180141002;
  r1.w = r0.z * r1.w + -0.330299497;
  r0.z = r0.z * r1.w + 0.999866009;
  r1.w = r0.y * r0.z;
  r1.w = r1.w * -2 + 1.57079637;
  r2.x = cmp(abs(r1.y) < abs(r1.x));
  r1.w = r2.x ? r1.w : 0;
  r0.y = r0.y * r0.z + r1.w;
  r0.z = cmp(r1.y < -r1.y);
  r0.z = r0.z ? -3.141593 : 0;
  r0.y = r0.y + r0.z;
  r0.z = min(r1.x, r1.y);
  r0.z = cmp(r0.z < -r0.z);
  r1.x = max(r1.x, r1.y);
  r2.y = r1.z * 0.5 + 0.5;
  r1.x = cmp(r1.x >= -r1.x);
  r0.z = r0.z ? r1.x : 0;
  r0.y = r0.z ? -r0.y : r0.y;
  r2.x = r0.y * 0.159154907 + 0.5;
  r1.xyzw = texSphericalMap.SampleLevel(
    _texSphericalMap, r2.xy, r0.x).xyzw;
  r0.xyz = float3(-1.03999996,-1.03999996,-1.03999996) + r1.xyz;
  r0.xyz = r1.xyz / r0.xyz;
  r0.xyz = float3(-0.200000003,-0.200000003,-0.200000003) * r0.xyz;
  r1.xyz = spatchVehiclePaintEnvironmentFresnel * r0.xyz +
    float3(0.200000003,0.200000003,0.200000003);
  r0.xyz = spatchVehiclePaintEnvironmentFresnel * r0.xyz;
  r0.xyz = float3(1.03999996,1.03999996,1.03999996) * r0.xyz;
  o2.xyz = r0.xyz / r1.xyz;
  o2.w = 0;
  return;
}

#else
#error Unsupported SPATCH_PBR_VARIANT.
#endif
