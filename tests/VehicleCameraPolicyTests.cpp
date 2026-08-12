#include "../src/VehicleCameraPolicy.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace {

using namespace spatch::vehicle_camera;

static_assert(SelectAddresses(false) == AddressProfile{
    0x003C18B0,
    0x003C98C0,
});
static_assert(SelectAddresses(true) == AddressProfile{
    0x003C1760,
    0x003C9760,
});
static_assert(SelectDynamicAddresses(false) == DynamicAddressProfile{
    0x003D0000,
    0x003CB1C0,
    0x00181A50,
    0x003D0919,
    0x003D286D,
});
static_assert(SelectDynamicAddresses(true) == DynamicAddressProfile{
    0x003CFEA0,
    0x003CB060,
    0x00181AE0,
    0x003D07B9,
    0x003D270D,
});

static_assert(kParameterSetterSignature ==
              std::array<std::uint8_t, 31>{
                  0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                  0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55,
                  0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                  0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70,
              });
static_assert(kChaseUpdateSignature ==
              std::array<std::uint8_t, 18>{
                  0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xFD,
                  0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x58, 0x03, 0x00, 0x00,
              });
static_assert(kDesiredEyeLookUpSignature ==
              std::array<std::uint8_t, 23>{
                  0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x48,
                  0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41,
                  0x54, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xB8,
               });
static_assert(SelectAngularApproachSignature(false) ==
              kLegacyAngularApproachSignature);
static_assert(SelectAngularApproachSignature(true) ==
              kLatestSteamAngularApproachSignature);

static_assert(kSubjectOffset == 0xB30);
static_assert(kAlternateLookFlagOffset == 0x380);
static_assert(kPhysicsMoverOffset == 0x1A8);
static_assert(kSelectorFleeStateOffset == 0x344);
static_assert(kSelectorDriveObjectOffset == 0x1D8);
static_assert(kSelectorDriveObjectStateOffset == 0x68);
static_assert(kSelectorHijackObjectOffset == 0x148);
static_assert(kSelectorRaceObjectOffset == 0x178);
static_assert(kSelectorRaceObjectStateOffset == 0x688);
static_assert(kTargetParametersOffset == 0xB50);
static_assert(kLookingBackFlagOffset == 0x6D0);
static_assert(kAimOrFocusFlagOffset == 0xB5C);
static_assert(kPhysicsMoverSteeringInputOffset == 0x58);
static_assert(kPhysicsMoverInputFlagsOffset == 0x68);
static_assert(kPhysicsMoverForwardSpeedOffset == 0x2B4);
static_assert(kPhysicsMoverInReverseFlag == 0x1u);
static_assert(kPhysicsMoverHandbrakeFlag == 0x4u);
static_assert(kDriveParametersSlotOffset == 0x388);
static_assert(kParameterSlotOffsets ==
              std::array<std::size_t, 13>{
                  0x370, 0x378, 0x380, 0x388, 0x390, 0x398, 0x3A0,
                  0x3A8, 0x3B0, 0x3B8, 0x3C0, 0x3C8, 0x3D0,
              });
static_assert(kParameterBlockSize == 0x140);
static_assert(kTransitionSourceParametersOffset == 0xBE8);
static_assert(kTransitionSourceWeightOffset == 0xD28);
static_assert(IsValidTransitionSourceWeight(0.0f));
static_assert(IsValidTransitionSourceWeight(1.0f));
static_assert(!IsValidTransitionSourceWeight(-0.1f));
static_assert(!IsValidTransitionSourceWeight(1.1f));
static_assert(!IsValidTransitionSourceWeight(
    std::bit_cast<float>(std::uint32_t{0x7FC00000u})));
static_assert(!IsValidTransitionSourceWeight(
    std::bit_cast<float>(std::uint32_t{0xDEDEDEDEu})));
