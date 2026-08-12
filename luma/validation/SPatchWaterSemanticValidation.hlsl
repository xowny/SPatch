// WARP-only semantic entry point for the production water helper include.
// These declarations reproduce the two runtime constant-buffer contracts that
// the include consumes; the helper implementation itself is never copied.
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

#include "../overlay/Shaders/Sleeping Dogs Definitive Edition/SPatchWaterScattering.hlsli"

cbuffer cbSPatchWaterSemanticValidation : register(b12)
{
  float4 ValidationA;
  float4 ValidationB;
  float4 ValidationC;
  float4 ValidationD;
}

float4 SPatchWaterSemanticValidationMain() : SV_Target0
{
#if SPATCH_SEMANTIC_NEGATIVE_CONTROL
  return 0.0;
#else
  return float4(SPatchEvaluateWaterSingleScattering(
    ValidationB.xyz, ValidationB.www, ValidationA.y,
    ValidationC.xyz, ValidationD.xyz), 1.0);
#endif
}
