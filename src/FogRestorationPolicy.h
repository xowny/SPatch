#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace spatch::fog_restoration {

// Sleeping Dogs (2012) has the regular time-of-day/night-fog path but no
// Definitive Edition volumetric-fog layer. Zero is therefore the exact
// original-equivalent multiplier for the DE-only layer.
inline constexpr float kOriginalGameIntensity = 0.0f;
inline constexpr std::uintptr_t kIntensityRva = 0x02163804;
inline constexpr std::uintptr_t kLegacySetterRva = 0x004EF4E0;
inline constexpr std::uintptr_t kLatestSteamSetterRva = 0x004EF500;

inline constexpr std::array<std::uint8_t, 25> kLegacySetterSignature{
    0x48, 0x8B, 0x41, 0x60, 0x48, 0x8B, 0x08, 0x48, 0x8B, 0x41, 0x08, 0xF3, 0x0F,
    0x10, 0x40, 0x20, 0xF3, 0x0F, 0x11, 0x05, 0x0C, 0x43, 0xC7, 0x01, 0xC3};
inline constexpr std::array<std::uint8_t, 25> kLatestSteamSetterSignature{
    0x48, 0x8B, 0x41, 0x60, 0x48, 0x8B, 0x08, 0x48, 0x8B, 0x41, 0x08, 0xF3, 0x0F,
    0x10, 0x40, 0x20, 0xF3, 0x0F, 0x11, 0x05, 0xEC, 0x42, 0xC7, 0x01, 0xC3};

constexpr std::uintptr_t SetterRva(bool latest_steam) noexcept {
    return latest_steam ? kLatestSteamSetterRva : kLegacySetterRva;
}

inline std::span<const std::uint8_t> SetterSignature(bool latest_steam) noexcept {
    return latest_steam ? std::span<const std::uint8_t>(kLatestSteamSetterSignature)
                        : std::span<const std::uint8_t>(kLegacySetterSignature);
}

constexpr bool HasSetterShape(std::span<const std::uint8_t> code) noexcept {
    constexpr std::array<std::uint8_t, 20> prefix{0x48, 0x8B, 0x41, 0x60, 0x48, 0x8B, 0x08,
                                                  0x48, 0x8B, 0x41, 0x08, 0xF3, 0x0F, 0x10,
                                                  0x40, 0x20, 0xF3, 0x0F, 0x11, 0x05};
    if (code.size() != 25 || code[24] != 0xC3) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (code[index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

constexpr std::uintptr_t DecodeIntensityRva(std::uintptr_t setter_rva,
                                            std::span<const std::uint8_t> code) noexcept {
    if (!HasSetterShape(code)) {
        return 0;
    }

    const std::uint32_t displacement_bits =
        static_cast<std::uint32_t>(code[20]) | (static_cast<std::uint32_t>(code[21]) << 8) |
        (static_cast<std::uint32_t>(code[22]) << 16) | (static_cast<std::uint32_t>(code[23]) << 24);
    const std::int64_t displacement =
        (displacement_bits & 0x80000000u) != 0
            ? static_cast<std::int64_t>(displacement_bits) - 0x100000000LL
            : static_cast<std::int64_t>(displacement_bits);
    return static_cast<std::uintptr_t>(static_cast<std::int64_t>(setter_rva + 24) + displacement);
}

constexpr bool SignatureTargetsIntensity(std::uintptr_t setter_rva,
                                         std::span<const std::uint8_t> code) noexcept {
    return DecodeIntensityRva(setter_rva, code) == kIntensityRva;
}

static_assert(SignatureTargetsIntensity(kLegacySetterRva, kLegacySetterSignature));
static_assert(SignatureTargetsIntensity(kLatestSteamSetterRva, kLatestSteamSetterSignature));

}  // namespace spatch::fog_restoration
