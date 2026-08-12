#include "CharacterEyeFix.h"

#include <array>
#include <cstring>

namespace spatch::character_eye {
namespace {

constexpr std::size_t kTypeUidOffset = 0x30;
constexpr std::size_t kParamCountOffset = 0x70;
constexpr std::size_t kParamsOffset = 0x80;
constexpr std::size_t kParamStride = 0x38;
constexpr std::size_t kParamStateUidOffset = 0x00;
constexpr std::size_t kParamValueTypeUidOffset = 0x04;
constexpr std::size_t kParamResourceUidOffset = 0x28;
constexpr std::size_t kParamResourceTypeUidOffset = 0x30;

constexpr std::uint32_t kMaterialTypeUid = 0xB4C26312;
constexpr std::uint32_t kParamCount = 7;
constexpr std::uint32_t kShaderStateUid = 0x5C19C934;
constexpr std::uint32_t kShaderResourceUid = 0x84889C9C;
constexpr std::uint32_t kShaderResourceTypeUid = 0x8B5561A1;
constexpr std::uint32_t kDiffuseStateUid = 0xDCE06689;
constexpr std::uint32_t kBumpStateUid = 0xADBE1A5A;
constexpr std::uint32_t kSphericalMapStateUid = 0x0490650C;
constexpr std::uint32_t kSphericalMapResourceUid = 0x785D3471;
constexpr std::uint32_t kTextureValueTypeUid = 0xC8377453;
constexpr std::uint32_t kTextureResourceTypeUid = 0x8B43FABF;

static_assert(kParamsOffset + kParamCount * kParamStride == kRequiredMaterialBytes);
static_assert(kParamsOffset + 3 * kParamStride + kParamResourceUidOffset ==
              kDiffuseResourceUidOffset);

std::uint32_t ReadU32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void WriteU32(std::span<std::byte> bytes,
              std::size_t offset,
              std::uint32_t value) noexcept {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

struct RequiredParam {
    std::uint32_t state_uid;
    std::uint32_t value_type_uid;
    std::uint32_t resource_uid;
    std::uint32_t resource_type_uid;
    bool allow_original_diffuse = false;
};

struct EyeVariant {
    std::uint32_t material_uid;
    std::uint32_t original_diffuse_uid;
    std::uint32_t bump_uid;
    std::uint32_t log_bit;
};

constexpr std::array kEyeVariants{
    EyeVariant{kWeiHdEyeMaterialUid,
               kOriginalWeiHeadHdDiffuseUid,
               kWeiHeadHdBumpUid,
               1u << 0},
    EyeVariant{kWeiGangEyeMaterialUid,
               kOriginalWeiHeadSdDiffuseUid,
               kWeiGangBumpUid,
               1u << 1},
    EyeVariant{kWeiGangHdEyeMaterialUid,
               kOriginalWeiHeadHdDiffuseUid,
               kWeiGangHdBumpUid,
               1u << 2},
};

const EyeVariant* FindVariant(std::uint32_t material_uid) noexcept {
    for (const EyeVariant& variant : kEyeVariants) {
        if (variant.material_uid == material_uid) {
            return &variant;
        }
    }
    return nullptr;
}

}  // namespace

bool IsTargetMaterialUid(std::uint32_t material_uid) noexcept {
    return FindVariant(material_uid) != nullptr;
}

std::uint32_t OriginalDiffuseForMaterial(std::uint32_t material_uid) noexcept {
    const EyeVariant* variant = FindVariant(material_uid);
    return variant == nullptr ? 0 : variant->original_diffuse_uid;
}

std::uint32_t LogBitForMaterial(std::uint32_t material_uid) noexcept {
    const EyeVariant* variant = FindVariant(material_uid);
    return variant == nullptr ? 0 : variant->log_bit;
}

RestoreResult ApplyOriginalWeiEyeDiffuse(std::span<std::byte> material) noexcept {
    if (material.size() < kMaterialNameUidOffset + sizeof(std::uint32_t)) {
        return RestoreResult::InvalidLayout;
    }
    const EyeVariant* variant =
        FindVariant(ReadU32(material, kMaterialNameUidOffset));
    if (variant == nullptr) {
        return RestoreResult::NotTarget;
    }
    if (material.size() < kRequiredMaterialBytes ||
        ReadU32(material, kTypeUidOffset) != kMaterialTypeUid ||
        ReadU32(material, kParamCountOffset) != kParamCount) {
        return RestoreResult::InvalidLayout;
    }

    const std::array required_params{
        RequiredParam{kShaderStateUid,
                      kShaderStateUid,
                      kShaderResourceUid,
                      kShaderResourceTypeUid},
        RequiredParam{kDiffuseStateUid,
                      kTextureValueTypeUid,
                      kDefinitiveFallbackDiffuseUid,
                      kTextureResourceTypeUid,
                      true},
        RequiredParam{kBumpStateUid,
                      kTextureValueTypeUid,
                      variant->bump_uid,
                      kTextureResourceTypeUid},
        RequiredParam{kSphericalMapStateUid,
                      kTextureValueTypeUid,
                      kSphericalMapResourceUid,
                      kTextureResourceTypeUid},
    };
    std::array<bool, required_params.size()> matched{};
    std::size_t diffuse_resource_offset = 0;
    std::uint32_t diffuse_resource_uid = 0;

    for (std::size_t index = 0; index < kParamCount; ++index) {
        const std::size_t base = kParamsOffset + index * kParamStride;
        const std::uint32_t state_uid = ReadU32(material, base + kParamStateUidOffset);

        for (std::size_t required_index = 0;
             required_index < required_params.size();
             ++required_index) {
            const RequiredParam& required = required_params[required_index];
            if (state_uid != required.state_uid) {
                continue;
            }
            if (matched[required_index] ||
                ReadU32(material, base + kParamValueTypeUidOffset) !=
                    required.value_type_uid ||
                ReadU32(material, base + kParamResourceTypeUidOffset) !=
                    required.resource_type_uid) {
                return RestoreResult::InvalidLayout;
            }

            const std::uint32_t resource_uid =
                ReadU32(material, base + kParamResourceUidOffset);
            if (required.allow_original_diffuse) {
                if (resource_uid != required.resource_uid &&
                    resource_uid != variant->original_diffuse_uid) {
                    return RestoreResult::InvalidLayout;
                }
                diffuse_resource_offset = base + kParamResourceUidOffset;
                diffuse_resource_uid = resource_uid;
            } else if (resource_uid != required.resource_uid) {
                return RestoreResult::InvalidLayout;
            }
            matched[required_index] = true;
            break;
        }
    }

    for (const bool found : matched) {
        if (!found) {
            return RestoreResult::InvalidLayout;
        }
    }
    if (diffuse_resource_offset != kDiffuseResourceUidOffset) {
        return RestoreResult::InvalidLayout;
    }
    if (diffuse_resource_uid == variant->original_diffuse_uid) {
        return RestoreResult::AlreadyApplied;
    }

    WriteU32(material, diffuse_resource_offset, variant->original_diffuse_uid);
    return RestoreResult::Applied;
}

}  // namespace spatch::character_eye