static_assert(ResolveGtaIvFollowRate(2.0f, false) == 1.0f);
static_assert(ResolveGtaIvFollowRate(2.0f, true) == 0.4f);
static_assert(ShouldHoldGtaIvManualYaw(0.5f, 5.0f, false));
static_assert(ShouldHoldGtaIvManualYaw(0.0f, 0.5f, false));
static_assert(!ShouldHoldGtaIvManualYaw(0.0f, 1.0f, false));
static_assert(!ShouldHoldGtaIvManualYaw(0.5f, 0.5f, true));
static_assert(ResolveGtaIvManualYawRecenterRate(2.0f) == 1.3f);
static_assert(!IsPhysicsMoverReversing(0));
static_assert(IsPhysicsMoverReversing(kPhysicsMoverInReverseFlag));
static_assert(ShouldResetGtaIvManualPitchForUpdate(1, 0.0f));
static_assert(!ShouldResetGtaIvManualPitchForUpdate(2, 0.0f));
static_assert(!ShouldResetGtaIvManualPitchForUpdate(1, 0.01f));
constexpr bool ManualPitchIntegratesAndClampsDeltaTime() {
    const ManualPitchState state = StepGtaIvManualPitch(
        {}, ManualPitchFrameInput{0.1f, 1.0f});
    return state.initialized && state.angle_radians < -0.089f &&
           state.angle_radians > -0.091f && state.idle_seconds == 0.0f;
}
static_assert(ManualPitchIntegratesAndClampsDeltaTime());
constexpr bool ManualPitchIgnoresSubDeadzoneNoise() {
    const ManualPitchState state = StepGtaIvManualPitch(
        ManualPitchState{0.1f, 0.0f, true},
        ManualPitchFrameInput{0.01f, 0.005f});
    return state.angle_radians == 0.1f && state.idle_seconds == 0.01f;
}
static_assert(ManualPitchIgnoresSubDeadzoneNoise());
static_assert(ResolveGtaIvManualPitchOffset(0.1f, 0.2f) > 0.189f);
static_assert(ResolveGtaIvManualPitchOffset(0.1f, 0.2f) < 0.191f);
static_assert(ResolveGtaIvManualPitchOffset(0.0f, 10.0f) ==
              kGtaIvManualPitchMaximumRadians);
static_assert(ResolveGtaIvManualPitchOffset(0.0f, -10.0f) ==
              kGtaIvManualPitchMinimumRadians);
constexpr bool ManualPitchRecentersOnlyAfterDelay() {
    ManualPitchState state{0.2f, 0.73f, true};
    const ManualPitchState before_delay = StepGtaIvManualPitch(
        state, ManualPitchFrameInput{0.01f, 0.0f});
    const ManualPitchState after_delay = StepGtaIvManualPitch(
        before_delay, ManualPitchFrameInput{0.02f, 0.0f});
    return before_delay.angle_radians == 0.2f &&
           after_delay.angle_radians < before_delay.angle_radians &&
           after_delay.angle_radians > 0.19f;
}
static_assert(ManualPitchRecentersOnlyAfterDelay());
static_assert(kDriverSideLateralParameterOffsets ==
              std::array<std::size_t, 6>{
                  0x84, 0x94, 0xA4, 0xB4, 0xC4, 0xD4,
              });
static_assert(kDriverSideLateralOffsetMeters < 0.0f);

static_assert(kEyeLateralComponentOffsets ==
              std::array<std::size_t, 4>{0x54C, 0x578, 0x584, 0x590});
static_assert(kTargetLateralComponentOffsets ==
              std::array<std::size_t, 4>{0x558, 0x5A8, 0x5B4, 0x5C0});

static_assert(ProfileSlotOffsetFor(
                  0x2222,
                  std::array<std::uintptr_t, 13>{
                      0x1111, 0, 0, 0x2222, 0, 0, 0,
                      0, 0, 0, 0, 0, 0}) ==
              kDriveParametersSlotOffset);
static_assert(ProfileSlotOffsetFor(
                  0x1111,
                  std::array<std::uintptr_t, 13>{
                      0x1111, 0, 0, 0x2222, 0, 0, 0,
                      0, 0, 0, 0, 0, 0}) == 0x370);
static_assert(ProfileSlotOffsetFor(
                  0x9999,
                  std::array<std::uintptr_t, 13>{
                      0x1111, 0, 0, 0x2222, 0, 0, 0,
                      0, 0, 0, 0, 0, 0}) == 0);
