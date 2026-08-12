// Exact native-pipeline PBR replacement for Sleeping Dogs: Definitive Edition.
//
// The replacement shaders preserve each native shader ABI and use only the
// resources and constant buffers already bound by the game. The eighteen
// replaceable variants are loaded and driver-validated before the component
// becomes ready. Ambient-only variant 0 and irradiance-volume variant 13
// deliberately remain native and therefore have no replacement-cache
// dependency.
// Any cache or validation failure leaves every native pipeline untouched.

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <examples/utils/crc32_hash.hpp>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "SPatchGraphicsComponents.hpp"
#include "SPatchIni.hpp"
#include "SPatchReShadeCallbackSafety.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::size_t kShaderCount = 20;
constexpr std::uint32_t kAllShaderMask = (1u << kShaderCount) - 1u;
constexpr std::uint32_t kNativeAmbientShaderMask = 1u << 0u;
constexpr std::uint32_t kNativeCompatibilityShaderMask = 1u << 13u;
constexpr std::uint32_t kReplaceableShaderMask =
    kAllShaderMask & ~kNativeAmbientShaderMask & ~kNativeCompatibilityShaderMask;
constexpr std::size_t kReplaceableShaderCount = kShaderCount - 2u;
static_assert(kReplaceableShaderMask == 0xFDFFEu);
static_assert(kReplaceableShaderCount == 18u);
#if defined(SPATCH_PBR_DEVELOPMENT)
constexpr char kBindTelemetryMode[] = "first-per-variant-bounded";
constexpr char kBindTelemetrySummary[] =
    "Development bind telemetry stops after all replacement variants bind or at 1800 presents.";
#else
constexpr char kBindTelemetryMode[] = "first-success-only";
constexpr char kBindTelemetrySummary[] =
    "Bind telemetry stops after the first tagged replacement bind; capture-only identities are opportunistic.";
#endif

constexpr bool IsReplaceableVariant(std::size_t variant) noexcept {
    return variant < kShaderCount &&
        (kReplaceableShaderMask & (1u << variant)) != 0u;
}

struct NativeShaderIdentity {
    std::uint32_t crc32 = 0;
    std::size_t byte_size = 0;
    std::array<std::uint8_t, 16> dxbc_checksum{};
};

// Variant order is the SPATCH_PBR_VARIANT macro order in SPatchPBR.hlsl.
constexpr std::array<NativeShaderIdentity, kShaderCount> kNativeIdentities = {{
    {0x12489767u, 3948u,
     {0x7F, 0x3F, 0x98, 0x47, 0x49, 0x04, 0x05, 0xCE,
      0xC8, 0x3E, 0x4F, 0x89, 0xC8, 0xE0, 0x18, 0x33}},
    {0x223AA776u, 12572u,
     {0x7B, 0x47, 0xD7, 0x2B, 0x48, 0x13, 0x0D, 0xB3,
      0xE6, 0x17, 0x61, 0x9B, 0xDB, 0xA0, 0xD1, 0x76}},
    {0x2AF235E8u, 4784u,
     {0x49, 0xAE, 0x36, 0x9D, 0xE2, 0x09, 0x95, 0x39,
      0x45, 0x97, 0xC2, 0xEC, 0x02, 0x3F, 0xAC, 0xFD}},
    {0x2D062589u, 10780u,
     {0xE7, 0xC2, 0x3F, 0x35, 0xC4, 0x1C, 0xF0, 0x15,
      0x70, 0x6F, 0xF4, 0x41, 0xB2, 0xD0, 0x84, 0x52}},
    {0x32E195A0u, 5120u,
     {0x1C, 0xB3, 0xEC, 0xB9, 0xA4, 0xD2, 0xD2, 0x88,
      0x00, 0x59, 0x27, 0x99, 0xC6, 0xDE, 0xDB, 0x62}},
    {0x398DA3BFu, 5576u,
     {0xC6, 0xF7, 0x99, 0xAD, 0x60, 0x8B, 0x63, 0x73,
      0x78, 0x55, 0xEB, 0x54, 0x54, 0xC9, 0xAE, 0xFB}},
    {0x386DA32Cu, 3492u,
     {0x97, 0xC9, 0x4B, 0x08, 0x98, 0xFE, 0x2D, 0x63,
      0x04, 0x26, 0x0D, 0x86, 0x9B, 0xC2, 0xC1, 0x1D}},
    {0x5167FBBEu, 3636u,
     {0x37, 0x19, 0x3F, 0xAC, 0xCF, 0x30, 0x5E, 0xBF,
      0xAC, 0x43, 0x25, 0x39, 0x87, 0x35, 0xD1, 0x44}},
    {0x5EBBA455u, 6592u,
     {0x3F, 0x6D, 0x72, 0x0A, 0x81, 0x35, 0x2D, 0xC2,
      0x77, 0x61, 0x8B, 0xB1, 0x6E, 0x26, 0xF1, 0x5C}},
    {0x66072A23u, 3820u,
     {0x6B, 0x73, 0x76, 0xC2, 0x86, 0x02, 0x5D, 0xFF,
      0x6E, 0x5F, 0xF0, 0xA2, 0xCE, 0x61, 0x82, 0x2B}},
    {0x8A331B0Fu, 5364u,
     {0x1E, 0x8C, 0xA3, 0xC0, 0x10, 0xB2, 0x9D, 0xE3,
      0x69, 0xCD, 0xFE, 0x7C, 0x4E, 0x7A, 0x52, 0x99}},
    {0xA30CEF48u, 3964u,
     {0xB6, 0x14, 0xDC, 0x06, 0x1A, 0x1F, 0x11, 0x63,
      0xD9, 0x6C, 0x8C, 0x99, 0xFA, 0xC6, 0x33, 0x99}},
    {0xDCF9CD0Cu, 12400u,
     {0xF1, 0x1B, 0x4D, 0x1E, 0xAD, 0xCA, 0xEA, 0x71,
      0x45, 0xCE, 0x4E, 0xD8, 0x07, 0xF1, 0x2B, 0x1B}},
    {0xD71D285Bu, 4704u,
     {0x12, 0xCB, 0xE9, 0x90, 0x81, 0xBE, 0xAA, 0xAF,
      0x63, 0x62, 0x27, 0xA4, 0x26, 0xC3, 0x14, 0x74}},
    {0xE5E2CE1Cu, 6768u,
     {0x6C, 0x0B, 0x40, 0x4C, 0x4F, 0xF0, 0x17, 0x34,
      0xC9, 0x4A, 0xDE, 0x40, 0xCF, 0xF6, 0x32, 0xA5}},
    {0xEFD8577Du, 10608u,
     {0x92, 0xE7, 0x3C, 0x05, 0x08, 0xC4, 0xE7, 0xFC,
      0x40, 0x42, 0xB9, 0x59, 0xBC, 0x1F, 0x54, 0x5D}},
    {0xF74BCE96u, 4960u,
     {0xBC, 0x12, 0xD1, 0x73, 0xDA, 0x9E, 0x11, 0x85,
      0x4F, 0xAC, 0xF7, 0x3F, 0xD6, 0x7A, 0x59, 0x89}},
    {0x282EE2DCu, 6408u,
     {0x68, 0xDA, 0x85, 0x0F, 0x28, 0xFB, 0xBE, 0x99,
      0x18, 0x05, 0x07, 0x41, 0x5A, 0x37, 0xA5, 0xFF}},
    {0x5DB1CB6Eu, 6672u,
     {0x61, 0xBE, 0xF4, 0x77, 0x4A, 0x18, 0x8E, 0x2A,
      0xCC, 0x2A, 0xD7, 0x5A, 0x5C, 0x6F, 0xE2, 0x88}},
    {0xE611C192u, 4916u,
     {0xBC, 0xE8, 0x9F, 0x85, 0x12, 0xCB, 0x38, 0xCE,
      0xCB, 0xFB, 0xAC, 0xE8, 0x84, 0xD0, 0xD2, 0x33}},

}};

// These two long-established variants remain individual runtime canaries. Full
// replacement coverage is enforced by the discovery/replacement masks; no
// single benchmark run is required to bind every exact identity.
constexpr std::array<std::size_t, 2> kRuntimeProvenVariants = {11u, 16u};
#if defined(SPATCH_PBR_DEVELOPMENT)
constexpr std::size_t kReflectionProbeShaderCount = 20;
constexpr std::uint32_t kAllReflectionProbeShaderMask =
    (1u << kReflectionProbeShaderCount) - 1u;
constexpr std::uint64_t kReflectionProbePresentCutoff = 11000u;

