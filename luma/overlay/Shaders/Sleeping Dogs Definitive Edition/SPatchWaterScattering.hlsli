#ifndef SPATCH_WATER_SCATTERING_HLSLI
#define SPATCH_WATER_SCATTERING_HLSLI

// Set by SPatchWater immediately around an exact water draw. The original
// game uses b0-b5 (or b0-b4 for the blend permutation); b13 is otherwise
// unbound at all three verified water draw sites.
cbuffer cbSPatchWater : register(b13)
{
  // x: fixed normalized strength. The verified Light0 input cannot support a
  // directional phase function, so the remaining values are padding.
  float4 SPatchWaterScattering;
}

static const float SPatchInvFourPi = 0.0795774683;

float3 SPatchEvaluateWaterSingleScattering(
  float3 normal,
  float3 viewPosition,
  float transmittance,
  float3 sceneAlbedo,
  float3 directLighting)
{
  const float strength = clamp(SPatchWaterScattering.x, 0.0, 2.0);
  if (strength <= 0.0)
  {
    return 0.0;
  }

  // Light0 is the aggregate direct-diffuse term already used by the stock
  // shader. Recover its RGB irradiance without collapsing it to a scalar or
  // recoloring it as sunlight. The lower albedo bound limits amplification to
  // 25x, while the finite cap prevents malformed HDR inputs from producing
  // infinities in the scattering result.
  const float3 boundedDirectDiffuse =
    min(max(directLighting, 0.0), 65504.0);
  const float3 boundedSceneAlbedo = max(saturate(sceneAlbedo), 0.04);
  const float3 recoveredDirectIrradiance = min(
    boundedDirectDiffuse / boundedSceneAlbedo, 65504.0);

  // The stock shader has already evaluated Beer transmittance T over the
  // camera path. An isotropic single-scattering lobe is the only defensible
  // phase model for aggregate Light0: there is no per-light direction or
  // visibility signal here. This coefficient is energy bounded below 0.16
  // even at the maximum user strength (2 / 4pi).
  const float safeTransmittance = saturate(transmittance);
  const float scatterIntegral = 1.0 - safeTransmittance;
  const float scatterWeight =
    strength * SPatchInvFourPi * scatterIntegral;
  const float3 mediumAlbedo = min(max(
    sbWaterLook.AbsorbExtinct.xyz * sbWaterLook.AbsorbExtinct.xyz,
    0.0), 1.0);
  return recoveredDirectIrradiance * mediumAlbedo * scatterWeight;
}

#endif