static_assert(ProfileSlotOffsetFor(
                  0,
                  std::array<std::uintptr_t, 13>{
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 0);

static_assert(SelectPhysicsMoverVtables(false) == PhysicsMoverVtableProfile{
    0x017F7388,
    0x017F60A8,
    0x017F7218,
});
static_assert(SelectPhysicsMoverVtables(true) == PhysicsMoverVtableProfile{
    0x017F7408,
    0x017F6128,
    0x017F7298,
});
static_assert(ClassifyVehicle(0x017F7388, false) ==
              VehicleClass::CarPhysicsMover);
static_assert(ClassifyVehicle(0x017F60A8, false) ==
              VehicleClass::Motorcycle);
static_assert(ClassifyVehicle(0x017F7218, false) == VehicleClass::Boat);
static_assert(ClassifyVehicle(0x017F7408, true) ==
              VehicleClass::CarPhysicsMover);
static_assert(ClassifyVehicle(0x017F6128, true) ==
              VehicleClass::Motorcycle);
static_assert(ClassifyVehicle(0x017F7298, true) == VehicleClass::Boat);
static_assert(ClassifyVehicle(0, false) == VehicleClass::Unknown);
static_assert(ClassifyVehicle(0x017F7408, false) == VehicleClass::Unknown);

constexpr BaseDriveBranchInputs kBaseDriveBranch{
    false,
    true,
    true,
    false,
    false,
    false,
};
static_assert(IsBaseDriveBranchSelected(kBaseDriveBranch));
constexpr BaseDriveBranchInputs WithFleeActive(bool active) {
    BaseDriveBranchInputs value = kBaseDriveBranch;
    value.flee_active = active;
    return value;
}
constexpr BaseDriveBranchInputs WithDriveObject(bool present) {
    BaseDriveBranchInputs value = kBaseDriveBranch;
    value.drive_object_present = present;
    return value;
}
constexpr BaseDriveBranchInputs WithDriveObjectState(bool present) {
    BaseDriveBranchInputs value = kBaseDriveBranch;
    value.drive_object_state_present = present;
    return value;
}
constexpr BaseDriveBranchInputs WithHijackObject(bool present) {
    BaseDriveBranchInputs value = kBaseDriveBranch;
    value.hijack_object_present = present;
    return value;
}
constexpr BaseDriveBranchInputs WithRaceState(bool object_present,
                                               bool state_present) {
    BaseDriveBranchInputs value = kBaseDriveBranch;
    value.race_object_present = object_present;
    value.race_object_state_present = state_present;
    return value;
}
static_assert(!IsBaseDriveBranchSelected(WithFleeActive(true)));
static_assert(!IsBaseDriveBranchSelected(WithDriveObject(false)));
static_assert(!IsBaseDriveBranchSelected(WithDriveObjectState(false)));
static_assert(!IsBaseDriveBranchSelected(WithHijackObject(true)));
static_assert(!IsBaseDriveBranchSelected(WithRaceState(true, true)));
static_assert(IsBaseDriveBranchSelected(WithRaceState(true, false)));
static_assert(IsBaseDriveBranchSelected(WithRaceState(false, false)));

constexpr std::array<std::uintptr_t, kParameterSlotOffsets.size()>
MakeUniqueCameraSlots() {
    std::array<std::uintptr_t, kParameterSlotOffsets.size()> slots{};
    for (std::size_t index = 0; index < slots.size(); ++index) {
        slots[index] = 0x10000 + index * 0x100;
    }
    return slots;
}

constexpr auto kUniqueCameraSlots = MakeUniqueCameraSlots();
constexpr ParameterIdentity kDriveIdentity{0x20000, 0x30000, kDriveContext};
constexpr ParameterIdentity kFleeIdentity{0x21000, 0x31000, kFleeContext};

static_assert(ProfileSlotMatchMask(
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots) == kDriveParametersSlotMask);
static_assert(ProfileSlotMatchMask(
                  kUniqueCameraSlots[kFleeParametersSlotIndex],
                  kUniqueCameraSlots) == kFleeParametersSlotMask);
constexpr bool BikeDriveHijackFrontAliasMaskIsComplete() {
    auto slots = kUniqueCameraSlots;
    slots[kHijackFrontParametersSlotIndex] =
        slots[kDriveParametersSlotIndex];
    return ProfileSlotMatchMask(slots[kDriveParametersSlotIndex], slots) ==
           (kDriveParametersSlotMask | kHijackFrontParametersSlotMask);
}
static_assert(BikeDriveHijackFrontAliasMaskIsComplete());
constexpr bool ScooterRaceDriveAliasMaskIsComplete() {
    auto slots = kUniqueCameraSlots;
    slots[kRaceParametersSlotIndex] = slots[kDriveParametersSlotIndex];
    return ProfileSlotMatchMask(slots[kDriveParametersSlotIndex], slots) ==
           (kRaceParametersSlotMask | kDriveParametersSlotMask);
}
static_assert(ScooterRaceDriveAliasMaskIsComplete());
static_assert(ProfileSlotMatchMask(0, kUniqueCameraSlots) == 0);
static_assert(ProfileSlotMatchMask(0xDEADBEEF, kUniqueCameraSlots) == 0);

static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) == DriverSideTargetProfile::RoadDrive);
static_assert(ClassifyDriverSideTargetProfileFromMatchMask(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kDriveParametersSlotMask,
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) == DriverSideTargetProfile::RoadDrive);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  false) == DriverSideTargetProfile::None);
static_assert(ClassifyDriverSideTargetProfileFromMatchMask(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kFleeParametersSlotIndex],
                  kDriveParametersSlotMask,
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) == DriverSideTargetProfile::None);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kFleeParametersSlotIndex],
                  kUniqueCameraSlots,
                  kFleeIdentity,
                  false) == DriverSideTargetProfile::RoadFlee);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  ParameterIdentity{0x20000, 0x30000, kFleeContext},
                  true) ==
              DriverSideTargetProfile::None);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::CarPhysicsMover,
                  kUniqueCameraSlots[kFleeParametersSlotIndex],
                  kUniqueCameraSlots,
                  ParameterIdentity{0, 0x31000, kFleeContext},
                  false) ==
              DriverSideTargetProfile::None);