// Exact three-MRT material writers that bind texReflection. Development builds
// tag these native shaders without replacing them so a real draw can be proven
// before any mirror/reflection BRDF work changes the operating path.
constexpr std::array<NativeShaderIdentity, kReflectionProbeShaderCount>
    kReflectionProbeIdentities = {{
        {0x0DCA10DAu, 6308u,
         {0x96, 0xD8, 0x99, 0xA1, 0x62, 0xB9, 0x8E, 0xF4,
          0xD2, 0x1C, 0x0D, 0xA4, 0x81, 0x5C, 0x37, 0xA0}},
        {0x24574ED3u, 6228u,
         {0xBC, 0xE3, 0x0C, 0x2C, 0xFB, 0xC0, 0x55, 0xD2,
          0xD4, 0x84, 0x3F, 0x05, 0x76, 0x90, 0x46, 0x54}},
        {0x2CFF2B5Du, 5156u,
         {0x30, 0x63, 0xC6, 0x36, 0xBB, 0x41, 0x7C, 0x56,
          0x79, 0x36, 0x2C, 0x64, 0x18, 0xA0, 0x6C, 0x4F}},
        {0x2D8DA2BAu, 4752u,
         {0x45, 0xAC, 0xFE, 0xF1, 0x41, 0x32, 0x90, 0x78,
          0xCB, 0xA3, 0x78, 0xD1, 0x32, 0x66, 0x2F, 0x41}},
        {0x2EC4A963u, 6888u,
         {0x31, 0x0B, 0xAA, 0x0C, 0xE9, 0xDF, 0xB2, 0x54,
          0x8B, 0xAF, 0x7F, 0xF7, 0x1F, 0x0F, 0xD5, 0x76}},
        {0x3257E098u, 6632u,
         {0x64, 0xED, 0xE7, 0x90, 0x45, 0x7F, 0x94, 0xB2,
          0xEB, 0x3A, 0xF8, 0x25, 0x4B, 0x4E, 0xD8, 0xD5}},
        {0x42DBD5BDu, 5332u,
         {0x63, 0x86, 0x7C, 0xAA, 0xCE, 0x92, 0x0F, 0xF3,
          0xBB, 0x54, 0xCD, 0x87, 0xBA, 0xBC, 0xD7, 0x32}},
        {0x454B57B1u, 5536u,
         {0x9F, 0xA9, 0x88, 0xE5, 0x12, 0xDF, 0x1C, 0x63,
          0xAD, 0xA8, 0xD7, 0x6A, 0x6A, 0xFA, 0x06, 0x8C}},
        {0x4FB439C7u, 4576u,
         {0xC7, 0x95, 0x05, 0x63, 0x29, 0xD2, 0xAF, 0x95,
          0x79, 0x95, 0xE5, 0xC8, 0xF7, 0x30, 0x64, 0x70}},
        {0x5597B000u, 4828u,
         {0x51, 0xB1, 0x71, 0x02, 0x6E, 0xA0, 0x08, 0xAF,
          0xC1, 0xF7, 0x1B, 0x21, 0x3D, 0x8C, 0xAB, 0xDD}},
        {0x77CC2C5Fu, 6808u,
         {0x16, 0xF0, 0x8E, 0xBE, 0x62, 0x60, 0xFD, 0x33,
          0x64, 0xCD, 0xF0, 0x61, 0x0A, 0x65, 0xDF, 0xBC}},
        {0x8A252CB6u, 4956u,
         {0xB4, 0x69, 0x7F, 0xB5, 0x64, 0x18, 0xB0, 0xBE,
          0x08, 0x91, 0x9D, 0xFA, 0xC6, 0x2B, 0x85, 0x8C}},
        {0xAB2D12B2u, 5412u,
         {0x63, 0x9D, 0xB2, 0x3A, 0xD5, 0xE4, 0x40, 0x85,
          0xCE, 0x4B, 0x8D, 0xDB, 0x80, 0x79, 0x5D, 0xE3}},
        {0xAC542847u, 6052u,
         {0x28, 0x48, 0x7F, 0xBD, 0x61, 0x63, 0xF5, 0xAD,
          0xA7, 0x4C, 0x3D, 0xE8, 0x2E, 0xFC, 0x92, 0xAD}},
        {0xB00889E5u, 3488u,
         {0x28, 0x0A, 0xAB, 0xFA, 0x91, 0xD0, 0x45, 0xBF,
          0x1A, 0xE7, 0xED, 0xE9, 0x63, 0xD6, 0xAB, 0x02}},
        {0xC0B35F57u, 4780u,
         {0x5E, 0x37, 0x5F, 0x3C, 0x31, 0xC5, 0xC9, 0x34,
          0x23, 0x06, 0x65, 0xAB, 0xE9, 0xB6, 0x50, 0x98}},
        {0xE5EA07EBu, 3892u,
         {0x93, 0xED, 0x4D, 0x70, 0x43, 0x02, 0x62, 0x21,
          0xC8, 0x93, 0x56, 0x47, 0x07, 0x34, 0xA1, 0x22}},
        {0xE8430298u, 5360u,
         {0x10, 0xC5, 0x5A, 0xDF, 0x2A, 0x89, 0xF4, 0x0E,
          0x4D, 0xF7, 0x14, 0x11, 0x7F, 0x1E, 0xCD, 0x82}},
        {0xED3699E9u, 3308u,
         {0xE8, 0x31, 0x39, 0x5C, 0x38, 0x80, 0x0C, 0x7C,
          0x07, 0xBA, 0xBD, 0xD6, 0x54, 0xA2, 0x33, 0x9F}},
        {0xFE2350DDu, 4068u,
         {0xCB, 0x41, 0xB7, 0x66, 0x29, 0xA4, 0xBF, 0x16,
          0xB0, 0x13, 0xBC, 0x5B, 0x18, 0x6A, 0xBC, 0xDE}},
    }};
#endif

struct ReplacementIdentity {
    std::uint32_t crc32 = 0;
    std::size_t byte_size = 0;
    std::array<std::uint8_t, 16> dxbc_checksum{};
};

// Isolated replacement identity pins measured from the corrected full-strength
// HLSL build. The same bytes are present in both Development and Publishing
// package caches; structural DXBC, byte-unique, and driver validation remain
// mandatory in addition to these exact pins.
constexpr std::array<std::optional<ReplacementIdentity>, kShaderCount>
    kPinnedReplacementIdentities = {{
        ReplacementIdentity{0x0FD932A3u, 3936u,
            {{0xB5, 0x3D, 0x7A, 0xAF, 0x72, 0x3C, 0xF0, 0x53,
              0xCE, 0x1A, 0x76, 0xE0, 0x7B, 0x1B, 0xCB, 0x64}}},
        ReplacementIdentity{0xD5FBDF70u, 13820u,
            {{0xBD, 0x1C, 0x8C, 0x8C, 0x23, 0xEF, 0xFA, 0x4F,
              0xB6, 0x28, 0xDA, 0xFB, 0x20, 0x9A, 0xFF, 0x89}}},
        ReplacementIdentity{0x2F4A905Eu, 6076u,
            {{0x74, 0x4B, 0x94, 0x62, 0x1C, 0x5E, 0x02, 0x60,
              0x7E, 0xCD, 0xC8, 0x3A, 0xA0, 0x3A, 0x87, 0x1A}}},
        ReplacementIdentity{0xD6F90E53u, 12072u,
            {{0x7A, 0xA4, 0x8E, 0xD7, 0xCE, 0x65, 0xAA, 0xA6,
              0x76, 0xB9, 0x88, 0x0D, 0x28, 0x35, 0x9A, 0x63}}},
        ReplacementIdentity{0xFBA3A8B8u, 6412u,
            {{0x59, 0xA9, 0xEB, 0xCD, 0x7A, 0xBB, 0x20, 0x2B,
              0x31, 0xDE, 0x7F, 0x2D, 0xA8, 0x3B, 0x21, 0xD8}}},
        ReplacementIdentity{0x162481CEu, 7540u,
            {{0x1F, 0xE0, 0x58, 0x0B, 0xE7, 0x8F, 0x8E, 0xB6,
              0x26, 0xDB, 0xB7, 0xB9, 0xAA, 0xAE, 0x29, 0xDE}}},
        ReplacementIdentity{0x69C3C98Fu, 4804u,
            {{0xDD, 0x2B, 0xB4, 0x88, 0xBF, 0x9C, 0xEE, 0x97,
              0xAB, 0xBC, 0x91, 0xF0, 0xD9, 0xAE, 0x11, 0x54}}},
        ReplacementIdentity{0x91132438u, 4948u,
            {{0x10, 0x0B, 0x52, 0x84, 0x67, 0xCF, 0x18, 0x28,
              0xB6, 0x15, 0xC5, 0x65, 0x86, 0x10, 0xE8, 0xD0}}},
        ReplacementIdentity{0xDFCD9F2Fu, 7736u,
            {{0x18, 0x20, 0xB4, 0x71, 0x70, 0x60, 0xF3, 0xC6,
              0x94, 0xD1, 0xD6, 0x95, 0x7E, 0xEA, 0x1E, 0x73}}},
        ReplacementIdentity{0x8463A2C8u, 5092u,
            {{0x81, 0x8C, 0x0F, 0xF9, 0xED, 0x13, 0x80, 0xB3,
              0x92, 0xB3, 0x0E, 0x2C, 0x44, 0xC7, 0xD5, 0xE4}}},
        ReplacementIdentity{0xC12DADB2u, 6876u,
            {{0xC9, 0xCA, 0x07, 0xA3, 0x53, 0x2B, 0x4D, 0x6F,
              0x86, 0x1E, 0xD6, 0xAA, 0x8D, 0xAB, 0x8C, 0x8C}}},
        ReplacementIdentity{0xE8F50C5Bu, 5236u,
            {{0x66, 0x0E, 0x9D, 0x31, 0xF0, 0x96, 0x14, 0xD6,
              0x1E, 0xC5, 0x4D, 0x45, 0x42, 0xA7, 0x2B, 0xF5}}},
        ReplacementIdentity{0x557F67E5u, 13676u,
            {{0x7B, 0xD7, 0x7E, 0xD1, 0x9D, 0xAB, 0x15, 0x8B,
              0x0C, 0xC5, 0x6F, 0x70, 0xA9, 0x73, 0x14, 0xE7}}},
        ReplacementIdentity{0xC41227D1u, 5996u,
            {{0x91, 0x39, 0x55, 0xCF, 0x27, 0x0C, 0xA8, 0x99,
              0x20, 0x39, 0x58, 0x52, 0xAA, 0x28, 0x7A, 0x07}}},
        ReplacementIdentity{0xB6C9FE8Du, 7928u,
            {{0x23, 0x04, 0x15, 0x00, 0xDC, 0x5E, 0x69, 0x6F,
              0xEA, 0xD2, 0xB3, 0x10, 0x4C, 0x25, 0x5D, 0x98}}},
        ReplacementIdentity{0x8B078D24u, 11948u,
            {{0x9B, 0x63, 0x75, 0x86, 0x70, 0x64, 0xCC, 0x67,
              0x2E, 0xE6, 0xC6, 0x5B, 0xA2, 0x48, 0xA9, 0xE9}}},
        ReplacementIdentity{0xCCA1FF6Fu, 6248u,
            {{0x40, 0xCB, 0xBC, 0x72, 0xB7, 0xEF, 0xD5, 0xB2,
              0x89, 0xB8, 0x9D, 0xE1, 0xB2, 0x57, 0xF7, 0xF3}}},
        ReplacementIdentity{0xE87B3E61u, 6808u,
            {{0x03, 0xD6, 0xE7, 0xDD, 0x20, 0xE5, 0xD1, 0x8F,
              0x0D, 0x63, 0x10, 0x40, 0xA5, 0xE5, 0xEF, 0xB1}}},
        ReplacementIdentity{0x308BFF59u, 6776u,
            {{0x92, 0x49, 0xC7, 0xA7, 0x72, 0x5B, 0x7D, 0x3F,
              0xD1, 0xB1, 0x72, 0x88, 0x11, 0x4C, 0xAB, 0x26}}},
        ReplacementIdentity{0xD70A96E5u, 4960u,
            {{0xB6, 0x52, 0x4C, 0xDD, 0xE2, 0xDB, 0x66, 0x60,
              0xBB, 0x9F, 0x49, 0xE9, 0x17, 0x8F, 0x8E, 0xD8}}}

    }};

struct Settings {
    bool enabled = true;
};
#if defined(SPATCH_PBR_DEVELOPMENT)
constexpr std::size_t kReflectionPayloadResourceCapacity = 4;
constexpr std::size_t kReflectionConsumerCapacity = 16;
constexpr std::size_t kReflectionSrvSlotCount = 16;

