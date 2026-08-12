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

cbuffer sbWaterLook : register(b4)
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
SamplerState _texDepth_s : register(s5);
SamplerState _texDiffuseBlend_s : register(s6);
Texture3D<float4> texFogCube : register(t0);
TextureCube<float4> texEnvMap : register(t1);
Texture2D<float4> texNormal : register(t2);
Texture2D<float4> texAmbient : register(t3);
Texture2D<float4> texDiffuse2 : register(t4);
Texture2D<float4> texDepth : register(t5);
Texture2D<float4> texDiffuseBlend : register(t6);

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
  r0.xyz = r0.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.xyz = r1.xyz * float3(2,2,2) + r0.xyz;
  r0.xyz = float3(-1,-1,-1) + r0.xyz;
  r1.xy = float2(20,20) * v5.xy;
  r1.xy = cbTimebase.SimTime.xx * float2(0.282842726,-0.282842726) + r1.xy;
  r1.xyzw = texNormal.Sample(_texNormal_s, r1.xy).xyzw;
  r0.xyz = r1.xyz * float3(2,2,2) + r0.xyz;
  r0.xyz = float3(-1,-1,-1) + r0.xyz;
  r0.w = dot(v3.xyz, v3.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = v3.xyz * r0.www;
  r1.xyz = r1.xyz * r0.yyy;
  r0.y = dot(v2.xyz, v2.xyz);
  r0.y = rsqrt(r0.y);
  r2.xyz = v2.xyz * r0.yyy;
  r0.yzw = r2.xyz * r0.zzz + r1.xyz;
  r1.x = dot(v4.xyz, v4.xyz);
  r1.x = rsqrt(r1.x);
  r1.xyz = v4.xyz * r1.xxx;
  r0.xyz = r1.xyz * r0.xxx + r0.yzw;
  r0.w = dot(r0.xyz, r0.xyz);
  r0.w = rsqrt(r0.w);
  r0.xyz = r0.xyz * r0.www;
  r1.xyz = cbViewTransform.WorldView._m10_m11_m12 * r0.yyy;
  r1.xyz = r0.xxx * cbViewTransform.WorldView._m00_m01_m02 + r1.xyz;
  r1.xyz = r0.zzz * cbViewTransform.WorldView._m20_m21_m22 + r1.xyz;
  r0.w = dot(r1.xyz, r1.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = r1.xyz * r0.www;
  r0.w = dot(v7.xyz, v7.xyz);
  r0.w = rsqrt(r0.w);
  r2.xyz = v7.xyz * r0.www;
  r0.w = dot(r2.xyz, r1.xyz);
  r0.w = r0.w + r0.w;
  r1.xyz = r1.xyz * -r0.www + r2.xyz;
  r0.w = max(-1, r1.z);
  r1.w = min(0, r0.w);
  r1.xyzw = texEnvMap.Sample(_texEnvMap_s, r1.xyw).xyzw;
  r2.xyz = float3(-1.03999996,-1.03999996,-1.03999996) + r1.xyz;
  r1.xyz = r1.xyz / r2.xyz;
  r0.w = dot(r0.xyz, v2.xyz);
  r2.xyz = v2.xyz * r0.www;
  r2.xyz = -r2.xyz * sbWaterLook.SpecSparkleRefraction.zzz + r0.xyz;
  r0.w = dot(r2.xyz, r2.xyz);
  r0.w = rsqrt(r0.w);
  r2.xyz = r2.xyz * r0.www;
  r0.w = dot(-v6.xyz, -v6.xyz);
  r0.w = rsqrt(r0.w);
  r3.xyz = -v6.xyz * r0.www + cbEnvironmentSettings.SunDir.xyz;
  r0.w = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.w);
  r3.xyz = r3.xyz * r0.www;
  r0.w = saturate(dot(r3.xyz, r2.xyz));
  r0.w = log2(r0.w);
  r0.w = sbWaterLook.SpecSparkleRefraction.x * r0.w;
  r0.w = exp2(r0.w);
  r1.w = 2 + sbWaterLook.SpecSparkleRefraction.x;
  r1.w = 0.125 * r1.w;
  r0.w = r1.w * r0.w;
  r0.w = sbWaterLook.SpecSparkleRefraction.y * r0.w;
  r1.w = dot(cbEnvironmentSettings.SunColor.xyz,
    float3(0.300000012,0.600000024,0.100000001));
  r0.w = r1.w * r0.w;
  r1.xyz = r1.xyz * float3(-0.200000003,-0.200000003,-0.200000003) + r0.www;
  r3.x = dot(r2.xyz, v3.xyz);
  r3.y = dot(r2.xyz, v4.xyz);
  r2.xy = v8.xy / v8.zz;
  r2.xy = float2(0.5,0.5) + r2.xy;
  r2.zw = r3.xy * sbWaterLook.SpecSparkleRefraction.ww + r2.xy;
  r3.xy = saturate(r2.zw);
  r3.xyzw = texDepth.Sample(_texDepth_s, r3.xy).xyzw;
  r0.w = cbExternalViewTransform.ViewScaleAndNearFar.w + -cbExternalViewTransform.ViewScaleAndNearFar.z;
  r1.w = -r3.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r3.x = cbExternalViewTransform.ViewScaleAndNearFar.w * cbExternalViewTransform.ViewScaleAndNearFar.z;
  r1.w = r3.x / r1.w;
  r3.yz = saturate(r2.xy);
  r2.xy = r2.xy + -r2.zw;
  r4.xyzw = texDepth.Sample(_texDepth_s, r3.yz).xyzw;
  r0.w = -r4.x * r0.w + cbExternalViewTransform.ViewScaleAndNearFar.w;
  r0.w = r3.x / r0.w;
  r3.x = r0.w + -r1.w;
  r0.w = -v6.z + -r0.w;
  r3.y = 0.00999999978 + -v6.z;
  r3.y = cmp(r3.y >= r1.w);
  r3.y = r3.y ? 1.000000 : 0;
  r3.x = r3.x * r3.y + r1.w;
  r1.w = -v6.z + -r1.w;
  r2.xy = r2.xy * r3.yy + r2.zw;
  r2.zw = r2.xy * float2(2,2) + float2(-1,-1);
  r2.zw = r2.zw * r3.xx;
  r4.z = -r3.x;
  r4.xy = cbExternalViewTransform.ViewScaleAndNearFar.xy * r2.zw;
  r4.w = -r4.y;
  r2.z = dot(r4.xwz, cbExternalViewTransform.SkyFogDir.xyz);
  r2.w = dot(v6.xyz, cbExternalViewTransform.SkyFogDir.xyz);
  r2.z = r2.z + -r2.w;
  r2.z = sbWaterLook.VerticalAbsorbExtinct.w * -abs(r2.z);
  r2.z = 1.44269502 * r2.z;
  r2.z = exp2(r2.z);
  r4.xyzw = texAmbient.Sample(_texAmbient_s, r2.xy).xyzw;
  r5.xyzw = texDiffuse2.Sample(_texDiffuse2_s, r2.xy).xyzw;
  spatchSceneAlbedo = r4.xyz * r4.xyz;
  spatchDirectLighting = r5.xyz;
  r2.xyw = r4.xyz * r4.xyz;
  r2.xyw = r2.xyw * cbEnvironmentSettings.AmbientColorHorizon.xyz + r5.xyz;
  r2.xyw = -sbWaterLook.VerticalAbsorbExtinct.xyz * sbWaterLook.VerticalAbsorbExtinct.xyz + r2.xyw;
  r3.xzw = sbWaterLook.VerticalAbsorbExtinct.xyz * sbWaterLook.VerticalAbsorbExtinct.xyz;
  r2.xyz = r2.zzz * r2.xyw + r3.xzw;
  r2.xyz = -sbWaterLook.AbsorbExtinct.xyz * sbWaterLook.AbsorbExtinct.xyz + r2.xyz;
  r0.w = -abs(r1.w) + abs(r0.w);
  r0.w = r0.w * r3.y + abs(r1.w);
  r0.w = sbWaterLook.AbsorbExtinct.w * -r0.w;
  r0.w = 1.44269502 * r0.w;
  r0.w = exp2(r0.w);
  r4.xyz = sbWaterLook.AbsorbExtinct.xyz * sbWaterLook.AbsorbExtinct.xyz;
  r2.xyz = r0.www * r2.xyz + r4.xyz;
  r2.xyz += SPatchEvaluateWaterSingleScattering(
    r0.xyz, v6.xyz, r0.w, spatchSceneAlbedo, spatchDirectLighting);
  r1.xyz = -r2.xyz + r1.xyz;
  r1.w = dot(v6.xyz, v6.xyz);
  r2.w = rsqrt(r1.w);
  r1.w = sqrt(r1.w);
  r4.xyz = v6.xyz * r2.www;
  r0.x = dot(r0.xyz, -r4.xyz);
  r0.x = min(1, abs(r0.x));
  r0.xw = float2(1,1) + -r0.xw;
  r0.x = log2(r0.x);
  r0.x = sbWaterLook.FresnelAndBias.x * r0.x;
  r0.x = exp2(r0.x);
  r0.x = saturate(sbWaterLook.FresnelAndBias.y + r0.x);
  r0.x = 0.5 * r0.x;
  r0.x = r0.x * r0.w;
  r0.xyz = r0.xxx * r1.xyz + r2.xyz;
  r0.w = v6.w * 5 + cbTimebase.SimTime.x;
  r0.w = cos(r0.w);
  r1.y = r0.w * 0.0199999996 + v7.w;
  r0.w = v7.w * 5 + cbTimebase.SimTime.x;
  r0.w = sin(r0.w);
  r1.x = r0.w * 0.0199999996 + v6.w;
  r2.xyzw = texDiffuseBlend.Sample(_texDiffuseBlend_s, r1.xy).xyzw;
  r1.xyz = r2.xyz * r2.xyz;
  r0.w = v1.w * r2.w;
  r1.xyz = r1.xyz * float3(0.25,0.25,0.25) + -r3.xzw;
  r1.xyz = r1.xyz * float3(0.949999988,0.949999988,0.949999988) + r3.xzw;
  r1.xyz = r1.xyz + -r0.xyz;
  r0.xyz = r0.www * r1.xyz + r0.xyz;
  r1.xyz = v6.xyz / r1.www;
  r0.w = -cbEnvironmentSettings.FogStartStopSky.x + r1.w;
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