constexpr bool AliasedRoadDriveIsAcceptedOnlyForDriveBranch() {
    auto slots = kUniqueCameraSlots;
    slots[kRaceParametersSlotIndex] = slots[kDriveParametersSlotIndex];
    slots[kHijackFrontParametersSlotIndex] =
        slots[kDriveParametersSlotIndex];
    return
        ClassifyDriverSideTargetProfile(
            VehicleClass::CarPhysicsMover,
            slots[kDriveParametersSlotIndex],
            slots,
            kDriveIdentity,
            true) == DriverSideTargetProfile::RoadDrive &&
        ClassifyDriverSideTargetProfile(
            VehicleClass::CarPhysicsMover,
            slots[kDriveParametersSlotIndex],
            slots,
            kDriveIdentity,
            false) == DriverSideTargetProfile::None;
}
static_assert(AliasedRoadDriveIsAcceptedOnlyForDriveBranch());
constexpr bool AliasedRoadFleeIsRejected() {
    auto slots = kUniqueCameraSlots;
    slots[kAimParametersSlotIndex] = slots[kFleeParametersSlotIndex];
    return ClassifyDriverSideTargetProfile(
               VehicleClass::CarPhysicsMover,
               slots[kFleeParametersSlotIndex],
               slots,
               kFleeIdentity,
               false) == DriverSideTargetProfile::None;
}
static_assert(AliasedRoadFleeIsRejected());
constexpr bool RoadDriveWithUnknownAliasIsRejected() {
    auto slots = kUniqueCameraSlots;
    slots[kLookBackParametersSlotIndex] = slots[kDriveParametersSlotIndex];
    return ClassifyDriverSideTargetProfile(
               VehicleClass::CarPhysicsMover,
               slots[kDriveParametersSlotIndex],
               slots,
               kDriveIdentity,
               true) == DriverSideTargetProfile::None;
}
static_assert(RoadDriveWithUnknownAliasIsRejected());

static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::Motorcycle,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) ==
              DriverSideTargetProfile::MotorcycleDriveBlock);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::Motorcycle,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  false) == DriverSideTargetProfile::None);