struct PixelShaderTraceIdentity {
    std::uint32_t crc32 = 0;
    std::uint32_t byte_size = 0;
    std::array<std::uint8_t, 16> dxbc_checksum{};
};
static_assert(sizeof(PixelShaderTraceIdentity) == 24);
static_assert(std::is_trivially_copyable_v<PixelShaderTraceIdentity>);

struct ReflectionConsumerTrace {
    PixelShaderTraceIdentity identity{};
    std::uint32_t srv_slot_mask = 0;
    std::uint32_t writer_mask = 0;
};
#endif


struct __declspec(uuid("4E499442-DE75-43E9-868E-D8D6297A0DB5")) DeviceData {
    Settings settings;
    std::atomic<bool> ready{false};
    std::array<ComPtr<ID3DBlob>, kShaderCount> replacement_bytecode;
    std::atomic<std::uint64_t> presents{0};
    std::array<std::atomic<std::uint64_t>, kShaderCount> replacement_requests{};
    std::array<std::atomic<std::uint64_t>, kShaderCount> replacement_pipelines{};
    std::array<std::atomic<std::uint64_t>, kShaderCount> replacement_binds{};
    std::atomic<bool> bind_gate_registered{false};
    std::atomic<bool> first_bind_claimed{false};
    std::atomic<std::uint32_t> discovery_mask{0};
    std::atomic<std::uint32_t> replacement_mask{0};
    std::atomic<std::uint32_t> bound_mask{0};
    std::atomic<bool> collect_bind_telemetry{true};
    std::atomic<std::uint64_t> tag_failures{0};
    std::atomic<bool> logged_identity_failure{false};
    std::atomic<bool> logged_tag_failure{false};
#if defined(SPATCH_PBR_DEVELOPMENT)
    std::array<std::atomic<std::uint64_t>, kReflectionProbeShaderCount>
        reflection_probe_binds{};
    std::atomic<std::uint32_t> reflection_probe_discovery_mask{0};
    std::atomic<std::uint32_t> reflection_probe_pipeline_mask{0};
    std::atomic<std::uint32_t> reflection_probe_bound_mask{0};
    std::atomic<bool> collect_reflection_probe_telemetry{true};
    std::atomic<std::uint64_t> reflection_probe_tag_failures{0};
    std::atomic<bool> logged_reflection_probe_tag_failure{false};
    std::atomic<bool> collect_reflection_resource_telemetry{true};
    std::atomic<std::uint32_t> bound_reflection_probe_index{
        static_cast<std::uint32_t>(kReflectionProbeShaderCount)};
    std::atomic<std::uint32_t> bound_pixel_shader_crc32{0};
    std::atomic<std::uint32_t> bound_pixel_shader_byte_size{0};
    std::array<std::atomic<std::uint32_t>, 4>
        bound_pixel_shader_dxbc_checksum{};
    std::array<std::atomic<std::uint32_t>, kReflectionSrvSlotCount>
        reflection_payload_slot_writer_masks{};
    std::atomic<std::uint32_t> reflection_writer_draw_mask{0};
    std::atomic<std::uint64_t> reflection_descriptor_callbacks{0};
    std::atomic<std::uint64_t> reflection_payload_bind_updates{0};
    std::atomic<std::uint64_t> reflection_consumer_draws{0};
    std::atomic<std::uint64_t> reflection_resource_capture_failures{0};
    std::atomic<std::uint64_t> pixel_shader_identity_tag_failures{0};
    std::atomic<bool> logged_reflection_resource_capture_failure{false};
    std::atomic<bool> logged_pixel_shader_identity_tag_failure{false};
    std::mutex reflection_resource_trace_mutex;
    std::array<ComPtr<ID3D11Resource>, kReflectionPayloadResourceCapacity>
        reflection_payload_resources{};
    std::array<std::uint32_t, kReflectionPayloadResourceCapacity>
        reflection_payload_writer_masks{};
    std::atomic<std::size_t> reflection_payload_resource_count{0};
    std::array<ReflectionConsumerTrace, kReflectionConsumerCapacity>
        reflection_consumers{};
    std::atomic<std::size_t> reflection_consumer_count{0};
#endif
};

// Stored on each successfully replaced native D3D11 pixel-shader object. The
// payload is the zero-based SPATCH_PBR_VARIANT index.
constexpr GUID kPbrVariantTag = {
    0x5137A9BBu, 0x7096u, 0x490Eu,
    {0xA2, 0xA1, 0x59, 0x7E, 0x06, 0xC8, 0xD5, 0xAC}};
#if defined(SPATCH_PBR_DEVELOPMENT)
// Stored on exact native texReflection material-writer pixel shaders. These are
// telemetry-only tags; the native shader bytecode and bindings stay untouched.
constexpr GUID kPbrReflectionProbeTag = {
    0x7F4D76B3u, 0xA39Du, 0x45F8u,
    {0x8B, 0x33, 0x13, 0xA9, 0xC5, 0x42, 0x7E, 0x11}};
// Stored on every structurally valid Development pixel shader so a proven
// material payload can be followed into its exact downstream consumer.
constexpr GUID kPbrPixelShaderIdentityTag = {
    0xA184671Eu, 0x09A6u, 0x4D7Bu,
    {0xA3, 0xF5, 0xE2, 0x2A, 0x8D, 0x44, 0xC9, 0x70}};
#endif

HMODULE g_module = nullptr;
std::atomic<bool> g_bind_telemetry_armed{false};
std::atomic<std::uint32_t> g_bind_telemetry_devices{0};
std::mutex g_bind_telemetry_mutex;

void ArmBindTelemetry(DeviceData& data) {
    std::lock_guard<std::mutex> lock(g_bind_telemetry_mutex);
    bool expected = false;
    if (!data.bind_gate_registered.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }
    g_bind_telemetry_devices.fetch_add(1, std::memory_order_relaxed);
    g_bind_telemetry_armed.store(true, std::memory_order_release);
}

void DisarmBindTelemetry(DeviceData& data) {
    std::lock_guard<std::mutex> lock(g_bind_telemetry_mutex);
    if (!data.bind_gate_registered.exchange(
            false, std::memory_order_acq_rel)) {
        return;
    }
    const std::uint32_t devices =
        g_bind_telemetry_devices.load(std::memory_order_relaxed);
    if (devices != 0) {
        g_bind_telemetry_devices.store(devices - 1u, std::memory_order_relaxed);
    }
    g_bind_telemetry_armed.store(devices > 1u, std::memory_order_release);
}

template <typename Interface, typename Handle>
Interface* NativePointer(Handle handle) noexcept {
    static_assert(std::is_integral_v<Handle>);
    static_assert(sizeof(Handle) >= sizeof(std::uintptr_t));
    return reinterpret_cast<Interface*>(static_cast<std::uintptr_t>(handle));
}

void Log(reshade::log::level level, const char* format, ...) noexcept {
    std::array<char, 1024> message{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message.data(), message.size(), _TRUNCATE, format, args);
    va_end(args);
    reshade::log::message(level, message.data());
}

void ReportCallbackFailure(
    spatch::graphics::detail::CallbackFailure failure,
    const char* detail) noexcept {
    const char* const kind =
        failure == spatch::graphics::detail::CallbackFailure::allocation
        ? "allocation failure"
        : failure == spatch::graphics::detail::CallbackFailure::standard
        ? "C++ exception"
        : "unknown C++ exception";
    Log(reshade::log::level::error,
        "[ShenLong-PBR] ReShade callback dropped after %s%s%s; native lighting remains active.",
        kind,
        detail != nullptr ? ": " : "",
        detail != nullptr ? detail : "");
}

template <auto Callback>
using GuardedCallback = spatch::graphics::detail::ReShadeCallbackBoundary<
    Callback, ReportCallbackFailure>;

std::wstring ModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            g_module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        if (path.size() >= 32768) {
            return {};
        }
        path.resize((std::min)(path.size() * 2, std::size_t{32768}), L'\0');
    }
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    path.resize(separator + 1);
    return path;
}

Settings LoadSettings() {
    Settings settings;
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        settings.enabled = false;
        Log(reshade::log::level::error,
            "[ShenLong-PBR] Could not resolve the add-on directory; all native lighting shaders remain active.");
        return settings;
    }

    const std::wstring path = directory + L"ShenLong.ini";
    const bool master_enabled = spatch::graphics::ini::ReadBool(
        path, spatch::graphics::ini::kMasterEnabledKeys, false);
    using spatch::graphics::ini::Key;
    constexpr std::array pbr_keys{
        Key{L"PhysicallyBasedRendering", L"PhysicallyBasedRendering"},
        Key{L"Graphics", L"PhysicallyBasedRendering"},
        Key{L"ShenLong", L"PhysicallyBasedRendering"},
        Key{L"PhysicallyBasedRendering", L"pbr"},
        Key{L"Graphics", L"pbr"},
        Key{L"ShenLong", L"pbr"},
        Key{L"PhysicallyBasedRendering", L"physically_based_rendering"},
        Key{L"Graphics", L"physically_based_rendering"},
        Key{L"ShenLong", L"physically_based_rendering"},
    };
    const bool pbr_enabled = spatch::graphics::ini::ParseBool(
        spatch::graphics::ini::ReadFirst(path, pbr_keys), false);
    settings.enabled = master_enabled && pbr_enabled;
    Log(reshade::log::level::info,
        "[ShenLong-PBR] configured enabled=%d replaceable_variants=%zu native_passthrough_variants=2 strength=100 cache=PBR pipeline_replace=1 bind_telemetry=%s draw_replay=0.",
        settings.enabled ? 1 : 0,
        kReplaceableShaderCount,
        kBindTelemetryMode);
    return settings;
}

std::wstring CachePath(
    const std::wstring& directory,
    const NativeShaderIdentity& identity) {
    std::array<wchar_t, 64> file_name{};
    _snwprintf_s(file_name.data(), file_name.size(), _TRUNCATE,
        L"PBR-0x%08X.ps_4_0.cso", identity.crc32);
    return directory + L"ShenLong\\ShaderCache\\v1\\PBR\\" + file_name.data();
}

bool ReadUint32(
    const std::uint8_t* bytes,
    std::size_t byte_size,
    std::size_t offset,
    std::uint32_t& value) noexcept {
    constexpr std::size_t kUint32Bytes = sizeof(std::uint32_t);
    if (!bytes || byte_size < kUint32Bytes ||
        offset > byte_size - kUint32Bytes) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
    return true;
}

constexpr std::uint32_t FourCc(
    char first, char second, char third, char fourth) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(first)) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(second)) << 8u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(third)) << 16u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(fourth)) << 24u);
}

struct ChunkRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

