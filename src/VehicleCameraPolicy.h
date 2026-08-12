#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace spatch::vehicle_camera {

struct AddressProfile {
    std::uintptr_t selector_rva = 0;
    std::uintptr_t parameter_setter_rva = 0;

    bool operator==(const AddressProfile&) const = default;
};

struct DynamicAddressProfile {
    std::uintptr_t update_rva = 0;
    std::uintptr_t desired_eye_look_up_rva = 0;
    std::uintptr_t angular_approach_rva = 0;
    std::uintptr_t follow_yaw_return_rva = 0;
    std::uintptr_t manual_yaw_recenter_return_rva = 0;

    bool operator==(const DynamicAddressProfile&) const = default;
};

inline constexpr std::uintptr_t kLegacySelectorRva = 0x003C18B0;
inline constexpr std::uintptr_t kLegacyChaseParameterSetterRva = 0x003C98C0;
inline constexpr std::uintptr_t kLatestSteamSelectorRva = 0x003C1760;
inline constexpr std::uintptr_t kLatestSteamChaseParameterSetterRva = 0x003C9760;
inline constexpr std::uintptr_t kLegacyChaseUpdateRva = 0x003D0000;
inline constexpr std::uintptr_t kLegacyDesiredEyeLookUpRva = 0x003CB1C0;
inline constexpr std::uintptr_t kLegacyAngularApproachRva = 0x00181A50;
inline constexpr std::uintptr_t kLegacyFollowYawReturnRva = 0x003D0919;
inline constexpr std::uintptr_t kLegacyManualYawRecenterReturnRva = 0x003D286D;
inline constexpr std::uintptr_t kLatestSteamChaseUpdateRva = 0x003CFEA0;
inline constexpr std::uintptr_t kLatestSteamDesiredEyeLookUpRva = 0x003CB060;
inline constexpr std::uintptr_t kLatestSteamAngularApproachRva = 0x00181AE0;
inline constexpr std::uintptr_t kLatestSteamFollowYawReturnRva = 0x003D07B9;
inline constexpr std::uintptr_t kLatestSteamManualYawRecenterReturnRva =
    0x003D270D;

inline constexpr AddressProfile kLegacyResearchedAddresses{
    kLegacySelectorRva,
    kLegacyChaseParameterSetterRva,
};
inline constexpr AddressProfile kLatestSteamAddresses{
    kLatestSteamSelectorRva,
    kLatestSteamChaseParameterSetterRva,
};
inline constexpr DynamicAddressProfile kLegacyDynamicAddresses{
    kLegacyChaseUpdateRva,
    kLegacyDesiredEyeLookUpRva,
    kLegacyAngularApproachRva,
    kLegacyFollowYawReturnRva,
    kLegacyManualYawRecenterReturnRva,
};
inline constexpr DynamicAddressProfile kLatestSteamDynamicAddresses{
    kLatestSteamChaseUpdateRva,
    kLatestSteamDesiredEyeLookUpRva,
    kLatestSteamAngularApproachRva,
    kLatestSteamFollowYawReturnRva,
    kLatestSteamManualYawRecenterReturnRva,
};

[[nodiscard]] constexpr AddressProfile SelectAddresses(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamAddresses
                               : kLegacyResearchedAddresses;
}

[[nodiscard]] constexpr DynamicAddressProfile SelectDynamicAddresses(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamDynamicAddresses
                               : kLegacyDynamicAddresses;
}

// The paired Ghidra disassembly and decompilation show the same Win64 ABI and
// prologue in both supported executables: RCX is ChaseCameraComponent, RDX is
// the selected (or transition-blended) ChaseCameraParameters block.
inline constexpr std::array<std::uint8_t, 31> kParameterSetterSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70,
};

// ChaseCameraComponent::Update(this, dt). The verified GotoAngleSnap path can
// invoke it recursively with dt=0, so any stateful detour must be nesting-safe
// and must not integrate time in the nested snap update.
inline constexpr std::array<std::uint8_t, 18> kChaseUpdateSignature{
    0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xFD,
    0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x58, 0x03, 0x00, 0x00,
};

// GameCameraComponent::SetDesiredEyeLookUp(this, dt, eye, look, up,
// collide, out_eye, out_look). Chase Update reaches this once after composing
// its desired pose and before the helper performs the stock collision path.
inline constexpr std::array<std::uint8_t, 23>
    kDesiredEyeLookUpSignature{
        0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x48,
        0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41,
        0x54, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xB8,
    };

// Shortest-path angular approach ABI: RCX=float*, XMM1=target,
// XMM2=radians/second, XMM3=delta seconds. The helper is shared, so the live
// detour additionally requires one of the two exact Chase return addresses and
// the matching component-field pointer.
inline constexpr std::array<std::uint8_t, 23>
    kLegacyAngularApproachSignature{
        0x48, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x2D,
        0xB0, 0xBB, 0x4E, 0x01, 0x0F, 0x28, 0xE1, 0x0F,
        0x29, 0x34, 0x24, 0xF3, 0x0F, 0x5C, 0x21,
    };