constexpr bool BikeDriveHijackFrontAliasIsAccepted() {
    auto slots = kUniqueCameraSlots;
    slots[kHijackFrontParametersSlotIndex] =
        slots[kDriveParametersSlotIndex];
    return ClassifyDriverSideTargetProfile(
               VehicleClass::Motorcycle,
               slots[kDriveParametersSlotIndex],
               slots,
               kDriveIdentity,
               true) ==
           DriverSideTargetProfile::MotorcycleDriveBlock;
}
static_assert(BikeDriveHijackFrontAliasIsAccepted());
constexpr bool ScooterRaceDriveAliasIsAccepted() {
    auto slots = kUniqueCameraSlots;
    slots[kRaceParametersSlotIndex] = slots[kDriveParametersSlotIndex];
    return ClassifyDriverSideTargetProfile(
               VehicleClass::Motorcycle,
               slots[kDriveParametersSlotIndex],
               slots,
               kDriveIdentity,
               true) ==
           DriverSideTargetProfile::MotorcycleDriveBlock;
}
static_assert(ScooterRaceDriveAliasIsAccepted());
constexpr bool BikeDriveWithAllAllowedAliasesIsAccepted() {
    auto slots = kUniqueCameraSlots;
    slots[kRaceParametersSlotIndex] = slots[kDriveParametersSlotIndex];
    slots[kHijackFrontParametersSlotIndex] =
        slots[kDriveParametersSlotIndex];
    return ClassifyDriverSideTargetProfile(
               VehicleClass::Motorcycle,
               slots[kDriveParametersSlotIndex],
               slots,
               kDriveIdentity,
               true) ==
           DriverSideTargetProfile::MotorcycleDriveBlock;
}
static_assert(BikeDriveWithAllAllowedAliasesIsAccepted());
constexpr bool BikeDriveWithUnknownAliasIsRejected() {
    auto slots = kUniqueCameraSlots;
    slots[kLookBackParametersSlotIndex] = slots[kDriveParametersSlotIndex];
    return ClassifyDriverSideTargetProfile(
               VehicleClass::Motorcycle,
               slots[kDriveParametersSlotIndex],
               slots,
               kDriveIdentity,
               true) == DriverSideTargetProfile::None;
}
static_assert(BikeDriveWithUnknownAliasIsRejected());
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::Motorcycle,
                  kUniqueCameraSlots[kRaceParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) == DriverSideTargetProfile::None);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::Motorcycle,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  ParameterIdentity{0x20000, 0x30000, kFleeContext},
                  true) ==
              DriverSideTargetProfile::None);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::Boat,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) == DriverSideTargetProfile::None);
static_assert(ClassifyDriverSideTargetProfile(
                  VehicleClass::Unknown,
                  kUniqueCameraSlots[kDriveParametersSlotIndex],
                  kUniqueCameraSlots,
                  kDriveIdentity,
                  true) == DriverSideTargetProfile::None);

constexpr DriverSideEligibility kEligibleRoadCar{
    true,
    false,
    true,
    VehicleClass::CarPhysicsMover,
    DriverSideTargetProfile::RoadDrive,
    false,
    false,
    0x2000,
    0x2000,
    0.0f,
};
static_assert(ShouldApplyDriverSideOffset(kEligibleRoadCar));
static_assert(ResolveDriverSideBlendFactor(kEligibleRoadCar) == 1.0f);
constexpr DriverSideEligibility WithTransactionReady(bool ready) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.transaction_ready = ready;
    return value;
}
constexpr DriverSideEligibility WithVehicleClass(VehicleClass vehicle_class) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.vehicle_class = vehicle_class;
    return value;
}
constexpr DriverSideEligibility WithTargetProfile(
    DriverSideTargetProfile target_profile) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.target_profile = target_profile;
    return value;
}
constexpr DriverSideEligibility WithLookingBack(bool looking_back) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.looking_back = looking_back;
    return value;
}
constexpr DriverSideEligibility WithAimOrFocus(bool aim_or_focus) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.aim_or_focus = aim_or_focus;
    return value;
}
constexpr DriverSideEligibility WithSelectedParameters(
    std::uintptr_t parameters) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.selected_parameters = parameters;
    return value;
}
constexpr DriverSideEligibility WithTargetParameters(
    std::uintptr_t parameters) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.target_parameters = parameters;
    return value;
}
constexpr DriverSideEligibility WithTransition(float source_weight) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.selected_parameters = 0x3000;
    value.transition_source_weight = source_weight;
    return value;
}
constexpr DriverSideEligibility MakeEligibleBike(bool car_enabled,
                                                 bool bike_enabled) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.car_feature_enabled = car_enabled;
    value.bike_feature_enabled = bike_enabled;
    value.vehicle_class = VehicleClass::Motorcycle;
    value.target_profile = DriverSideTargetProfile::MotorcycleDriveBlock;
    return value;
}
constexpr DriverSideEligibility MakeEligibleRoad(bool car_enabled,
                                                 bool bike_enabled) {
    DriverSideEligibility value = kEligibleRoadCar;
    value.car_feature_enabled = car_enabled;
    value.bike_feature_enabled = bike_enabled;
    return value;
}
static_assert(ShouldApplyDriverSideOffset(MakeEligibleRoad(true, false)));
static_assert(ShouldApplyDriverSideOffset(MakeEligibleRoad(true, true)));
static_assert(!ShouldApplyDriverSideOffset(MakeEligibleRoad(false, false)));
static_assert(!ShouldApplyDriverSideOffset(MakeEligibleRoad(false, true)));
constexpr bool BikeOnlyDoesNotEnableRoadFlee() {
    DriverSideEligibility value = MakeEligibleRoad(false, true);
    value.target_profile = DriverSideTargetProfile::RoadFlee;
    return !ShouldApplyDriverSideOffset(value);
}
static_assert(BikeOnlyDoesNotEnableRoadFlee());
static_assert(!ShouldApplyDriverSideOffset(WithTransactionReady(false)));
static_assert(!ShouldApplyDriverSideOffset(WithTargetProfile(
    DriverSideTargetProfile::None)));
