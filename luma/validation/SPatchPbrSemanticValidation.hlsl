// WARP-only semantic entry point for the production PBR helper definitions.
// Including the shipped shader is intentional: the validator must execute the
// same HLSL functions used by the runtime variants, not a copied model.
#define SPATCH_PBR_VARIANT 0
#define SPATCH_PBR_STRENGTH 1.0
#include "../overlay/Shaders/Sleeping Dogs Definitive Edition/SPatchPBR.hlsl"

cbuffer cbSPatchPbrSemanticValidation : register(b12)
{
  float4 ValidationA;
  float4 ValidationB;
  float4 ValidationC;
  float4 ValidationD;
  float4 ValidationE;
  // x: operation, y: native exponent, z: geometric variance, w: metallic.
  float4 ValidationParams;
}

float4 SPatchPbrSemanticValidationMain() : SV_Target0
{
#if SPATCH_SEMANTIC_NEGATIVE_CONTROL
  return 0.0;
#else
  const uint operation = (uint)ValidationParams.x;
  if (operation == 0)
  {
    return float4(SPatchPBRFresnelCosineWithF90(
      ValidationA.x, ValidationD.xyz, ValidationE.xyz), 1.0);
  }
  if (operation == 1)
  {
    return float4(SPatchPBRDirectSpecularWithF90(
      ValidationA.xyz, ValidationB.xyz, ValidationC.xyz,
      ValidationParams.y, ValidationD.xyz, ValidationParams.z,
      ValidationE.xyz), 1.0);
  }
  if (operation == 2)
  {
    return float4(SPatchPBRDiffuseWeightWithF90(
      ValidationA.xyz, ValidationB.xyz, ValidationC.xyz,
      ValidationD.xyz, ValidationParams.w, ValidationE.xyz), 1.0);
  }
  if (operation == 3)
  {
    return float4(SPatchPBRGlassDirectSpecular(
      ValidationA.xyz, ValidationB.xyz, ValidationC.xyz,
      ValidationParams.y, ValidationD.xyz), 1.0);
  }
  if (operation == 4)
  {
    return float4(SPatchPBRNativeOpaqueF90(ValidationD.xyz), 1.0);
  }
  return float4(-10000.0, -10000.0, -10000.0, 1.0);
#endif
}