inline constexpr std::array<std::uint8_t, 23>
    kLatestSteamAngularApproachSignature{
        0x48, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x2D,
        0x88, 0xBB, 0x4E, 0x01, 0x0F, 0x28, 0xE1, 0x0F,
        0x29, 0x34, 0x24, 0xF3, 0x0F, 0x5C, 0x21,
    };

[[nodiscard]] constexpr std::array<std::uint8_t, 23>
SelectAngularApproachSignature(bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamAngularApproachSignature
                               : kLegacyAngularApproachSignature;
}

inline constexpr std::size_t kSubjectOffset = 0xB30;
inline constexpr std::size_t kPhysicsMoverOffset = 0x1A8;
// The paired selector disassembly/decompilation proves these subject fields.
// They distinguish the real Drive branch from aliased Race/HijackFront camera
// blocks used by trucks, vans, buses, motorcycles, and scooters.
inline constexpr std::size_t kSelectorFleeStateOffset = 0x344;
inline constexpr std::size_t kSelectorDriveObjectOffset = 0x1D8;
inline constexpr std::size_t kSelectorDriveObjectStateOffset = 0x68;
inline constexpr std::size_t kSelectorHijackObjectOffset = 0x148;
inline constexpr std::size_t kSelectorRaceObjectOffset = 0x178;
inline constexpr std::size_t kSelectorRaceObjectStateOffset = 0x688;
inline constexpr std::size_t kTargetParametersOffset = 0xB50;
inline constexpr std::size_t kUpdateEyeOffsetFlagOffset = 0xB59;
inline constexpr std::size_t kAimOrFocusFlagOffset = 0xB5C;
inline constexpr std::size_t kLookingBackFlagOffset = 0x6D0;
inline constexpr std::size_t kIsLookingAroundFlagOffset = 0x4F0;
inline constexpr std::size_t kLookaroundJoyInputOffset = 0x4F4;
inline constexpr std::size_t kLookupJoyInputOffset = 0x4F8;
inline constexpr std::size_t kLookupMouseOffset = 0x4FC;
inline constexpr std::size_t kAlternateLookFlagOffset = 0x380;
inline constexpr std::size_t kLookaroundCenterTimerOffset = 0x500;
inline constexpr std::size_t kLookaroundAngleOffset = 0x504;
inline constexpr std::size_t kLookaroundAngleDesiredOffset = 0x508;
inline constexpr std::size_t kCenteringSpeedMinComponentOffset = 0x514;
inline constexpr std::size_t kCenteringSpeedMaxComponentOffset = 0x518;
inline constexpr std::size_t kReverseSpeedComponentOffset = 0x51C;
inline constexpr std::size_t kOrbitSpeedComponentOffset = 0x520;
inline constexpr std::size_t kYawAngVelTimespanComponentOffset = 0x524;
inline constexpr std::size_t kLookOffsetMaxComponentOffset = 0x528;
inline constexpr std::size_t kLookOffsetAngVelMinComponentOffset = 0x52C;
inline constexpr std::size_t kLookOffsetAngVelMaxComponentOffset = 0x530;
inline constexpr std::size_t kPitchOffsetMaxComponentOffset = 0x534;
inline constexpr std::size_t kPitchOffsetLookFactorUpComponentOffset = 0x538;
inline constexpr std::size_t kPitchOffsetLookFactorDownComponentOffset = 0x53C;
inline constexpr std::size_t kPitchOffsetEyeFactorUpComponentOffset = 0x540;
inline constexpr std::size_t kPitchOffsetEyeFactorDownComponentOffset = 0x544;
inline constexpr std::size_t kForwardAngleOffset = 0x958;
inline constexpr std::size_t kForwardAngleDesiredOffset = 0x95C;
inline constexpr std::size_t kTargetPitchPositionOffset = 0x9A8;
inline constexpr std::size_t kTargetPitchTargetOffset = 0x9AC;
inline constexpr std::size_t kYawAngVelRunningSumOffset = 0xA64;
inline constexpr std::size_t kYawAngVelRunningTimeOffset = 0xA68;
inline constexpr std::size_t kEyeLockFlagOffset = 0xB6C;
inline constexpr std::size_t kLookLockFlagOffset = 0xB6D;
inline constexpr std::size_t kPhysicsMoverForwardSpeedOffset = 0x2B4;
inline constexpr std::size_t kPhysicsMoverSteeringInputOffset = 0x58;
inline constexpr std::size_t kPhysicsMoverInputFlagsOffset = 0x68;
inline constexpr std::uint32_t kPhysicsMoverInReverseFlag = 1u << 0;
inline constexpr std::uint32_t kPhysicsMoverHandbrakeFlag = 1u << 2;

[[nodiscard]] constexpr bool IsPhysicsMoverReversing(
    std::uint32_t input_flags) noexcept {
    return (input_flags & kPhysicsMoverInReverseFlag) != 0;
}
// The selector copies the outgoing parameter state here, then overwrites its
// eye/target ranges with the live Homer-spring values before blending. D28 is
// the remaining source weight: 1 at transition start and 0 at completion.
inline constexpr std::size_t kTransitionSourceParametersOffset = 0xBE8;
inline constexpr std::size_t kTransitionSourceWeightOffset = 0xD28;

