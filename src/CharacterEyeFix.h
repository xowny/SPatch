#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace spatch::character_eye {

enum class RestoreResult : std::uint8_t {
    NotTarget,
    Applied,
    AlreadyApplied,
    InvalidLayout,
};

inline constexpr std::uint32_t kWeiHdEyeMaterialUid = 0x73253070;
inline constexpr std::uint32_t kWeiGangEyeMaterialUid = 0xDDEE3C42;
inline constexpr std::uint32_t kWeiGangHdEyeMaterialUid = 0x226C4AA5;
inline constexpr std::uint32_t kDefinitiveFallbackDiffuseUid = 0xB459B07C;
inline constexpr std::uint32_t kOriginalWeiHeadSdDiffuseUid = 0xD3BDF81A;
inline constexpr std::uint32_t kOriginalWeiHeadHdDiffuseUid = 0x13C67A2A;
inline constexpr std::uint32_t kOriginalWeiHeadDiffuseUid =
    kOriginalWeiHeadHdDiffuseUid;
inline constexpr std::uint32_t kWeiHeadHdBumpUid = 0x5636CAF9;
inline constexpr std::uint32_t kWeiGangBumpUid = 0x419D5106;
inline constexpr std::uint32_t kWeiGangHdBumpUid = 0xD7333651;
inline constexpr std::size_t kMaterialNameUidOffset = 0x18;
inline constexpr std::size_t kDiffuseResourceUidOffset = 0x150;
inline constexpr std::size_t kRequiredMaterialBytes = 0x208;

[[nodiscard]] bool IsTargetMaterialUid(std::uint32_t material_uid) noexcept;
[[nodiscard]] std::uint32_t OriginalDiffuseForMaterial(
    std::uint32_t material_uid) noexcept;
[[nodiscard]] std::uint32_t LogBitForMaterial(std::uint32_t material_uid) noexcept;

// Validates the complete identifying subset of each affected Wei eye material
// before changing its diffuse resource. The engine resolves the replacement
// handle later in rMaterial::OnLoad, so this only repairs the incorrect DE
// resource UID and does not take ownership of any engine object.
[[nodiscard]] RestoreResult ApplyOriginalWeiEyeDiffuse(
    std::span<std::byte> material) noexcept;

}  // namespace spatch::character_eye
