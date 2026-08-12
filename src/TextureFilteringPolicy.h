#pragma once

#include <array>
#include <cstdint>

namespace spatch::texture_filtering {

inline constexpr int kOriginalAnisotropy = -1;
inline constexpr int kAnisotropy4x = 4;
inline constexpr int kAnisotropy8x = 8;
inline constexpr int kAnisotropy16x = 16;

[[nodiscard]] constexpr bool IsAcceptedAnisotropy(int value) noexcept {
    return value == kOriginalAnisotropy || value == kAnisotropy4x ||
           value == kAnisotropy8x || value == kAnisotropy16x;
}

// The game stores 1 << exponent.  Returning the stock exponent for Original
// (and for a defensive invalid input) keeps the detour fully transparent.
[[nodiscard]] constexpr int ResolveWriterExponent(
    int stock_exponent,
    int configured_anisotropy) noexcept {
    switch (configured_anisotropy) {
        case kAnisotropy4x:
            return 2;
        case kAnisotropy8x:
            return 3;
        case kAnisotropy16x:
            return 4;
        default:
            return stock_exponent;
    }
}

[[nodiscard]] constexpr bool ShouldInstallWriter(
    int configured_anisotropy) noexcept {
    return configured_anisotropy == kAnisotropy4x ||
           configured_anisotropy == kAnisotropy8x ||
           configured_anisotropy == kAnisotropy16x;
}

struct AddressProfile {
    std::uintptr_t sampler_builder_rva = 0;
    std::uintptr_t anisotropy_writer_rva = 0;
    std::uintptr_t force_trilinear_instruction_rva = 0;

    bool operator==(const AddressProfile&) const = default;
};

inline constexpr AddressProfile kLegacyResearchedAddresses{
    0x00A196E0,
    0x00A21F00,
    0x00A19788,
};
inline constexpr AddressProfile kLatestSteamAddresses{
    0x00A195B0,
    0x00A21DD0,
    0x00A19658,
};
inline constexpr std::uintptr_t kAnisotropyValueRva = 0x020F2A0C;

[[nodiscard]] constexpr AddressProfile SelectAddresses(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamAddresses
                               : kLegacyResearchedAddresses;
}

// Both supported executables contain this exact settings-writer prologue.
inline constexpr std::array<std::uint8_t, 19> kAnisotropyWriterSignature{
    0x48, 0x83, 0xEC, 0x28, 0x85, 0xC9, 0x74, 0x09, 0xB8, 0x01,
    0x00, 0x00, 0x00, 0xD3, 0xE0, 0xEB, 0x02, 0x33, 0xC0,
};

// Validate the sampler builder independently from the five bytes being
// replaced.  The prefix and suffix identify the exact trilinear-selection
// branch while still allowing Registry::Apply to classify either stock or an
// already-owned replacement at the instruction itself.
inline constexpr std::array<std::uint8_t, 34> kSamplerBuilderPrologue{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8D,
    0xBA, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0F, 0x48,
    0x85, 0xC9, 0x74, 0x0D, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10,
};
inline constexpr std::uintptr_t kForceBranchPrefixOffset = 0x94;
inline constexpr std::array<std::uint8_t, 20> kForceBranchPrefix{
    0x85, 0xC9, 0x74, 0x2C, 0xFF, 0xC9, 0x74, 0x0C, 0xFF, 0xC9,
    0x74, 0x1A, 0xFF, 0xC9, 0x74, 0x0F, 0xFF, 0xC9, 0x75, 0x31,
};
inline constexpr std::uintptr_t kForceInstructionOffset = 0xA8;
inline constexpr std::array<std::uint8_t, 5> kStockTrilinearInstruction{
    0xB8, 0x15, 0x00, 0x00, 0x00,
};
inline constexpr std::array<std::uint8_t, 5> kForcedAnisotropicInstruction{
    0xB8, 0x55, 0x00, 0x00, 0x00,
};
inline constexpr std::uintptr_t kForceBranchSuffixOffset = 0xAD;
inline constexpr std::array<std::uint8_t, 13> kForceBranchSuffix{
    0x89, 0x44, 0x24, 0x20, 0xEB, 0x26, 0xB9,
    0x55, 0x00, 0x00, 0x00, 0xEB, 0x1B,
};

static_assert(kForceBranchPrefixOffset + kForceBranchPrefix.size() ==
              kForceInstructionOffset);
static_assert(kForceInstructionOffset + kStockTrilinearInstruction.size() ==
              kForceBranchSuffixOffset);
static_assert(kLegacyResearchedAddresses.force_trilinear_instruction_rva -
                  kLegacyResearchedAddresses.sampler_builder_rva ==
              kForceInstructionOffset);
static_assert(kLatestSteamAddresses.force_trilinear_instruction_rva -
                  kLatestSteamAddresses.sampler_builder_rva ==
              kForceInstructionOffset);

}  // namespace spatch::texture_filtering