inline constexpr std::size_t kParameterNamePointerOffset = 0x0;
inline constexpr std::size_t kParameterNameSymbolOffset = 0x8;
inline constexpr std::size_t kParameterContextOffset = 0xC;
inline constexpr std::size_t kParameterYawAngVelTimespanOffset = 0x40;
inline constexpr std::size_t kParameterLookOffsetMaxOffset = 0x44;
inline constexpr std::size_t kParameterLookOffsetAngVelMinOffset = 0x48;
inline constexpr std::size_t kParameterLookOffsetAngVelMaxOffset = 0x4C;
inline constexpr std::size_t kParameterPitchOffsetMaxOffset = 0x50;
inline constexpr std::size_t kParameterPitchOffsetLookFactorUpOffset = 0x54;
inline constexpr std::size_t kParameterPitchOffsetLookFactorDownOffset = 0x58;
inline constexpr std::size_t kParameterPitchOffsetEyeFactorUpOffset = 0x5C;
inline constexpr std::size_t kParameterPitchOffsetEyeFactorDownOffset = 0x60;
inline constexpr std::size_t kParameterCenteringSpeedMinOffset = 0xF0;
inline constexpr std::size_t kParameterCenteringSpeedMaxOffset = 0xF4;
inline constexpr std::size_t kParameterReverseSpeedOffset = 0xF8;
inline constexpr std::size_t kParameterOrbitSpeedOffset = 0x108;
inline constexpr std::uint32_t kFleeContext = 1;
inline constexpr std::uint32_t kDriveContext = 3;

// VehicleSubjectComponent::OnAttach resolves the complete ChaseCameraContext
// array into these pointer fields. The SDK enum order is confirmed by the
// selector's paired disassembly/decompilation. Normal player driving selects
// +0x388 at runtime; +0x3B0 is HijackFront, not the default road camera.
inline constexpr std::size_t kAimParametersSlotOffset = 0x370;
inline constexpr std::size_t kFleeParametersSlotOffset = 0x378;
inline constexpr std::size_t kRaceParametersSlotOffset = 0x380;
inline constexpr std::size_t kDriveParametersSlotOffset = 0x388;
inline constexpr std::size_t kBurnoutParametersSlotOffset = 0x390;
inline constexpr std::size_t kPassengerParametersSlotOffset = 0x398;
inline constexpr std::size_t kHijackBackParametersSlotOffset = 0x3A0;
inline constexpr std::size_t kHijackSideParametersSlotOffset = 0x3A8;
inline constexpr std::size_t kHijackFrontParametersSlotOffset = 0x3B0;
inline constexpr std::size_t kHijackTopParametersSlotOffset = 0x3B8;
inline constexpr std::size_t kLookBackParametersSlotOffset = 0x3C0;
inline constexpr std::size_t kLookSideParametersSlotOffset = 0x3C8;
inline constexpr std::size_t kLookUpParametersSlotOffset = 0x3D0;
inline constexpr std::array<std::size_t, 13> kParameterSlotOffsets{
    0x370,
    0x378,
    0x380,
    0x388,
    0x390,
    0x398,
    0x3A0,
    0x3A8,
    0x3B0,
    0x3B8,
    0x3C0,
    0x3C8,
    0x3D0,
};
inline constexpr std::size_t kAimParametersSlotIndex = 0;
inline constexpr std::size_t kFleeParametersSlotIndex = 1;
inline constexpr std::size_t kRaceParametersSlotIndex = 2;
inline constexpr std::size_t kDriveParametersSlotIndex = 3;
inline constexpr std::size_t kBurnoutParametersSlotIndex = 4;
inline constexpr std::size_t kPassengerParametersSlotIndex = 5;
inline constexpr std::size_t kHijackBackParametersSlotIndex = 6;
inline constexpr std::size_t kHijackSideParametersSlotIndex = 7;
inline constexpr std::size_t kHijackFrontParametersSlotIndex = 8;
inline constexpr std::size_t kHijackTopParametersSlotIndex = 9;
inline constexpr std::size_t kLookBackParametersSlotIndex = 10;
inline constexpr std::size_t kLookSideParametersSlotIndex = 11;
inline constexpr std::size_t kLookUpParametersSlotIndex = 12;

using ParameterSlotMask = std::uint16_t;

[[nodiscard]] constexpr ParameterSlotMask ParameterSlotMaskForIndex(
    std::size_t index) noexcept {
    return index < kParameterSlotOffsets.size()
               ? static_cast<ParameterSlotMask>(
                     ParameterSlotMask{1} << static_cast<unsigned int>(index))
               : ParameterSlotMask{0};
}

inline constexpr ParameterSlotMask kAimParametersSlotMask =
    ParameterSlotMaskForIndex(kAimParametersSlotIndex);
inline constexpr ParameterSlotMask kFleeParametersSlotMask =
    ParameterSlotMaskForIndex(kFleeParametersSlotIndex);