static_assert(!ShouldApplyDriverSideOffset(
    WithVehicleClass(VehicleClass::Boat)));
static_assert(!ShouldApplyDriverSideOffset(
    WithVehicleClass(VehicleClass::Unknown)));
static_assert(!ShouldApplyDriverSideOffset(WithLookingBack(true)));
static_assert(!ShouldApplyDriverSideOffset(WithAimOrFocus(true)));
static_assert(!ShouldApplyDriverSideOffset(WithSelectedParameters(0)));
static_assert(!ShouldApplyDriverSideOffset(WithTargetParameters(0)));

static_assert(ShouldApplyDriverSideOffset(MakeEligibleBike(false, true)));
static_assert(ShouldApplyDriverSideOffset(MakeEligibleBike(true, true)));
static_assert(!ShouldApplyDriverSideOffset(MakeEligibleBike(false, false)));
static_assert(!ShouldApplyDriverSideOffset(MakeEligibleBike(true, false)));
static_assert(!ShouldApplyDriverSideOffset(
    WithVehicleClass(VehicleClass::Motorcycle)));
static_assert(!ShouldApplyDriverSideOffset(WithTargetProfile(
    DriverSideTargetProfile::MotorcycleDriveBlock)));
constexpr bool MotorcycleRoadProfileIsRejected() {
    DriverSideEligibility value = MakeEligibleBike(false, true);
    value.target_profile = DriverSideTargetProfile::RoadDrive;
    return !ShouldApplyDriverSideOffset(value);
}
static_assert(MotorcycleRoadProfileIsRejected());

static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(1.0f)) == 0.0f);
static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(0.75f)) == 0.25f);
static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(0.25f)) == 0.75f);
constexpr float kObservedPursuitSourceWeight = 0.947232f;
static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(kObservedPursuitSourceWeight)) ==
              1.0f - kObservedPursuitSourceWeight);
static_assert(kObservedPursuitSourceWeight +
                  ResolveDriverSideBlendFactor(
                      WithTransition(kObservedPursuitSourceWeight)) ==
              1.0f);
constexpr bool ObservedDriveFleeLateralDeltaIsContinuous() {
    const float target_factor = ResolveDriverSideBlendFactor(
        WithTransition(kObservedPursuitSourceWeight));
    const float combined_delta =
        kDriverSideLateralOffsetMeters * kObservedPursuitSourceWeight +
        kDriverSideLateralOffsetMeters * target_factor;
    float difference = combined_delta - kDriverSideLateralOffsetMeters;
    if (difference < 0.0f) {
        difference = -difference;
    }
    return difference <= 0.000001f;
}
static_assert(ObservedDriveFleeLateralDeltaIsContinuous());
constexpr bool MotorcycleTransitionUsesTargetShare() {
    DriverSideEligibility value = MakeEligibleBike(false, true);
    value.selected_parameters = 0x3000;
    value.transition_source_weight = 0.75f;
    return ResolveDriverSideBlendFactor(value) == 0.25f;
}
static_assert(MotorcycleTransitionUsesTargetShare());
constexpr bool PositiveWeightWithCanonicalPointerIsRejected() {
    DriverSideEligibility value = kEligibleRoadCar;
    value.transition_source_weight = 0.25f;
    return ResolveDriverSideBlendFactor(value) == 0.0f;
}
static_assert(PositiveWeightWithCanonicalPointerIsRejected());
static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(-0.1f)) == 0.0f);
static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(1.1f)) == 0.0f);
static_assert(ResolveDriverSideBlendFactor(
                  WithTransition(
                      std::bit_cast<float>(std::uint32_t{0x7FC00000u}))) ==
              0.0f);