bool HasValidPixelShaderDxbcStructure(
    const void* code,
    std::size_t byte_size) noexcept {
    constexpr std::size_t kFixedHeaderSize = 32;
    constexpr std::size_t kMaximumChunkCount = 64;
    if (!code || byte_size < kFixedHeaderSize + sizeof(std::uint32_t) ||
        byte_size > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }

    std::uint32_t container_version = 0;
    std::uint32_t declared_size = 0;
    std::uint32_t chunk_count = 0;
    if (!ReadUint32(bytes, byte_size, 20, container_version) ||
        !ReadUint32(bytes, byte_size, 24, declared_size) ||
        !ReadUint32(bytes, byte_size, 28, chunk_count) ||
        container_version != 1u || declared_size != byte_size ||
        chunk_count == 0 || chunk_count > kMaximumChunkCount ||
        chunk_count > (byte_size - kFixedHeaderSize) / sizeof(std::uint32_t)) {
        return false;
    }
    const std::size_t header_size =
        kFixedHeaderSize + static_cast<std::size_t>(chunk_count) * sizeof(std::uint32_t);

    std::array<ChunkRange, kMaximumChunkCount> ranges{};
    std::size_t range_count = 0;
    std::uint32_t shader_chunk_count = 0;
    for (std::uint32_t index = 0; index < chunk_count; ++index) {
        std::uint32_t chunk_offset_value = 0;
        if (!ReadUint32(bytes, byte_size,
                kFixedHeaderSize + static_cast<std::size_t>(index) * sizeof(std::uint32_t),
                chunk_offset_value)) {
            return false;
        }
        const std::size_t chunk_offset = chunk_offset_value;
        if (chunk_offset < header_size || (chunk_offset & 3u) != 0u ||
            chunk_offset > byte_size || byte_size - chunk_offset < 8u) {
            return false;
        }

        std::uint32_t chunk_kind = 0;
        std::uint32_t chunk_data_size_value = 0;
        if (!ReadUint32(bytes, byte_size, chunk_offset, chunk_kind) ||
            !ReadUint32(bytes, byte_size, chunk_offset + 4u, chunk_data_size_value)) {
            return false;
        }
        const std::size_t chunk_data_size = chunk_data_size_value;
        if (chunk_data_size > byte_size - chunk_offset - 8u) {
            return false;
        }
        const ChunkRange range{chunk_offset, chunk_offset + 8u + chunk_data_size};
        for (std::size_t previous = 0; previous < range_count; ++previous) {
            if (range.begin < ranges[previous].end &&
                ranges[previous].begin < range.end) {
                return false;
            }
        }
        ranges[range_count++] = range;

        if (chunk_kind != FourCc('S', 'H', 'D', 'R') &&
            chunk_kind != FourCc('S', 'H', 'E', 'X')) {
            continue;
        }
        if (chunk_data_size < 2u * sizeof(std::uint32_t)) {
            return false;
        }
        std::uint32_t version_token = 0;
        std::uint32_t token_count = 0;
        if (!ReadUint32(bytes, byte_size, chunk_offset + 8u, version_token) ||
            !ReadUint32(bytes, byte_size, chunk_offset + 12u, token_count)) {
            return false;
        }
        const std::uint32_t program_type = version_token >> 16u;
        const std::uint32_t major_version = (version_token >> 4u) & 0x0Fu;
        const std::uint32_t minor_version = version_token & 0x0Fu;
        if (program_type != 0u || major_version != 4u || minor_version != 0u ||
            token_count < 2u ||
            static_cast<std::size_t>(token_count) * sizeof(std::uint32_t) !=
                chunk_data_size) {
            return false;
        }
        ++shader_chunk_count;
    }
    return shader_chunk_count == 1u;
}

bool MatchesPinnedReplacementIdentity(
    std::size_t variant,
    const void* code,
    std::size_t byte_size) noexcept {
    if (variant >= kPinnedReplacementIdentities.size()) {
        return false;
    }
    const auto& expected = kPinnedReplacementIdentities[variant];
    if (!expected) {
        return true;
    }
    if (!code || byte_size != expected->byte_size || byte_size < 20u) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    return std::memcmp(bytes + 4u, expected->dxbc_checksum.data(),
               expected->dxbc_checksum.size()) == 0 &&
        compute_crc32(bytes, byte_size) == expected->crc32;
}

bool IsUniqueReplacement(
    const std::array<ComPtr<ID3DBlob>, kShaderCount>& bytecode,
    std::size_t variant) noexcept {
    ID3DBlob* candidate = bytecode[variant].Get();
    if (!candidate) {
        return false;
    }
    for (std::size_t previous = 0; previous < variant; ++previous) {
        ID3DBlob* other = bytecode[previous].Get();
        if (!other || candidate->GetBufferSize() != other->GetBufferSize()) {
            continue;
        }
        if (std::memcmp(candidate->GetBufferPointer(), other->GetBufferPointer(),
                candidate->GetBufferSize()) == 0) {
            return false;
        }
    }
    return true;
}

bool InitializeReplacements(ID3D11Device* device, DeviceData& data) {
    data.ready.store(false, std::memory_order_release);
    if (!device) {
        return false;
    }
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        Log(reshade::log::level::error,
            "[ShenLong-PBR] Could not resolve the cache directory; no PBR pipelines will be replaced.");
        return false;
    }

    std::array<ComPtr<ID3DBlob>, kShaderCount> bytecode;
    for (std::size_t variant = 0; variant < kShaderCount; ++variant) {
        if (!IsReplaceableVariant(variant)) {
            continue;
        }
        const std::wstring cache_path = CachePath(directory, kNativeIdentities[variant]);
        const HRESULT load_result = D3DReadFileToBlob(
            cache_path.c_str(), bytecode[variant].ReleaseAndGetAddressOf());
        if (FAILED(load_result) || !bytecode[variant]) {
            Log(reshade::log::level::error,
                "[ShenLong-PBR] Required cache 0x%08X could not be loaded from %ls (HRESULT=0x%08X); all PBR replacements are disabled.",
                kNativeIdentities[variant].crc32,
                cache_path.c_str(),
                static_cast<unsigned int>(load_result));
            return false;
        }

        const void* code = bytecode[variant]->GetBufferPointer();
        const std::size_t byte_size = bytecode[variant]->GetBufferSize();
        if (!HasValidPixelShaderDxbcStructure(code, byte_size)) {
            Log(reshade::log::level::error,
                "[ShenLong-PBR] Required cache 0x%08X is not a structurally valid ps_4_0 DXBC container; all PBR replacements are disabled.",
                kNativeIdentities[variant].crc32);
            return false;
        }
        if (!MatchesPinnedReplacementIdentity(variant, code, byte_size)) {
            Log(reshade::log::level::error,
                "[ShenLong-PBR] Required cache 0x%08X failed its pinned replacement identity; all PBR replacements are disabled.",
                kNativeIdentities[variant].crc32);
            return false;
        }
        if (!IsUniqueReplacement(bytecode, variant)) {
            Log(reshade::log::level::error,
                "[ShenLong-PBR] Required cache 0x%08X duplicates another PBR variant byte-for-byte; all PBR replacements are disabled.",
                kNativeIdentities[variant].crc32);
            return false;
        }

        // Validate the driver path before publishing any replacement pointer.
        ComPtr<ID3D11PixelShader> validation_shader;
        const HRESULT validation_result = device->CreatePixelShader(
            code, byte_size, nullptr, validation_shader.ReleaseAndGetAddressOf());
        if (FAILED(validation_result) || !validation_shader) {
            Log(reshade::log::level::error,
                "[ShenLong-PBR] Driver rejected required cache 0x%08X (HRESULT=0x%08X); all PBR replacements are disabled.",
                kNativeIdentities[variant].crc32,
                static_cast<unsigned int>(validation_result));
            return false;
        }
    }

    data.replacement_bytecode = std::move(bytecode);
    data.ready.store(true, std::memory_order_release);
    Log(reshade::log::level::info,
        "[ShenLong-PBR] ready=1 validated=%zu/%zu unique=%zu driver_accepted=%zu replacement_mask=0x%05X atomic_all_or_nothing=1.",
        kReplaceableShaderCount,
        kReplaceableShaderCount,
        kReplaceableShaderCount,
        kReplaceableShaderCount,
        kReplaceableShaderMask);
    Log(reshade::log::level::info,
        "[ShenLong-PBR] runtime replacement policy active=%zu/%zu replacement_target_mask=0x%05X native_ambient_mask=0x%05X native_compatibility_mask=0x%05X direct_specular_aa=opaque-normal-derivative,vehicle-glass=none.",
        kReplaceableShaderCount,
        kShaderCount,
        kReplaceableShaderMask,
        kNativeAmbientShaderMask,
        kNativeCompatibilityShaderMask);
    return true;
}