inline constexpr ParameterSlotMask kRaceParametersSlotMask =
    ParameterSlotMaskForIndex(kRaceParametersSlotIndex);
inline constexpr ParameterSlotMask kDriveParametersSlotMask =
    ParameterSlotMaskForIndex(kDriveParametersSlotIndex);
inline constexpr ParameterSlotMask kBurnoutParametersSlotMask =
    ParameterSlotMaskForIndex(kBurnoutParametersSlotIndex);
inline constexpr ParameterSlotMask kPassengerParametersSlotMask =
    ParameterSlotMaskForIndex(kPassengerParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackBackParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackBackParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackSideParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackSideParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackFrontParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackFrontParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackTopParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackTopParametersSlotIndex);
inline constexpr ParameterSlotMask kLookBackParametersSlotMask =
    ParameterSlotMaskForIndex(kLookBackParametersSlotIndex);
inline constexpr ParameterSlotMask kLookSideParametersSlotMask =
    ParameterSlotMaskForIndex(kLookSideParametersSlotIndex);
inline constexpr ParameterSlotMask kLookUpParametersSlotMask =
    ParameterSlotMaskForIndex(kLookUpParametersSlotIndex);
inline constexpr ParameterSlotMask kDriveBlockAllowedSlotMask =
    kRaceParametersSlotMask | kDriveParametersSlotMask |
    kHijackFrontParametersSlotMask;
inline constexpr ParameterSlotMask kMotorcycleDriveBlockAllowedSlotMask =
    kDriveBlockAllowedSlotMask;

struct BaseDriveBranchInputs {
    bool flee_active = false;
    bool drive_object_present = false;
    bool drive_object_state_present = false;
    bool hijack_object_present = false;
    bool race_object_present = false;
    bool race_object_state_present = false;
};

// Base selector order is Flee, Race, then Drive/HijackFront. When the Drive
// predicate D is true, Race additionally requires the +178/+688 pair. This
// exact replay is necessary because several vehicle property sets reuse one
// context-3 block for Race, Drive, and HijackFront.
[[nodiscard]] constexpr bool IsBaseDriveBranchSelected(
    const BaseDriveBranchInputs& inputs) noexcept {
    if (inputs.flee_active) {
        return false;
    }
    const bool drive_predicate =
        inputs.drive_object_present && inputs.drive_object_state_present &&
        !inputs.hijack_object_present;
    if (!drive_predicate) {
        return false;
    }
    const bool race_predicate =
        inputs.race_object_present && inputs.race_object_state_present;
    return !race_predicate;
}

struct ParameterIdentity {
    std::uintptr_t name_pointer = 0;
    std::uint32_t name_symbol = 0;
    std::uint32_t context = 0;

    bool operator==(const ParameterIdentity&) const = default;
};

[[nodiscard]] constexpr ParameterSlotMask ProfileSlotMatchMask(
    std::uintptr_t parameters,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>&
        slots) noexcept {
    if (parameters == 0) {
        return 0;
    }
    ParameterSlotMask mask = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index] == parameters) {
            mask = static_cast<ParameterSlotMask>(
                mask | ParameterSlotMaskForIndex(index));
        }
    }
    return mask;
}

// SetChaseParameters copies three low/medium/high eye and target ranges into
// the component, then resolves current target and (when +0xB59 is nonzero)
// current eye. These are the second qVector3 components identified as lateral.
// Paired CameraSubject vtable and chase-update evidence maps matrix basis v1 to
// GetLeft and consumes it without negation. The user gameplay trace proved the
// Drive-slot and road-car eligibility path before activation.
inline constexpr std::array<std::size_t, 4> kEyeLateralComponentOffsets{
    0x54C,
    0x578,
    0x584,
    0x590,
};
inline constexpr std::array<std::size_t, 4> kTargetLateralComponentOffsets{
    0x558,
    0x5A8,
    0x5B4,
    0x5C0,
};

inline constexpr std::size_t kParameterBlockSize = 0x140;
inline constexpr std::array<std::size_t, 6>
    kDriverSideLateralParameterOffsets{
        0x84,
        0x94,
        0xA4,
        0xB4,
        0xC4,
        0xD4,
    };

// The CameraSubject vtable and chase-update matrix multiply prove that the
// parameter block's positive second component follows vehicle-local Left.
// Wei sits on the right, so his driver-side offset is negative. Magnitude is a
// first-pass value for the user's visual gameplay A/B.
inline constexpr float kDriverSideLateralOffsetMeters = -0.35f;

using ParameterBlock = std::array<std::byte, kParameterBlockSize>;

enum class ParameterOffsetResult : std::uint8_t {
    Applied,
    InvalidDelta,
    InvalidParameter,
};

[[nodiscard]] constexpr std::uint32_t ReadParameterU32(
    const ParameterBlock& block,
    std::size_t offset) noexcept {
    if (offset > block.size() - sizeof(std::uint32_t)) {
        return 0;
    }
    return std::to_integer<std::uint32_t>(block[offset]) |
           (std::to_integer<std::uint32_t>(block[offset + 1]) << 8) |
           (std::to_integer<std::uint32_t>(block[offset + 2]) << 16) |
           (std::to_integer<std::uint32_t>(block[offset + 3]) << 24);
}