constexpr bool ZeroWeightTransitionLocalIsRejected() {
    DriverSideEligibility value = WithTransition(0.0f);
    return ResolveDriverSideBlendFactor(value) == 0.0f;
}
static_assert(ZeroWeightTransitionLocalIsRejected());
constexpr bool RoadFleeCanonicalIsEligible() {
    DriverSideEligibility value = kEligibleRoadCar;
    value.target_profile = DriverSideTargetProfile::RoadFlee;
    return ResolveDriverSideBlendFactor(value) == 1.0f;
}
static_assert(RoadFleeCanonicalIsEligible());
constexpr bool RoadFleeTransitionIsEligible() {
    DriverSideEligibility value = WithTransition(0.75f);
    value.target_profile = DriverSideTargetProfile::RoadFlee;
    return ResolveDriverSideBlendFactor(value) == 0.25f;
}
static_assert(RoadFleeTransitionIsEligible());

constexpr ParameterBlock MakeCameraParameterBlock() {
    ParameterBlock block{};
    for (std::size_t index = 0; index < block.size(); ++index) {
        block[index] = static_cast<std::byte>((index * 37u + 11u) & 0xFFu);
    }
    for (std::size_t index = 0;
         index < kDriverSideLateralParameterOffsets.size(); ++index) {
        WriteParameterFloat(
            block,
            kDriverSideLateralParameterOffsets[index],
            static_cast<float>(index) * 0.5f - 1.0f);
    }
    return block;
}

constexpr bool IsOffsetFloatByte(std::size_t byte_index) {
    for (const std::size_t offset :
         kDriverSideLateralParameterOffsets) {
        if (byte_index >= offset && byte_index < offset + sizeof(float)) {
            return true;
        }
    }
    return false;
}

constexpr bool DriverSideOffsetChangesOnlySixFloats() {
    ParameterBlock block = MakeCameraParameterBlock();
    const ParameterBlock original = block;
    constexpr float delta = 0.375f;
    if (OffsetDriverSideLateralParameters(block, delta) !=
        ParameterOffsetResult::Applied) {
        return false;
    }
    for (std::size_t index = 0;
         index < kDriverSideLateralParameterOffsets.size(); ++index) {
        const std::size_t offset =
            kDriverSideLateralParameterOffsets[index];
        const float expected = static_cast<float>(index) * 0.5f - 1.0f +
                               delta;
        if (ReadParameterFloat(block, offset) != expected) {
            return false;
        }
    }
    for (std::size_t index = 0; index < block.size(); ++index) {
        if (!IsOffsetFloatByte(index) && block[index] != original[index]) {
            return false;
        }
    }
    return true;
}
static_assert(DriverSideOffsetChangesOnlySixFloats());

constexpr bool InvalidCameraParameterIsAtomic() {
    ParameterBlock block = MakeCameraParameterBlock();
    WriteParameterFloat(
        block,
        kDriverSideLateralParameterOffsets[4],
        std::bit_cast<float>(std::uint32_t{0x7FC00000u}));
    const ParameterBlock original = block;
    return OffsetDriverSideLateralParameters(block, 0.25f) ==
               ParameterOffsetResult::InvalidParameter &&
           block == original;
}
static_assert(InvalidCameraParameterIsAtomic());

constexpr bool SignalingNanCameraParameterIsAtomic() {
    ParameterBlock block = MakeCameraParameterBlock();
    WriteParameterFloat(
        block,
        kDriverSideLateralParameterOffsets[1],
        std::bit_cast<float>(std::uint32_t{0x7F800001u}));
    const ParameterBlock original = block;
    return OffsetDriverSideLateralParameters(block, -0.25f) ==
               ParameterOffsetResult::InvalidParameter &&
           block == original;
}
static_assert(SignalingNanCameraParameterIsAtomic());