std::optional<std::size_t> ExactNativeIdentityIndex(
    const reshade::api::shader_desc& description) noexcept {
    if (!description.code || description.code_size < 20u) {
        return std::nullopt;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(description.code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return std::nullopt;
    }
    for (std::size_t variant = 0; variant < kNativeIdentities.size(); ++variant) {
        const NativeShaderIdentity& identity = kNativeIdentities[variant];
        if (description.code_size != identity.byte_size ||
            std::memcmp(bytes + 4u, identity.dxbc_checksum.data(),
                identity.dxbc_checksum.size()) != 0) {
            continue;
        }
        if (compute_crc32(bytes, description.code_size) == identity.crc32) {
            return variant;
        }
    }
    return std::nullopt;
}
#if defined(SPATCH_PBR_DEVELOPMENT)
std::optional<std::size_t> ExactReflectionProbeIdentityIndex(
    const reshade::api::shader_desc& description) noexcept {
    if (!description.code || description.code_size < 20u) {
        return std::nullopt;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(description.code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return std::nullopt;
    }
    for (std::size_t variant = 0;
         variant < kReflectionProbeIdentities.size();
         ++variant) {
        const NativeShaderIdentity& identity =
            kReflectionProbeIdentities[variant];
        if (description.code_size != identity.byte_size ||
            std::memcmp(bytes + 4u, identity.dxbc_checksum.data(),
                identity.dxbc_checksum.size()) != 0) {
            continue;
        }
        if (compute_crc32(bytes, description.code_size) == identity.crc32) {
            return variant;
        }
    }
    return std::nullopt;
}
#endif

std::optional<std::size_t> NearNativeIdentityIndex(
    const reshade::api::shader_desc& description) noexcept {
    if (!description.code || description.code_size < 20u) {
        return std::nullopt;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(description.code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return std::nullopt;
    }
    for (std::size_t variant = 0; variant < kNativeIdentities.size(); ++variant) {
        const NativeShaderIdentity& identity = kNativeIdentities[variant];
        if (description.code_size != identity.byte_size) {
            continue;
        }
        const bool checksum_matches = std::memcmp(
            bytes + 4u, identity.dxbc_checksum.data(),
            identity.dxbc_checksum.size()) == 0;
        const bool crc_matches =
            compute_crc32(bytes, description.code_size) == identity.crc32;
        if (checksum_matches != crc_matches) {
            return variant;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> ReplacementIdentityIndex(
    const DeviceData& data,
    const reshade::api::shader_desc& description) noexcept {
    if (!description.code || description.code_size == 0) {
        return std::nullopt;
    }
    for (std::size_t variant = 0; variant < kShaderCount; ++variant) {
        ID3DBlob* replacement = data.replacement_bytecode[variant].Get();
        if (replacement && description.code == replacement->GetBufferPointer() &&
            description.code_size == replacement->GetBufferSize()) {
            return variant;
        }
    }
    return std::nullopt;
}

void OnInitDevice(reshade::api::device* device) {
    DeviceData* data =
        spatch::graphics::detail::CreatePrivateData<DeviceData>(device);
    if (!data) {
        return;
    }
    data->settings = LoadSettings();
    if (!data->settings.enabled) {
        return;
    }
    if (device->get_api() != reshade::api::device_api::d3d11) {
        Log(reshade::log::level::warning,
            "[ShenLong-PBR] Unsupported graphics API; all native lighting shaders remain active.");
        return;
    }
    if (!InitializeReplacements(
            NativePointer<ID3D11Device>(device->get_native()), *data)) {
        Log(reshade::log::level::warning,
            "[ShenLong-PBR] ready=0; initialization was atomic and no PBR pipeline replacement is active.");
    } else {
        ArmBindTelemetry(*data);
    }
}

void OnDestroyDevice(reshade::api::device* device) {
    if (DeviceData* data = device->get_private_data<DeviceData>()) {
        DisarmBindTelemetry(*data);
    }
    device->destroy_private_data<DeviceData>();
}

bool OnCreatePipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!data || !data->ready.load(std::memory_order_acquire) ||
        !data->settings.enabled ||
        device->get_api() != reshade::api::device_api::d3d11) {
        return false;
    }
    if (subobjects == nullptr && subobject_count != 0) {
        return false;
    }

    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto& subobject = subobjects[index];
        if (subobject.type != reshade::api::pipeline_subobject_type::pixel_shader ||
            subobject.count == 0 || !subobject.data) {
            continue;
        }
        auto* descriptions = static_cast<reshade::api::shader_desc*>(subobject.data);
        for (std::uint32_t shader_index = 0;
             shader_index < subobject.count;
             ++shader_index) {
            auto& description = descriptions[shader_index];
#if defined(SPATCH_PBR_DEVELOPMENT)
            if (const std::optional<std::size_t> reflection_variant =
                    ExactReflectionProbeIdentityIndex(description)) {
                data->reflection_probe_discovery_mask.fetch_or(
                    1u << *reflection_variant, std::memory_order_release);
            }
#endif
            const std::optional<std::size_t> variant =
                ExactNativeIdentityIndex(description);
            if (!variant) {
                const std::optional<std::size_t> near_variant =
                    NearNativeIdentityIndex(description);
                if (near_variant && !data->logged_identity_failure.exchange(
                        true, std::memory_order_relaxed)) {
                    Log(reshade::log::level::error,
                        "[ShenLong-PBR] Refused a near-match for shader 0x%08X because its native DXBC identity was not exact; the native shader remains active.",
                        kNativeIdentities[*near_variant].crc32);
                }
                continue;
            }
            if (!IsReplaceableVariant(*variant)) {
                // Variant 0 is a synthetic environment pass, while variant 13
                // is a six-axis irradiance-volume pass with only a dominant
                // direction proxy. Neither exposes the reflected-radiance input
                // needed to replace its native energy split safely.
                continue;
            }

            ID3DBlob* replacement = data->replacement_bytecode[*variant].Get();
            if (!replacement) {
                // Readiness is all-or-nothing, so this is only a defensive
                // fail-open guard against unexpected lifetime corruption.
                continue;
            }
            description.code = replacement->GetBufferPointer();
            description.code_size = replacement->GetBufferSize();
            data->replacement_requests[*variant].fetch_add(
                1, std::memory_order_relaxed);
            data->discovery_mask.fetch_or(
                1u << *variant, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void OnInitPipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!data || !data->ready.load(std::memory_order_acquire) ||
        pipeline.handle == 0) {
        return;
    }
    if (subobjects == nullptr && subobject_count != 0) {
        return;
    }

    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto& subobject = subobjects[index];
        if (subobject.type != reshade::api::pipeline_subobject_type::pixel_shader ||
            subobject.count == 0 || !subobject.data) {
            continue;
        }
        const auto* descriptions =
            static_cast<const reshade::api::shader_desc*>(subobject.data);
        for (std::uint32_t shader_index = 0;
             shader_index < subobject.count;
             ++shader_index) {
#if defined(SPATCH_PBR_DEVELOPMENT)
            const reshade::api::shader_desc& trace_description =
                descriptions[shader_index];
            if (trace_description.code &&
                trace_description.code_size >= 20u &&
                trace_description.code_size <=
                    (std::numeric_limits<std::uint32_t>::max)()) {
                const auto* trace_bytes = static_cast<const std::uint8_t*>(
                    trace_description.code);
                if (std::memcmp(trace_bytes, "DXBC", 4) == 0) {
                    PixelShaderTraceIdentity trace_identity{};
                    trace_identity.crc32 =
                        compute_crc32(trace_bytes, trace_description.code_size);
                    trace_identity.byte_size = static_cast<std::uint32_t>(
                        trace_description.code_size);
                    std::memcpy(trace_identity.dxbc_checksum.data(),
                        trace_bytes + 4u,
                        trace_identity.dxbc_checksum.size());
                    ID3D11PixelShader* pipeline_shader =
                        NativePointer<ID3D11PixelShader>(pipeline.handle);
                    const HRESULT trace_tag_result =
                        pipeline_shader->SetPrivateData(
                            kPbrPixelShaderIdentityTag,
                            sizeof(trace_identity), &trace_identity);
                    if (FAILED(trace_tag_result)) {
                        data->pixel_shader_identity_tag_failures.fetch_add(
                            1, std::memory_order_relaxed);
                        if (!data->logged_pixel_shader_identity_tag_failure
                                .exchange(true, std::memory_order_relaxed)) {
                            Log(reshade::log::level::warning,
                                "[ShenLong-PBR] Development pixel-shader identity tag failed (HRESULT=0x%08X); native rendering is unchanged, but reflection consumer evidence may be incomplete.",
                                static_cast<unsigned int>(trace_tag_result));
                        }
                    }
                }
            }
#endif
#if defined(SPATCH_PBR_DEVELOPMENT)
            if (const std::optional<std::size_t> reflection_variant =
                    ExactReflectionProbeIdentityIndex(
                        descriptions[shader_index])) {
                const std::uint32_t tag_value =
                    static_cast<std::uint32_t>(*reflection_variant);
                ID3D11PixelShader* native_shader =
                    NativePointer<ID3D11PixelShader>(pipeline.handle);
                const HRESULT tag_result = native_shader->SetPrivateData(
                    kPbrReflectionProbeTag, sizeof(tag_value), &tag_value);
                if (FAILED(tag_result)) {
                    data->reflection_probe_tag_failures.fetch_add(
                        1, std::memory_order_relaxed);
                    if (!data->logged_reflection_probe_tag_failure.exchange(
                            true, std::memory_order_relaxed)) {
                        Log(reshade::log::level::warning,
                            "[ShenLong-PBR] Reflection probe shader tag failed (HRESULT=0x%08X); native rendering is unchanged, but bind evidence may be incomplete.",
                            static_cast<unsigned int>(tag_result));
                    }
                } else {
                    data->reflection_probe_pipeline_mask.fetch_or(
                        1u << *reflection_variant, std::memory_order_release);
                }
                return;
            }
#endif
            const std::optional<std::size_t> variant =
                ReplacementIdentityIndex(*data, descriptions[shader_index]);
            if (!variant) {
                continue;
            }

            data->replacement_pipelines[*variant].fetch_add(
                1, std::memory_order_relaxed);

            const std::uint32_t tag_value = static_cast<std::uint32_t>(*variant);
            ID3D11PixelShader* native_shader =
                NativePointer<ID3D11PixelShader>(pipeline.handle);
            const HRESULT tag_result = native_shader->SetPrivateData(
                kPbrVariantTag, sizeof(tag_value), &tag_value);
            if (FAILED(tag_result)) {
                data->tag_failures.fetch_add(1, std::memory_order_relaxed);
                if (!data->logged_tag_failure.exchange(
                        true, std::memory_order_relaxed)) {
                    Log(reshade::log::level::warning,
                        "[ShenLong-PBR] Replacement shader telemetry tag failed (HRESULT=0x%08X); the validated PBR pipeline remains active, but bind evidence may be incomplete.",
                        static_cast<unsigned int>(tag_result));
                }
            }
            // Publish confirmation after the native object's telemetry tag is
            // installed so a concurrent bind cannot observe the mask first.
            data->replacement_mask.fetch_or(
                1u << *variant, std::memory_order_release);
            return;
        }
    }
}

void OnBindPipeline(
    reshade::api::command_list* command_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline pipeline) {
#if defined(SPATCH_PBR_DEVELOPMENT)
    if (!command_list ||
        (stages & reshade::api::pipeline_stage::pixel_shader) == 0) {
        return;
    }
#else
    if (!command_list || pipeline.handle == 0 ||
        (stages & reshade::api::pipeline_stage::pixel_shader) == 0) {
        return;
    }
#endif
    if (!g_bind_telemetry_armed.load(std::memory_order_acquire)) {
        return;
    }
    DeviceData* data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!data || !data->ready.load(std::memory_order_acquire)) {
        return;
    }
#if defined(SPATCH_PBR_DEVELOPMENT)
    if (pipeline.handle == 0) {
        if (data->collect_reflection_resource_telemetry.load(
                std::memory_order_acquire)) {
            data->bound_pixel_shader_byte_size.store(
                0, std::memory_order_relaxed);
            for (auto& checksum_word :
                 data->bound_pixel_shader_dxbc_checksum) {
                checksum_word.store(0, std::memory_order_relaxed);
            }
            data->bound_reflection_probe_index.store(
                static_cast<std::uint32_t>(kReflectionProbeShaderCount),
                std::memory_order_relaxed);
            data->bound_pixel_shader_crc32.store(
                0, std::memory_order_release);
        }
        return;
    }
#endif

    ID3D11PixelShader* native_shader =
        NativePointer<ID3D11PixelShader>(pipeline.handle);
#if defined(SPATCH_PBR_DEVELOPMENT)
    const bool collect_replacement =
        data->collect_bind_telemetry.load(std::memory_order_acquire);
    const bool collect_reflection_probe =
        data->collect_reflection_probe_telemetry.load(
            std::memory_order_acquire);
    const bool collect_reflection_resource =
        data->collect_reflection_resource_telemetry.load(
            std::memory_order_acquire);
    if (!collect_replacement && !collect_reflection_probe &&
        !collect_reflection_resource) {
        DisarmBindTelemetry(*data);
        return;
    }

    if (collect_reflection_resource) {
        PixelShaderTraceIdentity trace_identity{};
        UINT trace_size = sizeof(trace_identity);
        const HRESULT trace_result = native_shader->GetPrivateData(
            kPbrPixelShaderIdentityTag, &trace_size, &trace_identity);
        if (SUCCEEDED(trace_result) && trace_size == sizeof(trace_identity)) {
            data->bound_pixel_shader_byte_size.store(
                trace_identity.byte_size, std::memory_order_relaxed);
            for (std::size_t checksum_index = 0;
                 checksum_index <
                    data->bound_pixel_shader_dxbc_checksum.size();
                 ++checksum_index) {
                std::uint32_t checksum_word = 0;
                std::memcpy(&checksum_word,
                    trace_identity.dxbc_checksum.data() +
                        checksum_index * sizeof(checksum_word),
                    sizeof(checksum_word));
                data->bound_pixel_shader_dxbc_checksum[checksum_index].store(
                    checksum_word, std::memory_order_relaxed);
            }
            data->bound_pixel_shader_crc32.store(
                trace_identity.crc32, std::memory_order_release);
        } else {
            data->bound_pixel_shader_byte_size.store(
                0, std::memory_order_relaxed);
            for (auto& checksum_word :
                 data->bound_pixel_shader_dxbc_checksum) {
                checksum_word.store(0, std::memory_order_relaxed);
            }
            data->bound_pixel_shader_crc32.store(
                0, std::memory_order_release);
        }
        data->bound_reflection_probe_index.store(
            static_cast<std::uint32_t>(kReflectionProbeShaderCount),
            std::memory_order_release);
    }

    if (collect_replacement &&
        data->replacement_mask.load(std::memory_order_acquire) != 0) {
        std::uint32_t variant = static_cast<std::uint32_t>(kShaderCount);
        UINT tag_size = sizeof(variant);
        const HRESULT result = native_shader->GetPrivateData(
            kPbrVariantTag, &tag_size, &variant);
        if (SUCCEEDED(result) && tag_size == sizeof(variant) &&
            variant < kShaderCount) {
            const std::uint32_t bit = 1u << variant;
            std::uint32_t observed =
                data->bound_mask.load(std::memory_order_acquire);
            if (data->replacement_binds[variant].exchange(
                    1, std::memory_order_acq_rel) == 0) {
                observed = data->bound_mask.fetch_or(
                    bit, std::memory_order_acq_rel) | bit;
            }
            if ((observed & kReplaceableShaderMask) ==
                kReplaceableShaderMask) {
                data->collect_bind_telemetry.store(
                    false, std::memory_order_release);
            }
        }
    }

    if (collect_reflection_probe || collect_reflection_resource) {
        std::uint32_t reflection_variant =
            static_cast<std::uint32_t>(kReflectionProbeShaderCount);
        UINT tag_size = sizeof(reflection_variant);
        const HRESULT result = native_shader->GetPrivateData(
            kPbrReflectionProbeTag, &tag_size, &reflection_variant);
        if (SUCCEEDED(result) && tag_size == sizeof(reflection_variant) &&
            reflection_variant < kReflectionProbeShaderCount) {
            if (collect_reflection_resource) {
                data->bound_reflection_probe_index.store(
                    reflection_variant, std::memory_order_release);
            }
            if (collect_reflection_probe) {
                const std::uint32_t bit = 1u << reflection_variant;
                std::uint32_t observed =
                    data->reflection_probe_bound_mask.load(
                        std::memory_order_acquire);
                if (data->reflection_probe_binds[reflection_variant].exchange(
                        1, std::memory_order_acq_rel) == 0) {
                    observed = data->reflection_probe_bound_mask.fetch_or(
                        bit, std::memory_order_acq_rel) | bit;
                    Log(reshade::log::level::info,
                        "[ShenLong-PBR] reflection-probe first-bind shader=0x%08X index=%u present=%llu native_pipeline=1 replacement=0.",
                        kReflectionProbeIdentities[reflection_variant].crc32,
                        static_cast<unsigned int>(reflection_variant),
                        static_cast<unsigned long long>(
                            data->presents.load(std::memory_order_relaxed)));
                }
                if ((observed & kAllReflectionProbeShaderMask) ==
                    kAllReflectionProbeShaderMask) {
                    data->collect_reflection_probe_telemetry.store(
                        false, std::memory_order_release);
                }
            }
        }
    }

    if (!data->collect_bind_telemetry.load(std::memory_order_acquire) &&
        !data->collect_reflection_probe_telemetry.load(
            std::memory_order_acquire) &&
        !data->collect_reflection_resource_telemetry.load(
            std::memory_order_acquire)) {
        DisarmBindTelemetry(*data);
    }
#else
    if (!data->collect_bind_telemetry.load(std::memory_order_acquire) ||
        data->replacement_mask.load(std::memory_order_acquire) == 0) {
        return;
    }

    std::uint32_t variant = static_cast<std::uint32_t>(kShaderCount);
    UINT tag_size = sizeof(variant);
    const HRESULT result = native_shader->GetPrivateData(
        kPbrVariantTag, &tag_size, &variant);
    if (FAILED(result) || tag_size != sizeof(variant) ||
        variant >= kShaderCount) {
        return;
    }

    // Publishing records exactly one successfully tagged replacement bind
    // globally, then disables the private-data lookup immediately.
    bool expected_claim = false;
    if (!data->first_bind_claimed.compare_exchange_strong(
            expected_claim, true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        data->collect_bind_telemetry.store(false, std::memory_order_release);
        DisarmBindTelemetry(*data);
        return;
    }
    data->replacement_binds[variant].fetch_add(1, std::memory_order_relaxed);
    // Republish the winning bit after the sample counter so checkpoint acquire
    // loads observe a coherent first-bind sample.
    data->bound_mask.store(1u << variant, std::memory_order_release);
    data->collect_bind_telemetry.store(false, std::memory_order_release);
    DisarmBindTelemetry(*data);
#endif
}

#if defined(SPATCH_PBR_DEVELOPMENT)
void ReportReflectionResourceCaptureFailure(
    DeviceData& data,
    std::uint32_t writer_index,
    const char* reason) noexcept {
    data.reflection_resource_capture_failures.fetch_add(
        1, std::memory_order_relaxed);
    if (!data.logged_reflection_resource_capture_failure.exchange(
            true, std::memory_order_relaxed)) {
        const std::uint32_t writer_crc =
            writer_index < kReflectionProbeShaderCount
            ? kReflectionProbeIdentities[writer_index].crc32
            : 0u;
        Log(reshade::log::level::warning,
            "[ShenLong-PBR] Development reflection resource trace failed writer=0x%08X index=%u reason=%s; native rendering is unchanged.",
            writer_crc,
            static_cast<unsigned int>(writer_index),
            reason ? reason : "unknown");
    }
}

bool GetTexture2DDescription(
    ID3D11Resource* resource,
    D3D11_TEXTURE2D_DESC& description) noexcept {
    if (!resource) {
        return false;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(
            IID_PPV_ARGS(texture.GetAddressOf()))) ||
        !texture) {
        return false;
    }
    texture->GetDesc(&description);
    return true;
}

void GetSrvMipRange(
    const D3D11_SHADER_RESOURCE_VIEW_DESC& description,
    UINT& most_detailed_mip,
    UINT& mip_levels) noexcept {
    most_detailed_mip = 0;
    mip_levels = 0;
    switch (description.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        most_detailed_mip = description.Texture2D.MostDetailedMip;
        mip_levels = description.Texture2D.MipLevels;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        most_detailed_mip = description.Texture2DArray.MostDetailedMip;
        mip_levels = description.Texture2DArray.MipLevels;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        most_detailed_mip = description.TextureCube.MostDetailedMip;
        mip_levels = description.TextureCube.MipLevels;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
        most_detailed_mip = description.TextureCubeArray.MostDetailedMip;
        mip_levels = description.TextureCubeArray.MipLevels;
        break;
    default:
        break;
    }
}

bool SamePixelShaderTraceIdentity(
    const PixelShaderTraceIdentity& first,
    const PixelShaderTraceIdentity& second) noexcept {
    return first.crc32 == second.crc32 &&
        first.byte_size == second.byte_size &&
        first.dxbc_checksum == second.dxbc_checksum;
}

bool CaptureReflectionWriterDraw(
    DeviceData& data,
    ID3D11DeviceContext* context,
    std::uint32_t writer_index) noexcept {
    if (!context || writer_index >= kReflectionProbeShaderCount) {
        return false;
    }
    const std::uint32_t writer_bit = 1u << writer_index;
    if ((data.reflection_writer_draw_mask.load(std::memory_order_acquire) &
            writer_bit) != 0u) {
        return true;
    }

    std::array<ID3D11RenderTargetView*, 3> raw_rtvs{};
    context->OMGetRenderTargets(
        static_cast<UINT>(raw_rtvs.size()), raw_rtvs.data(), nullptr);
    std::array<ComPtr<ID3D11RenderTargetView>, 3> rtvs{};
    for (std::size_t index = 0; index < rtvs.size(); ++index) {
        rtvs[index].Attach(raw_rtvs[index]);
        if (!rtvs[index]) {
            ReportReflectionResourceCaptureFailure(
                data, writer_index, "missing-three-mrt-binding");
            return false;
        }
    }

    std::array<D3D11_RENDER_TARGET_VIEW_DESC, 3> rtv_descriptions{};
    std::array<D3D11_TEXTURE2D_DESC, 3> rt_descriptions{};
    std::array<ComPtr<ID3D11Resource>, 3> rt_resources{};
    for (std::size_t index = 0; index < rtvs.size(); ++index) {
        rtvs[index]->GetDesc(&rtv_descriptions[index]);
        rtvs[index]->GetResource(rt_resources[index].GetAddressOf());
        if (!GetTexture2DDescription(
                rt_resources[index].Get(), rt_descriptions[index])) {
            ReportReflectionResourceCaptureFailure(
                data, writer_index, "mrt-is-not-texture2d");
            return false;
        }
    }

    ComPtr<ID3D11ShaderResourceView> reflection_srv;
    ID3D11ShaderResourceView* raw_reflection_srv = nullptr;
    context->PSGetShaderResources(1, 1, &raw_reflection_srv);
    reflection_srv.Attach(raw_reflection_srv);
    if (!reflection_srv) {
        ReportReflectionResourceCaptureFailure(
            data, writer_index, "missing-texReflection-t1");
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC reflection_srv_description{};
    reflection_srv->GetDesc(&reflection_srv_description);
    ComPtr<ID3D11Resource> reflection_resource;
    reflection_srv->GetResource(reflection_resource.GetAddressOf());
    D3D11_TEXTURE2D_DESC reflection_description{};
    if (!GetTexture2DDescription(
            reflection_resource.Get(), reflection_description)) {
        ReportReflectionResourceCaptureFailure(
            data, writer_index, "texReflection-is-not-texture2d");
        return false;
    }
    ComPtr<ID3D11SamplerState> reflection_sampler;
    ID3D11SamplerState* raw_reflection_sampler = nullptr;
    context->PSGetSamplers(1, 1, &raw_reflection_sampler);
    reflection_sampler.Attach(raw_reflection_sampler);
    if (!reflection_sampler) {
        ReportReflectionResourceCaptureFailure(
            data, writer_index, "missing-texReflection-s1");
        return false;
    }
    D3D11_SAMPLER_DESC sampler_description{};
    reflection_sampler->GetDesc(&sampler_description);

    std::size_t payload_index = 0;
    bool payload_stored = false;
    {
        std::lock_guard<std::mutex> lock(
            data.reflection_resource_trace_mutex);
        const std::size_t payload_count =
            data.reflection_payload_resource_count.load(
                std::memory_order_relaxed);
        for (; payload_index < payload_count; ++payload_index) {
            if (data.reflection_payload_resources[payload_index].Get() ==
                rt_resources[2].Get()) {
                data.reflection_payload_writer_masks[payload_index] |=
                    writer_bit;
                payload_stored = true;
                break;
            }
        }
        if (!payload_stored &&
            payload_count < data.reflection_payload_resources.size()) {
            payload_index = payload_count;
            data.reflection_payload_resources[payload_index] =
                rt_resources[2];
            data.reflection_payload_writer_masks[payload_index] =
                writer_bit;
            data.reflection_payload_resource_count.store(
                payload_count + 1u, std::memory_order_release);
            payload_stored = true;
        }
    }
    if (!payload_stored) {
        ReportReflectionResourceCaptureFailure(
            data, writer_index, "payload-resource-capacity");
        return false;
    }

    data.reflection_writer_draw_mask.fetch_or(
        writer_bit, std::memory_order_release);

    UINT reflection_most_detailed_mip = 0;
    UINT reflection_mip_levels = 0;
    GetSrvMipRange(
        reflection_srv_description,
        reflection_most_detailed_mip,
        reflection_mip_levels);
    Log(reshade::log::level::info,
        "[ShenLong-PBR] reflection-writer-resource shader=0x%08X index=%u present=%llu payload=%zu rt0(fmt=%u view_fmt=%u view_dim=%u %ux%u mips=%u samples=%u) rt1(fmt=%u view_fmt=%u view_dim=%u %ux%u mips=%u samples=%u) rt2(fmt=%u view_fmt=%u view_dim=%u %ux%u mips=%u samples=%u) texReflection=t1/s1(fmt=%u view_fmt=%u view_dim=%u %ux%u resource_mips=%u srv_mip=%u+%u filter=0x%X aniso=%u address=%u/%u/%u lod_bias=%.3f lod=%.3f..%.3f).",
        kReflectionProbeIdentities[writer_index].crc32,
        static_cast<unsigned int>(writer_index),
        static_cast<unsigned long long>(
            data.presents.load(std::memory_order_relaxed)),
        payload_index,
        static_cast<unsigned int>(rt_descriptions[0].Format),
        static_cast<unsigned int>(rtv_descriptions[0].Format),
        static_cast<unsigned int>(rtv_descriptions[0].ViewDimension),
        rt_descriptions[0].Width,
        rt_descriptions[0].Height,
        rt_descriptions[0].MipLevels,
        rt_descriptions[0].SampleDesc.Count,
        static_cast<unsigned int>(rt_descriptions[1].Format),
        static_cast<unsigned int>(rtv_descriptions[1].Format),
        static_cast<unsigned int>(rtv_descriptions[1].ViewDimension),
        rt_descriptions[1].Width,
        rt_descriptions[1].Height,
        rt_descriptions[1].MipLevels,
        rt_descriptions[1].SampleDesc.Count,
        static_cast<unsigned int>(rt_descriptions[2].Format),
        static_cast<unsigned int>(rtv_descriptions[2].Format),
        static_cast<unsigned int>(rtv_descriptions[2].ViewDimension),
        rt_descriptions[2].Width,
        rt_descriptions[2].Height,
        rt_descriptions[2].MipLevels,
        rt_descriptions[2].SampleDesc.Count,
        static_cast<unsigned int>(reflection_description.Format),
        static_cast<unsigned int>(reflection_srv_description.Format),
        static_cast<unsigned int>(
            reflection_srv_description.ViewDimension),
        reflection_description.Width,
        reflection_description.Height,
        reflection_description.MipLevels,
        reflection_most_detailed_mip,
        reflection_mip_levels,
        static_cast<unsigned int>(sampler_description.Filter),
        sampler_description.MaxAnisotropy,
        static_cast<unsigned int>(sampler_description.AddressU),
        static_cast<unsigned int>(sampler_description.AddressV),
        static_cast<unsigned int>(sampler_description.AddressW),
        sampler_description.MipLODBias,
        sampler_description.MinLOD,
        sampler_description.MaxLOD);
    return true;
}

std::uint32_t FindReflectionPayloadWriterMask(
    DeviceData& data,
    ID3D11Resource* resource) noexcept {
    if (!resource) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(
        data.reflection_resource_trace_mutex);
    const std::size_t payload_count =
        data.reflection_payload_resource_count.load(
            std::memory_order_acquire);
    for (std::size_t index = 0; index < payload_count; ++index) {
        if (data.reflection_payload_resources[index].Get() == resource) {
            return data.reflection_payload_writer_masks[index];
        }
    }
    return 0;
}

void OnPushDescriptors(
    reshade::api::command_list* command_list,
    reshade::api::shader_stage stages,
    reshade::api::pipeline_layout,
    std::uint32_t,
    const reshade::api::descriptor_table_update& update) {
    if (!command_list ||
        (stages & reshade::api::shader_stage::pixel) !=
            reshade::api::shader_stage::pixel ||
        update.type !=
            reshade::api::descriptor_type::shader_resource_view ||
        update.count == 0 || !update.descriptors ||
        update.binding >= kReflectionSrvSlotCount) {
        return;
    }
    DeviceData* data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!data || !data->ready.load(std::memory_order_acquire) ||
        !data->settings.enabled ||
        !data->collect_reflection_resource_telemetry.load(
            std::memory_order_acquire)) {
        return;
    }
    auto* context = NativePointer<ID3D11DeviceContext>(
        command_list->get_native());
    if (!context ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return;
    }

    data->reflection_descriptor_callbacks.fetch_add(
        1, std::memory_order_relaxed);
    const auto* views =
        static_cast<const reshade::api::resource_view*>(
            update.descriptors);
    for (std::uint32_t descriptor_index = 0;
         descriptor_index < update.count;
         ++descriptor_index) {
        const std::size_t slot =
            static_cast<std::size_t>(update.binding) +
            descriptor_index;
        if (slot >= kReflectionSrvSlotCount) {
            break;
        }

        std::uint32_t writer_mask = 0;
        if (views[descriptor_index].handle != 0) {
            auto* srv = NativePointer<ID3D11ShaderResourceView>(
                views[descriptor_index].handle);
            ComPtr<ID3D11Resource> resource;
            srv->GetResource(resource.GetAddressOf());
            writer_mask = FindReflectionPayloadWriterMask(
                *data, resource.Get());
        }
        data->reflection_payload_slot_writer_masks[slot].store(
            writer_mask, std::memory_order_release);
        if (writer_mask != 0) {
            data->reflection_payload_bind_updates.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

bool LoadBoundPixelShaderTraceIdentity(
    const DeviceData& data,
    PixelShaderTraceIdentity& identity) noexcept {
    const std::uint32_t first_crc =
        data.bound_pixel_shader_crc32.load(std::memory_order_acquire);
    if (first_crc == 0) {
        return false;
    }
    identity.crc32 = first_crc;
    identity.byte_size =
        data.bound_pixel_shader_byte_size.load(
            std::memory_order_relaxed);
    for (std::size_t checksum_index = 0;
         checksum_index <
            data.bound_pixel_shader_dxbc_checksum.size();
         ++checksum_index) {
        const std::uint32_t checksum_word =
            data.bound_pixel_shader_dxbc_checksum[checksum_index].load(
                std::memory_order_relaxed);
        std::memcpy(
            identity.dxbc_checksum.data() +
                checksum_index * sizeof(checksum_word),
            &checksum_word,
            sizeof(checksum_word));
    }
    const std::uint32_t second_crc =
        data.bound_pixel_shader_crc32.load(std::memory_order_acquire);
    return first_crc == second_crc && identity.byte_size >= 20u;
}

bool ObserveReflectionDraw(
    reshade::api::command_list* command_list) noexcept {
    if (!command_list) {
        return false;
    }
    DeviceData* data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!data || !data->ready.load(std::memory_order_acquire) ||
        !data->settings.enabled ||
        !data->collect_reflection_resource_telemetry.load(
            std::memory_order_acquire)) {
        return false;
    }
    auto* context = NativePointer<ID3D11DeviceContext>(
        command_list->get_native());
    if (!context ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }

    const std::uint32_t writer_index =
        data->bound_reflection_probe_index.load(
            std::memory_order_acquire);
    if (writer_index < kReflectionProbeShaderCount) {
        const std::uint32_t writer_bit = 1u << writer_index;
        if ((data->reflection_writer_draw_mask.load(
                std::memory_order_acquire) & writer_bit) == 0u) {
            CaptureReflectionWriterDraw(
                *data, context, writer_index);
        }
    }

    std::uint32_t srv_slot_mask = 0;
    std::uint32_t writer_mask = 0;
    for (std::size_t slot = 0;
         slot < data->reflection_payload_slot_writer_masks.size();
         ++slot) {
        const std::uint32_t slot_writer_mask =
            data->reflection_payload_slot_writer_masks[slot].load(
                std::memory_order_acquire);
        if (slot_writer_mask != 0) {
            srv_slot_mask |= 1u << slot;
            writer_mask |= slot_writer_mask;
        }
    }
    if (srv_slot_mask == 0 || writer_mask == 0) {
        return false;
    }

    PixelShaderTraceIdentity identity{};
    if (!LoadBoundPixelShaderTraceIdentity(*data, identity)) {
        return false;
    }
    data->reflection_consumer_draws.fetch_add(
        1, std::memory_order_relaxed);

    bool should_log = false;
    bool capacity_failure = false;
    std::uint32_t observed_slot_mask = srv_slot_mask;
    std::uint32_t observed_writer_mask = writer_mask;
    {
        std::lock_guard<std::mutex> lock(
            data->reflection_resource_trace_mutex);
        const std::size_t consumer_count =
            data->reflection_consumer_count.load(
                std::memory_order_relaxed);
        std::size_t consumer_index = 0;
        for (; consumer_index < consumer_count; ++consumer_index) {
            ReflectionConsumerTrace& consumer =
                data->reflection_consumers[consumer_index];
            if (!SamePixelShaderTraceIdentity(
                    consumer.identity, identity)) {
                continue;
            }
            const std::uint32_t previous_slot_mask =
                consumer.srv_slot_mask;
            const std::uint32_t previous_writer_mask =
                consumer.writer_mask;
            consumer.srv_slot_mask |= srv_slot_mask;
            consumer.writer_mask |= writer_mask;
            observed_slot_mask = consumer.srv_slot_mask;
            observed_writer_mask = consumer.writer_mask;
            should_log =
                previous_slot_mask != consumer.srv_slot_mask ||
                previous_writer_mask != consumer.writer_mask;
            break;
        }
        if (consumer_index == consumer_count) {
            if (consumer_count >=
                data->reflection_consumers.size()) {
                capacity_failure = true;
            } else {
                ReflectionConsumerTrace& consumer =
                    data->reflection_consumers[consumer_count];
                consumer.identity = identity;
                consumer.srv_slot_mask = srv_slot_mask;
                consumer.writer_mask = writer_mask;
                data->reflection_consumer_count.store(
                    consumer_count + 1u,
                    std::memory_order_release);
                should_log = true;
            }
        }
    }
    if (capacity_failure) {
        ReportReflectionResourceCaptureFailure(
            *data,
            static_cast<std::uint32_t>(kReflectionProbeShaderCount),
            "consumer-capacity");
        return false;
    }
    if (should_log) {
        const auto& checksum = identity.dxbc_checksum;
        Log(reshade::log::level::info,
            "[ShenLong-PBR] reflection-consumer shader=0x%08X bytes=%u dxbc=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X srv_slots=0x%04X writer_mask=0x%05X present=%llu native_pipeline=1 replacement=0.",
            identity.crc32,
            identity.byte_size,
            checksum[0], checksum[1], checksum[2], checksum[3],
            checksum[4], checksum[5], checksum[6], checksum[7],
            checksum[8], checksum[9], checksum[10], checksum[11],
            checksum[12], checksum[13], checksum[14], checksum[15],
            observed_slot_mask,
            observed_writer_mask,
            static_cast<unsigned long long>(
                data->presents.load(std::memory_order_relaxed)));
    }
    return false;
}

bool OnDraw(
    reshade::api::command_list* command_list,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t) {
    return ObserveReflectionDraw(command_list);
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::int32_t,
    std::uint32_t) {
    return ObserveReflectionDraw(command_list);
}
#endif


std::uint64_t SumCounters(
    const std::array<std::atomic<std::uint64_t>, kShaderCount>& counters) noexcept {
    std::uint64_t total = 0;
    for (const auto& counter : counters) {
        total += counter.load(std::memory_order_relaxed);
    }
    return total;
}
#if defined(SPATCH_PBR_DEVELOPMENT)
std::uint64_t SumReflectionProbeCounters(
    const std::array<std::atomic<std::uint64_t>,
        kReflectionProbeShaderCount>& counters) noexcept {
    std::uint64_t total = 0;
    for (const auto& counter : counters) {
        total += counter.load(std::memory_order_relaxed);
    }
    return total;
}
#endif

void LogCheckpoint(DeviceData& data, std::uint64_t presents) noexcept {
    const std::uint32_t discovered =
        data.discovery_mask.load(std::memory_order_acquire);
    const std::uint32_t replaced =
        data.replacement_mask.load(std::memory_order_acquire);
    const std::uint32_t bound = data.bound_mask.load(std::memory_order_acquire);
    const std::size_t a30 = kRuntimeProvenVariants[0];
    const std::size_t f74 = kRuntimeProvenVariants[1];
    Log(reshade::log::level::info,
        "[ShenLong-PBR] present=%llu enabled=1 ready=1 strength=100 validated=%zu/%zu native_passthrough=2 discovery_mask=0x%05X replacement_mask=0x%05X first_bound_mask=0x%05X requested=%llu confirmed=%llu first_bind_samples=%llu tag_failures=%llu; runtime-proven A30CEF48(requested=%llu,confirmed=%llu,first_bound=%llu) F74BCE96(requested=%llu,confirmed=%llu,first_bound=%llu). %s",
        static_cast<unsigned long long>(presents),
        kReplaceableShaderCount,
        kReplaceableShaderCount,
        discovered,
        replaced,
        bound,
        static_cast<unsigned long long>(SumCounters(data.replacement_requests)),
        static_cast<unsigned long long>(SumCounters(data.replacement_pipelines)),
        static_cast<unsigned long long>(SumCounters(data.replacement_binds)),
        static_cast<unsigned long long>(
            data.tag_failures.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.replacement_requests[a30].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.replacement_pipelines[a30].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.replacement_binds[a30].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.replacement_requests[f74].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.replacement_pipelines[f74].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.replacement_binds[f74].load(std::memory_order_relaxed)),
        kBindTelemetrySummary);
#if defined(SPATCH_PBR_DEVELOPMENT)
    Log(reshade::log::level::info,
        "[ShenLong-PBR] reflection-probe present=%llu candidates=%zu discovery_mask=0x%05X pipeline_mask=0x%05X first_bound_mask=0x%05X first_bind_samples=%llu tag_failures=%llu cutoff=%llu native_pipeline=1 replacement=0.",
        static_cast<unsigned long long>(presents),
        kReflectionProbeShaderCount,
        data.reflection_probe_discovery_mask.load(std::memory_order_acquire),
        data.reflection_probe_pipeline_mask.load(std::memory_order_acquire),
        data.reflection_probe_bound_mask.load(std::memory_order_acquire),
        static_cast<unsigned long long>(
            SumReflectionProbeCounters(data.reflection_probe_binds)),
        static_cast<unsigned long long>(
            data.reflection_probe_tag_failures.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(kReflectionProbePresentCutoff));
    Log(reshade::log::level::info,
        "[ShenLong-PBR] reflection-resource present=%llu writer_draw_mask=0x%05X payload_resources=%zu consumers=%zu descriptor_callbacks=%llu payload_bind_updates=%llu consumer_draws=%llu capture_failures=%llu identity_tag_failures=%llu cutoff=%llu native_pipeline=1 replacement=0.",
        static_cast<unsigned long long>(presents),
        data.reflection_writer_draw_mask.load(std::memory_order_acquire),
        data.reflection_payload_resource_count.load(
            std::memory_order_acquire),
        data.reflection_consumer_count.load(std::memory_order_acquire),
        static_cast<unsigned long long>(
            data.reflection_descriptor_callbacks.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.reflection_payload_bind_updates.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.reflection_consumer_draws.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.reflection_resource_capture_failures.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            data.pixel_shader_identity_tag_failures.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            kReflectionProbePresentCutoff));
#endif
}

void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    std::uint32_t,
    const reshade::api::rect*) {
    if (!swapchain) {
        return;
    }
    DeviceData* data =
        swapchain->get_device()->get_private_data<DeviceData>();
    if (!data || !data->ready.load(std::memory_order_acquire) ||
        !data->settings.enabled) {
        return;
    }
    const std::uint64_t presents =
        data->presents.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(SPATCH_PBR_DEVELOPMENT)
    if (presents == 300u || presents == 1800u || presents == 6000u ||
        presents == 10000u || presents == kReflectionProbePresentCutoff) {
        LogCheckpoint(*data, presents);
    }
    if (presents == 1800u) {
        data->collect_bind_telemetry.store(false, std::memory_order_release);
    }
    if (presents == kReflectionProbePresentCutoff) {
        data->collect_reflection_probe_telemetry.store(
            false, std::memory_order_release);
        data->collect_reflection_resource_telemetry.store(
            false, std::memory_order_release);
    }
    if (!data->collect_bind_telemetry.load(std::memory_order_acquire) &&
        !data->collect_reflection_probe_telemetry.load(
            std::memory_order_acquire) &&
        !data->collect_reflection_resource_telemetry.load(
            std::memory_order_acquire)) {
        DisarmBindTelemetry(*data);
    }
#else
    if (presents == 300u || presents == 1800u) {
        LogCheckpoint(*data, presents);
        if (presents == 1800u) {
            data->collect_bind_telemetry.store(
                false, std::memory_order_release);
            DisarmBindTelemetry(*data);
        }
    }
#endif
}

}  // namespace

namespace spatch::graphics::pbr {

void Attach(HMODULE module) {
    g_module = module;
    g_bind_telemetry_devices.store(0, std::memory_order_relaxed);
    g_bind_telemetry_armed.store(false, std::memory_order_release);
    reshade::register_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    reshade::register_event<reshade::addon_event::create_pipeline>(
        GuardedCallback<OnCreatePipeline>::Invoke);
    reshade::register_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::bind_pipeline>(
        GuardedCallback<OnBindPipeline>::Invoke);
#if defined(SPATCH_PBR_DEVELOPMENT)
    reshade::register_event<reshade::addon_event::push_descriptors>(
        GuardedCallback<OnPushDescriptors>::Invoke);
    reshade::register_event<reshade::addon_event::draw>(
        GuardedCallback<OnDraw>::Invoke);
    reshade::register_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
#endif
    reshade::register_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
}

void Detach() noexcept {
    g_bind_telemetry_devices.store(0, std::memory_order_relaxed);
    g_bind_telemetry_armed.store(false, std::memory_order_release);
    reshade::unregister_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::unregister_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
#if defined(SPATCH_PBR_DEVELOPMENT)
    reshade::unregister_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
    reshade::unregister_event<reshade::addon_event::draw>(
        GuardedCallback<OnDraw>::Invoke);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(
        GuardedCallback<OnPushDescriptors>::Invoke);
#endif
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(
        GuardedCallback<OnBindPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::create_pipeline>(
        GuardedCallback<OnCreatePipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    g_module = nullptr;
}

}  // namespace spatch::graphics::pbr