[[nodiscard]] constexpr float ReadParameterFloat(
    const ParameterBlock& block,
    std::size_t offset) noexcept {
    return std::bit_cast<float>(ReadParameterU32(block, offset));
}

constexpr void WriteParameterFloat(ParameterBlock& block,
                                   std::size_t offset,
                                   float value) noexcept {
    if (offset > block.size() - sizeof(std::uint32_t)) {
        return;
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    block[offset] = static_cast<std::byte>(bits & 0xFFu);
    block[offset + 1] = static_cast<std::byte>((bits >> 8) & 0xFFu);
    block[offset + 2] = static_cast<std::byte>((bits >> 16) & 0xFFu);
    block[offset + 3] = static_cast<std::byte>((bits >> 24) & 0xFFu);
}

[[nodiscard]] constexpr bool IsFiniteParameterFloat(float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x7F800000u) !=
           0x7F800000u;
}

// First runtime candidate, derived from the captured legacy Chase path and the
// GTA IV/FusionFix behavior contract. These values are deliberately centralized
// so the user's real-game A/B can tune the feel without changing hook logic.
inline constexpr float kGtaIvFollowRateScale = 0.50f;
inline constexpr float kGtaIvHandbrakeFollowRateScale = 0.20f;
inline constexpr float kGtaIvManualYawRecenterDelaySeconds = 0.75f;
inline constexpr float kGtaIvManualYawRecenterRateScale = 0.65f;
inline constexpr float kGtaIvManualLookInputDeadzone = 0.01f;
inline constexpr float kGtaIvManualPitchInputRateRadiansPerSecond = 1.80f;
inline constexpr float kGtaIvMousePitchRadiansPerEyeMeter = 0.45f;
inline constexpr float kGtaIvManualPitchMinimumRadians = -0.30f;
inline constexpr float kGtaIvManualPitchMaximumRadians = 0.35f;
inline constexpr float kGtaIvMinimumEyeOrbitPitchRadians = -0.05f;
inline constexpr float kGtaIvMaximumEyeOrbitPitchRadians = 0.75f;
inline constexpr float kGtaIvManualPitchRecenterDelaySeconds = 0.75f;
inline constexpr float kGtaIvManualPitchRecenterRateRadiansPerSecond = 0.45f;
inline constexpr float kGtaIvMaximumDynamicsDeltaSeconds = 0.05f;

[[nodiscard]] constexpr float CameraAbs(float value) noexcept {
    return value < 0.0f ? -value : value;
}

