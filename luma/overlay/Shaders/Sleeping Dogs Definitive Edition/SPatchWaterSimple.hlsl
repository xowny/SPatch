// ---- Created with 3Dmigoto v1.3.16 on Tue Jul 28 08:37:11 2026

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

cbuffer cbViewTransform : register(b2)
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

cbuffer cbTimebase : register(b3)
{

  struct
  {
    float4 SimTime;
  } cbTimebase : packoffset(c0);

}

cbuffer cbWaterOffset : register(b4)
{

  struct
  {
    float4 RippleOffset;
    float4 FogFalloff;
  } cbWaterOffset : packoffset(c0);

}

cbuffer sbWaterLook : register(b5)
{

  struct
  {
    float4 AbsorbExtinct;
    float4 VerticalAbsorbExtinct;
    float4 SpecSparkleRefraction;
    float4 FresnelAndBias;
  } sbWaterLook : packoffset(c0);

}

SamplerState _texFogCube_s : register(s0);
SamplerState _texEnvMap_s : register(s1);
SamplerState _texNormal_s : register(s2);
SamplerState _texAmbient_s : register(s3);
SamplerState _texDiffuse2_s : register(s4);
SamplerState _texOcclusion_s : register(s5);
SamplerState _texDepth_s : register(s6);
SamplerState _texRipple_s : register(s7);
Texture3D<float4> texFogCube : register(t0);
TextureCube<float4> texEnvMap : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texAmbient : register(t3);
Texture2D<float4> texDiffuse2 : register(t4);
Texture2D<float4> texOcclusion : register(t5);
Texture2D<float4> texDepth : register(t6);
Texture2D<float4> texRipple : register(t7);

#include "SPatchWaterScattering.hlsli"

// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : COLOR0,
  float4 v2 : TEXCOORD0,
  float4 v3 : TEXCOORD1,
  float4 v4 : TEXCOORD2,
  float4 v5 : TEXCOORD3,
  float4 v6 : TEXCOORD4,
  float4 v7 : TEXCOORD5,
  float3 v8 : TEXCOORD6,
  float3 v9 : TEXCOORD7,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;
  float3 spatchSceneAlbedo = 0;
  float3 spatchDirectLighting = 0;

  r0.xyzw = float4(-0.0353553407,0.0353553407,0.0894427225,0.178885445) * cbTimebase.SimTime.xxxx;
  r0.xy = v5.xy * float2(2,2) + r0.xy;
  r0.zw = v5.xy * float2(18,18) + r0.zw;
  r1.xyzw = texNormal.Sample(_texNormal_s, r0.zw).xyzw;
  r0.xyzw = texNormal.Sample(_texNormal_s, r0.xy).xyzw;
  r0.xy = r0.xy * float2(2,2) + float2(-1,-1);
  r0.xy = r1.xy * float2(2,2) + r0.xy;
  r0.xy = float2(-1,-1) + r0.xy;
  r0.zw = float2(20,20) * v5.xy;
  r0.zw = cbTimebase.SimTime.xx * float2(0.282842726,-0.282842726) + r0.zw;
  r1.xyzw = texNormal.Sample(_texNormal_s, r0.zw).xyzw;
  r0.xy = r1.xy * float2(2,2) + r0.xy;
  r0.xy = float2(-1,-1) + r0.xy;
  r1.xyzw = float4(-0.0009765625,0,0.0009765625,0) + v5.zwzw;
  r2.xyzw = texRipple.Sample(_texRipple_s, r1.xy).xyzw;
  r1.xyzw = texRipple.Sample(_texRipple_s, r1.zw).xyzw;
  r0.z = r1.x * 2 + -1;
  r0.w = r2.x * 2 + -1;
  r0.z = r0.z + -r0.w;
  r1.xy = -cbWaterOffset.RippleOffset.xy + v9.xy;
  r0.w = dot(r1.xy, r1.xy);
  r0.w = sqrt(r0.w);
  r0.w = 0.03125 * r0.w;
  r0.w = min(1, r0.w);
  r0.w = 1 + -r0.w;
  r0.z = r0.z * r0.w;
  r1.x = r0.z * 10 + r0.x;
  r2.xyzw = float4(0,-0.0009765625,0,0.0009765625) + v5.zwzw;
  r3.xyzw = texRipple.Sample(_texRipple_s, r2.xy).xyzw;
  r2.xyzw = texRipple.Sample(_texRipple_s, r2.zw).xyzw;
  r0.x = r2.x * 2 + -1;
  r0.z = r3.x * 2 + -1;
  r0.x = r0.x + -r0.z;
  r0.x = r0.x * r0.w;
  r1.y = r0.x * 10 + r0.y;
  r0.x = dot(r1.xy, r1.xy);
  r0.x = 1 + -r0.x;
  r0.x = sqrt(abs(r0.x));
  r1.z = r0.x + r0.x;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = rsqrt(r0.x);
  r0.xyz = r1.xyz * r0.xxx;
  r1.x = dot(v3.xyz, v3.xyz);
  r1.x = rsqrt(r1.x);
  r1.xyz = v3.xyz * r1.xxx;
  r1.xyz = r1.xyz * r0.yyy;
  r0.y = dot(v2.xyz, v2.xyz);
  r0.y = rsqrt(r0.y);
  r2.xyz = v2.xyz * r0.yyy;
  r1.xyz = r2.xyz * r0.zzz + r1.xyz;
  r0.y = dot(v4.xyz, v4.xyz);
  r0.y = rsqrt(r0.y);
  r2.xyz = v4.xyz * r0.yyy;
  r0.xyz = r2.xyz * r0.xxx + r1.xyz;
  r1.x = dot(r0.xyz, r0.xyz);
  r1.x = rsqrt(r1.x);
  r0.xyz = r1.xxx * r0.xyz;
  r1.xyz = cbViewTransform.WorldView._m10_m11_m12 * r0.yyy;
  r1.xyz = r0.xxx * cbViewTransform.WorldView._m00_m01_m02 + r1.xyz;
  r1.xyz = r0.zzz * cbViewTransform.WorldView._m20_m21_m22 + r1.xyz;
  r1.w = dot(r1.xyz, r1.xyz);
  r1.w = rsqrt(r1.w);
  r1.xyz = r1.xyz * r1.www;
  r1.w = dot(v7.xyz, v7.xyz);
  r1.w = rsqrt(r1.w);
  r2.xyz = v7.xyz * r1.www;
  r1.w = dot(r2.xyz, r1.xyz);
  r1.w = r1.w + r1.w;
  r1.xyz = r1.xyz * -r1.www + r2.xyz;
  r1.z = max(-1, r1.z);
  r1.w = min(0, r1.z);
  r1.xyzw = texEnvMap.Sample(_texEnvMap_s, r1.xyw).xyzw;
  r2.xyz = float3(-1.03999996,-1.03999996,-1.03999996) + r1.xyz;
  r1.xyz = r1.xyz / r2.xyz;
  r1.w = dot(r0.xyz, v2.xyz);
  r2.xyz = v2.xyz * r1.www;
  r2.xyz = -r2.xyz * sbWaterLook.SpecSparkleRefraction.zzz + r0.xyz;
  r1.w = dot(r2.xyz, r2.xyz);
  r1.w = rsqrt(r1.w);
  r2.xyz = r2.xyz * r1.www;
  r1.w = dot(-v6.xyz, -v6.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = -v6.xyz * r1.www + cbEnvironmentSettings.SunDir.xyz;
  r1.w = dot(r3.xyz, r3.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r3.xyz * r1.www;
  r1.w = saturate(dot(r3.xyz, r2.xyz));
  r1.w = log2(r1.w);
  r1.w = sbWaterLook.SpecSparkleRefraction.x * r1.w;
  r1.w = exp2(r1.w);
  r2.w = 2 + sbWaterLook.SpecSparkleRefraction.x;
  r2.w = 0.125 * r2.w;
  r1.w = r2.w * r1.w;
  r1.w = sbWaterLook.SpecSparkleRefraction.y * r1.w;
  r2.w = dot(cbEnvironmentSettings.SunColor.xyz,
    float3(0.300000012,0.600000024,0.100000001));
  r1.w = r2.w * r1.w;
  r1.xyz = r1.xyz * float3(-0.200000003,-0.200000003,-0.200000003) + r1.www;
  r3.x = dot(r2.xyz, v3.xyz);
  r3.y = dot(r2.xyz, v4.xyz);
  r2.xy = v8.xy / v8.zz;
  r2.xy = float2(0.5,0.5) + r2.xy;
  r3.xy = r3.xy * sbWaterLook.SpecSparkleRefraction.ww + r2.xy;
  r3.zw = -r3.xy + r2.xy;
  r2.xy = saturate(r2.xy);
  r4.xyzw = texDepth.Sample(_texDepth_s, r2.xy).xyzw;
  r2.xy = saturate(r3.xy);
  r5.xyzw = texDepth.Sample(_texDepth_s, r2.xy).xyzw;
  r1.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r2.x = -r5.x * r1.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r1.w = -r4.x * r1.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r2.y = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r2.x = r2.y / r2.x;
  r1.w = r2.y / r1.w;
  r1.w = -v6.z + -r1.w;
  r2.y = 0.00999999978 + -v6.z;
  r2.y = cmp(r2.y >= r2.x);
  r2.x = -v6.z + -r2.x;
  r2.y = r2.y ? 1.000000 : 0;
  r3.xy = r3.zw * r2.yy + r3.xy;
  r4.xyzw = texAmbient.Sample(_texAmbient_s, r3.xy).xyzw;
  r3.xyzw = texDiffuse2.Sample(_texDiffuse2_s, r3.xy).xyzw;
  spatchSceneAlbedo = r4.xyz * r4.xyz;
  spatchDirectLighting = r3.xyz;
  r4.xyz = r4.xyz * r4.xyz;
  r3.xyz = r4.xyz * cbEnvironmentSettings.AmbientColorHorizon.xyz + r3.xyz;
  r3.xyz = -sbWaterLook.VerticalAbsorbExtinct.xyz * sbWaterLook.VerticalAbsorbExtinct.xyz + r3.xyz;
  r4.xyz = sbWaterLook.VerticalAbsorbExtinct.xyz * sbWaterLook.VerticalAbsorbExtinct.xyz;
  r3.xyz = r3.xyz * float3(0.5,0.5,0.5) + r4.xyz;
  r3.xyz = -sbWaterLook.AbsorbExtinct.xyz * sbWaterLook.AbsorbExtinct.xyz + r3.xyz;
  r1.w = -abs(r2.x) + abs(r1.w);
  r1.w = r1.w * r2.y + abs(r2.x);
  r1.w = sbWaterLook.AbsorbExtinct.w * -r1.w;
  r1.w = 1.44269502 * r1.w;
  r1.w = exp2(r1.w);
  r2.xyz = sbWaterLook.AbsorbExtinct.xyz * sbWaterLook.AbsorbExtinct.xyz;
  r2.xyz = r1.www * r3.xyz + r2.xyz;
  r2.xyz += SPatchEvaluateWaterSingleScattering(
    r0.xyz, v6.xyz, r1.w, spatchSceneAlbedo, spatchDirectLighting);
  r1.xyz = -r2.xyz + r1.xyz;
  r3.x = dot(v6.xyz, v6.xyz);
  r3.y = rsqrt(r3.x);
  r3.x = sqrt(r3.x);
  r3.yzw = v6.xyz * r3.yyy;
  r3.y = dot(r0.xyz, -r3.yzw);
  r0.x = saturate(dot(r0.xyz, cbEnvironmentSettings.SunDir.xyz));
  r0.xyz = r0.xxx * r2.www + cbEnvironmentSettings.AmbientColorHorizon.xyz;
  r2.w = min(1, abs(r3.y));
  r2.w = 1 + -r2.w;
  r2.w = log2(r2.w);
  r2.w = sbWaterLook.FresnelAndBias.x * r2.w;
  r2.w = exp2(r2.w);
  r2.w = saturate(sbWaterLook.FresnelAndBias.y + r2.w);
  r2.w = 0.5 * r2.w;
  r3.y = 1 + -r1.w;
  r1.w = saturate(-0.75 + r1.w);
  r2.w = r3.y * r2.w;
  r1.xyz = r2.www * r1.xyz + r2.xyz;
  r2.xyzw = texRipple.Sample(_texRipple_s, v5.zw).xyzw;
  r2.x = r1.w * 16 + r2.y;
  r1.w = -r1.w * 4 + 1;
  r2.y = r2.z * 2 + -1;
  r2.x = saturate(abs(r2.y) * 2 + r2.x);
  r1.w = r2.x * r1.w;
  r1.w = r1.w * r1.w;
  r2.x = saturate(0.200000003 + v9.z);
  r0.w = saturate(r0.w * r1.w + r2.x);
  r2.xyzw = float4(25,25,100,100) * v5.zwzw;
  r2.zw = cbTimebase.SimTime.xx * float2(2,2) + r2.wz;
  r3.yz = float2(3.14159274,3.14159274) + r2.wz;
  r2.zw = sin(r2.zw);
  r4.xy = r2.zw * float2(0.0562499985,0.0562499985) + r2.xy;
  r2.zw = cos(r3.yz);
  r4.zw = r2.zw * float2(0.0562499985,0.0562499985) + r2.yx;
  r2.xyzw = texOcclusion.Sample(_texOcclusion_s, r4.xz).wxyz;
  r4.xyzw = texOcclusion.Sample(_texOcclusion_s, r4.wy).xyzw;
  r4.x = saturate(r4.x);
  r2.x = saturate(r2.x);
  r1.w = r2.x * r4.x;
  r0.w = r1.w * r0.w;
  r0.xyz = r0.www * r0.xyz + r1.xyz;
  r1.xyz = v6.xyz / r3.xxx;
  r0.w = -cbEnvironmentSettings.FogStartStopSky.x + r3.x;
  r0.w = max(0, r0.w);
  r1.w = dot(r1.xyz, cbExternalViewTransform.WorldView._m10_m11_m12);
  r2.x = dot(r1.xyz, cbExternalViewTransform.WorldView._m00_m01_m02);
  r1.x = dot(-r1.xyz, cbExternalViewTransform.WorldView._m20_m21_m22);
  r1.x = 1 + r1.x;
  r1.y = 0.5 * r1.x;
  r2.y = max(abs(r2.x), abs(r1.w));
  r2.y = 1 / r2.y;
  r2.z = min(abs(r2.x), abs(r1.w));
  r2.y = r2.z * r2.y;
  r2.z = r2.y * r2.y;
  r2.w = r2.z * 0.0208350997 + -0.0851330012;
  r2.w = r2.z * r2.w + 0.180141002;
  r2.w = r2.z * r2.w + -0.330299497;
  r2.z = r2.z * r2.w + 0.999866009;
  r2.w = r2.y * r2.z;
  r2.w = r2.w * -2 + 1.57079637;
  r3.x = cmp(abs(r2.x) < abs(r1.w));
  r2.w = r3.x ? r2.w : 0;
  r2.y = r2.y * r2.z + r2.w;
  r2.z = cmp(r2.x < -r2.x);
  r2.z = r2.z ? -3.141593 : 0;
  r2.y = r2.y + r2.z;
  r2.z = min(r2.x, r1.w);
  r1.w = max(r2.x, r1.w);
  r1.w = cmp(r1.w >= -r1.w);
  r2.x = cmp(r2.z < -r2.z);
  r1.w = r1.w ? r2.x : 0;
  r1.w = r1.w ? -r2.y : r2.y;
  r1.w = r1.w * 0.318310142 + 1;
  r1.x = 0.5 * r1.w;
  r1.w = cbEnvironmentSettings.FogStartStopSky.y + -cbEnvironmentSettings.FogStartStopSky.x;
  r0.w = r0.w / r1.w;
  r0.w = max(0, r0.w);
  r1.z = min(0.953125, r0.w);
  r1.xyzw = texFogCube.Sample(_texFogCube_s, r1.xyz).xyzw;
  r0.w = 1 + -r1.w;
  r0.xyz = r0.xyz * r0.www + r1.xyz;
  r1.xyz = float3(1.03999996,1.03999996,1.03999996) * r0.xyz;
  r0.xyz = float3(0.200000003,0.200000003,0.200000003) + r0.xyz;
  o0.xyz = r1.xyz / r0.xyz;
  o0.w = 1;
  return;
}