constexpr bool InvalidCameraDeltaIsAtomic() {
    ParameterBlock block = MakeCameraParameterBlock();
    const ParameterBlock original = block;
    return OffsetDriverSideLateralParameters(
               block,
               std::bit_cast<float>(std::uint32_t{0x7F800000u})) ==
               ParameterOffsetResult::InvalidDelta &&
           block == original;
}
static_assert(InvalidCameraDeltaIsAtomic());

constexpr bool ParameterSetterAdapterCallsOnce(bool apply_offset) {
    ParameterBlock block = MakeCameraParameterBlock();
    int original_parameters = 0;
    int calls = 0;
    const void* received = nullptr;
    InvokeSelectedParameterBlockOnce(
        [&](const void* parameters) {
            ++calls;
            received = parameters;
        },
        &original_parameters,
        block,
        apply_offset);
    const void* expected = apply_offset
                               ? static_cast<const void*>(block.data())
                               : static_cast<const void*>(&original_parameters);
    return calls == 1 && received == expected;
}
static_assert(ParameterSetterAdapterCallsOnce(false));
static_assert(ParameterSetterAdapterCallsOnce(true));

static_assert(ShouldAttemptOuterDynamicProbeCapture(0, true, true, true));
static_assert(!ShouldAttemptOuterDynamicProbeCapture(1, true, true, true));
static_assert(!ShouldAttemptOuterDynamicProbeCapture(0, false, true, true));
static_assert(!ShouldAttemptOuterDynamicProbeCapture(0, true, false, true));
static_assert(!ShouldAttemptOuterDynamicProbeCapture(0, true, true, false));
static_assert(ShouldEnterDynamicCameraScope(0, true, true, true));
static_assert(!ShouldEnterDynamicCameraScope(0, false, true, true));
static_assert(!ShouldEnterDynamicCameraScope(0, true, false, true));
static_assert(!ShouldEnterDynamicCameraScope(0, true, true, false));
static_assert(ShouldEnterDynamicCameraScope(1, false, false, false));

constexpr bool NestedProbeCountsAggregateExactlyOnce() {
    DynamicProbeCallCounts parent{1, 1, 0, 0, 0};
    const DynamicProbeCallCounts child{2, 3, 1, 4, 5};
    MergeNestedDynamicProbeCounts(parent, child);
    return parent.setter_calls == 1 && parent.desired_pose_calls == 1 &&
           parent.nested_updates == 2 &&
           parent.nested_setter_calls == 6 &&
           parent.nested_desired_pose_calls == 8;
}
static_assert(NestedProbeCountsAggregateExactlyOnce());

constexpr bool ChaseUpdateAdapterCallsOnce() {
    int component_storage = 0;
    void* expected_component = &component_storage;
    int calls = 0;
    void* received_component = nullptr;
    float received_delta = 0.0f;
    InvokeChaseCameraUpdateOnce(
        [&](void* component, float delta_seconds) {
            ++calls;
            received_component = component;
            received_delta = delta_seconds;
        },
        expected_component,
        1.0f / 60.0f);
    return calls == 1 && received_component == expected_component &&
           received_delta == 1.0f / 60.0f;
}
static_assert(ChaseUpdateAdapterCallsOnce());

constexpr bool DesiredPoseAdapterCallsOnce() {
    int component_storage = 0;
    int eye = 1;
    int look = 2;
    int up = 3;
    int new_eye = 4;
    int new_look = 5;
    int calls = 0;
    bool arguments_match = false;
    InvokeDesiredEyeLookUpOnce(
        [&](void* component,
            float delta_seconds,
            const int* received_eye,
            const int* received_look,
            const int* received_up,
            bool collide,
            int* received_new_eye,
            int* received_new_look) {
            ++calls;
            arguments_match =
                component == &component_storage &&
                delta_seconds == 0.25f && received_eye == &eye &&
                received_look == &look && received_up == &up && collide &&
                received_new_eye == &new_eye &&
                received_new_look == &new_look;
        },
        &component_storage,
        0.25f,
        &eye,
        &look,
        &up,
        true,
        &new_eye,
        &new_look);
    return calls == 1 && arguments_match;
}
static_assert(DesiredPoseAdapterCallsOnce());

}  // namespace