[[nodiscard]] constexpr float ClampCameraValue(float value,
                                                float minimum,
                                                float maximum) noexcept {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

[[nodiscard]] constexpr float ResolveGtaIvFollowRate(float stock_rate,
                                                     bool handbrake) noexcept {
    if (!IsFiniteParameterFloat(stock_rate) || stock_rate <= 0.0f) {
        return stock_rate;
    }
    return stock_rate * (handbrake ? kGtaIvHandbrakeFollowRateScale
                                  : kGtaIvFollowRateScale);
}

[[nodiscard]] constexpr bool ShouldHoldGtaIvManualYaw(
    float horizontal_input,
    float center_timer_seconds,
    bool reversing) noexcept {
    if (reversing || !IsFiniteParameterFloat(horizontal_input) ||
        !IsFiniteParameterFloat(center_timer_seconds)) {
        return false;
    }
    return CameraAbs(horizontal_input) >= kGtaIvManualLookInputDeadzone ||
           center_timer_seconds < kGtaIvManualYawRecenterDelaySeconds;
}

[[nodiscard]] constexpr float ResolveGtaIvManualYawRecenterRate(
    float stock_rate) noexcept {
    if (!IsFiniteParameterFloat(stock_rate) || stock_rate <= 0.0f) {
        return stock_rate;
    }
    return stock_rate * kGtaIvManualYawRecenterRateScale;
}

struct ManualPitchState {
    float angle_radians = 0.0f;
    float idle_seconds = 0.0f;
    bool initialized = false;

    bool operator==(const ManualPitchState&) const = default;
};

struct ManualPitchFrameInput {
    float delta_seconds = 0.0f;
    float vertical_input = 0.0f;
};

// Stock consumes +0x4FC as an absolute eye-Z offset before it acquires the
// current frame's mouse input. Map that absolute state to an angle instead of
// differentiating it: engine-driven decay then recenters pitch instead of
// masquerading as opposite mouse input.
[[nodiscard]] constexpr float ResolveGtaIvManualPitchOffset(
    float controller_pitch_radians,
    float stock_mouse_eye_offset) noexcept {
    if (!IsFiniteParameterFloat(controller_pitch_radians) ||
        !IsFiniteParameterFloat(stock_mouse_eye_offset)) {
        return 0.0f;
    }
    return ClampCameraValue(
        controller_pitch_radians +
            stock_mouse_eye_offset * kGtaIvMousePitchRadiansPerEyeMeter,
        kGtaIvManualPitchMinimumRadians,
        kGtaIvManualPitchMaximumRadians);
}

[[nodiscard]] constexpr bool ShouldResetGtaIvManualPitchForUpdate(
    unsigned int depth,
    float delta_seconds) noexcept {
    return depth == 1 && IsFiniteParameterFloat(delta_seconds) &&
           delta_seconds <= 0.0f;
}

[[nodiscard]] constexpr ManualPitchState StepGtaIvManualPitch(
    ManualPitchState state,
    const ManualPitchFrameInput& input) noexcept {
    if (!IsFiniteParameterFloat(input.delta_seconds) ||
        input.delta_seconds <= 0.0f ||
        !IsFiniteParameterFloat(input.vertical_input)) {
        return state;
    }

    const float delta_seconds = ClampCameraValue(
        input.delta_seconds, 0.0f, kGtaIvMaximumDynamicsDeltaSeconds);
    if (!state.initialized ||
        !IsFiniteParameterFloat(state.angle_radians) ||
        !IsFiniteParameterFloat(state.idle_seconds)) {
        state = {};
        state.initialized = true;
    }

    const bool has_manual_input =
        CameraAbs(input.vertical_input) >= kGtaIvManualLookInputDeadzone;

    if (has_manual_input) {
        state.angle_radians +=
            -input.vertical_input *
            kGtaIvManualPitchInputRateRadiansPerSecond * delta_seconds;
    }
    state.angle_radians = ClampCameraValue(
        state.angle_radians,
        kGtaIvManualPitchMinimumRadians,
        kGtaIvManualPitchMaximumRadians);

    if (has_manual_input) {
        state.idle_seconds = 0.0f;
    } else {
        state.idle_seconds += delta_seconds;
        if (state.idle_seconds >= kGtaIvManualPitchRecenterDelaySeconds) {
            const float step =
                kGtaIvManualPitchRecenterRateRadiansPerSecond * delta_seconds;
            if (CameraAbs(state.angle_radians) <= step) {
                state.angle_radians = 0.0f;
            } else if (state.angle_radians > 0.0f) {
                state.angle_radians -= step;
            } else {
                state.angle_radians += step;
            }
        }
    }
    return state;
}

[[nodiscard]] constexpr bool IsValidTransitionSourceWeight(
    float value) noexcept {
    return IsFiniteParameterFloat(value) && value >= 0.0f && value <= 1.0f;
}

[[nodiscard]] constexpr ParameterOffsetResult
OffsetDriverSideLateralParameters(ParameterBlock& block,
                                  float lateral_offset_meters) noexcept {
    if (!IsFiniteParameterFloat(lateral_offset_meters)) {
        return ParameterOffsetResult::InvalidDelta;
    }

    std::array<float, kDriverSideLateralParameterOffsets.size()> adjusted{};
    for (std::size_t index = 0; index < adjusted.size(); ++index) {
        const float current = ReadParameterFloat(
            block, kDriverSideLateralParameterOffsets[index]);
        if (!IsFiniteParameterFloat(current)) {
            return ParameterOffsetResult::InvalidParameter;
        }
        const float next = current + lateral_offset_meters;
        if (!IsFiniteParameterFloat(next)) {
            return ParameterOffsetResult::InvalidParameter;
        }
        adjusted[index] = next;
    }

    // Commit only after all six values pass validation so a rejected block is
    // byte-for-byte unchanged.
    for (std::size_t index = 0; index < adjusted.size(); ++index) {
        WriteParameterFloat(block,
                            kDriverSideLateralParameterOffsets[index],
                            adjusted[index]);
    }
    return ParameterOffsetResult::Applied;
}

template <typename Setter>
constexpr void InvokeSelectedParameterBlockOnce(
    Setter&& setter,
    const void* original_parameters,
    const ParameterBlock& adjusted_parameters,
    bool use_adjusted_parameters) noexcept {
    setter(use_adjusted_parameters
               ? static_cast<const void*>(adjusted_parameters.data())
               : original_parameters);
}

[[nodiscard]] constexpr bool ShouldAttemptOuterDynamicProbeCapture(
    unsigned int depth,
    bool transaction_ready,
    bool logging_enabled,
    bool feature_enabled) noexcept {
    return depth == 0 && transaction_ready && logging_enabled &&
           feature_enabled;
}

// The behavior scope is independent of diagnostics: yaw/pitch hooks need an
// active Chase frame even when Logging=0. Nested zero-delta updates inherit an
// already-active outer scope so they can be identified without integrating a
// second frame of mod-owned state.
[[nodiscard]] constexpr bool ShouldEnterDynamicCameraScope(
    unsigned int depth,
    bool transaction_ready,
    bool feature_enabled,
    bool active_subject) noexcept {
    return depth != 0 ||
           (transaction_ready && feature_enabled && active_subject);
}

struct DynamicProbeCallCounts {
    unsigned int setter_calls = 0;
    unsigned int desired_pose_calls = 0;
    unsigned int nested_updates = 0;
    unsigned int nested_setter_calls = 0;
    unsigned int nested_desired_pose_calls = 0;
};

constexpr void MergeNestedDynamicProbeCounts(
    DynamicProbeCallCounts& parent,
    const DynamicProbeCallCounts& child) noexcept {
    ++parent.nested_updates;
    parent.nested_updates += child.nested_updates;
    parent.nested_setter_calls +=
        child.setter_calls + child.nested_setter_calls;
    parent.nested_desired_pose_calls +=
        child.desired_pose_calls + child.nested_desired_pose_calls;
}

template <typename Update>
constexpr void InvokeChaseCameraUpdateOnce(Update&& update,
                                           void* component,
                                           float delta_seconds) {
    update(component, delta_seconds);
}

template <typename Submit, typename Vector>
constexpr void InvokeDesiredEyeLookUpOnce(
    Submit&& submit,
    void* component,
    float delta_seconds,
    const Vector* desired_eye,
    const Vector* desired_look,
    const Vector* desired_up,
    bool collide,
    Vector* new_eye,
    Vector* new_look) {
    submit(component,
           delta_seconds,
           desired_eye,
           desired_look,
           desired_up,
           collide,
           new_eye,
           new_look);
}

enum class VehicleClass : std::uint8_t {
    Unknown,
    CarPhysicsMover,
    Motorcycle,
    Boat,
};

enum class DriverSideTargetProfile : std::uint8_t {
    None,
    RoadDrive,
    RoadFlee,
    MotorcycleDriveBlock,
};

[[nodiscard]] constexpr bool IsValidParameterIdentityForContext(
    const ParameterIdentity& identity,
    std::uint32_t expected_context) noexcept {
    return identity.name_pointer != 0 && identity.name_symbol != 0 &&
           identity.context == expected_context;
}

// Paired disassembly/decompilation proves the selector order Flee, Race, then
// Drive/HijackFront in both executable layouts. Runtime evidence additionally
// proves that Truck Drive reuses one context-3 block in the Race, Drive, and
// HijackFront slots. The replayed base-branch predicate disambiguates normal
// Drive from those aliases; blocks aliased to any other specialized slot remain
// rejected.
[[nodiscard]] constexpr DriverSideTargetProfile
ClassifyDriverSideTargetProfileFromMatchMask(
    VehicleClass vehicle_class,
    std::uintptr_t target_parameters,
    ParameterSlotMask match_mask,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>& slots,
    const ParameterIdentity& target_identity,
    bool base_drive_branch_selected) noexcept {
    if (target_parameters == 0) {
        return DriverSideTargetProfile::None;
    }

    if (vehicle_class == VehicleClass::CarPhysicsMover) {
        if (base_drive_branch_selected &&
            target_parameters == slots[kDriveParametersSlotIndex] &&
            (match_mask & kDriveParametersSlotMask) != 0 &&
            (match_mask & static_cast<ParameterSlotMask>(
                              ~kDriveBlockAllowedSlotMask)) == 0 &&
            IsValidParameterIdentityForContext(target_identity,
                                               kDriveContext)) {
            return DriverSideTargetProfile::RoadDrive;
        }
        if (target_parameters == slots[kFleeParametersSlotIndex] &&
            match_mask == kFleeParametersSlotMask &&
            IsValidParameterIdentityForContext(target_identity,
                                               kFleeContext)) {
            return DriverSideTargetProfile::RoadFlee;
        }
        return DriverSideTargetProfile::None;
    }

    if (vehicle_class == VehicleClass::Motorcycle &&
        base_drive_branch_selected &&
        target_parameters == slots[kDriveParametersSlotIndex] &&
        (match_mask & kDriveParametersSlotMask) != 0 &&
        (match_mask & static_cast<ParameterSlotMask>(
                          ~kMotorcycleDriveBlockAllowedSlotMask)) == 0 &&
        IsValidParameterIdentityForContext(target_identity, kDriveContext)) {
        return DriverSideTargetProfile::MotorcycleDriveBlock;
    }
    return DriverSideTargetProfile::None;
}

[[nodiscard]] constexpr DriverSideTargetProfile
ClassifyDriverSideTargetProfile(
    VehicleClass vehicle_class,
    std::uintptr_t target_parameters,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>& slots,
    const ParameterIdentity& target_identity,
    bool base_drive_branch_selected) noexcept {
    return ClassifyDriverSideTargetProfileFromMatchMask(
        vehicle_class,
        target_parameters,
        ProfileSlotMatchMask(target_parameters, slots),
        slots,
        target_identity,
        base_drive_branch_selected);
}

[[nodiscard]] constexpr const char* DriverSideTargetProfileName(
    DriverSideTargetProfile profile) noexcept {
    switch (profile) {
        case DriverSideTargetProfile::RoadDrive:
            return "road_drive";
        case DriverSideTargetProfile::RoadFlee:
            return "road_flee";
        case DriverSideTargetProfile::MotorcycleDriveBlock:
            return "motorcycle_drive_block";
        default:
            return "none";
    }
}

struct DriverSideEligibility {
    bool car_feature_enabled = false;
    bool bike_feature_enabled = false;
    bool transaction_ready = false;
    VehicleClass vehicle_class = VehicleClass::Unknown;
    DriverSideTargetProfile target_profile = DriverSideTargetProfile::None;
    bool looking_back = false;
    bool aim_or_focus = false;
    std::uintptr_t selected_parameters = 0;
    std::uintptr_t target_parameters = 0;
    float transition_source_weight = 0.0f;
};

[[nodiscard]] constexpr bool IsDriverSideTargetProfileEnabled(
    const DriverSideEligibility& eligibility) noexcept {
    switch (eligibility.target_profile) {
        case DriverSideTargetProfile::RoadDrive:
        case DriverSideTargetProfile::RoadFlee:
            return eligibility.car_feature_enabled &&
                   eligibility.vehicle_class == VehicleClass::CarPhysicsMover;
        case DriverSideTargetProfile::MotorcycleDriveBlock:
            return eligibility.bike_feature_enabled &&
                   eligibility.vehicle_class == VehicleClass::Motorcycle;
        default:
            return false;
    }
}

[[nodiscard]] constexpr float ResolveDriverSideBlendFactor(
    const DriverSideEligibility& eligibility) noexcept {
    if (!eligibility.transaction_ready ||
        !IsDriverSideTargetProfileEnabled(eligibility) ||
        eligibility.looking_back || eligibility.aim_or_focus ||
        eligibility.selected_parameters == 0 ||
        eligibility.target_parameters == 0 ||
        !IsValidTransitionSourceWeight(
            eligibility.transition_source_weight)) {
        return 0.0f;
    }

    if (eligibility.transition_source_weight > 0.0f) {
        // The selector's transition-local block intentionally leaves its
        // 16-byte identity header uninitialized. Canonical B50 is the target;
        // D28 is the source weight. The source snapshot already contains the
        // live, previously offset camera ranges, so add only the eligible
        // target's (1 - source) share and let outgoing transitions fade.
        if (eligibility.selected_parameters == eligibility.target_parameters) {
            return 0.0f;
        }
        return 1.0f - eligibility.transition_source_weight;
    }

    // With no blend, the selector passes the canonical target block directly.
    return eligibility.selected_parameters == eligibility.target_parameters
               ? 1.0f
               : 0.0f;
}

[[nodiscard]] constexpr bool ShouldApplyDriverSideOffset(
    const DriverSideEligibility& eligibility) noexcept {
    return ResolveDriverSideBlendFactor(eligibility) > 0.0f;
}

struct PhysicsMoverVtableProfile {
    std::uintptr_t car_physics_mover_rva = 0;
    std::uintptr_t motorcycle_rva = 0;
    std::uintptr_t boat_rva = 0;

    bool operator==(const PhysicsMoverVtableProfile&) const = default;
};

// VehicleSubjectComponent::OnAttach binds the concrete PhysicsMoverInterface
// at subject +0x1A8. Paired constructor evidence establishes these primary
// vtables for the only three concrete mover classes present in both images.
inline constexpr PhysicsMoverVtableProfile kLegacyPhysicsMoverVtables{
    0x017F7388,
    0x017F60A8,
    0x017F7218,
};
inline constexpr PhysicsMoverVtableProfile kLatestSteamPhysicsMoverVtables{
    0x017F7408,
    0x017F6128,
    0x017F7298,
};

[[nodiscard]] constexpr PhysicsMoverVtableProfile SelectPhysicsMoverVtables(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamPhysicsMoverVtables
                               : kLegacyPhysicsMoverVtables;
}

[[nodiscard]] constexpr VehicleClass ClassifyVehicle(
    std::uintptr_t physics_mover_vtable_rva,
    bool latest_steam_layout) noexcept {
    if (physics_mover_vtable_rva == 0) {
        return VehicleClass::Unknown;
    }
    const PhysicsMoverVtableProfile vtables =
        SelectPhysicsMoverVtables(latest_steam_layout);
    if (physics_mover_vtable_rva == vtables.car_physics_mover_rva) {
        return VehicleClass::CarPhysicsMover;
    }
    if (physics_mover_vtable_rva == vtables.motorcycle_rva) {
        return VehicleClass::Motorcycle;
    }
    if (physics_mover_vtable_rva == vtables.boat_rva) {
        return VehicleClass::Boat;
    }
    return VehicleClass::Unknown;
}

[[nodiscard]] constexpr const char* VehicleClassName(
    VehicleClass vehicle_class) noexcept {
    switch (vehicle_class) {
        case VehicleClass::CarPhysicsMover:
            return "car_physics_mover";
        case VehicleClass::Motorcycle:
            return "motorcycle";
        case VehicleClass::Boat:
            return "boat";
        default:
            return "unknown";
    }
}

[[nodiscard]] constexpr std::size_t ProfileSlotOffsetFor(
    std::uintptr_t parameters,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>&
        slot_values) noexcept {
    if (parameters == 0) {
        return 0;
    }
    for (std::size_t index = 0; index < slot_values.size(); ++index) {
        if (slot_values[index] == parameters) {
            return kParameterSlotOffsets[index];
        }
    }
    return 0;
}

}  // namespace spatch::vehicle_camera
