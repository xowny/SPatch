#include "Hooks.h"

#include "ArchiveEntryPolicy.h"
#include "AverageWindowPolicy.h"
#include "CharacterEyeFix.h"
#include "CharacterHookPolicy.h"
#include "CharacterSweatPolicy.h"
#include "CharacterWetnessPolicy.h"
#include "ChunkStreamPolicy.h"
#include "CutContentProbe.h"
#include "CutsceneTiming.h"
#include "DisplaySettings.h"
#include "EngineFixes.h"
#include "FogRestorationPolicy.h"
#include "FogSlicingPolicy.h"
#include "HooksSummary.h"
#include "HookTargetGuard.h"
#include "Logger.h"
#include "NisActorProbe.h"
#include "PedestrianTiming.h"
#include "QFileIoPolicy.h"
#include "QcmpPolicy.h"
#include "RuntimePatch.h"
#include "SmaaRuntime.h"
#include "TextureFilteringPolicy.h"
#include "VehicleCameraPolicy.h"

#include <Windows.h>

#include <MinHook.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace spatch::hooks {
namespace {

// Final end-user builds intentionally disable summaries and verbose probes.
// Keeping hundreds of diagnostic snapshots as real atomics in that build
// still imposed locked read/modify/write instructions on render and NIS hot
// paths. Development builds retain the counters with relaxed ordering; final
// builds compile their operations away entirely.
template <typename T>
class DiagnosticAtomic {
public:
#if defined(SPATCH_FINAL_RELEASE)
    constexpr DiagnosticAtomic(T = {}) noexcept {}

    T fetch_add(T, std::memory_order = std::memory_order_relaxed) noexcept {
        return {};
    }
    T fetch_sub(T, std::memory_order = std::memory_order_relaxed) noexcept {
        return {};
    }
    [[nodiscard]] T load(std::memory_order = std::memory_order_relaxed) const noexcept {
        return {};
    }
    void store(T, std::memory_order = std::memory_order_relaxed) noexcept {}
    bool compare_exchange_strong(T& expected,
                                 T,
                                 std::memory_order = std::memory_order_relaxed) noexcept {
        expected = {};
        // Final builds intentionally have no diagnostic storage.  Report a
        // failed exchange rather than pretending a write happened; callers
        // use this primitive only for diagnostic one-shot gates, and the
        // false result preserves the safe no-op semantics of the other
        // operations in this specialization.
        return false;
    }
#else
    constexpr DiagnosticAtomic(T initial = {}) noexcept : value_(initial) {}

    T fetch_add(T operand, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value_.fetch_add(operand, order);
    }
    T fetch_sub(T operand, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value_.fetch_sub(operand, order);
    }
    [[nodiscard]] T load(std::memory_order order = std::memory_order_relaxed) const noexcept {
        return value_.load(order);
    }
    void store(T desired, std::memory_order order = std::memory_order_relaxed) noexcept {
        value_.store(desired, order);
    }
    bool compare_exchange_strong(T& expected,
                                 T desired,
                                 std::memory_order order = std::memory_order_relaxed) noexcept {
        return value_.compare_exchange_strong(expected, desired, order);
    }

private:
    std::atomic<T> value_;
#endif
};

constexpr std::uintptr_t kTaskReadyRva = 0x00A390C0;
constexpr std::uintptr_t kTaskDispatchRva = 0x00A37EC0;
constexpr std::uintptr_t kWaitHelperRva = 0x00A395C0;
constexpr std::uintptr_t kScaleformTimeRva = 0x0098DC90;
constexpr std::uintptr_t kScaleformInitRva = 0x009928B0;
constexpr std::uintptr_t kNisSetPlayTimeRva = 0x003E51B0;
constexpr std::uintptr_t kNisPlayRva = 0x003E3C40;
constexpr std::uintptr_t kNisBootstrapRva = 0x003E3B60;
constexpr std::uintptr_t kNisOwnerRva = 0x003E6CE0;
constexpr std::uintptr_t kNisActorSetupRva = 0x003E8050;
constexpr std::uintptr_t kNisActorRestoreRva = 0x003E7A80;
constexpr std::uintptr_t kTwitchStateTickRva = 0x000819D0;
constexpr std::uintptr_t kTwitchLoginCallbackRva = 0x00081CB0;
constexpr std::uintptr_t kFrameFlowRva = 0x00590AD0;
constexpr std::uintptr_t kCutsceneFlowOwnerRva = 0x00412240;
constexpr std::uintptr_t kScaleformTimeProviderRva = 0x02441150;
constexpr std::uintptr_t kFogSlicingModeRva = 0x0006BA60;
constexpr std::uintptr_t kAntiAliasOwnerRva = 0x00053230;
constexpr std::uintptr_t kAntiAliasFxHandoffRva = 0x000554E0;
constexpr std::uintptr_t kAntiAliasAuxStateQueryRva = 0x00008A00;
constexpr std::uintptr_t kRenderSubmitMaterialRva = 0x0000E880;
constexpr std::uintptr_t kPresentBufferRva = 0x00053A30;
constexpr std::uintptr_t kRumbleApplyHelperRva = 0x0014C170;
constexpr std::uintptr_t kRumbleApplyObjectRva = 0x02175EB0;
constexpr std::uintptr_t kCharacterHandleWaterCollisionRva = 0x00535140;
constexpr std::uintptr_t kCharacterEffectsUpdateRva = 0x00559AF0;
// SDK-backed legacy_researched helpers.  These are only enabled for the
// verified legacy DE image; the later Steam layout is fail-closed until its
// corresponding helpers are independently mapped.
constexpr std::uintptr_t kCharacterPhysicsGetVelocityRva = 0x00468150;
constexpr std::uintptr_t kUiIsPlayerInCombatRva = 0x005EE680;
constexpr std::uintptr_t kHealthApplyDamageRva = 0x00521F60;
constexpr std::uintptr_t kSimTimeIsPausedRva = 0x00510980;
constexpr std::uintptr_t kUiIsGamePausedRva = 0x005EE1B0;
constexpr std::uintptr_t kCharacterPaintOwnerRva = 0x004CF1E0;
constexpr std::uintptr_t kCharacterPaintConsumerRva = 0x00509200;
constexpr std::uintptr_t kCharacterEffectDispatchOwnerRva = 0x00400930;
constexpr std::uintptr_t kCharacterEffectDispatchConsumeRva = 0x00402CB0;
constexpr std::uintptr_t kCharacterEffectQueueBuilderRva = 0x003FE840;
constexpr std::uintptr_t kCharacterAnimationApplyCharredEffectRva = 0x0057F270;
constexpr std::uintptr_t kCharacterAnimationApplyPaintEffectRva = 0x0057F280;
constexpr std::uintptr_t kCharacterAnimationCreateDamageRigRva = 0x00580570;
constexpr std::uintptr_t kDamageRigApplyCharredEffectRva = 0x003A1EA0;
constexpr std::uintptr_t kDamageRigApplyPaintEffectRva = 0x003A2430;
constexpr std::uintptr_t kCharacterDamageRigResetDamageRva = 0x003AE110;
constexpr std::uintptr_t kSimObjectGetComponentRva = 0x00190AD0;
constexpr std::uintptr_t kTimeOfDayAccessorRva = 0x0006AA70;
constexpr std::uintptr_t kWetSurfaceBlockCounterRva = 0x0242E578;
constexpr std::uintptr_t kSceneryPrepareRva = 0x0005B190;
constexpr std::uintptr_t kScenerySetupRva = 0x0005C400;
constexpr std::uintptr_t kRenderSceneryBuilderRva = 0x0005BC60;
constexpr std::uintptr_t kRasterizeBucketBuilderRva = 0x000262B0;
constexpr std::uintptr_t kRenderTaskManagerRva = 0x0235B160;
constexpr std::uintptr_t kRenderContextInstanceRva = 0x021299D8;
constexpr std::uintptr_t kSceneryCounter0Rva = 0x021363C0;
constexpr std::uintptr_t kSceneryCounter1Rva = 0x021363D0;
constexpr std::uintptr_t kSceneryCounter2Rva = 0x021363E0;
constexpr std::uintptr_t kSceneryCounter3Rva = 0x021363F0;
constexpr std::uintptr_t kD3DDeviceSlotRva = 0x02439AE0;
constexpr std::uintptr_t kD3DContextSlotRva = 0x02439AE8;
constexpr std::uintptr_t kDxgiSwapChainSlotRva = 0x02439B10;
constexpr std::uintptr_t kPresentRtvSlotRva = 0x02439B18;
constexpr std::uintptr_t kPedestrianFrameRateThrottleRva = 0x0040B960;
constexpr std::uintptr_t kPedestrianSpawnUpdateRva = 0x00418180;
constexpr std::uintptr_t kAverageWindowInitializeRva = 0x0017CFC0;
constexpr std::uintptr_t kMaterialOnLoadRva = 0x000934A0;
constexpr std::uintptr_t kLegacyGameMemcpyRva = 0x012ADDD0;
constexpr std::uintptr_t kPcFileReadRva = 0x00A38290;
constexpr std::uintptr_t kPcFileSeekRva = 0x00A382D0;
constexpr std::uintptr_t kPcFileTellRva = 0x00A38840;
constexpr std::uintptr_t kPcFileSizeRva = 0x00A38890;
constexpr std::uintptr_t kQFileReadAtRva = 0x001896B0;
constexpr std::uintptr_t kQFileWriteAtRva = 0x0018CE50;
constexpr std::uintptr_t kQFileReadyRva = 0x0018CA00;
constexpr std::uintptr_t kQcmpDecompressRva = 0x00183080;
constexpr std::uintptr_t kStreamFileOpenRva = 0x0022BBD0;
constexpr std::uintptr_t kStreamFileCloseRva = 0x00228530;
constexpr std::uintptr_t kResourceChunkDispatchRva = 0x00176B50;
constexpr unsigned long kWaitLogThresholdMs = 16;
constexpr unsigned long kWaitLongThresholdMs = 100;
constexpr unsigned long kWaitVeryLongThresholdMs = 1000;
constexpr std::uintptr_t kLatestSteamNisSetPlayTimeRva = 0x003E50C0;
constexpr std::uintptr_t kLatestSteamNisOwnerRva = 0x003E6BF0;
constexpr std::uintptr_t kLatestSteamFrameFlowRva = 0x00590D30;
constexpr std::uintptr_t kLatestSteamCutsceneFlowOwnerRva = 0x00412490;
constexpr std::uintptr_t kLatestSteamFogSlicingModeRva = 0x0006BD70;
constexpr std::uintptr_t kLatestSteamPresentBufferRva = 0x00053D40;
constexpr std::uintptr_t kLatestSteamHealthApplyDamageRva = 0x005221C0;
constexpr std::uintptr_t kLatestSteamSimObjectGetComponentRva = 0x00190A60;
constexpr std::uintptr_t kLatestSteamTimeOfDayAccessorRva = 0x0006AD80;
constexpr std::uintptr_t kLatestSteamCharacterHandleWaterCollisionRva = 0x005353A0;
constexpr std::uintptr_t kLatestSteamCharacterEffectsUpdateRva = 0x00559D50;
constexpr std::uintptr_t kLatestSteamCharacterEffectDispatchConsumeRva = 0x00402E30;
constexpr std::uintptr_t kLatestSteamCharacterEffectQueueBuilderRva = 0x003FE900;
constexpr std::uintptr_t kLatestSteamDamageRigApplyPaintEffectRva = 0x003A2220;
constexpr std::uintptr_t kLatestSteamCharacterDamageRigResetDamageRva = 0x003ADF30;
constexpr std::uintptr_t kLatestSteamTaskReadyRva = 0x00A39060;
constexpr std::uintptr_t kLatestSteamTaskDispatchRva = 0x00A37E60;
constexpr std::uintptr_t kLatestSteamPedestrianFrameRateThrottleRva = 0x0040BBB0;
constexpr std::uintptr_t kLatestSteamPedestrianSpawnUpdateRva = 0x00418400;
constexpr std::uintptr_t kLatestSteamAverageWindowInitializeRva = 0x0017D040;
constexpr std::uintptr_t kLatestSteamMaterialOnLoadRva = 0x00093890;
constexpr std::uintptr_t kLatestSteamUiIsGamePausedRva = 0x005EE350;
constexpr std::uintptr_t kLatestSteamPcFileReadRva = 0x00A38230;
constexpr std::uintptr_t kLatestSteamPcFileSeekRva = 0x00A38270;
constexpr std::uintptr_t kLatestSteamPcFileTellRva = 0x00A387E0;
constexpr std::uintptr_t kLatestSteamPcFileSizeRva = 0x00A38830;
constexpr std::uintptr_t kLatestSteamQFileReadAtRva = 0x00189760;
constexpr std::uintptr_t kLatestSteamQFileWriteAtRva = 0x0018CF00;
constexpr std::uintptr_t kLatestSteamQFileReadyRva = 0x0018CAB0;
constexpr std::uintptr_t kLatestSteamQcmpDecompressRva = 0x00183110;
constexpr std::uintptr_t kLatestSteamStreamFileOpenRva = 0x0022BD20;
constexpr std::uintptr_t kLatestSteamStreamFileCloseRva = 0x002286F0;
constexpr std::uintptr_t kLatestSteamResourceChunkDispatchRva = 0x00176BF0;

constexpr std::array<std::uint8_t, 13> kPedestrianSpawnUpdateSignature{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0xC7, 0x41, 0x20, 0x01, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 14> kLegacyPedestrianThrottleSignature{
    0x8B, 0x05, 0xA6, 0xF5, 0xC6, 0x01, 0xF3, 0x0F, 0x10, 0x15, 0x9A, 0xF9, 0xC6, 0x01};
constexpr std::array<std::uint8_t, 14> kLatestPedestrianThrottleSignature{
    0x8B, 0x05, 0x56, 0xF3, 0xC6, 0x01, 0xF3, 0x0F, 0x10, 0x15, 0x4A, 0xF7, 0xC6, 0x01};
constexpr std::array<std::uint8_t, 20> kAverageWindowInitializeSignature{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24,
    0x40, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x09, 0x0F, 0x29, 0x7C};
constexpr std::array<std::uint8_t, 16> kMaterialOnLoadSignature{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};
constexpr std::array<std::uint8_t, 6> kUiIsGamePausedSignature{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::array<std::uint8_t, 10> kLegacyGameMemcpySignature{
    0x4C, 0x8B, 0xD9, 0x4C, 0x8B, 0xD2, 0x49, 0x83, 0xF8, 0x10};
constexpr std::array<std::uint8_t, 16> kCharacterPhysicsGetVelocitySignature{
    0x48, 0x8B, 0x81, 0xB0, 0x01, 0x00, 0x00, 0x48,
    0x8B, 0x48, 0x20, 0x48, 0x8B, 0xC2, 0x0F, 0x28};
constexpr std::array<std::uint8_t, 11> kUiIsPlayerInCombatSignature{
    0x48, 0x8B, 0x0D, 0xC9, 0xB7, 0xDC, 0x01, 0x33, 0xD2, 0xE9, 0x12};
constexpr std::array<std::uint8_t, 17> kPcFileReadSignature{0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B,
                                                            0x4A, 0x50, 0x49, 0x8B, 0xC1, 0x4D,
                                                            0x8B, 0xD0, 0x48, 0x85, 0xC9};
constexpr std::array<std::uint8_t, 15> kPcFileSeekSignature{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x4A, 0x50, 0x4D, 0x8B, 0xD1, 0x48, 0x85, 0xC9, 0x75};
constexpr std::array<std::uint8_t, 13> kPcFileTellAndSizeSignature{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x4A, 0x50, 0x48, 0x85, 0xC9, 0x75, 0x09};
constexpr std::array<std::uint8_t, 17> kQFileReadAtSignature{0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
                                                             0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                             0xEC, 0x20, 0x4D, 0x8B, 0xF9};
constexpr std::array<std::uint8_t, 17> kQFileWriteAtSignature{0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
                                                              0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                              0xEC, 0x40, 0x4D, 0x8B, 0xF9};
constexpr std::array<std::uint8_t, 16> kQcmpDecompressSignature{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48,
    0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x0F, 0x10};
constexpr std::array<std::uint8_t, 6> kQFileReadySignature{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::array<std::uint8_t, 16> kStreamFileOpenSignature{
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x40, 0xD8};
constexpr std::array<std::uint8_t, 16> kStreamFileCloseSignature{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x20, 0x00, 0x48, 0x8B, 0xD9, 0x75, 0x11};
constexpr std::array<std::uint8_t, 16> kResourceChunkDispatchSignature{
    0x40, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83,
    0xEC, 0x30, 0x45, 0x8B, 0xE0, 0x48, 0x8B, 0xFA};

using TaskReadyFn = void (*)(void* task_manager_owner, void* task_header);
using TaskDispatchFn = void (*)(void* task_manager, unsigned int worker_index);
using WaitHelperFn = void (*)(void* task_manager_owner, void* task_header);
using ScaleformTimeFn = void (*)();
using ScaleformInitFn = void (*)(void* heap_desc, void* sys_alloc);
using NisSetPlayTimeFn = void (*)(void* nis_manager, float scene_time, bool sync_scene_time);
using NisPlayFn = void (*)(void* nis_manager);
using NisBootstrapFn = unsigned char (*)(void* nis_manager, void* scene_asset);
using NisOwnerFn = void (*)(void* nis_manager, float delta_seconds);
using NisActorSetupFn = void (*)(std::uintptr_t actor_state, std::uintptr_t actor_target);
using NisActorRestoreFn = void (*)(std::uintptr_t actor_state);
using TwitchStateTickFn = void (*)(std::uintptr_t owner);
using TwitchLoginCallbackFn = void (*)(int result, std::uintptr_t owner);
using FrameFlowFn = void (*)(float delta_seconds, void* callback);
using CutsceneFlowOwnerFn = void (*)(void* owner, float delta_seconds);
using VolumetricFogSetterFn = void (*)(void* script_context);
using FogSlicingModeFn = void (*)(void* time_of_day_manager, int update_interval);
using AntiAliasOwnerFn = void (*)(std::uintptr_t render_context,
                                  std::uintptr_t param_2,
                                  std::uintptr_t param_3,
                                  std::uintptr_t* param_4,
                                  std::uintptr_t* param_5);
using AntiAliasFxHandoffFn = void (*)(std::uintptr_t render_context,
                                      std::uintptr_t arg1,
                                      std::uintptr_t arg2,
                                      std::uintptr_t arg3);
using AntiAliasAuxStateQueryFn = char (*)(void* state);
using RenderSubmitMaterialFn = void (*)(void* render_cmd,
                                        void* submit_params,
                                        void* material,
                                        int flags);
using PresentBufferFn = void (*)(void* render_packet);
using PedestrianFrameRateThrottleFn = void (*)();
using PedestrianSpawnUpdateFn = void (*)(void* manager, float delta_seconds);
using AverageWindowInitializeFn = void (*)(void* window,
                                           float maximum_timespan,
                                           float assumed_sample_rate);
using MaterialOnLoadFn = void (*)(void* material);
using ChaseCameraSetParametersFn = void (*)(void* component,
                                             const void* parameters);
using ChaseCameraUpdateFn = void (*)(void* component, float delta_seconds);
using AngularApproachFn = bool (*)(float* current,
                                   float target,
                                   float rate,
                                   float delta_seconds);
struct CameraVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
using GameCameraSetDesiredEyeLookUpFn = void (*)(
    void* component,
    float delta_seconds,
    const CameraVector3* desired_eye,
    const CameraVector3* desired_look,
    const CameraVector3* desired_up,
    bool collide,
    CameraVector3* new_eye,
    CameraVector3* new_look);
using SamplerBuilderFn = void (*)(void* sampler_state, void* descriptor_source);
using AnisotropyWriterFn = void (*)(int exponent);
using PcFileReadFn = qfile_io::DeviceReadFn;
using PcFileSeekFn = qfile_io::DeviceSeekFn;
using PcFileTellFn = std::uint64_t (*)(void* device, void* file);
using PcFileSizeFn = std::uint64_t (*)(void* device, void* file);
using QFileReadAtFn = std::uint64_t (*)(void* file, void* buffer, std::uint64_t byte_count,
                                        std::int64_t offset, std::uint32_t origin);
using QFileWriteAtFn = std::uint64_t (*)(void* file, const void* buffer, std::uint64_t byte_count,
                                         std::int64_t offset, std::uint32_t origin,
                                         bool* disk_full);
using QcmpDecompressFn = std::uint64_t (*)(void* source,
                                           std::uint64_t source_size,
                                           void* destination,
                                           std::uint64_t destination_capacity,
                                           const char* label);
using StreamFileOpenFn = void* (*)(const char* path, std::uint32_t access, bool allow_loose_file,
                                   void* device, void* user_data);
using StreamFileCloseFn = void (*)(void* stream_file);
using ResourceChunkDispatchFn = void (*)(void* manager,
                                         void* buffer,
                                         std::uint32_t size);
using GameMemcpyFn = void* (*)(void* destination, const void* source, std::size_t size);
using RumbleApplyHelperFn = void (*)(std::uintptr_t rumble_object, bool enabled);
using CharacterHandleWaterCollisionFn = void (*)(void* character_effects_component, const void* mat,
                                                 const void* character_velocity);
using CharacterEffectsUpdateFn = void (*)(void* character_effects_component, float delta_seconds);
using CharacterPhysicsGetVelocityFn = void* (*)(void* character_physics_component,
                                                float* output_velocity);
using UiIsPlayerInCombatFn = bool (*)();
using HealthApplyDamageFn = bool (*)(void* health_component,
                                     int damage,
                                     void* attacker,
                                     void* hit_record,
                                     bool projectile_damage);
using SimTimeIsPausedFn = bool (*)();
using UiIsGamePausedFn = bool (*)();
using CharacterPaintOwnerFn = void (*)(std::uintptr_t owner, std::uintptr_t character_effects_component);
using CharacterPaintConsumerFn = void (*)(std::uintptr_t character_effects_component,
                                          std::uintptr_t paint_block_owner);
using CharacterEffectDispatchOwnerFn = void* (*)();
using CharacterEffectDispatchConsumeFn = void (*)(void* dispatch_owner,
                                                  void* character_effects_component);
using CharacterEffectQueueBuilderFn = void (*)(std::uintptr_t owner,
                                               std::uintptr_t character_effects_component,
                                               std::uint32_t mode);
using CharacterAnimationApplyCharredEffectFn = void (*)(void* character_animation_component,
                                                        float amount);
using CharacterAnimationApplyPaintEffectFn = void (*)(void* character_animation_component,
                                                      bool enable,
                                                      float r,
                                                      float g,
                                                      float b);
using CharacterAnimationCreateDamageRigFn = void (*)(void* character_animation_component);
using DamageRigApplyCharredEffectFn = void (*)(void* damage_rig, float amount);
using DamageRigApplyPaintEffectFn = void (*)(void* damage_rig,
                                             bool enable,
                                             float r,
                                             float g,
                                             float b);
using CharacterDamageRigResetDamageFn = void (*)(void* damage_rig);
using SimObjectGetComponentFn = void* (*)(void* sim_object, std::uint32_t type_uid);
using TimeOfDayAccessorFn = std::uintptr_t (*)();
using SceneryPrepareFn = void (*)(std::uintptr_t scenery_state);
using ScenerySetupFn = void (*)(std::uintptr_t query_context,
                                std::uint32_t query_kind,
                                void* param_3,
                                void* param_4,
                                float range_scale);
using RenderSceneryBuilderFn = std::uintptr_t (*)(std::uintptr_t param_1,
                                                  std::uintptr_t param_2,
                                                  void* param_3,
                                                  std::uint32_t param_4,
                                                  std::uint32_t param_5);
using RasterizeBucketBuilderFn = unsigned long long (*)(std::uintptr_t state,
                                                        int bucket_index,
                                                        void* dependency);
TaskReadyFn g_task_ready_original = nullptr;
TaskDispatchFn g_task_dispatch_original = nullptr;
WaitHelperFn g_wait_helper_original = nullptr;
ScaleformTimeFn g_scaleform_time_original = nullptr;
ScaleformInitFn g_scaleform_init_original = nullptr;
NisSetPlayTimeFn g_nis_set_play_time_original = nullptr;
NisPlayFn g_nis_play_original = nullptr;
NisBootstrapFn g_nis_bootstrap_original = nullptr;
NisOwnerFn g_nis_owner_original = nullptr;
NisActorSetupFn g_nis_actor_setup_original = nullptr;
NisActorRestoreFn g_nis_actor_restore_original = nullptr;
TwitchStateTickFn g_twitch_state_tick_original = nullptr;
TwitchLoginCallbackFn g_twitch_login_callback_original = nullptr;
FrameFlowFn g_frame_flow_original = nullptr;
CutsceneFlowOwnerFn g_cutscene_flow_owner_original = nullptr;
VolumetricFogSetterFn g_volumetric_fog_setter_original = nullptr;
FogSlicingModeFn g_fog_slicing_mode_original = nullptr;
AntiAliasOwnerFn g_antialias_owner_original = nullptr;
AntiAliasFxHandoffFn g_antialias_fx_handoff_original = nullptr;
AntiAliasAuxStateQueryFn g_antialias_aux_state_query_original = nullptr;
RenderSubmitMaterialFn g_render_submit_material_original = nullptr;
PresentBufferFn g_present_buffer_original = nullptr;
PedestrianFrameRateThrottleFn g_pedestrian_frame_rate_throttle_original = nullptr;
PedestrianSpawnUpdateFn g_pedestrian_spawn_update_original = nullptr;
AverageWindowInitializeFn g_average_window_initialize_original = nullptr;
MaterialOnLoadFn g_material_on_load_original = nullptr;
ChaseCameraSetParametersFn g_chase_camera_set_parameters_original = nullptr;
ChaseCameraUpdateFn g_chase_camera_update_original = nullptr;
AngularApproachFn g_angular_approach_original = nullptr;
GameCameraSetDesiredEyeLookUpFn g_game_camera_set_desired_eye_look_up_original =
    nullptr;
std::atomic<unsigned long long> g_gtaiv_camera_probe_sample_count = 0;
std::atomic<unsigned int> g_gtaiv_camera_probe_log_budget = 0;
std::atomic<unsigned long long> g_gtaiv_camera_probe_next_sample_tick = 0;
std::mutex g_gtaiv_camera_probe_state_mutex;
unsigned long long g_gtaiv_camera_probe_last_state =
    (std::numeric_limits<unsigned long long>::max)();
std::uintptr_t g_gtaiv_camera_probe_last_subject =
    (std::numeric_limits<std::uintptr_t>::max)();
std::atomic<unsigned long long> g_gtaiv_camera_dynamic_probe_sample_count = 0;
std::atomic<unsigned int> g_gtaiv_camera_dynamic_probe_log_budget = 0;
std::atomic<unsigned long long> g_gtaiv_camera_dynamic_probe_next_sample_tick =
    0;
SamplerBuilderFn g_sampler_builder_original = nullptr;
std::atomic<bool> g_sampler_builder_first_verified_invocation_logged = false;
std::atomic<bool> g_sampler_builder_failure_logged = false;
AnisotropyWriterFn g_anisotropy_writer_original = nullptr;
std::atomic<bool> g_anisotropy_writer_first_verified_invocation_logged = false;
PcFileReadFn g_pc_file_read_original = nullptr;
PcFileSeekFn g_pc_file_seek_original = nullptr;
PcFileTellFn g_pc_file_tell_original = nullptr;
PcFileSizeFn g_pc_file_size_original = nullptr;
QFileReadAtFn g_qfile_read_at_original = nullptr;
QFileWriteAtFn g_qfile_write_at_original = nullptr;
qfile_io::QFileReadyFn g_qfile_ready = nullptr;
QcmpDecompressFn g_qcmp_decompress_original = nullptr;
std::atomic<bool> g_qcmp_rejection_logged = false;
StreamFileOpenFn g_stream_file_open_original = nullptr;
StreamFileCloseFn g_stream_file_close = nullptr;
ResourceChunkDispatchFn g_resource_chunk_dispatch_original = nullptr;
std::atomic<bool> g_resource_chunk_rejection_logged = false;
constexpr std::size_t kArchiveSizeCacheCapacity = 32;
constexpr std::uintptr_t kArchiveSizeCacheClaimed = 1;
struct ArchiveSizeCacheEntry {
    std::atomic<std::uintptr_t> inventory{0};
    std::atomic<std::uintptr_t> file{0};
    std::atomic<std::uintptr_t> native_handle{0};
    std::atomic<std::uint32_t> resource_uid{0};
    std::atomic<std::uint64_t> size{qfile_io::kOperationFailure};
};
std::array<ArchiveSizeCacheEntry, kArchiveSizeCacheCapacity> g_archive_size_cache{};
std::array<std::atomic<std::uint64_t>, kArchiveSizeCacheCapacity> g_archive_rejection_keys{};
#if !defined(SPATCH_FINAL_RELEASE)
GameMemcpyFn g_game_memcpy_original = nullptr;
#endif
RumbleApplyHelperFn g_rumble_apply_helper = nullptr;
CharacterHandleWaterCollisionFn g_character_handle_water_collision_original = nullptr;
CharacterEffectsUpdateFn g_character_effects_update_original = nullptr;
CharacterPhysicsGetVelocityFn g_character_physics_get_velocity = nullptr;
UiIsPlayerInCombatFn g_ui_is_player_in_combat = nullptr;
HealthApplyDamageFn g_health_apply_damage_original = nullptr;
SimTimeIsPausedFn g_simtime_is_paused = nullptr;
UiIsGamePausedFn g_ui_is_game_paused = nullptr;
CharacterPaintOwnerFn g_character_paint_owner_original = nullptr;
CharacterPaintConsumerFn g_character_paint_consumer_original = nullptr;
CharacterEffectDispatchOwnerFn g_character_effect_dispatch_owner_original = nullptr;
CharacterEffectDispatchConsumeFn g_character_effect_dispatch_consume_original = nullptr;
CharacterEffectQueueBuilderFn g_character_effect_queue_builder_original = nullptr;
CharacterAnimationApplyCharredEffectFn g_character_animation_apply_charred_effect_original = nullptr;
CharacterAnimationApplyPaintEffectFn g_character_animation_apply_paint_effect_original = nullptr;
CharacterAnimationCreateDamageRigFn g_character_animation_create_damage_rig_original = nullptr;
DamageRigApplyCharredEffectFn g_damage_rig_apply_charred_effect_original = nullptr;
DamageRigApplyPaintEffectFn g_damage_rig_apply_paint_effect_original = nullptr;
CharacterDamageRigResetDamageFn g_character_damage_rig_reset_damage_original = nullptr;
SimObjectGetComponentFn g_simobject_get_component = nullptr;
SceneryPrepareFn g_scenery_prepare_original = nullptr;
ScenerySetupFn g_scenery_setup_original = nullptr;
RenderSceneryBuilderFn g_render_scenery_builder_original = nullptr;
RasterizeBucketBuilderFn g_rasterize_bucket_builder_original = nullptr;
TimeOfDayAccessorFn g_time_of_day_accessor = nullptr;

Config g_config{};
bool g_use_latest_steam_layout = false;
bool g_sweat_only_health_fast_path = false;
std::uintptr_t g_module_base = 0;
std::uintptr_t g_module_end = 0;
std::uintptr_t g_provider_slot = 0;
std::uintptr_t g_render_task_manager = 0;
std::uintptr_t g_render_context_instance_slot = 0;
std::uintptr_t g_scenery_counter0 = 0;
std::uintptr_t g_scenery_counter1 = 0;
std::uintptr_t g_scenery_counter2 = 0;
std::uintptr_t g_scenery_counter3 = 0;
std::uintptr_t g_d3d_device_slot = 0;
std::uintptr_t g_d3d_context_slot = 0;
std::uintptr_t g_dxgi_swapchain_slot = 0;
std::uintptr_t g_present_rtv_slot = 0;
std::uintptr_t g_rumble_apply_object = 0;
std::uintptr_t g_wet_surface_block_counter = 0;
std::uintptr_t g_volumetric_fog_intensity = 0;
std::filesystem::path g_config_path;
DiagnosticAtomic<unsigned long long> g_task_ready_count = 0;
DiagnosticAtomic<unsigned long long> g_task_dispatch_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_helper_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_helper_nonzero_task_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_16ms_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_100ms_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_1000ms_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_5000ms_count = 0;
DiagnosticAtomic<unsigned long long> g_pedestrian_throttle_frame_count = 0;
DiagnosticAtomic<unsigned long long> g_pedestrian_throttle_stock_call_count = 0;
DiagnosticAtomic<unsigned long long> g_pedestrian_throttle_clamped_frame_count = 0;
DiagnosticAtomic<unsigned long long> g_average_window_initialize_count = 0;
DiagnosticAtomic<unsigned long long> g_average_window_expanded_count = 0;
DiagnosticAtomic<bool> g_average_window_fallback_logged = false;
std::atomic<bool> g_original_fog_applied = false;
std::atomic<bool> g_original_fog_frame_initialized = false;
std::atomic<bool> g_original_fog_failure_logged = false;
std::atomic<bool> g_original_fog_setter_logged = false;
std::atomic<std::uint32_t> g_character_eye_restore_applied_mask = 0;
std::atomic<std::uint32_t> g_character_eye_restore_already_present_mask = 0;
std::atomic<std::uint32_t> g_character_eye_restore_failure_mask = 0;
DiagnosticAtomic<unsigned long long> g_scaleform_time_count = 0;
DiagnosticAtomic<unsigned long long> g_scaleform_provider_non_null_count = 0;
DiagnosticAtomic<unsigned long long> g_scaleform_init_count = 0;
DiagnosticAtomic<unsigned long long> g_rumble_override_apply_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_set_play_time_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_sync_scene_time_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_30hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_other_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_scene_time_fix_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_advanced_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_repeat_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_multi_tick_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_state1_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_state2_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_fail_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_30hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_other_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_advanced_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_repeat_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_multi_tick_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_setup_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_untracked_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_setup_duplicate_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_duplicate_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_never_seen_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_suppressed_count = 0;
DiagnosticAtomic<unsigned long long> g_twitch_state_tick_count = 0;
DiagnosticAtomic<unsigned long long> g_twitch_login_callback_count = 0;
DiagnosticAtomic<unsigned long long> g_twitch_login_failure_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_dt_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_dt_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_dt_other_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_from_cutscene_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_dt_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_forwarded_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_forwarded_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_forwarded_other_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_zero_dt_override_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_zero_dt_pause_override_count = 0;
DiagnosticAtomic<unsigned long long> g_fog_slicing_mode_count = 0;
DiagnosticAtomic<unsigned long long> g_fog_slicing_clamp_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_skip_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_main_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_hair_blur_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_fx_handoff_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_variant_apply_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_aux_apply_count = 0;
DiagnosticAtomic<unsigned long long> g_post_material_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_composite_lights_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_composite_final_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_bloom_threshold_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_lightshaft_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_shadow_collector_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_composite_final_snapshot_change_count = 0;
DiagnosticAtomic<unsigned long long> g_character_water_collision_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effects_update_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effects_wet_surface_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_apply_damage_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_apply_projectile_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_apply_melee_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_anim_found_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_hitreact_found_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_consumer_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_dispatch_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_dispatch_consume_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_queue_builder_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_queue_builder_tracked_count = 0;
DiagnosticAtomic<unsigned long long> g_character_charred_anim_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_anim_count = 0;
DiagnosticAtomic<unsigned long long> g_character_damage_rig_create_count = 0;
DiagnosticAtomic<unsigned long long> g_character_charred_rig_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_rig_count = 0;
DiagnosticAtomic<unsigned long long> g_character_damage_rig_reset_count = 0;
DiagnosticAtomic<unsigned long long> g_character_wet_force_count = 0;
DiagnosticAtomic<unsigned long long> g_character_wet_force_verify_count = 0;
#if !defined(SPATCH_FINAL_RELEASE)
DiagnosticAtomic<unsigned long long> g_unique_callback_count = 0;
#endif
DiagnosticAtomic<unsigned long long> g_scenery_prepare_count = 0;
DiagnosticAtomic<unsigned long long> g_scenery_setup_count = 0;
DiagnosticAtomic<unsigned long long> g_render_scenery_builder_count = 0;
DiagnosticAtomic<unsigned long long> g_rasterize_bucket_builder_count = 0;
DiagnosticAtomic<unsigned long long> g_render_scenery_queue_delta_total = 0;
DiagnosticAtomic<unsigned long long> g_rasterize_bucket_queue_delta_total = 0;
DiagnosticAtomic<unsigned long long> g_scenery_setup_queue_delta_total = 0;
DiagnosticAtomic<unsigned long long> g_scenery_prepare_ready_total = 0;
DiagnosticAtomic<unsigned long long> g_scenery_setup_ready_total = 0;
DiagnosticAtomic<unsigned long long> g_render_scenery_ready_total = 0;
DiagnosticAtomic<unsigned long long> g_rasterize_bucket_ready_total = 0;
// A malformed fog interval is a useful diagnostic, but the hook can be
// reached once per render update.  Emit the warning once per process and keep
// the counter for the complete occurrence count instead of turning a bad
// engine value into synchronous logger I/O on every frame.
std::atomic<bool> g_fog_clamp_warning_emitted = false;
DiagnosticAtomic<unsigned int> g_last_scenery_counter0 = 0;
DiagnosticAtomic<unsigned int> g_last_scenery_counter1 = 0;
DiagnosticAtomic<unsigned int> g_last_scenery_counter2 = 0;
DiagnosticAtomic<unsigned int> g_last_scenery_counter3 = 0;
DiagnosticAtomic<std::uint32_t> g_last_nis_owner_dt_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_frame_flow_dt_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_cutscene_flow_input_dt_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_cutscene_flow_forwarded_dt_bits = 0;
DiagnosticAtomic<int> g_last_aa_state_gate = 0;
DiagnosticAtomic<int> g_last_aa_hair_blur_gate = 0;
DiagnosticAtomic<int> g_last_aa_variant_mode = 0;
DiagnosticAtomic<int> g_last_aa_aux_mode = 0;
DiagnosticAtomic<unsigned int> g_last_aa_shader_uid = 0;
DiagnosticAtomic<unsigned int> g_last_aa_raster_uid = 0;
DiagnosticAtomic<unsigned int> g_last_aa_aux_uid = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_material = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_target = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_source_a = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_source_b = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_fx_arg1 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_fx_arg2 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_fx_arg3 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_material = 0;
DiagnosticAtomic<int> g_last_post_final_flags = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_cmd = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_params = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param0 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param1 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param2 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param3 = 0;
DiagnosticAtomic<int> g_last_rumble_override_value = -1;
DiagnosticAtomic<std::uintptr_t> g_last_character_wet_component = 0;
std::atomic<std::uintptr_t> g_character_sweat_active_component = 0;
std::atomic<std::uintptr_t> g_player_water_collision_component = 0;
std::atomic<unsigned long long> g_player_water_collision_generation = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_water_speed_bits = 0;
DiagnosticAtomic<unsigned int> g_last_character_active_surface_uid = 0;
DiagnosticAtomic<unsigned int> g_last_character_active_wet_surface_uid = 0;
DiagnosticAtomic<int> g_last_character_wet_gate_counter = 0;
DiagnosticAtomic<unsigned int> g_last_character_is_on_fire = 0;
DiagnosticAtomic<unsigned int> g_last_character_is_smoldering = 0;
DiagnosticAtomic<unsigned int> g_last_character_is_attached_to_player = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_fire_extinguish_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_smolder_extinguish_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_queued_health_damage_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_timeofday_weather_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_timeofday_override_bits = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_anim_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_hitreact_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_attacker = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_hit_record = 0;
DiagnosticAtomic<int> g_last_character_health_damage = 0;
DiagnosticAtomic<unsigned int> g_last_character_health_projectile = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_charred_amount_bits = 0;
DiagnosticAtomic<unsigned int> g_last_character_paint_enable = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_paint_r_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_paint_g_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_paint_b_bits = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_consumer_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_consumer_block = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_owner_ptr = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_owner_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_dispatch_owner = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_dispatch_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_queue_builder_owner = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_queue_builder_component = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_effect_queue_builder_mode = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_damage_anim_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_damage_rig = 0;
std::atomic<int> g_aa_runtime_variant_mode = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_variant_up_key_tick = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_variant_down_key_tick = 0;
std::atomic<int> g_aa_runtime_aux_mode = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_aux_up_key_tick = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_aux_down_key_tick = 0;
DiagnosticAtomic<unsigned long long> g_last_smaa_toggle_key_tick = 0;
DiagnosticAtomic<long> g_character_trace_budget = 0;
#if !defined(SPATCH_FINAL_RELEASE)
std::atomic<bool> g_force_wetness_field_probe = false;
std::atomic<bool> g_force_wetness_field_probe_logged = false;
std::atomic<int> g_force_wetness_field_probe_mode = 1;
DiagnosticAtomic<unsigned long long> g_force_wetness_field_probe_toggle_tick = 0;
std::atomic<std::uintptr_t> g_wetness_memcpy_watch_address = 0;
std::atomic<bool> g_wetness_memcpy_capture_claimed = false;
std::atomic<bool> g_wetness_memcpy_capture_ready = false;
std::atomic<bool> g_wetness_memcpy_capture_logged = false;
std::atomic<std::uintptr_t> g_wetness_memcpy_capture_return = 0;
std::atomic<std::uintptr_t> g_wetness_memcpy_capture_destination = 0;
std::atomic<std::uintptr_t> g_wetness_memcpy_capture_source = 0;
std::atomic<std::size_t> g_wetness_memcpy_capture_size = 0;
std::atomic<unsigned long> g_wetness_memcpy_capture_thread = 0;
std::atomic<bool> g_sweat_field_probe = false;
std::atomic<unsigned long long> g_sweat_field_probe_last_player_log_tick = 0;
std::atomic<unsigned long long> g_sweat_field_probe_last_npc_log_tick = 0;
std::atomic<unsigned long long> g_sweat_update_probe_last_log_tick = 0;
#endif

#if !defined(SPATCH_FINAL_RELEASE)
DiagnosticAtomic<unsigned long> g_task_ready_verbose = 0;
DiagnosticAtomic<unsigned long> g_task_dispatch_verbose = 0;
DiagnosticAtomic<unsigned long> g_wait_helper_verbose = 0;
DiagnosticAtomic<unsigned long> g_scaleform_time_verbose = 0;
DiagnosticAtomic<unsigned long> g_scaleform_init_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_set_play_time_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_play_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_bootstrap_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_owner_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_actor_state_verbose = 0;
DiagnosticAtomic<unsigned long> g_twitch_probe_verbose = 0;
DiagnosticAtomic<unsigned long> g_frame_flow_verbose = 0;
DiagnosticAtomic<unsigned long> g_cutscene_flow_owner_verbose = 0;
DiagnosticAtomic<unsigned long> g_fog_slicing_verbose = 0;
DiagnosticAtomic<unsigned long> g_aa_probe_verbose = 0;
DiagnosticAtomic<unsigned long> g_aa_fx_probe_verbose = 0;
DiagnosticAtomic<unsigned long> g_post_material_verbose = 0;
DiagnosticAtomic<unsigned long> g_scenery_prepare_verbose = 0;
DiagnosticAtomic<unsigned long> g_scenery_setup_verbose = 0;
DiagnosticAtomic<unsigned long> g_render_scenery_builder_verbose = 0;
DiagnosticAtomic<unsigned long> g_rasterize_bucket_builder_verbose = 0;
DiagnosticAtomic<unsigned long> g_character_regression_verbose = 0;

DiagnosticAtomic<unsigned long long> g_last_summary_tick = 0;
#endif
DiagnosticAtomic<std::uintptr_t> g_last_nis_manager = 0;
DiagnosticAtomic<std::uint32_t> g_last_nis_scene_time_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_nis_scene_delta_bits = 0;
std::atomic<bool> g_hooks_initialized = false;
std::atomic<bool> g_cleanup_pending = false;
// MinHook enables each detour as it is created and this fork cannot disable a
// target without freeing its original trampoline.  Keep every mutating detour
// transparent until the complete static/hook transaction commits; a late
// failure may retain code hooks for process lifetime, but not partial behavior.
std::atomic<bool> g_behavior_transaction_ready = false;
// The bundled MinHook fork exposes no disable-only operation and frees a
// trampoline from MH_RemoveHook.  Retain all created hooks for process
// lifetime; Main.cpp pins this module, so the OS reclaims the state at process
// exit without ever invalidating an in-flight original call.
std::atomic<bool> g_minhook_retained_process_lifetime = false;
std::vector<std::uintptr_t> g_created_hook_targets;
std::mutex g_hook_lifecycle_mutex;
bool g_hook_creation_transaction_open = false;
hook_guard::Guard g_hook_target_guard;
bool g_rumble_override_attempted = false;
#if !defined(SPATCH_FINAL_RELEASE)
std::mutex g_seen_callback_mutex;
std::vector<std::uintptr_t> g_seen_callbacks;
#endif
std::mutex g_nis_actor_state_mutex;
nisprobe::Tracker g_nis_actor_state_tracker;

nisprobe::SetupResult TrackNisActorSetupLocked(std::uintptr_t actor_state,
                                               std::uintptr_t actor_target) {
    std::lock_guard<std::mutex> lock(g_nis_actor_state_mutex);
    return nisprobe::TrackSetup(g_nis_actor_state_tracker, actor_state, actor_target);
}

nisprobe::RestoreResult TrackNisActorRestoreLocked(std::uintptr_t actor_state) {
    std::lock_guard<std::mutex> lock(g_nis_actor_state_mutex);
    return nisprobe::TrackRestore(g_nis_actor_state_tracker, actor_state);
}

void ResetNisActorTrackerLocked() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_nis_actor_state_mutex);
        nisprobe::ResetTracker(g_nis_actor_state_tracker);
    } catch (...) {
        // The tracker is optional fail-open state.  A process-lifetime mutex is
        // not expected to throw, but no cleanup exception may replace the
        // original engine exception that triggered this reset.
    }
}

struct QueueSnapshot {
    int queued_total = 0;
    int completed_total = 0;
};

struct SceneryCounters {
    unsigned int c0 = 0;
    unsigned int c1 = 0;
    unsigned int c2 = 0;
    unsigned int c3 = 0;
};

enum class BuilderScope : std::uint8_t {
    None = 0,
    SceneryPrepare,
    ScenerySetup,
    RenderScenery,
    RasterizeBucket,
};

thread_local BuilderScope g_current_builder_scope = BuilderScope::None;
thread_local unsigned int g_nis_play_scope_depth = 0;
thread_local unsigned int g_nis_play_scope_calls = 0;
thread_local unsigned int g_nis_play_scope_advanced_calls = 0;
thread_local unsigned int g_nis_play_scope_zero_delta_calls = 0;
thread_local unsigned int g_nis_owner_scope_depth = 0;
thread_local unsigned int g_nis_owner_scope_calls = 0;
thread_local unsigned int g_nis_owner_scope_advanced_calls = 0;
thread_local unsigned int g_nis_owner_scope_zero_delta_calls = 0;
thread_local std::uintptr_t g_current_nis_owner_manager = 0;
thread_local unsigned int g_cutscene_flow_owner_scope_depth = 0;
thread_local unsigned int g_antialias_owner_scope_depth = 0;
thread_local bool g_cutscene_flow_forwarded = false;
thread_local float g_cutscene_flow_forwarded_dt = 0.0f;
thread_local void* g_cutscene_flow_forwarded_callback = nullptr;
thread_local float g_cutscene_flow_input_dt = 0.0f;
thread_local float g_current_cutscene_dt = 0.0f;
thread_local float g_current_nis_owner_dt = 0.0f;
thread_local std::uintptr_t g_last_nis_manager_tls = 0;
thread_local std::uintptr_t g_last_nis_instance_tls = 0;
thread_local float g_last_nis_raw_scene_time_tls = 0.0f;
thread_local float g_last_nis_applied_scene_time_tls = 0.0f;
thread_local bool g_last_nis_scene_history_valid_tls = false;
thread_local float g_last_cutscene_flow_completed_dt_tls = 0.0f;
thread_local CutsceneCadenceTracker g_cutscene_cadence_tracker_tls{};
thread_local pedestrian_timing::FixedRateScheduler g_pedestrian_throttle_scheduler_tls{};
thread_local bool g_pedestrian_update_delta_valid_tls = false;
thread_local float g_pedestrian_update_delta_tls = 0.0f;
thread_local void* g_pedestrian_manager_tls = nullptr;
thread_local std::uintptr_t g_last_cutscene_owner_tls = 0;
struct CharacterWetnessRuntimeState {
    character_wetness::State policy{};
    unsigned long long observed_water_generation = 0;
    std::uintptr_t effects_component = 0;
    std::uintptr_t sim_object = 0;
    std::uintptr_t character_look_component = 0;
    float baseline_amount = 0.0f;
    float last_written_amount = 0.0f;
    bool owns_amount = false;
};
CharacterWetnessRuntimeState g_character_wetness_state{};
std::mutex g_character_wetness_mutex;
struct CharacterSweatRuntimeState {
    character_sweat::State policy{};
    std::uintptr_t effects_component = 0;
    std::uintptr_t sim_object = 0;
    std::uintptr_t physics_component = 0;
    std::uintptr_t character_look_component = 0;
    float baseline_amount = 0.0f;
    float last_written_amount = 0.0f;
    bool owns_amount = false;
};
CharacterSweatRuntimeState g_character_sweat_state{};
std::mutex g_character_sweat_mutex;

// CharacterEffects::Update runs for every visible character, not just the
// player. Keep per-component state so an NPC's wetness/sweat timer cannot
// overwrite another character's material or inherit Wei's timer. Entries are
// bounded by inactivity pruning; all pointer reads/writes remain guarded by
// SafeRead/SafeWrite before a stale pooled component can be touched.
struct NpcCharacterEffectsRuntimeState {
    std::uintptr_t effects_component = 0;
    std::uintptr_t sim_object = 0;
    std::uintptr_t physics_component = 0;
    std::uintptr_t character_look_component = 0;
    character_wetness::State wetness_policy{};
    std::uint32_t last_active_wet_surface_uid = 0;
    float wetness_baseline_amount = 0.0f;
    float wetness_last_written_amount = 0.0f;
    bool wetness_owns_amount = false;
    character_sweat::State sweat_policy{};
    float sweat_baseline_amount = 0.0f;
    float sweat_last_written_amount = 0.0f;
    bool sweat_owns_amount = false;
    bool water_contact_pending = false;
    unsigned long long last_seen_tick = 0;
};

constexpr unsigned long long kNpcCharacterStateKeepAliveMs = 10000;
constexpr unsigned long long kNpcCharacterStatePruneIntervalMs = 1000;
constexpr std::size_t kNpcCharacterStateMaximumEntries = 512;
std::unordered_map<std::uintptr_t, NpcCharacterEffectsRuntimeState>
    g_npc_character_effects_states;
std::mutex g_npc_character_effects_mutex;
unsigned long long g_npc_character_effects_last_prune_tick = 0;

std::unordered_map<std::uintptr_t, unsigned long long> g_character_combat_until_ticks;
std::mutex g_character_combat_activity_mutex;
struct CutsceneOwnerTimingState {
    float input_dt = 0.0f;
    float current_dt = 0.0f;
    bool forwarded = false;
    float forwarded_dt = 0.0f;
    void* forwarded_callback = nullptr;
};
thread_local std::array<CutsceneOwnerTimingState, 256> g_cutscene_owner_timing_stack{};

void ResetTrackedCutsceneCadence() {
    ResetCutsceneCadenceTracker(g_cutscene_cadence_tracker_tls);
}

void ResetTrackedCutsceneTimeline() {
    ResetTrackedCutsceneCadence();
    g_last_nis_scene_history_valid_tls = false;
}

float TrackCutsceneCadence(float live_delta) {
    return TrackCutsceneBaseDelta(
        g_cutscene_cadence_tracker_tls, live_delta, g_config.cutscene_fps);
}

template <typename T>
bool SafeRead(std::uintptr_t address, T& out_value) {
    if (address == 0) {
        out_value = T{};
        return false;
    }
    __try {
        out_value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_value = T{};
        return false;
    }
}

bool SafeCopyBytes(std::uintptr_t source_address,
                   void* destination,
                   std::size_t size) noexcept {
    if (source_address == 0 || destination == nullptr || size == 0) {
        return false;
    }
    __try {
        std::memcpy(destination,
                    reinterpret_cast<const void*>(source_address),
                    size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(destination, 0, size);
        return false;
    }
}

template <typename T>
bool SafeWrite(std::uintptr_t address, const T& value) {
    if (address == 0) {
        return false;
    }
    __try {
        *reinterpret_cast<T*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AppendCharacterTraceLine(const char* stage,
                              void* health_component,
                              void* anim_component,
                              void* hitreact_component,
                              int damage,
                              bool projectile_damage,
                              bool result) {
#if defined(SPATCH_FINAL_RELEASE)
    (void)stage;
    (void)health_component;
    (void)anim_component;
    (void)hitreact_component;
    (void)damage;
    (void)projectile_damage;
    (void)result;
    return;
#else
    if (!g_use_latest_steam_layout) {
        return;
    }

    if (g_character_trace_budget.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    long budget_before = g_character_trace_budget.fetch_sub(1);
    if (budget_before <= 0) {
        g_character_trace_budget.fetch_add(1);
        return;
    }

    const std::filesystem::path trace_path =
        g_config_path.empty() ? std::filesystem::path(L"SPatch.chartrace.log")
                              : (g_config_path.parent_path() / L"SPatch.chartrace.log");

    char line[512]{};
    const int line_length = _snprintf_s(line,
                                        sizeof(line),
                                        _TRUNCATE,
                                        "%s hc=%p anim=%p hitreact=%p damage=%d projectile=%d result=%d\r\n",
                                        stage,
                                        health_component,
                                        anim_component,
                                        hitreact_component,
                                        damage,
                                        projectile_damage ? 1 : 0,
                                        result ? 1 : 0);
    if (line_length <= 0) {
        return;
    }

    HANDLE file = CreateFileW(trace_path.c_str(),
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(line_length), &written, nullptr);
    CloseHandle(file);
#endif
}

#if defined(SPATCH_FINAL_RELEASE)
#define ShouldLogVerbose(counter) false
#else
bool ShouldLogVerbose(DiagnosticAtomic<unsigned long>& counter) noexcept {
    if (g_config.max_verbose_events == 0) {
        return false;
    }
    const unsigned long current = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return current <= g_config.max_verbose_events;
}
#endif

std::uint32_t FloatToBits(float value);
#if defined(SPATCH_FINAL_RELEASE)
#define MaybeWriteSummary() ((void)0)
#else
void MaybeWriteSummary();
#endif

std::uintptr_t ReadSimObjectFromComponent(void* component) {
    if (component == nullptr) {
        return 0;
    }

    // SimComponent inherits qSafePointerNode:
    //   vptr (0x00) + qList (0x08..0x17) => base size 0x18
    // Then:
    //   m_TypeUID (0x18), m_NameUID (0x1C), m_Flags (0x20), m_SimObjIndex (0x22),
    //   padding (0x24), m_pSimObject (0x28)
    std::uintptr_t sim_object = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(component) + 0x28, sim_object);
    return sim_object;
}

std::uintptr_t ResolveSimObjectComponent(std::uintptr_t sim_object, std::uint32_t type_uid) {
    if (sim_object == 0 || g_simobject_get_component == nullptr) {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(
        g_simobject_get_component(reinterpret_cast<void*>(sim_object), type_uid));
}

#if !defined(SPATCH_FINAL_RELEASE)
bool FieldProbeRequested(const std::filesystem::path& trigger_path,
                         const wchar_t* environment_name) noexcept {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        environment_name, value, static_cast<DWORD>(std::size(value)));
    if (length != 0 && length < std::size(value)) {
        wchar_t* end = nullptr;
        if (std::wcstol(value, &end, 10) != 0 && end != value) {
            return true;
        }
    }

    std::error_code error;
    return !trigger_path.empty() && std::filesystem::is_regular_file(trigger_path, error);
}
#endif

#if !defined(SPATCH_FINAL_RELEASE)
void* DetourGameMemcpy(void* destination, const void* source, std::size_t size) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_game_memcpy_original(destination, source, size);
    }
    const std::uintptr_t watched =
        g_wetness_memcpy_watch_address.load(std::memory_order_acquire);
    const std::uintptr_t source_begin = reinterpret_cast<std::uintptr_t>(source);
    const bool contains_wetness =
        watched != 0 && watched >= source_begin && watched - source_begin < size;
    if (contains_wetness &&
        !g_wetness_memcpy_capture_claimed.load(std::memory_order_relaxed)) {
        bool expected = false;
        if (g_wetness_memcpy_capture_claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            g_wetness_memcpy_capture_return.store(
                reinterpret_cast<std::uintptr_t>(_ReturnAddress()),
                std::memory_order_relaxed);
            g_wetness_memcpy_capture_destination.store(
                reinterpret_cast<std::uintptr_t>(destination),
                std::memory_order_relaxed);
            g_wetness_memcpy_capture_source.store(source_begin, std::memory_order_relaxed);
            g_wetness_memcpy_capture_size.store(size, std::memory_order_relaxed);
            g_wetness_memcpy_capture_thread.store(GetCurrentThreadId(),
                                                  std::memory_order_relaxed);
            g_wetness_memcpy_capture_ready.store(true, std::memory_order_release);
        }
    }

    void* result = g_game_memcpy_original(destination, source, size);
    if (contains_wetness && size >= 0x40 &&
        g_force_wetness_field_probe_mode.load(std::memory_order_relaxed) == 1) {
        // The original renderer consumes wetness at float 14 (+0x38) of this
        // same 64-byte block. DE's CharacterLook bridge writes it to float 9
        // (+0x24), leaving the original material slot dry. Mirror the live
        // engine value only in the per-draw staging copy so this probe tests
        // water entry, exit, and stock drying without forcing gameplay state.
        const std::uintptr_t destination_begin = reinterpret_cast<std::uintptr_t>(destination);
        const float source_wetness = *reinterpret_cast<const float*>(destination_begin + 0x24);
        const float render_wetness =
            std::isfinite(source_wetness) ? std::clamp(source_wetness, 0.0f, 1.0f) : 0.0f;
        *reinterpret_cast<float*>(destination_begin + 0x38) = render_wetness;
    }
    return result;
}
#endif

void DetourCharacterAnimationApplyCharredEffect(void* character_animation_component, float amount) {
    g_character_charred_anim_count.fetch_add(1);
    g_last_character_damage_anim_component.store(
        reinterpret_cast<std::uintptr_t>(character_animation_component));
    g_last_character_charred_amount_bits.store(FloatToBits(amount));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_charred_anim component=0x%p amount=%.3f",
                   character_animation_component,
                   amount);
    }

    g_character_animation_apply_charred_effect_original(character_animation_component, amount);
    MaybeWriteSummary();
}

void DetourCharacterAnimationApplyPaintEffect(void* character_animation_component,
                                              bool enable,
                                              float r,
                                              float g,
                                              float b) {
    g_character_paint_anim_count.fetch_add(1);
    g_last_character_damage_anim_component.store(
        reinterpret_cast<std::uintptr_t>(character_animation_component));
    g_last_character_paint_enable.store(enable ? 1u : 0u);
    g_last_character_paint_r_bits.store(FloatToBits(r));
    g_last_character_paint_g_bits.store(FloatToBits(g));
    g_last_character_paint_b_bits.store(FloatToBits(b));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_paint_anim component=0x%p enable=%d rgb=%.3f/%.3f/%.3f",
                   character_animation_component,
                   enable ? 1 : 0,
                   r,
                   g,
                   b);
    }

    g_character_animation_apply_paint_effect_original(character_animation_component, enable, r, g, b);
    MaybeWriteSummary();
}

void DetourDamageRigApplyCharredEffect(void* damage_rig, float amount) {
    g_character_charred_rig_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));
    g_last_character_charred_amount_bits.store(FloatToBits(amount));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_charred_rig rig=0x%p amount=%.3f", damage_rig, amount);
    }

    g_damage_rig_apply_charred_effect_original(damage_rig, amount);
    MaybeWriteSummary();
}

void DetourDamageRigApplyPaintEffect(void* damage_rig, bool enable, float r, float g, float b) {
    g_character_paint_rig_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));
    g_last_character_paint_enable.store(enable ? 1u : 0u);
    g_last_character_paint_r_bits.store(FloatToBits(r));
    g_last_character_paint_g_bits.store(FloatToBits(g));
    g_last_character_paint_b_bits.store(FloatToBits(b));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_paint_rig rig=0x%p enable=%d rgb=%.3f/%.3f/%.3f",
                   damage_rig,
                   enable ? 1 : 0,
                   r,
                   g,
                   b);
    }

    g_damage_rig_apply_paint_effect_original(damage_rig, enable, r, g, b);
    MaybeWriteSummary();
}

void DetourCharacterPaintConsumer(std::uintptr_t character_effects_component,
                                  std::uintptr_t paint_block_owner) {
    g_character_paint_consumer_count.fetch_add(1);
    g_last_character_paint_consumer_component.store(character_effects_component);
    g_last_character_paint_consumer_block.store(paint_block_owner);

    const auto read_ptr = [](std::uintptr_t base, std::uintptr_t offset) -> std::uintptr_t {
        std::uintptr_t value = 0;
        if (base != 0) {
            SafeRead(base + offset, value);
        }
        return value;
    };

    bool enable = false;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    const std::uintptr_t paint_lanes = read_ptr(paint_block_owner, 0x60);
    if (paint_lanes != 0) {
        const std::uintptr_t lane0 = read_ptr(paint_lanes, 0x00);
        const std::uintptr_t lane1 = read_ptr(paint_lanes, 0x08);
        const std::uintptr_t lane2 = read_ptr(paint_lanes, 0x10);
        const std::uintptr_t lane3 = read_ptr(paint_lanes, 0x18);
        const std::uintptr_t lane0_node = read_ptr(lane0, 0x08);
        const std::uintptr_t lane1_node = read_ptr(lane1, 0x08);
        const std::uintptr_t lane2_node = read_ptr(lane2, 0x08);
        const std::uintptr_t lane3_node = read_ptr(lane3, 0x08);
        enable = read_ptr(lane0_node, 0x20) != 0;
        SafeRead(lane1_node + 0x20, r);
        SafeRead(lane2_node + 0x20, g);
        SafeRead(lane3_node + 0x20, b);
        g_last_character_paint_enable.store(enable ? 1u : 0u);
        g_last_character_paint_r_bits.store(FloatToBits(r));
        g_last_character_paint_g_bits.store(FloatToBits(g));
        g_last_character_paint_b_bits.store(FloatToBits(b));
    }

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF(
            "character_paint_consumer component=0x%p block=0x%p enable=%d rgb=%.3f/%.3f/%.3f",
            reinterpret_cast<void*>(character_effects_component),
            reinterpret_cast<void*>(paint_block_owner),
            enable ? 1 : 0,
            r,
            g,
            b);
    }

    g_character_paint_consumer_original(character_effects_component, paint_block_owner);
    MaybeWriteSummary();
}

void* DetourCharacterEffectDispatchOwner() {
    g_character_effect_dispatch_owner_count.fetch_add(1);
    void* dispatch_owner = g_character_effect_dispatch_owner_original();
    g_last_character_effect_dispatch_owner.store(reinterpret_cast<std::uintptr_t>(dispatch_owner));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_effect_dispatch_owner owner=0x%p", dispatch_owner);
    }

    return dispatch_owner;
}

void DetourCharacterPaintOwner(std::uintptr_t owner, std::uintptr_t character_effects_component) {
    g_character_paint_owner_count.fetch_add(1);
    g_last_character_paint_owner_ptr.store(owner);
    g_last_character_paint_owner_component.store(character_effects_component);

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_paint_owner owner=0x%p component=0x%p",
                   reinterpret_cast<void*>(owner),
                   reinterpret_cast<void*>(character_effects_component));
    }

    g_character_paint_owner_original(owner, character_effects_component);
    MaybeWriteSummary();
}

void DetourCharacterEffectDispatchConsume(void* dispatch_owner, void* character_effects_component) {
    g_character_effect_dispatch_consume_count.fetch_add(1);
    g_last_character_effect_dispatch_owner.store(reinterpret_cast<std::uintptr_t>(dispatch_owner));
    g_last_character_effect_dispatch_component.store(
        reinterpret_cast<std::uintptr_t>(character_effects_component));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_effect_dispatch_consume owner=0x%p component=0x%p",
                   dispatch_owner,
                   character_effects_component);
    }

    g_character_effect_dispatch_consume_original(dispatch_owner, character_effects_component);
    MaybeWriteSummary();
}

void DetourCharacterEffectQueueBuilder(std::uintptr_t owner,
                                       std::uintptr_t character_effects_component,
                                       std::uint32_t mode) {
    g_character_effect_queue_builder_count.fetch_add(1);
    g_last_character_effect_queue_builder_owner.store(owner);
    g_last_character_effect_queue_builder_component.store(character_effects_component);
    g_last_character_effect_queue_builder_mode.store(mode);

    const std::uintptr_t tracked_wet_component = g_last_character_wet_component.load();
    const bool is_tracked_wet_component =
        character_effects_component != 0 && tracked_wet_component != 0 &&
        character_effects_component == tracked_wet_component;
    if (is_tracked_wet_component) {
        g_character_effect_queue_builder_tracked_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose) &&
        (is_tracked_wet_component || g_character_effect_queue_builder_count.load() <= 4)) {
        log::InfoF("character_effect_queue_builder owner=0x%p component=0x%p mode=%u tracked=%u",
                   reinterpret_cast<void*>(owner),
                   reinterpret_cast<void*>(character_effects_component),
                   mode,
                   is_tracked_wet_component ? 1u : 0u);
    }

    g_character_effect_queue_builder_original(owner, character_effects_component, mode);
    MaybeWriteSummary();
}

std::uintptr_t RemapRvaForCurrentBuild(std::uintptr_t rva) {
    if (!g_use_latest_steam_layout) {
        return rva;
    }

    switch (rva) {
        case kTaskReadyRva:
            return kLatestSteamTaskReadyRva;
        case kTaskDispatchRva:
            return kLatestSteamTaskDispatchRva;
        case kNisSetPlayTimeRva:
            return kLatestSteamNisSetPlayTimeRva;
        case kNisOwnerRva:
            return kLatestSteamNisOwnerRva;
        case kFrameFlowRva:
            return kLatestSteamFrameFlowRva;
        case kCutsceneFlowOwnerRva:
            return kLatestSteamCutsceneFlowOwnerRva;
        case fog_restoration::kLegacySetterRva:
            return fog_restoration::kLatestSteamSetterRva;
        case kFogSlicingModeRva:
            return kLatestSteamFogSlicingModeRva;
        case kPresentBufferRva:
            return kLatestSteamPresentBufferRva;
        case kAverageWindowInitializeRva:
            return kLatestSteamAverageWindowInitializeRva;
        case kMaterialOnLoadRva:
            return kLatestSteamMaterialOnLoadRva;
        case kUiIsGamePausedRva:
            return kLatestSteamUiIsGamePausedRva;
        case kHealthApplyDamageRva:
            return kLatestSteamHealthApplyDamageRva;
        case kSimObjectGetComponentRva:
            return kLatestSteamSimObjectGetComponentRva;
        case kTimeOfDayAccessorRva:
            return kLatestSteamTimeOfDayAccessorRva;
        case kCharacterHandleWaterCollisionRva:
            return kLatestSteamCharacterHandleWaterCollisionRva;
        case kCharacterEffectsUpdateRva:
            return kLatestSteamCharacterEffectsUpdateRva;
        case kCharacterEffectDispatchConsumeRva:
            return kLatestSteamCharacterEffectDispatchConsumeRva;
        case kCharacterEffectQueueBuilderRva:
            return kLatestSteamCharacterEffectQueueBuilderRva;
        case kDamageRigApplyPaintEffectRva:
            return kLatestSteamDamageRigApplyPaintEffectRva;
        case kCharacterDamageRigResetDamageRva:
            return kLatestSteamCharacterDamageRigResetDamageRva;
        case kPedestrianFrameRateThrottleRva:
            return kLatestSteamPedestrianFrameRateThrottleRva;
        case kPedestrianSpawnUpdateRva:
            return kLatestSteamPedestrianSpawnUpdateRva;
        case kPcFileReadRva:
            return kLatestSteamPcFileReadRva;
        case kPcFileSeekRva:
            return kLatestSteamPcFileSeekRva;
        case kPcFileTellRva:
            return kLatestSteamPcFileTellRva;
        case kPcFileSizeRva:
            return kLatestSteamPcFileSizeRva;
        case kQFileReadAtRva:
            return kLatestSteamQFileReadAtRva;
        case kQFileWriteAtRva:
            return kLatestSteamQFileWriteAtRva;
        case kQFileReadyRva:
            return kLatestSteamQFileReadyRva;
        case kQcmpDecompressRva:
            return kLatestSteamQcmpDecompressRva;
        case kStreamFileOpenRva:
            return kLatestSteamStreamFileOpenRva;
        case kStreamFileCloseRva:
            return kLatestSteamStreamFileCloseRva;
        case kResourceChunkDispatchRva:
            return kLatestSteamResourceChunkDispatchRva;
        default:
            return rva;
        }
}

std::uintptr_t ResolveAddress(std::uintptr_t rva) noexcept {
    const std::uintptr_t mapped_rva = RemapRvaForCurrentBuild(rva);
    if (g_module_base == 0 || g_module_end <= g_module_base ||
        mapped_rva >= g_module_end - g_module_base) {
        return 0;
    }
    return g_module_base + mapped_rva;
}

std::uint32_t FloatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float Clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

float ScalePercent(float value, int percent) {
    return value * (static_cast<float>(percent) / 100.0f);
}

bool NearlyEqual(float a, float b, float epsilon = 0.0005f) {
    return std::fabs(a - b) <= epsilon;
}

std::uintptr_t SafeResolveSimObjectComponent(std::uintptr_t sim_object,
                                             std::uint32_t type_uid) noexcept {
    std::uintptr_t component = 0;
    __try {
        component = ResolveSimObjectComponent(sim_object, type_uid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        component = 0;
    }
    return component;
}

bool IsValidCharacterWetnessAmount(float amount) noexcept {
    return std::isfinite(amount) && amount >= -1.0f && amount <= 1.0f;
}

void RestoreOwnedCharacterWetness(CharacterWetnessRuntimeState& state) {
    if (!state.owns_amount || state.character_look_component == 0) {
        state.owns_amount = false;
        return;
    }

    float current = 0.0f;
    if (SafeRead(state.character_look_component + 0xB8, current) &&
        NearlyEqual(current, state.last_written_amount, 0.002f)) {
        SafeWrite(state.character_look_component + 0xB8, state.baseline_amount);
    }
    state.owns_amount = false;
}

void ApplyCharacterWetnessRestore(void* character_effects_component,
                                  float delta_seconds,
                                  float weather_state,
                                  float weather_surface_wetness,
                                  float override_surface_wetness) {
    const std::uintptr_t effects_component =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    if (!g_config.restore_character_wetness || effects_component == 0) {
        return;
    }

    const std::uintptr_t sim_object = ReadSimObjectFromComponent(character_effects_component);
    if (sim_object == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_character_wetness_mutex);
    CharacterWetnessRuntimeState& state = g_character_wetness_state;
    const character_wetness::Timing timing{
        static_cast<float>(g_config.wetness_full_time_seconds),
        static_cast<float>(g_config.wetness_fade_time_seconds)};

    const unsigned long long water_generation =
        g_player_water_collision_generation.load(std::memory_order_acquire);
    if (water_generation != state.observed_water_generation) {
        state.observed_water_generation = water_generation;
        if (g_player_water_collision_component.load(std::memory_order_acquire) ==
            effects_component) {
            character_wetness::MarkWaterCollision(state.policy, timing);
        }
    }

    const bool raining = std::isfinite(weather_state) && weather_state > 1.0f;
    const float effective_surface_wetness =
        IsValidCharacterWetnessAmount(override_surface_wetness) &&
                override_surface_wetness >= 0.0f
            ? override_surface_wetness
            : weather_surface_wetness;
    const character_wetness::StepResult step = character_wetness::Advance(
        state.policy, delta_seconds, raining, effective_surface_wetness, timing);

    if (state.effects_component != effects_component || state.sim_object != sim_object) {
        RestoreOwnedCharacterWetness(state);
        state.effects_component = effects_component;
        state.sim_object = sim_object;
        state.character_look_component = 0;
    }

    if (state.character_look_component == 0) {
        state.character_look_component =
            SafeResolveSimObjectComponent(sim_object, 0xCC000001u);
    }
    const std::uintptr_t character_look = state.character_look_component;
    if (character_look == 0) {
        return;
    }

    std::uint32_t component_type_uid = 0;
    std::uintptr_t component_sim_object = 0;
    float current = 0.0f;
    if (!SafeRead(character_look + 0x18, component_type_uid) ||
        component_type_uid != 0xCC000001u ||
        !SafeRead(character_look + 0x28, component_sim_object) ||
        component_sim_object != sim_object || !SafeRead(character_look + 0xB8, current) ||
        !IsValidCharacterWetnessAmount(current)) {
        RestoreOwnedCharacterWetness(state);
        state.character_look_component = 0;
        return;
    }

    if (!step.active) {
        RestoreOwnedCharacterWetness(state);
        return;
    }

    const float desired = step.amount;
    if (state.owns_amount && character_wetness::ShouldYieldToStrongerOwner(
                                 current, state.last_written_amount, desired)) {
        // CharacterLook's stock update writes zero again every frame in DE.
        // Keep the physical water state authoritative over a lower value, but
        // yield to a stronger scripted/rain value and reacquire naturally if
        // that owner later releases the field while water is still active.
        state.owns_amount = false;
    }

    if (!state.owns_amount) {
        if (current + 0.0005f >= desired) {
            return;
        }
        state.baseline_amount = current;
    } else if (NearlyEqual(current, desired)) {
        return;
    }

    if (!SafeWrite(character_look + 0xB8, desired)) {
        state.owns_amount = false;
        return;
    }

    state.last_written_amount = desired;
    state.owns_amount = true;
    g_character_wet_force_count.fetch_add(1);
    float verified = 0.0f;
    if (SafeRead(character_look + 0xB8, verified) && NearlyEqual(verified, desired)) {
        g_character_wet_force_verify_count.fetch_add(1);
    }
}

bool IsValidCharacterSweatAmount(float amount) noexcept {
    return std::isfinite(amount) && amount >= -1.0f && amount <= 1.0f;
}

void RestoreOwnedCharacterSweat(CharacterSweatRuntimeState& state) {
    if (state.owns_amount && state.character_look_component != 0) {
        float current = 0.0f;
        if (SafeRead(state.character_look_component + 0xBC, current) &&
            NearlyEqual(current, state.last_written_amount, 0.002f)) {
            SafeWrite(state.character_look_component + 0xBC, state.baseline_amount);
        }
    }
    state = {};
    g_character_sweat_active_component.store(0, std::memory_order_release);
}

// Release only the material ownership while retaining the component identity
// and exertion timer.  The sweat policy has an onset phase during which its
// amount is intentionally zero; clearing the whole runtime state here would
// restart that timer every CharacterEffects::Update and make sweat impossible
// to trigger at the configured onset.
void ReleaseOwnedCharacterSweat(CharacterSweatRuntimeState& state) {
    if (state.owns_amount && state.character_look_component != 0) {
        float current = 0.0f;
        if (SafeRead(state.character_look_component + 0xBC, current) &&
            NearlyEqual(current, state.last_written_amount, 0.002f)) {
            SafeWrite(state.character_look_component + 0xBC, state.baseline_amount);
        }
    }
    state.owns_amount = false;
    state.last_written_amount = 0.0f;
}

void RestoreOwnedNpcCharacterWetness(NpcCharacterEffectsRuntimeState& state) {
    if (!state.wetness_owns_amount || state.character_look_component == 0) {
        state.wetness_owns_amount = false;
        state.wetness_policy = {};
        return;
    }

    float current = 0.0f;
    if (SafeRead(state.character_look_component + 0xB8, current) &&
        NearlyEqual(current, state.wetness_last_written_amount, 0.002f)) {
        SafeWrite(state.character_look_component + 0xB8, state.wetness_baseline_amount);
    }
    state.wetness_owns_amount = false;
    state.wetness_policy = {};
    state.wetness_baseline_amount = 0.0f;
    state.wetness_last_written_amount = 0.0f;
}

void RestoreOwnedNpcCharacterSweat(NpcCharacterEffectsRuntimeState& state) {
    if (!state.sweat_owns_amount || state.character_look_component == 0) {
        state.sweat_owns_amount = false;
        state.sweat_policy = {};
        return;
    }

    float current = 0.0f;
    if (SafeRead(state.character_look_component + 0xBC, current) &&
        NearlyEqual(current, state.sweat_last_written_amount, 0.002f)) {
        SafeWrite(state.character_look_component + 0xBC, state.sweat_baseline_amount);
    }
    state.sweat_owns_amount = false;
    state.sweat_policy = {};
    state.sweat_baseline_amount = 0.0f;
    state.sweat_last_written_amount = 0.0f;
}

// See ReleaseOwnedCharacterSweat.  Keep the per-NPC onset/exertion timer
// alive when the policy is still dry, otherwise every inactive step would
// erase the very state needed to cross the onset threshold.
void ReleaseOwnedNpcCharacterSweat(NpcCharacterEffectsRuntimeState& state) {
    if (state.sweat_owns_amount && state.character_look_component != 0) {
        float current = 0.0f;
        if (SafeRead(state.character_look_component + 0xBC, current) &&
            NearlyEqual(current, state.sweat_last_written_amount, 0.002f)) {
            SafeWrite(state.character_look_component + 0xBC,
                      state.sweat_baseline_amount);
        }
    }
    state.sweat_owns_amount = false;
    state.sweat_last_written_amount = 0.0f;
}

void ResetNpcCharacterEffectsState(NpcCharacterEffectsRuntimeState& state) {
    RestoreOwnedNpcCharacterWetness(state);
    RestoreOwnedNpcCharacterSweat(state);
    state = {};
}

void PruneNpcCharacterEffectsStatesLocked(unsigned long long now) {
    if (now < g_npc_character_effects_last_prune_tick ||
        now - g_npc_character_effects_last_prune_tick <
            kNpcCharacterStatePruneIntervalMs) {
        return;
    }
    g_npc_character_effects_last_prune_tick = now;
    for (auto it = g_npc_character_effects_states.begin();
         it != g_npc_character_effects_states.end();) {
        const auto& state = it->second;
        const bool stale = state.last_seen_tick == 0 ||
                           (now >= state.last_seen_tick &&
                            now - state.last_seen_tick >
                                kNpcCharacterStateKeepAliveMs);
        if (!stale) {
            ++it;
            continue;
        }
        ResetNpcCharacterEffectsState(it->second);
        it = g_npc_character_effects_states.erase(it);
    }
    while (g_npc_character_effects_states.size() > kNpcCharacterStateMaximumEntries) {
        auto oldest = g_npc_character_effects_states.end();
        for (auto it = g_npc_character_effects_states.begin();
             it != g_npc_character_effects_states.end();
             ++it) {
            if (oldest == g_npc_character_effects_states.end() ||
                it->second.last_seen_tick < oldest->second.last_seen_tick) {
                oldest = it;
            }
        }
        if (oldest == g_npc_character_effects_states.end()) {
            break;
        }
        ResetNpcCharacterEffectsState(oldest->second);
        g_npc_character_effects_states.erase(oldest);
    }
}

NpcCharacterEffectsRuntimeState* GetNpcCharacterEffectsStateLocked(
    std::uintptr_t effects_component,
    std::uintptr_t sim_object,
    unsigned long long now) {
    if (effects_component == 0 || sim_object == 0) {
        return nullptr;
    }

    try {
        auto [it, inserted] = g_npc_character_effects_states.try_emplace(effects_component);
        NpcCharacterEffectsRuntimeState& state = it->second;
        if (inserted || state.effects_component != effects_component ||
            state.sim_object != sim_object) {
            const bool water_contact_pending = state.water_contact_pending;
            if (!inserted) {
                ResetNpcCharacterEffectsState(state);
            }
            state.effects_component = effects_component;
            state.sim_object = sim_object;
            state.water_contact_pending = water_contact_pending;
        }
        state.last_seen_tick = now;
        return &state;
    } catch (...) {
        // NPCs are an optional enhancement. If the process cannot grow the
        // bounded cache, leave the stock path untouched for this update.
        return nullptr;
    }
}

void MarkNpcCharacterWaterContact(std::uintptr_t effects_component) noexcept {
    if (effects_component == 0) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        PruneNpcCharacterEffectsStatesLocked(now);
        NpcCharacterEffectsRuntimeState* state = nullptr;
        const auto existing = g_npc_character_effects_states.find(effects_component);
        if (existing != g_npc_character_effects_states.end()) {
            // The water callback does not carry a sim-object pointer. Preserve
            // the real identity resolved by an earlier effects update instead
            // of replacing it with the pre-update sentinel.
            existing->second.last_seen_tick = now;
            state = &existing->second;
        } else {
            state = GetNpcCharacterEffectsStateLocked(effects_component, 1, now);
        }
        if (state != nullptr) {
            // A water callback can arrive before the first effects update. A
            // sentinel sim object is replaced by the real one on that update.
            state->water_contact_pending = true;
        }
    } catch (...) {
        // Fail open if allocation/locking infrastructure is unavailable.
    }
}

void MarkCharacterCombatActivity(std::uintptr_t sim_object) noexcept {
    if (sim_object == 0 || g_config.sweat_combat_time_seconds <= 0) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    const unsigned long long duration =
        static_cast<unsigned long long>(g_config.sweat_combat_time_seconds) * 1000ULL;
    const unsigned long long until =
        now > (std::numeric_limits<unsigned long long>::max() - duration)
            ? std::numeric_limits<unsigned long long>::max()
            : now + duration;
    try {
        std::lock_guard<std::mutex> lock(g_character_combat_activity_mutex);
        auto& current = g_character_combat_until_ticks[sim_object];
        current = std::max(current, until);
        for (auto it = g_character_combat_until_ticks.begin();
             it != g_character_combat_until_ticks.end();) {
            if (it->second < now) {
                it = g_character_combat_until_ticks.erase(it);
            } else {
                ++it;
            }
        }
        while (g_character_combat_until_ticks.size() > kNpcCharacterStateMaximumEntries) {
            auto oldest = g_character_combat_until_ticks.begin();
            for (auto it = g_character_combat_until_ticks.begin();
                 it != g_character_combat_until_ticks.end();
                 ++it) {
                if (it->second < oldest->second) {
                    oldest = it;
                }
            }
            g_character_combat_until_ticks.erase(oldest);
        }
    } catch (...) {
        // Combat-driven sweat is optional; never affect the stock damage path.
    }
}

bool IsCharacterCombatActive(std::uintptr_t sim_object,
                             unsigned long long now) noexcept {
    if (sim_object == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(g_character_combat_activity_mutex);
        const auto it = g_character_combat_until_ticks.find(sim_object);
        if (it == g_character_combat_until_ticks.end()) {
            return false;
        }
        if (it->second < now) {
            g_character_combat_until_ticks.erase(it);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void ResetNpcCharacterEffectsStates() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        for (auto& [component, state] : g_npc_character_effects_states) {
            (void)component;
            ResetNpcCharacterEffectsState(state);
        }
        g_npc_character_effects_states.clear();
        g_npc_character_effects_last_prune_tick = 0;
    } catch (...) {
        // Teardown must not replace the original shutdown path with an
        // exception from optional NPC bookkeeping.
    }
    try {
        std::lock_guard<std::mutex> lock(g_character_combat_activity_mutex);
        g_character_combat_until_ticks.clear();
    } catch (...) {
    }
}

bool ReadPlayerHorizontalSpeedFromComponent(std::uintptr_t physics_component,
                                            float& speed) noexcept {
    speed = 0.0f;
    if (physics_component == 0 || g_character_physics_get_velocity == nullptr) {
        return false;
    }

    float velocity[3]{};
    bool call_succeeded = false;
    __try {
        call_succeeded = g_character_physics_get_velocity(
                             reinterpret_cast<void*>(physics_component), velocity) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        call_succeeded = false;
    }
    if (!call_succeeded || !std::isfinite(velocity[0]) || !std::isfinite(velocity[1]) ||
        !std::isfinite(velocity[2])) {
        return false;
    }

    const float horizontal_speed = std::sqrt((velocity[0] * velocity[0]) +
                                             (velocity[1] * velocity[1]));
    if (!std::isfinite(horizontal_speed) || horizontal_speed > 1000.0f) {
        return false;
    }
    speed = horizontal_speed;
    return true;
}

bool ReadPlayerHorizontalSpeed(std::uintptr_t sim_object, float& speed) noexcept {
    if (sim_object == 0) {
        speed = 0.0f;
        return false;
    }
    return ReadPlayerHorizontalSpeedFromComponent(
        SafeResolveSimObjectComponent(sim_object, 0x7A000001u), speed);
}

bool ReadPlayerCombatState(bool& in_combat) noexcept {
    in_combat = false;
    if (g_ui_is_player_in_combat == nullptr) {
        return false;
    }

    bool call_succeeded = false;
    __try {
        in_combat = g_ui_is_player_in_combat();
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        in_combat = false;
        call_succeeded = false;
    }
    return call_succeeded;
}

float ReadCharacterSweatPolicyAmount() {
    std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
    return g_character_sweat_state.policy.amount;
}

#if !defined(SPATCH_FINAL_RELEASE)
float ReadNpcCharacterSweatPolicyAmount(std::uintptr_t effects_component) {
    if (effects_component == 0) {
        return 0.0f;
    }
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        const auto it = g_npc_character_effects_states.find(effects_component);
        return it == g_npc_character_effects_states.end()
                   ? 0.0f
                   : it->second.sweat_policy.amount;
    } catch (...) {
        return 0.0f;
    }
}
#endif

character_sweat::Timing ConfiguredCharacterSweatTiming() noexcept;

void ApplyCharacterSweatRestore(void* character_effects_component,
                                float delta_seconds,
                                bool is_attached_to_player) {
    if (!g_config.restore_character_sweat) {
        std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
        RestoreOwnedCharacterSweat(g_character_sweat_state);
        return;
    }
    if (character_effects_component == nullptr) {
        return;
    }

    const std::uintptr_t effects_component =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    if (!is_attached_to_player) {
        if (g_character_sweat_active_component.load(std::memory_order_acquire) !=
            effects_component) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
        if (g_character_sweat_state.effects_component == effects_component) {
            RestoreOwnedCharacterSweat(g_character_sweat_state);
        }
        return;
    }
    const std::uintptr_t sim_object = ReadSimObjectFromComponent(character_effects_component);
    if (sim_object == 0) {
        std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
        RestoreOwnedCharacterSweat(g_character_sweat_state);
        return;
    }

    std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
    CharacterSweatRuntimeState& state = g_character_sweat_state;
    if (state.effects_component != effects_component || state.sim_object != sim_object) {
        RestoreOwnedCharacterSweat(state);
        state.effects_component = effects_component;
        state.sim_object = sim_object;
        state.physics_component =
            SafeResolveSimObjectComponent(sim_object, 0x7A000001u);
        g_character_sweat_active_component.store(effects_component, std::memory_order_release);
    }
    if (state.physics_component == 0) {
        state.physics_component =
            SafeResolveSimObjectComponent(sim_object, 0x7A000001u);
    }

    if (state.character_look_component == 0) {
        state.character_look_component =
            SafeResolveSimObjectComponent(sim_object, 0xCC000001u);
    }
    const std::uintptr_t character_look = state.character_look_component;
    if (character_look == 0) {
        return;
    }

    std::uint32_t component_type_uid = 0;
    std::uintptr_t component_sim_object = 0;
    float current = 0.0f;
    if (!SafeRead(character_look + 0x18, component_type_uid) ||
        component_type_uid != 0xCC000001u || !SafeRead(character_look + 0x28, component_sim_object) ||
        component_sim_object != sim_object || !SafeRead(character_look + 0xBC, current) ||
        !IsValidCharacterSweatAmount(current)) {
        state.character_look_component = 0;
        return;
    }

    // A value that changed without being written by this policy belongs to a
    // stock/scripted owner.  Preserve it and let the policy reacquire only
    // after that owner falls back to its baseline.
    if (state.owns_amount &&
        !NearlyEqual(current, state.last_written_amount, 0.002f) &&
        current > state.last_written_amount) {
        state.owns_amount = false;
        state.policy.amount = std::max(state.policy.amount, current);
    }

    float horizontal_speed = 0.0f;
    bool in_combat = false;
    const bool velocity_valid =
        ReadPlayerHorizontalSpeedFromComponent(state.physics_component, horizontal_speed);
    const bool combat_valid = ReadPlayerCombatState(in_combat);
    if (!velocity_valid && !combat_valid) {
        // Never write a guessed value when the build-specific inputs are not
        // available.  This is important for the un-mapped later Steam image.
        return;
    }

    const bool exerting = (velocity_valid &&
                           character_sweat::IsRunning(horizontal_speed,
                                                       g_config.sweat_run_speed)) ||
                          (combat_valid && in_combat);
    const character_sweat::StepResult step =
        character_sweat::Advance(state.policy,
                                 delta_seconds,
                                 exerting,
                                 ConfiguredCharacterSweatTiming());

    if (!state.owns_amount && current > step.amount + 0.002f) {
        state.policy.amount = std::max(state.policy.amount, current);
        return;
    }
    if (!step.active) {
        ReleaseOwnedCharacterSweat(state);
        return;
    }

    if (state.owns_amount && NearlyEqual(current, step.amount, 0.002f)) {
        return;
    }
    if (!state.owns_amount) {
        state.baseline_amount = current;
    }
    if (!SafeWrite(character_look + 0xBC, step.amount)) {
        state.owns_amount = false;
        return;
    }

    state.last_written_amount = step.amount;
    state.owns_amount = true;
}

character_sweat::Timing ConfiguredCharacterSweatTiming() noexcept {
    return character_sweat::Timing{
        static_cast<float>(g_config.sweat_build_time_seconds),
        static_cast<float>(g_config.sweat_fade_time_seconds),
        static_cast<float>(g_config.sweat_onset_time_seconds)};
}

void AdvanceNpcWetnessLocked(NpcCharacterEffectsRuntimeState& state,
                             std::uintptr_t character_look,
                             float delta_seconds,
                             bool water_contact,
                             std::uint32_t active_wet_surface_uid,
                             float weather_state,
                             float weather_surface_wetness,
                             float override_surface_wetness) {
    const character_wetness::Timing timing{
        static_cast<float>(g_config.wetness_full_time_seconds),
        static_cast<float>(g_config.wetness_fade_time_seconds)};
    const bool surface_contact = active_wet_surface_uid != 0 &&
                                 active_wet_surface_uid !=
                                     state.last_active_wet_surface_uid;
    state.last_active_wet_surface_uid = active_wet_surface_uid;
    if (water_contact || surface_contact) {
        character_wetness::MarkWaterCollision(state.wetness_policy, timing);
        state.water_contact_pending = false;
    }

    const bool raining = std::isfinite(weather_state) && weather_state > 1.0f;
    const float effective_surface_wetness =
        IsValidCharacterWetnessAmount(override_surface_wetness) &&
                override_surface_wetness >= 0.0f
            ? override_surface_wetness
            : weather_surface_wetness;
    const character_wetness::StepResult step = character_wetness::Advance(
        state.wetness_policy,
        delta_seconds,
        raining,
        effective_surface_wetness,
        timing);
    if (!step.active) {
        RestoreOwnedNpcCharacterWetness(state);
        return;
    }

    float current = 0.0f;
    if (!SafeRead(character_look + 0xB8, current) ||
        !IsValidCharacterWetnessAmount(current)) {
        RestoreOwnedNpcCharacterWetness(state);
        return;
    }

    const float desired = step.amount;
    if (state.wetness_owns_amount && character_wetness::ShouldYieldToStrongerOwner(
                                  current,
                                  state.wetness_last_written_amount,
                                  desired)) {
        state.wetness_owns_amount = false;
    }
    if (!state.wetness_owns_amount && current > desired + 0.002f) {
        state.wetness_policy.amount = std::max(state.wetness_policy.amount, current);
        return;
    }
    if ((state.wetness_owns_amount && NearlyEqual(current, desired)) ||
        (!state.wetness_owns_amount && current + 0.0005f >= desired)) {
        return;
    }
    if (!state.wetness_owns_amount) {
        state.wetness_baseline_amount = current;
    }
    if (!SafeWrite(character_look + 0xB8, desired)) {
        state.wetness_owns_amount = false;
        return;
    }
    state.wetness_last_written_amount = desired;
    state.wetness_owns_amount = true;
}

void AdvanceNpcSweatLocked(NpcCharacterEffectsRuntimeState& state,
                           std::uintptr_t character_look,
                           float delta_seconds,
                           unsigned long long now) {
    float horizontal_speed = 0.0f;
    const bool velocity_valid =
        ReadPlayerHorizontalSpeedFromComponent(state.physics_component, horizontal_speed);
    const bool combat_active = IsCharacterCombatActive(state.sim_object, now);
    if (!velocity_valid && !combat_active) {
        // A missing build-specific input must never turn into a guessed write.
        return;
    }

    const bool exerting = (velocity_valid && character_sweat::IsRunning(
                                             horizontal_speed,
                                             g_config.sweat_run_speed)) ||
                          combat_active;
    const character_sweat::StepResult step = character_sweat::Advance(
        state.sweat_policy, delta_seconds, exerting, ConfiguredCharacterSweatTiming());

    float current = 0.0f;
    if (!SafeRead(character_look + 0xBC, current) ||
        !IsValidCharacterSweatAmount(current)) {
        RestoreOwnedNpcCharacterSweat(state);
        return;
    }
    if (state.sweat_owns_amount && current > state.sweat_last_written_amount + 0.002f) {
        state.sweat_owns_amount = false;
        state.sweat_policy.amount = std::max(state.sweat_policy.amount, current);
    }
    if (!state.sweat_owns_amount && current > step.amount + 0.002f) {
        state.sweat_policy.amount = std::max(state.sweat_policy.amount, current);
        return;
    }
    if (!step.active) {
        ReleaseOwnedNpcCharacterSweat(state);
        return;
    }
    if (state.sweat_owns_amount && NearlyEqual(current, step.amount, 0.002f)) {
        return;
    }
    if (!state.sweat_owns_amount) {
        state.sweat_baseline_amount = current;
    }
    if (!SafeWrite(character_look + 0xBC, step.amount)) {
        state.sweat_owns_amount = false;
        return;
    }
    state.sweat_last_written_amount = step.amount;
    state.sweat_owns_amount = true;
}

void ApplyNpcCharacterEffectsRestore(void* character_effects_component,
                                     float delta_seconds,
                                     std::uint32_t active_wet_surface_uid,
                                     float weather_state,
                                     float weather_surface_wetness,
                                     float override_surface_wetness) {
    if (character_effects_component == nullptr ||
        (!g_config.restore_character_wetness && !g_config.restore_character_sweat)) {
        return;
    }
    const std::uintptr_t effects_component =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    const std::uintptr_t sim_object = ReadSimObjectFromComponent(character_effects_component);
    if (sim_object == 0) {
        return;
    }

    const unsigned long long now = GetTickCount64();
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        // Pruning may erase map entries, so do it before retaining a pointer to
        // the state selected for this update.
        PruneNpcCharacterEffectsStatesLocked(now);
        NpcCharacterEffectsRuntimeState* state =
            GetNpcCharacterEffectsStateLocked(effects_component, sim_object, now);
        if (state == nullptr) {
            return;
        }

        if (g_config.restore_character_wetness && state->water_contact_pending) {
            // The water callback may precede this update by several frames;
            // consume it exactly once and let the policy own the fade timer.
            state->water_contact_pending = false;
            character_wetness::MarkWaterCollision(
                state->wetness_policy,
                character_wetness::Timing{
                    static_cast<float>(g_config.wetness_full_time_seconds),
                    static_cast<float>(g_config.wetness_fade_time_seconds)});
        }

        if (state->character_look_component == 0) {
            state->character_look_component =
                SafeResolveSimObjectComponent(sim_object, 0xCC000001u);
        }
        if (state->character_look_component == 0) {
            return;
        }

        std::uint32_t component_type_uid = 0;
        std::uintptr_t component_sim_object = 0;
        if (!SafeRead(state->character_look_component + 0x18, component_type_uid) ||
            component_type_uid != 0xCC000001u ||
            !SafeRead(state->character_look_component + 0x28, component_sim_object) ||
            component_sim_object != sim_object) {
            RestoreOwnedNpcCharacterWetness(*state);
            RestoreOwnedNpcCharacterSweat(*state);
            state->character_look_component = 0;
            return;
        }

        if (g_config.restore_character_wetness) {
            AdvanceNpcWetnessLocked(*state,
                                    state->character_look_component,
                                    delta_seconds,
                                    false,
                                    active_wet_surface_uid,
                                    weather_state,
                                    weather_surface_wetness,
                                    override_surface_wetness);
        }
        if (g_config.restore_character_sweat) {
            if (state->physics_component == 0) {
                state->physics_component =
                    SafeResolveSimObjectComponent(sim_object, 0x7A000001u);
            }
            AdvanceNpcSweatLocked(*state,
                                  state->character_look_component,
                                  delta_seconds,
                                  now);
        }
    } catch (...) {
        // The NPC path is supplemental. Preserve stock behavior if an
        // allocation or synchronization failure occurs in the cache.
    }
}

template <typename T>
T* ReadGlobalPointer(std::uintptr_t address) {
    T* value = nullptr;
    SafeRead(address, value);
    return value;
}

float ReadVector3Magnitude(const void* vector_ptr) {
    const auto address = reinterpret_cast<std::uintptr_t>(vector_ptr);
    if (address == 0) {
        return 0.0f;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!SafeRead(address + 0x0, x) || !SafeRead(address + 0x4, y) || !SafeRead(address + 0x8, z)) {
        return 0.0f;
    }

    return std::sqrt((x * x) + (y * y) + (z * z));
}

bool IsApprox60HzDelta(float value) {
    return IsApprox60HzCutsceneDelta(value);
}

const char* ClassifySmallDelta(float delta) {
    if (delta >= 0.0f && delta < 0.0005f) {
        return "zero";
    }
    if (delta >= 0.030f && delta <= 0.036f) {
        return "approx_30hz";
    }
    if (delta >= 0.015f && delta <= 0.019f) {
        return "approx_60hz";
    }
    if (delta > 0.0f && delta < 0.2f) {
        return "other_small";
    }
    return "reset_or_large";
}

std::uintptr_t ToRva(std::uintptr_t address) {
    if (g_module_base == 0 || g_module_end <= g_module_base || address < g_module_base ||
        address >= g_module_end) {
        return 0;
    }

    return address - g_module_base;
}

std::uintptr_t ReadPointerSlot(std::uintptr_t slot_address) {
    std::uintptr_t value = 0;
    if (slot_address != 0) {
        SafeRead(slot_address, value);
    }
    return value;
}

void PersistSmaaConfig() {
    if (g_config_path.empty()) {
        return;
    }

    WriteBoolValue(g_config_path, L"smaa_enable", smaa::GetEnabled());
}

void MaybeApplyRumbleOverride() {
    if (g_rumble_override_attempted || g_config.override_rumble_enabled < 0) {
        return;
    }

    g_rumble_override_attempted = true;

    if (g_rumble_apply_helper == nullptr || g_rumble_apply_object == 0) {
        log::Warn("rumble_override skipped because helper/object was unresolved");
        return;
    }

    const bool enabled = g_config.override_rumble_enabled != 0;
    g_rumble_apply_helper(g_rumble_apply_object, enabled);
    g_rumble_override_apply_count.fetch_add(1);
    g_last_rumble_override_value.store(enabled ? 1 : 0);
    log::InfoF("rumble_override_applied enabled=%d object=0x%p helper=0x%p",
               enabled ? 1 : 0,
               reinterpret_cast<void*>(g_rumble_apply_object),
               reinterpret_cast<void*>(g_rumble_apply_helper));
}

CutscenePauseState ReadCutscenePauseStateSafely() {
    CutscenePauseState pause_state{};

    __try {
        if (g_ui_is_game_paused != nullptr && g_ui_is_game_paused()) {
            pause_state.ui_paused = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    if (g_simtime_is_paused != nullptr) {
        __try {
            if (g_simtime_is_paused()) {
                pause_state.simtime_paused = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    return pause_state;
}

bool IsGamePausedSafely() {
    const CutscenePauseState pause_state = ReadCutscenePauseStateSafely();
    return pause_state.ui_paused || pause_state.simtime_paused;
}

void PersistAaConfig() {
    if (g_config_path.empty()) {
        return;
    }

    WriteIntValue(g_config_path, L"aa_variant_mode", g_aa_runtime_variant_mode.load());
    WriteIntValue(g_config_path, L"aa_aux_mode", g_aa_runtime_aux_mode.load());
}

int ClampAaVariantMode(int mode) {
    if (mode < 0) {
        return 0;
    }
    if (mode > 2) {
        return 2;
    }
    return mode;
}

int ClampAaAuxMode(int mode) {
    if (mode < 0) {
        return 0;
    }
    if (mode > 2) {
        return 2;
    }
    return mode;
}

void SetAaVariantMode(int mode) {
    mode = ClampAaVariantMode(mode);
    const int previous = g_aa_runtime_variant_mode.exchange(mode);
    if (previous == mode) {
        return;
    }

    PersistAaConfig();
    log::InfoF("aa_variant mode=%d", mode);
}

void SetAaAuxMode(int mode) {
    mode = ClampAaAuxMode(mode);
    const int previous = g_aa_runtime_aux_mode.exchange(mode);
    if (previous == mode) {
        return;
    }

    PersistAaConfig();
    log::InfoF("aa_aux mode=%d", mode);
}

int ReadOptionSymbolValue(std::uintptr_t address) {
    int value = 0;
    if (address != 0) {
        SafeRead(address, value);
    }
    return value;
}

const char* NormalizeTextLookupToken(const char* token) {
    if (token == nullptr) {
        return nullptr;
    }
    return token[0] == '$' ? token + 1 : token;
}

bool TextLookupTokenEquals(const char* token, const char* expected) {
    const char* normalized = NormalizeTextLookupToken(token);
    if (normalized == nullptr || expected == nullptr) {
        return false;
    }
    return std::strcmp(normalized, expected) == 0;
}

bool ShouldProbeTextLookupToken(const char* token) {
    const char* normalized = NormalizeTextLookupToken(token);
    if (normalized == nullptr) {
        return false;
    }
    return std::strstr(normalized, "OPTIONS_QUALITY_AA") != nullptr ||
           std::strstr(normalized, "FXAA") != nullptr ||
           std::strstr(normalized, "AA") != nullptr;
}

bool ConsumeHotkeyPress(int virtual_key,
                        DiagnosticAtomic<unsigned long long>& last_tick,
                        unsigned long debounce_ms) {
    if ((GetAsyncKeyState(virtual_key) & 1) == 0) {
        return false;
    }

    const unsigned long long now = GetTickCount64();
    const unsigned long long last = last_tick.load();
    if (now - last < debounce_ms) {
        return false;
    }

    last_tick.store(now);
    return true;
}

void PollGraphicsDebugKeys() {
    if (!g_config.aa_variant_debug_keys && !g_config.aa_aux_debug_keys &&
        !g_config.smaa_debug_keys) {
        return;
    }

    if (g_config.smaa_debug_keys && smaa::GetDebugKeysEnabled()) {
        if (ConsumeHotkeyPress(VK_F2, g_last_smaa_toggle_key_tick, 200)) {
            const bool enabled = !smaa::GetEnabled();
            smaa::SetEnabled(enabled);
            PersistSmaaConfig();
            log::InfoF("smaa_toggle enabled=%d preset=%d", enabled ? 1 : 0, g_config.smaa_preset);
        }
    }

    if (g_config.aa_variant_debug_keys) {
        if (ConsumeHotkeyPress(VK_F7, g_last_aa_variant_up_key_tick, 150)) {
            int mode = g_aa_runtime_variant_mode.load();
            mode += 1;
            if (mode > 2) {
                mode = 2;
            }
            SetAaVariantMode(mode);
        }

        if (ConsumeHotkeyPress(VK_F6, g_last_aa_variant_down_key_tick, 150)) {
            int mode = g_aa_runtime_variant_mode.load();
            mode -= 1;
            if (mode < 0) {
                mode = 0;
            }
            SetAaVariantMode(mode);
        }
    }

    if (g_config.aa_aux_debug_keys) {
        if (ConsumeHotkeyPress(VK_F5, g_last_aa_aux_up_key_tick, 150)) {
            int mode = g_aa_runtime_aux_mode.load();
            mode += 1;
            if (mode > 2) {
                mode = 2;
            }
            SetAaAuxMode(mode);
        }

        if (ConsumeHotkeyPress(VK_F4, g_last_aa_aux_down_key_tick, 150)) {
            int mode = g_aa_runtime_aux_mode.load();
            mode -= 1;
            if (mode < 0) {
                mode = 0;
            }
            SetAaAuxMode(mode);
        }
    }
}

std::uintptr_t ReadProviderValue() {
    std::uintptr_t provider = 0;
    if (g_provider_slot != 0) {
        SafeRead(g_provider_slot, provider);
    }
    return provider;
}

int ReadCutsceneFlowStage(std::uintptr_t owner) {
    int stage = 0;
    if (owner != 0) {
        SafeRead(owner + 0x6C, stage);
    }
    return stage;
}

unsigned int ReadCutsceneFlowFlags(std::uintptr_t owner) {
    unsigned int flags = 0;
    if (owner != 0) {
        SafeRead(owner + 0x68, flags);
    }
    return flags;
}

int ReadTwitchState(std::uintptr_t owner) {
    int state = 0;
    if (owner != 0) {
        SafeRead(owner + 0x750, state);
    }
    return state;
}

std::uintptr_t ReadTwitchHandle(std::uintptr_t owner) {
    std::uintptr_t handle = 0;
    if (owner != 0) {
        SafeRead(owner + 0x50, handle);
    }
    return handle;
}

unsigned int ReadTwitchFlagByte(std::uintptr_t owner, std::uintptr_t offset) {
    unsigned char value = 0;
    if (owner != 0) {
        SafeRead(owner + offset, value);
    }
    return value;
}

std::uintptr_t ReadRenderContextInstance() {
    std::uintptr_t render_context = 0;
    if (g_render_context_instance_slot != 0) {
        SafeRead(g_render_context_instance_slot, render_context);
    }
    return render_context;
}

std::uintptr_t ReadTimeOfDayManagerInstance() {
    if (g_time_of_day_accessor == nullptr) {
        return 0;
    }
    return g_time_of_day_accessor();
}

std::uintptr_t ReadPointerField(std::uintptr_t base, std::uintptr_t offset) {
    std::uintptr_t value = 0;
    if (base != 0) {
        SafeRead(base + offset, value);
    }
    return value;
}

unsigned int ReadU32Field(std::uintptr_t base, std::uintptr_t offset) {
    unsigned int value = 0;
    if (base != 0) {
        SafeRead(base + offset, value);
    }
    return value;
}

std::uintptr_t ReadRenderContextMaterial(std::uintptr_t render_context, std::uintptr_t offset) {
    std::uintptr_t material = 0;
    if (render_context != 0) {
        SafeRead(render_context + offset, material);
    }
    return material;
}

QueueSnapshot ReadQueueSnapshot() {
    QueueSnapshot snapshot{};
    if (g_render_task_manager != 0) {
        SafeRead(g_render_task_manager + 0x218, snapshot.queued_total);
        SafeRead(g_render_task_manager + 0x21C, snapshot.completed_total);
    }
    return snapshot;
}

SceneryCounters ReadSceneryCounters() {
    SceneryCounters counters{};
    if (g_scenery_counter0 != 0) {
        SafeRead(g_scenery_counter0, counters.c0);
    }
    if (g_scenery_counter1 != 0) {
        SafeRead(g_scenery_counter1, counters.c1);
    }
    if (g_scenery_counter2 != 0) {
        SafeRead(g_scenery_counter2, counters.c2);
    }
    if (g_scenery_counter3 != 0) {
        SafeRead(g_scenery_counter3, counters.c3);
    }
    return counters;
}

void StoreSceneryCounters(const SceneryCounters& counters) {
    g_last_scenery_counter0.store(counters.c0);
    g_last_scenery_counter1.store(counters.c1);
    g_last_scenery_counter2.store(counters.c2);
    g_last_scenery_counter3.store(counters.c3);
}

unsigned long long ReadReadyCounterForScope(BuilderScope scope) {
    switch (scope) {
    case BuilderScope::SceneryPrepare:
        return g_scenery_prepare_ready_total.load();
    case BuilderScope::ScenerySetup:
        return g_scenery_setup_ready_total.load();
    case BuilderScope::RenderScenery:
        return g_render_scenery_ready_total.load();
    case BuilderScope::RasterizeBucket:
        return g_rasterize_bucket_ready_total.load();
    default:
        return 0;
    }
}

void AddReadyCounterForScope(BuilderScope scope) {
    switch (scope) {
    case BuilderScope::SceneryPrepare:
        g_scenery_prepare_ready_total.fetch_add(1);
        break;
    case BuilderScope::ScenerySetup:
        g_scenery_setup_ready_total.fetch_add(1);
        break;
    case BuilderScope::RenderScenery:
        g_render_scenery_ready_total.fetch_add(1);
        break;
    case BuilderScope::RasterizeBucket:
        g_rasterize_bucket_ready_total.fetch_add(1);
        break;
    default:
        break;
    }
}

struct BuilderScopeGuard {
    explicit BuilderScopeGuard(BuilderScope scope) : previous(g_current_builder_scope) {
        g_current_builder_scope = scope;
    }

    ~BuilderScopeGuard() {
        g_current_builder_scope = previous;
    }

    BuilderScope previous;
};

std::string ReadCString(std::uintptr_t address, std::size_t max_length = 96) {
    if (address == 0 || max_length == 0) {
        return {};
    }

    try {
        std::string text;
        text.reserve(max_length);

        for (std::size_t index = 0; index < max_length; ++index) {
            if (address > (std::numeric_limits<std::uintptr_t>::max)() - index) {
                break;
            }
            char value = '\0';
            if (!SafeRead(address + index, value) || value == '\0') {
                break;
            }

            const unsigned char code = static_cast<unsigned char>(value);
            if (code < 0x20 || code > 0x7E) {
                break;
            }

            text.push_back(value);
        }
        return text;
    } catch (...) {
        // Names are diagnostic-only and may be read from malformed game
        // metadata. Never let a diagnostic allocation failure escape a hook.
        return {};
    }
}

std::uintptr_t PeekQueuedTaskHeader(std::uintptr_t manager) {
    if (manager == 0) {
        return 0;
    }

    std::uintptr_t node = 0;
    if (!SafeRead(manager + 0x210, node) || node == 0 || node == manager + 0x208) {
        return 0;
    }

    return node - 0x10;
}

std::uintptr_t ReadTaskCallback(std::uintptr_t task_header) {
    std::uintptr_t callback = 0;
    if (task_header != 0) {
        SafeRead(task_header + 0x58, callback);
    }
    return callback;
}

#if !defined(SPATCH_FINAL_RELEASE)
bool IsInterestingCallback(std::uintptr_t callback) {
    if (callback == 0 || callback == static_cast<std::uintptr_t>(-1) ||
        callback == static_cast<std::uintptr_t>(-2)) {
        return false;
    }

    return g_module_base != 0 && g_module_end > g_module_base && callback >= g_module_base &&
           callback < g_module_end;
}

bool RecordSeenCallback(std::uintptr_t callback) {
    if (g_config.max_unique_callbacks == 0 || !IsInterestingCallback(callback)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_seen_callback_mutex);
    for (const std::uintptr_t seen : g_seen_callbacks) {
        if (seen == callback) {
            return false;
        }
    }

    if (g_seen_callbacks.size() >= g_config.max_unique_callbacks) {
        return false;
    }

    try {
        g_seen_callbacks.push_back(callback);
    } catch (...) {
        // Callback discovery is optional telemetry. Allocation failure must
        // not cross a game worker detour or turn a diagnostic setting into a
        // process crash.
        return false;
    }
    g_unique_callback_count.fetch_add(1);
    return true;
}
#endif

struct ChaseParameterProbeSnapshot {
    std::uintptr_t address = 0;
    std::uintptr_t name_pointer = 0;
    std::uint32_t name_symbol = 0;
    std::uint32_t context = (std::numeric_limits<std::uint32_t>::max)();
    float yaw_ang_vel_timespan = 0.0f;
    float look_offset_max = 0.0f;
    float look_offset_ang_vel_min = 0.0f;
    float look_offset_ang_vel_max = 0.0f;
    float pitch_offset_max = 0.0f;
    float pitch_offset_look_factor_up = 0.0f;
    float pitch_offset_look_factor_down = 0.0f;
    float pitch_offset_eye_factor_up = 0.0f;
    float pitch_offset_eye_factor_down = 0.0f;
    float centering_speed_min = 0.0f;
    float centering_speed_max = 0.0f;
    float reverse_speed = 0.0f;
    float orbit_speed = 0.0f;
    bool readable = false;
};

ChaseParameterProbeSnapshot ReadChaseParameterProbeSnapshot(
    std::uintptr_t address) {
    ChaseParameterProbeSnapshot snapshot{};
    snapshot.address = address;
    if (address == 0) {
        return snapshot;
    }

    ChaseParameterProbeSnapshot candidate{};
    candidate.address = address;
    candidate.readable =
        SafeRead(address + vehicle_camera::kParameterNamePointerOffset,
                 candidate.name_pointer) &&
        SafeRead(address + vehicle_camera::kParameterNameSymbolOffset,
                 candidate.name_symbol) &&
        SafeRead(address + vehicle_camera::kParameterContextOffset,
                 candidate.context) &&
        SafeRead(address +
                     vehicle_camera::kParameterYawAngVelTimespanOffset,
                 candidate.yaw_ang_vel_timespan) &&
        SafeRead(address + vehicle_camera::kParameterLookOffsetMaxOffset,
                 candidate.look_offset_max) &&
        SafeRead(address +
                     vehicle_camera::kParameterLookOffsetAngVelMinOffset,
                 candidate.look_offset_ang_vel_min) &&
        SafeRead(address +
                     vehicle_camera::kParameterLookOffsetAngVelMaxOffset,
                 candidate.look_offset_ang_vel_max) &&
        SafeRead(address + vehicle_camera::kParameterPitchOffsetMaxOffset,
                 candidate.pitch_offset_max) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetLookFactorUpOffset,
                 candidate.pitch_offset_look_factor_up) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetLookFactorDownOffset,
                 candidate.pitch_offset_look_factor_down) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetEyeFactorUpOffset,
                 candidate.pitch_offset_eye_factor_up) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetEyeFactorDownOffset,
                 candidate.pitch_offset_eye_factor_down) &&
        SafeRead(address + vehicle_camera::kParameterCenteringSpeedMinOffset,
                 candidate.centering_speed_min) &&
        SafeRead(address + vehicle_camera::kParameterCenteringSpeedMaxOffset,
                 candidate.centering_speed_max) &&
        SafeRead(address + vehicle_camera::kParameterReverseSpeedOffset,
                 candidate.reverse_speed) &&
        SafeRead(address + vehicle_camera::kParameterOrbitSpeedOffset,
                 candidate.orbit_speed);
    return candidate.readable ? candidate : snapshot;
}

bool ReadCameraParameterIdentity(
    std::uintptr_t address,
    vehicle_camera::ParameterIdentity& identity) noexcept {
    identity = {};
    return address != 0 &&
           SafeRead(address + vehicle_camera::kParameterNamePointerOffset,
                    identity.name_pointer) &&
           SafeRead(address + vehicle_camera::kParameterNameSymbolOffset,
                    identity.name_symbol) &&
           SafeRead(address + vehicle_camera::kParameterContextOffset,
                    identity.context);
}

bool ClaimCameraProbeLogBudget() noexcept {
    unsigned int remaining =
        g_gtaiv_camera_probe_log_budget.load(std::memory_order_relaxed);
    while (remaining != 0) {
        if (g_gtaiv_camera_probe_log_budget.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool ClaimCameraProbeSample() noexcept {
    if (g_gtaiv_camera_probe_log_budget.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    constexpr unsigned long long kSampleIntervalMilliseconds = 100;
    const unsigned long long now = GetTickCount64();
    unsigned long long next =
        g_gtaiv_camera_probe_next_sample_tick.load(std::memory_order_relaxed);
    while (now >= next) {
        if (g_gtaiv_camera_probe_next_sample_tick.compare_exchange_weak(
                next,
                now + kSampleIntervalMilliseconds,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

struct DriverSideCameraDecision {
    bool policy_evaluated = false;
    bool base_drive_branch_readable = false;
    bool base_drive_branch_selected = false;
    std::uintptr_t subject = 0;
    std::uintptr_t physics_mover = 0;
    vehicle_camera::VehicleClass vehicle_class =
        vehicle_camera::VehicleClass::Unknown;
    vehicle_camera::DriverSideTargetProfile target_profile =
        vehicle_camera::DriverSideTargetProfile::None;
    vehicle_camera::ParameterSlotMask target_slot_match_mask = 0;
    vehicle_camera::ParameterSlotMask selected_slot_match_mask = 0;
    float blend_factor = 0.0f;
};

struct ChaseCameraDynamicSnapshot {
    bool readable = false;
    unsigned char is_looking_around = 0;
    unsigned char looking_back = 0;
    unsigned char aim_or_focus = 0;
    unsigned char eye_locked = 0;
    unsigned char look_locked = 0;
    float lookaround_joy_input = 0.0f;
    float lookup_joy_input = 0.0f;
    float lookup_mouse = 0.0f;
    float lookaround_center_timer = 0.0f;
    float lookaround_angle = 0.0f;
    float lookaround_angle_desired = 0.0f;
    float centering_speed_min = 0.0f;
    float centering_speed_max = 0.0f;
    float reverse_speed = 0.0f;
    float orbit_speed = 0.0f;
    float yaw_ang_vel_timespan = 0.0f;
    float look_offset_max = 0.0f;
    float look_offset_ang_vel_min = 0.0f;
    float look_offset_ang_vel_max = 0.0f;
    float pitch_offset_max = 0.0f;
    float pitch_offset_look_factor_up = 0.0f;
    float pitch_offset_look_factor_down = 0.0f;
    float pitch_offset_eye_factor_up = 0.0f;
    float pitch_offset_eye_factor_down = 0.0f;
    float forward_angle = 0.0f;
    float forward_angle_desired = 0.0f;
    float target_pitch_position = 0.0f;
    float target_pitch_target = 0.0f;
    float yaw_ang_vel_running_sum = 0.0f;
    float yaw_ang_vel_running_time = 0.0f;
    std::uintptr_t subject = 0;
    std::uintptr_t physics_mover = 0;
    std::uintptr_t physics_mover_vtable_rva = 0;
    vehicle_camera::VehicleClass vehicle_class =
        vehicle_camera::VehicleClass::Unknown;
    std::uintptr_t target_parameters = 0;
    vehicle_camera::ParameterSlotMask target_slot_match_mask = 0;
    vehicle_camera::DriverSideTargetProfile target_profile =
        vehicle_camera::DriverSideTargetProfile::None;
    std::uint32_t target_context =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t target_symbol = 0;
    float transition_source_weight = 0.0f;
    bool transition_source_weight_valid = false;
    bool profile_readable = false;
    bool base_drive_branch_readable = false;
    bool base_drive_branch_selected = false;
    float forward_speed = 0.0f;
    bool forward_speed_readable = false;
    float steering_input = 0.0f;
    std::uint32_t input_flags = 0;
    bool input_state_readable = false;
};

bool TryReadBaseDriveBranchSelected(std::uintptr_t subject,
                                    bool& selected) noexcept {
    using namespace vehicle_camera;

    selected = false;
    if (subject == 0) {
        return false;
    }

    unsigned char flee_active = 0;
    if (!SafeRead(subject + kSelectorFleeStateOffset, flee_active)) {
        return false;
    }
    if (flee_active != 0) {
        return true;
    }

    std::uintptr_t drive_object = 0;
    std::uintptr_t hijack_object = 0;
    if (!SafeRead(subject + kSelectorDriveObjectOffset, drive_object) ||
        !SafeRead(subject + kSelectorHijackObjectOffset, hijack_object)) {
        return false;
    }
    if (drive_object == 0 || hijack_object != 0) {
        return true;
    }

    std::uintptr_t drive_object_state = 0;
    if (!SafeRead(drive_object + kSelectorDriveObjectStateOffset,
                  drive_object_state)) {
        return false;
    }
    if (drive_object_state == 0) {
        return true;
    }

    std::uintptr_t race_object = 0;
    if (!SafeRead(subject + kSelectorRaceObjectOffset, race_object)) {
        return false;
    }
    std::uintptr_t race_object_state = 0;
    if (race_object != 0 &&
        !SafeRead(race_object + kSelectorRaceObjectStateOffset,
                  race_object_state)) {
        return false;
    }

    selected = IsBaseDriveBranchSelected(BaseDriveBranchInputs{
        false,
        true,
        true,
        false,
        race_object != 0,
        race_object_state != 0,
    });
    return true;
}

bool ReadChaseCameraDynamicSnapshot(
    void* component,
    ChaseCameraDynamicSnapshot& snapshot) noexcept {
    using namespace vehicle_camera;

    snapshot = {};
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(component);
    if (address == 0) {
        return false;
    }

    ChaseCameraDynamicSnapshot candidate{};
    if (!SafeRead(address + kSubjectOffset, candidate.subject) ||
        candidate.subject == 0) {
        return false;
    }
    candidate.readable =
        SafeRead(address + kIsLookingAroundFlagOffset,
                 candidate.is_looking_around) &&
        SafeRead(address + kLookaroundJoyInputOffset,
                 candidate.lookaround_joy_input) &&
        SafeRead(address + kLookupJoyInputOffset,
                 candidate.lookup_joy_input) &&
        SafeRead(address + kLookupMouseOffset, candidate.lookup_mouse) &&
        SafeRead(address + kLookaroundCenterTimerOffset,
                 candidate.lookaround_center_timer) &&
        SafeRead(address + kLookaroundAngleOffset,
                 candidate.lookaround_angle) &&
        SafeRead(address + kLookaroundAngleDesiredOffset,
                 candidate.lookaround_angle_desired) &&
        SafeRead(address + kCenteringSpeedMinComponentOffset,
                 candidate.centering_speed_min) &&
        SafeRead(address + kCenteringSpeedMaxComponentOffset,
                 candidate.centering_speed_max) &&
        SafeRead(address + kReverseSpeedComponentOffset,
                 candidate.reverse_speed) &&
        SafeRead(address + kOrbitSpeedComponentOffset,
                 candidate.orbit_speed) &&
        SafeRead(address + kYawAngVelTimespanComponentOffset,
                 candidate.yaw_ang_vel_timespan) &&
        SafeRead(address + kLookOffsetMaxComponentOffset,
                 candidate.look_offset_max) &&
        SafeRead(address + kLookOffsetAngVelMinComponentOffset,
                 candidate.look_offset_ang_vel_min) &&
        SafeRead(address + kLookOffsetAngVelMaxComponentOffset,
                 candidate.look_offset_ang_vel_max) &&
        SafeRead(address + kPitchOffsetMaxComponentOffset,
                 candidate.pitch_offset_max) &&
        SafeRead(address + kPitchOffsetLookFactorUpComponentOffset,
                 candidate.pitch_offset_look_factor_up) &&
        SafeRead(address + kPitchOffsetLookFactorDownComponentOffset,
                 candidate.pitch_offset_look_factor_down) &&
        SafeRead(address + kPitchOffsetEyeFactorUpComponentOffset,
                 candidate.pitch_offset_eye_factor_up) &&
        SafeRead(address + kPitchOffsetEyeFactorDownComponentOffset,
                 candidate.pitch_offset_eye_factor_down) &&
        SafeRead(address + kForwardAngleOffset, candidate.forward_angle) &&
        SafeRead(address + kForwardAngleDesiredOffset,
                 candidate.forward_angle_desired) &&
        SafeRead(address + kTargetPitchPositionOffset,
                 candidate.target_pitch_position) &&
        SafeRead(address + kTargetPitchTargetOffset,
                 candidate.target_pitch_target) &&
        SafeRead(address + kYawAngVelRunningSumOffset,
                 candidate.yaw_ang_vel_running_sum) &&
        SafeRead(address + kYawAngVelRunningTimeOffset,
                 candidate.yaw_ang_vel_running_time) &&
        SafeRead(address + kLookingBackFlagOffset, candidate.looking_back) &&
        SafeRead(address + kAimOrFocusFlagOffset, candidate.aim_or_focus) &&
        SafeRead(address + kEyeLockFlagOffset, candidate.eye_locked) &&
        SafeRead(address + kLookLockFlagOffset, candidate.look_locked);
    if (!candidate.readable) {
        return false;
    }

    candidate.is_looking_around =
        candidate.is_looking_around != 0 ? 1 : 0;
    candidate.looking_back = candidate.looking_back != 0 ? 1 : 0;
    candidate.aim_or_focus = candidate.aim_or_focus != 0 ? 1 : 0;
    candidate.eye_locked = candidate.eye_locked != 0 ? 1 : 0;
    candidate.look_locked = candidate.look_locked != 0 ? 1 : 0;

    candidate.base_drive_branch_readable =
            TryReadBaseDriveBranchSelected(
                candidate.subject,
                candidate.base_drive_branch_selected);

    if (SafeRead(candidate.subject + kPhysicsMoverOffset,
                 candidate.physics_mover) &&
        candidate.physics_mover != 0) {
        std::uintptr_t physics_mover_vtable = 0;
        if (SafeRead(candidate.physics_mover, physics_mover_vtable)) {
            candidate.physics_mover_vtable_rva =
                ToRva(physics_mover_vtable);
            candidate.vehicle_class = ClassifyVehicle(
                candidate.physics_mover_vtable_rva,
                g_use_latest_steam_layout);
        }
        candidate.forward_speed_readable = SafeRead(
            candidate.physics_mover + kPhysicsMoverForwardSpeedOffset,
            candidate.forward_speed);
        candidate.input_state_readable =
            SafeRead(candidate.physics_mover +
                         kPhysicsMoverSteeringInputOffset,
                     candidate.steering_input) &&
            SafeRead(candidate.physics_mover +
                         kPhysicsMoverInputFlagsOffset,
                     candidate.input_flags);
    }

    float transition_source_weight = 0.0f;
    if (SafeRead(address + kTransitionSourceWeightOffset,
                 transition_source_weight) &&
        IsValidTransitionSourceWeight(transition_source_weight)) {
        candidate.transition_source_weight = transition_source_weight;
        candidate.transition_source_weight_valid = true;
    }

    std::uintptr_t target_parameters = 0;
    if (SafeRead(address + kTargetParametersOffset, target_parameters) &&
        target_parameters != 0) {
        std::array<std::uintptr_t, kParameterSlotOffsets.size()> slots{};
        bool all_slots_read = true;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (!SafeRead(candidate.subject + kParameterSlotOffsets[index],
                          slots[index])) {
                all_slots_read = false;
                break;
            }
        }
        ParameterIdentity identity{};
        if (all_slots_read &&
            ReadCameraParameterIdentity(target_parameters,
                                        identity)) {
            candidate.target_parameters = target_parameters;
            candidate.target_slot_match_mask = ProfileSlotMatchMask(
                target_parameters, slots);
            candidate.target_profile =
                ClassifyDriverSideTargetProfileFromMatchMask(
                    candidate.vehicle_class,
                    target_parameters,
                    candidate.target_slot_match_mask,
                    slots,
                    identity,
                    candidate.base_drive_branch_selected);
            candidate.target_context = identity.context;
            candidate.target_symbol = identity.name_symbol;
            candidate.profile_readable = true;
        }
    }

    std::uintptr_t verified_subject = 0;
    if (!SafeRead(address + kSubjectOffset, verified_subject) ||
        verified_subject != candidate.subject) {
        return false;
    }
    snapshot = candidate;
    return true;
}

bool IsGtaIvVehicleCameraDynamicDecisionEnabled(
    const DriverSideCameraDecision& decision) noexcept {
    using namespace vehicle_camera;

    if (!decision.policy_evaluated || decision.subject == 0 ||
        decision.physics_mover == 0) {
        return false;
    }
    switch (decision.target_profile) {
        case DriverSideTargetProfile::RoadDrive:
        case DriverSideTargetProfile::RoadFlee:
            return g_config.gta_iv_car_camera &&
                   decision.vehicle_class == VehicleClass::CarPhysicsMover;
        case DriverSideTargetProfile::MotorcycleDriveBlock:
            return g_config.gta_iv_bike_camera &&
                   decision.vehicle_class == VehicleClass::Motorcycle;
        default:
            return false;
    }
}

bool TryReadNativeVehicleCameraOverride(void* component,
                                        bool& has_override) noexcept {
    has_override = false;
    if (component == nullptr) {
        return false;
    }
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(component);
    unsigned char looking_back = 0;
    unsigned char aim_or_focus = 0;
    unsigned char eye_locked = 0;
    unsigned char look_locked = 0;
    if (!SafeRead(address + vehicle_camera::kLookingBackFlagOffset,
                  looking_back) ||
        !SafeRead(address + vehicle_camera::kAimOrFocusFlagOffset,
                  aim_or_focus) ||
        !SafeRead(address + vehicle_camera::kEyeLockFlagOffset, eye_locked) ||
        !SafeRead(address + vehicle_camera::kLookLockFlagOffset, look_locked)) {
        return false;
    }
    has_override = looking_back != 0 || aim_or_focus != 0 ||
                   eye_locked != 0 || look_locked != 0;
    return true;
}

float ResolveObservedYawAngularVelocity(
    const ChaseCameraDynamicSnapshot& snapshot) noexcept {
    if (!snapshot.readable ||
        !std::isfinite(snapshot.yaw_ang_vel_running_sum) ||
        !std::isfinite(snapshot.yaw_ang_vel_running_time) ||
        snapshot.yaw_ang_vel_running_time <= 0.0f) {
        return 0.0f;
    }
    const float value = snapshot.yaw_ang_vel_running_sum /
                        snapshot.yaw_ang_vel_running_time;
    return std::isfinite(value) ? value : 0.0f;
}

struct ChaseCameraUpdateProbeScope {
    void* component = nullptr;
    float delta_seconds = 0.0f;
    unsigned long thread_id = 0;
    unsigned int depth = 0;
    vehicle_camera::DynamicProbeCallCounts calls{};
    const void* selected_parameters = nullptr;
    DriverSideCameraDecision decision{};
    bool capture_sample = false;
    bool stock_mouse_eye_offset_readable = false;
    float stock_mouse_eye_offset = 0.0f;
    bool mutation_applied = false;
    bool dynamics_mutation_applied = false;
    float manual_pitch_offset_radians = 0.0f;
    bool desired_pose_readable = false;
    CameraVector3 desired_eye{};
    CameraVector3 desired_look{};
    CameraVector3 desired_up{};
    CameraVector3 effective_desired_eye{};
    CameraVector3 effective_desired_look{};
    CameraVector3 effective_desired_up{};
    bool collide = false;
    ChaseCameraDynamicSnapshot before{};
};

inline constexpr std::size_t kChaseCameraUpdateProbeMaxDepth = 8;
thread_local std::array<ChaseCameraUpdateProbeScope,
                        kChaseCameraUpdateProbeMaxDepth>
    g_chase_camera_update_probe_stack{};
thread_local unsigned int g_chase_camera_update_probe_depth = 0;

struct GtaIvVehicleCameraDynamicState {
    void* component = nullptr;
    std::uintptr_t subject = 0;
    vehicle_camera::ManualPitchState manual_pitch{};
};

thread_local GtaIvVehicleCameraDynamicState
    g_gtaiv_vehicle_camera_dynamic_state{};

void ResetGtaIvVehicleCameraDynamicState() noexcept {
    g_gtaiv_vehicle_camera_dynamic_state = {};
}

void ResetGtaIvVehicleCameraDynamicStateForComponent(
    void* component) noexcept {
    if (g_gtaiv_vehicle_camera_dynamic_state.component == component) {
        ResetGtaIvVehicleCameraDynamicState();
    }
}

ChaseCameraUpdateProbeScope* FindActiveChaseCameraUpdateProbeScope(
    void* component) noexcept {
    for (unsigned int index = g_chase_camera_update_probe_depth;
         index != 0;
         --index) {
        ChaseCameraUpdateProbeScope& scope =
            g_chase_camera_update_probe_stack[index - 1];
        if (scope.component == component) {
            return &scope;
        }
    }
    return nullptr;
}

__declspec(noinline) bool DetourAngularApproach(float* current,
                                                float target,
                                                float rate,
                                                float delta_seconds) {
    if (g_angular_approach_original == nullptr) {
        return false;
    }

    const auto invoke_native = [&]() noexcept {
        return g_angular_approach_original(
            current, target, rate, delta_seconds);
    };
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        (!g_config.gta_iv_car_camera && !g_config.gta_iv_bike_camera) ||
        current == nullptr || !std::isfinite(rate) ||
        !std::isfinite(delta_seconds) || delta_seconds <= 0.0f) {
        return invoke_native();
    }

    const vehicle_camera::DynamicAddressProfile addresses =
        vehicle_camera::SelectDynamicAddresses(g_use_latest_steam_layout);
    const std::uintptr_t return_rva = ToRva(
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    std::size_t field_offset = 0;
    enum class AngularPath : unsigned char {
        Native,
        FollowYaw,
        ManualYawRecenter,
    } path = AngularPath::Native;
    if (return_rva == addresses.follow_yaw_return_rva) {
        path = AngularPath::FollowYaw;
        field_offset = vehicle_camera::kForwardAngleOffset;
    } else if (return_rva == addresses.manual_yaw_recenter_return_rva) {
        path = AngularPath::ManualYawRecenter;
        field_offset = vehicle_camera::kLookaroundAngleOffset;
    } else {
        return invoke_native();
    }

    const std::uintptr_t current_address =
        reinterpret_cast<std::uintptr_t>(current);
    if (current_address < field_offset) {
        return invoke_native();
    }
    void* component = reinterpret_cast<void*>(current_address - field_offset);
    ChaseCameraUpdateProbeScope* scope =
        FindActiveChaseCameraUpdateProbeScope(component);
    bool has_native_override = false;
    if (scope == nullptr ||
        !IsGtaIvVehicleCameraDynamicDecisionEnabled(scope->decision) ||
        !TryReadNativeVehicleCameraOverride(component, has_native_override) ||
        has_native_override) {
        return invoke_native();
    }

    std::uint32_t input_flags = 0;
    if (!SafeRead(scope->decision.physics_mover +
                      vehicle_camera::kPhysicsMoverInputFlagsOffset,
                  input_flags)) {
        return invoke_native();
    }
    const bool reversing =
        vehicle_camera::IsPhysicsMoverReversing(input_flags);
    if (reversing) {
        return invoke_native();
    }
    if (path == AngularPath::FollowYaw) {
        const bool handbrake =
            (input_flags &
             vehicle_camera::kPhysicsMoverHandbrakeFlag) != 0;
        const float adjusted_rate =
            vehicle_camera::ResolveGtaIvFollowRate(rate, handbrake);
        if (adjusted_rate != rate) {
            scope->dynamics_mutation_applied = true;
        }
        return g_angular_approach_original(
            current, target, adjusted_rate, delta_seconds);
    }

    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(component);
    float horizontal_input = 0.0f;
    float center_timer = 0.0f;
    if (!SafeRead(component_address +
                      vehicle_camera::kLookaroundJoyInputOffset,
                  horizontal_input) ||
        !SafeRead(component_address +
                      vehicle_camera::kLookaroundCenterTimerOffset,
                  center_timer)) {
        return invoke_native();
    }
    if (vehicle_camera::ShouldHoldGtaIvManualYaw(
            horizontal_input,
            center_timer,
            reversing)) {
        scope->dynamics_mutation_applied = true;
        return false;
    }

    const float adjusted_rate =
        vehicle_camera::ResolveGtaIvManualYawRecenterRate(rate);
    if (adjusted_rate != rate) {
        scope->dynamics_mutation_applied = true;
    }
    return g_angular_approach_original(
        current, target, adjusted_rate, delta_seconds);
}

bool ClaimDynamicCameraProbeSample() noexcept {
    if (g_gtaiv_camera_dynamic_probe_log_budget.load(
            std::memory_order_relaxed) == 0) {
        return false;
    }
    constexpr unsigned long long kSampleIntervalMilliseconds = 100;
    const unsigned long long now = GetTickCount64();
    unsigned long long next =
        g_gtaiv_camera_dynamic_probe_next_sample_tick.load(
            std::memory_order_relaxed);
    while (now >= next) {
        if (g_gtaiv_camera_dynamic_probe_next_sample_tick.compare_exchange_weak(
                next,
                now + kSampleIntervalMilliseconds,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool ClaimDynamicCameraProbeLogBudget() noexcept {
    unsigned int remaining =
        g_gtaiv_camera_dynamic_probe_log_budget.load(
            std::memory_order_relaxed);
    while (remaining != 0) {
        if (g_gtaiv_camera_dynamic_probe_log_budget.compare_exchange_weak(
                remaining,
                remaining - 1,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool IsVehicleCameraClassEnabled(
    vehicle_camera::VehicleClass vehicle_class) noexcept {
    switch (vehicle_class) {
        case vehicle_camera::VehicleClass::CarPhysicsMover:
            return g_config.gta_iv_car_camera;
        case vehicle_camera::VehicleClass::Motorcycle:
            return g_config.gta_iv_bike_camera;
        default:
            return false;
    }
}

bool TryBuildDriverSideCameraParameters(
    void* component,
    const void* selected_parameters,
    vehicle_camera::ParameterBlock& adjusted_parameters,
    DriverSideCameraDecision& decision) noexcept {
    using namespace vehicle_camera;

    decision = {};
    const bool transaction_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(component);
    const std::uintptr_t selected_address =
        reinterpret_cast<std::uintptr_t>(selected_parameters);
    if ((!g_config.gta_iv_car_camera && !g_config.gta_iv_bike_camera) ||
        !transaction_ready ||
        component_address == 0 || selected_address == 0) {
        return false;
    }

    std::uintptr_t subject = 0;
    std::uintptr_t target_parameters = 0;
    if (!SafeRead(component_address + kSubjectOffset, subject) ||
        !SafeRead(component_address + kTargetParametersOffset,
                   target_parameters) ||
        subject == 0 || target_parameters == 0) {
        return false;
    }
    decision.base_drive_branch_readable =
        TryReadBaseDriveBranchSelected(
            subject, decision.base_drive_branch_selected);
    if (!decision.base_drive_branch_readable) {
        return false;
    }
    decision.subject = subject;

    std::uintptr_t physics_mover = 0;
    std::uintptr_t physics_mover_vtable = 0;
    if (!SafeRead(subject + kPhysicsMoverOffset, physics_mover) ||
        physics_mover == 0 ||
        !SafeRead(physics_mover, physics_mover_vtable)) {
        return false;
    }
    decision.physics_mover = physics_mover;
    decision.vehicle_class = ClassifyVehicle(
        ToRva(physics_mover_vtable), g_use_latest_steam_layout);
    const bool class_enabled =
        IsVehicleCameraClassEnabled(decision.vehicle_class);
    if (!class_enabled) {
        return false;
    }

    std::array<std::uintptr_t, kParameterSlotOffsets.size()> slot_values{};
    for (std::size_t index = 0; index < kParameterSlotOffsets.size(); ++index) {
        if (!SafeRead(subject + kParameterSlotOffsets[index],
                      slot_values[index])) {
            return false;
        }
    }
    ParameterIdentity target_identity{};
    if (!ReadCameraParameterIdentity(target_parameters, target_identity)) {
        return false;
    }
    decision.target_slot_match_mask =
        ProfileSlotMatchMask(target_parameters, slot_values);
    if (g_config.enable_logging) {
        decision.selected_slot_match_mask =
            ProfileSlotMatchMask(selected_address, slot_values);
    }
    decision.target_profile = ClassifyDriverSideTargetProfileFromMatchMask(
        decision.vehicle_class,
        target_parameters,
        decision.target_slot_match_mask,
        slot_values,
        target_identity,
        decision.base_drive_branch_selected);
    decision.policy_evaluated = true;
    if (decision.target_profile == DriverSideTargetProfile::None) {
        // When an outgoing supported snapshot targets an unsupported profile,
        // the game's stock transition naturally fades the existing offset.
        return false;
    }

    unsigned char looking_back = 0;
    unsigned char aim_or_focus = 0;
    float transition_source_weight = 0.0f;
    if (!SafeRead(component_address + kLookingBackFlagOffset,
                   looking_back) ||
        !SafeRead(component_address + kAimOrFocusFlagOffset,
                   aim_or_focus) ||
        !SafeRead(component_address + kTransitionSourceWeightOffset,
                  transition_source_weight)) {
        return false;
    }

    const DriverSideEligibility eligibility{
        g_config.gta_iv_car_camera,
        g_config.gta_iv_bike_camera,
        transaction_ready,
        decision.vehicle_class,
        decision.target_profile,
        looking_back != 0,
        aim_or_focus != 0,
        selected_address,
        target_parameters,
        transition_source_weight,
    };
    const float blend_factor = ResolveDriverSideBlendFactor(eligibility);
    decision.blend_factor = blend_factor;
    if (blend_factor <= 0.0f ||
        !SafeCopyBytes(selected_address,
                       adjusted_parameters.data(),
                       adjusted_parameters.size())) {
        return false;
    }

    if (OffsetDriverSideLateralParameters(
            adjusted_parameters,
            kDriverSideLateralOffsetMeters * blend_factor) !=
        ParameterOffsetResult::Applied) {
        return false;
    }
    return true;
}

void ObserveChaseCameraParameters(void* component,
                                  const void* selected_parameters,
                                  bool mutation_applied,
                                  const DriverSideCameraDecision& decision) {
    using namespace vehicle_camera;

    if (!ClaimCameraProbeSample()) {
        return;
    }
    const unsigned long long sample =
        g_gtaiv_camera_probe_sample_count.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(component);
    const std::uintptr_t selected_address =
        reinterpret_cast<std::uintptr_t>(selected_parameters);

    std::uintptr_t subject = 0;
    const bool subject_readable =
        component_address != 0 &&
        SafeRead(component_address + kSubjectOffset, subject);
    struct ActiveCameraProbeFields {
        std::uintptr_t target_parameters = 0;
        unsigned char update_eye_offset = 0;
        unsigned char state_b5a = 0;
        unsigned char state_b5b = 0;
        unsigned char state_b5c = 0;
        unsigned char looking_back = 0;
        float transition_source_weight = 0.0f;
    } candidate_fields{};
    const bool active_fields_readable =
        subject_readable && subject != 0 &&
        SafeRead(component_address + kTargetParametersOffset,
                 candidate_fields.target_parameters) &&
        SafeRead(component_address + kUpdateEyeOffsetFlagOffset,
                 candidate_fields.update_eye_offset) &&
        SafeRead(component_address + 0xB5A, candidate_fields.state_b5a) &&
        SafeRead(component_address + 0xB5B, candidate_fields.state_b5b) &&
        SafeRead(component_address + kAimOrFocusFlagOffset,
                 candidate_fields.state_b5c) &&
        SafeRead(component_address + kLookingBackFlagOffset,
                 candidate_fields.looking_back) &&
        SafeRead(component_address + kTransitionSourceWeightOffset,
                 candidate_fields.transition_source_weight);
    const bool source_weight_valid =
        active_fields_readable && IsValidTransitionSourceWeight(
            candidate_fields.transition_source_weight);
    ActiveCameraProbeFields active_fields{};
    if (active_fields_readable) {
        active_fields = candidate_fields;
        active_fields.update_eye_offset =
            active_fields.update_eye_offset != 0 ? 1 : 0;
        active_fields.state_b5a = active_fields.state_b5a != 0 ? 1 : 0;
        active_fields.state_b5b = active_fields.state_b5b != 0 ? 1 : 0;
        active_fields.state_b5c = active_fields.state_b5c != 0 ? 1 : 0;
        active_fields.looking_back =
            active_fields.looking_back != 0 ? 1 : 0;
        if (!source_weight_valid) {
            active_fields.transition_source_weight = 0.0f;
        }
    }
    const std::uintptr_t target_parameters =
        active_fields.target_parameters;
    const unsigned char update_eye_offset =
        active_fields.update_eye_offset;
    const unsigned char state_b5a = active_fields.state_b5a;
    const unsigned char state_b5b = active_fields.state_b5b;
    const unsigned char state_b5c = active_fields.state_b5c;
    const unsigned char looking_back = active_fields.looking_back;
    const float transition_source_weight =
        active_fields.transition_source_weight;

    std::array<std::uintptr_t, kParameterSlotOffsets.size()> slot_values{};
    unsigned int slot_read_mask = 0;
    if (subject != 0) {
        for (std::size_t index = 0; index < kParameterSlotOffsets.size();
             ++index) {
            if (SafeRead(subject + kParameterSlotOffsets[index],
                         slot_values[index])) {
                slot_read_mask |= 1u << static_cast<unsigned int>(index);
            }
        }
    }
    std::uintptr_t physics_mover = 0;
    std::uintptr_t physics_mover_vtable = 0;
    if (subject != 0 &&
        SafeRead(subject + kPhysicsMoverOffset, physics_mover) &&
        physics_mover != 0) {
        (void)SafeRead(physics_mover, physics_mover_vtable);
    }
    const std::uintptr_t physics_mover_vtable_rva =
        ToRva(physics_mover_vtable);
    const VehicleClass vehicle_class =
        ClassifyVehicle(physics_mover_vtable_rva,
                        g_use_latest_steam_layout);
    bool observed_base_drive_branch_selected = false;
    const bool observed_base_drive_branch_readable =
        TryReadBaseDriveBranchSelected(
            subject, observed_base_drive_branch_selected);

    const ChaseParameterProbeSnapshot target =
        ReadChaseParameterProbeSnapshot(target_parameters);
    const std::uintptr_t logged_target_parameters =
        target.readable ? target_parameters : 0;
    const std::size_t selected_slot =
        ProfileSlotOffsetFor(selected_address, slot_values);
    const std::size_t target_slot =
        ProfileSlotOffsetFor(logged_target_parameters, slot_values);
    const ParameterSlotMask observed_selected_slot_match_mask =
        ProfileSlotMatchMask(selected_address, slot_values);
    const ParameterSlotMask observed_target_slot_match_mask =
        ProfileSlotMatchMask(logged_target_parameters, slot_values);
    const bool transition_local =
        active_fields_readable && source_weight_valid && target.readable &&
        selected_address != target_parameters &&
        transition_source_weight > 0.0f;
    ChaseParameterProbeSnapshot selected{};
    selected.address = selected_address;
    if (!transition_local) {
        selected = ReadChaseParameterProbeSnapshot(selected_address);
    }
    const ChaseParameterProbeSnapshot transition_source =
        transition_local
            ? ReadChaseParameterProbeSnapshot(
                  component_address + kTransitionSourceParametersOffset)
            : ChaseParameterProbeSnapshot{};
    const ParameterIdentity observed_target_identity{
        target.name_pointer,
        target.name_symbol,
        target.context,
    };
    constexpr unsigned int kAllParameterSlotsReadMask =
        (1u << kParameterSlotOffsets.size()) - 1u;
    const bool all_parameter_slots_read =
        slot_read_mask == kAllParameterSlotsReadMask;
    const DriverSideTargetProfile observed_target_profile =
        target.readable && all_parameter_slots_read
            ? ClassifyDriverSideTargetProfileFromMatchMask(
                  vehicle_class,
                  logged_target_parameters,
                  observed_target_slot_match_mask,
                  slot_values,
                  observed_target_identity,
                  observed_base_drive_branch_selected)
            : DriverSideTargetProfile::None;
    const bool class_enabled = IsVehicleCameraClassEnabled(vehicle_class);
    const DriverSideTargetProfile logged_target_profile =
        decision.policy_evaluated ? decision.target_profile
                                  : observed_target_profile;
    const ParameterSlotMask logged_target_slot_match_mask =
        decision.policy_evaluated ? decision.target_slot_match_mask
                                  : observed_target_slot_match_mask;
    const ParameterSlotMask logged_selected_slot_match_mask =
        decision.policy_evaluated ? decision.selected_slot_match_mask
                                  : observed_selected_slot_match_mask;
    const bool logged_base_drive_branch_readable =
        decision.policy_evaluated
            ? decision.base_drive_branch_readable
            : observed_base_drive_branch_readable;
    const bool logged_base_drive_branch_selected =
        decision.policy_evaluated
            ? decision.base_drive_branch_selected
            : observed_base_drive_branch_selected;
    const bool probe_readable =
        active_fields_readable && source_weight_valid && target.readable &&
        (transition_local ? transition_source.readable : selected.readable) &&
        all_parameter_slots_read && logged_base_drive_branch_readable;

    const std::uint32_t state_context =
        target.readable ? target.context : selected.context;
    const std::uint32_t state_symbol =
        target.readable ? target.name_symbol : selected.name_symbol;
    unsigned long long state_key = 14695981039346656037ull;
    const auto mix_state = [&state_key](unsigned long long value) noexcept {
        state_key ^= value;
        state_key *= 1099511628211ull;
    };
    mix_state(state_symbol);
    mix_state(state_context);
    mix_state(logged_target_slot_match_mask);
    mix_state(logged_selected_slot_match_mask);
    mix_state(static_cast<unsigned long long>(looking_back != 0));
    mix_state(static_cast<unsigned long long>(state_b5a != 0));
    mix_state(static_cast<unsigned long long>(state_b5b != 0));
    mix_state(static_cast<unsigned long long>(state_b5c != 0));
    mix_state(static_cast<unsigned long long>(vehicle_class));
    mix_state(static_cast<unsigned long long>(logged_target_profile));
    mix_state(static_cast<unsigned long long>(
        logged_base_drive_branch_selected));
    mix_state(static_cast<unsigned long long>(class_enabled));
    mix_state(static_cast<unsigned long long>(mutation_applied));
    mix_state(static_cast<unsigned long long>(transition_local));
    mix_state(static_cast<unsigned long long>(probe_readable));
    {
        std::lock_guard<std::mutex> lock(g_gtaiv_camera_probe_state_mutex);
        if (g_gtaiv_camera_probe_last_state == state_key &&
            g_gtaiv_camera_probe_last_subject == subject) {
            return;
        }
        g_gtaiv_camera_probe_last_state = state_key;
        g_gtaiv_camera_probe_last_subject = subject;
    }
    if (!ClaimCameraProbeLogBudget()) {
        return;
    }

    std::uintptr_t subject_vtable = 0;
    std::uint32_t subject_type_uid = 0;
    std::uint32_t subject_name_uid = 0;
    std::uint16_t subject_flags = 0;
    std::uintptr_t sim_object = 0;
    if (subject != 0) {
        (void)SafeRead(subject, subject_vtable);
        (void)SafeRead(subject + 0x18, subject_type_uid);
        (void)SafeRead(subject + 0x1C, subject_name_uid);
        (void)SafeRead(subject + 0x20, subject_flags);
        (void)SafeRead(subject + 0x28, sim_object);
    }
    const std::string target_name =
        target.readable ? ReadCString(target.name_pointer) : std::string{};
    const std::string selected_name =
        selected.readable ? ReadCString(selected.name_pointer) : std::string{};
    const std::string transition_source_name =
        transition_source.readable
            ? ReadCString(transition_source.name_pointer)
            : std::string{};
    const ChaseParameterProbeSnapshot& observed_parameters =
        target.readable ? target : selected;
    const char* observed_parameter_source =
        target.readable ? "target" : (selected.readable ? "selected" : "none");

    log::InfoF(
        "gtaiv_vehicle_camera_probe event=state_change mode=active mutation=%d "
        "car_enabled=%d bike_enabled=%d class_enabled=%d "
        "policy_evaluated=%d base_drive_branch_readable=%d "
        "base_drive_branch_selected=%d target_profile=%s "
        "target_slot_match_mask=0x%04X selected_slot_match_mask=0x%04X "
        "readable=%d blend_factor=%.3f base_offset_m=%.3f "
        "applied_delta_m=%.3f active_fields_readable=%d "
        "source_weight_valid=%d "
        "sample=%llu component=0x%p subject=0x%p subject_vtable=0x%p "
        "subject_type_uid=0x%08X subject_name_uid=0x%08X subject_flags=0x%04X "
        "sim_object=0x%p physics_mover=0x%p mover_vtable=0x%p "
        "mover_vtable_rva=0x%llX vehicle_class=%s "
        "target_parameters=0x%p selected_parameters=0x%p "
        "target_slot=0x%zX selected_slot=0x%zX selected_kind=%s "
        "target_context=%u selected_context=%u target_symbol=0x%08X "
        "selected_symbol=0x%08X target_name=\"%s\" selected_name=\"%s\" "
        "transition_source=0x%p source_context=%u source_symbol=0x%08X "
        "source_name=\"%s\" source_weight=%.6f update_eye=%u looking_back=%u "
        "state_b5a=%u state_b5b=%u state_b5c=%u "
        "parameter_source=%s yaw_window_s=%.6f look_offset_max=%.6f "
        "look_yaw_rate_min=%.6f look_yaw_rate_max=%.6f "
        "pitch_max=%.6f pitch_look_up=%.6f pitch_look_down=%.6f "
        "pitch_eye_up=%.6f pitch_eye_down=%.6f "
        "centering_speed_min=%.6f centering_speed_max=%.6f "
        "reverse_threshold_mps=%.6f unused_orbit_speed=%.6f "
        "slot_read_mask=0x%04X "
        "slots=370:0x%p,378:0x%p,380:0x%p,388:0x%p,390:0x%p,"
        "398:0x%p,3A0:0x%p,3A8:0x%p,3B0:0x%p,3B8:0x%p,"
        "3C0:0x%p,3C8:0x%p,3D0:0x%p",
        mutation_applied ? 1 : 0,
        g_config.gta_iv_car_camera ? 1 : 0,
        g_config.gta_iv_bike_camera ? 1 : 0,
        class_enabled ? 1 : 0,
        decision.policy_evaluated ? 1 : 0,
        logged_base_drive_branch_readable ? 1 : 0,
        logged_base_drive_branch_selected ? 1 : 0,
        DriverSideTargetProfileName(logged_target_profile),
        static_cast<unsigned int>(logged_target_slot_match_mask),
        static_cast<unsigned int>(logged_selected_slot_match_mask),
        probe_readable ? 1 : 0,
        static_cast<double>(decision.blend_factor),
        static_cast<double>(kDriverSideLateralOffsetMeters),
        static_cast<double>(
            mutation_applied
                ? kDriverSideLateralOffsetMeters * decision.blend_factor
                : 0.0f),
        active_fields_readable ? 1 : 0,
        source_weight_valid ? 1 : 0,
        sample,
        component,
        reinterpret_cast<void*>(subject),
        reinterpret_cast<void*>(subject_vtable),
        subject_type_uid,
        subject_name_uid,
        static_cast<unsigned int>(subject_flags),
        reinterpret_cast<void*>(sim_object),
        reinterpret_cast<void*>(physics_mover),
        reinterpret_cast<void*>(physics_mover_vtable),
        static_cast<unsigned long long>(physics_mover_vtable_rva),
        VehicleClassName(vehicle_class),
        reinterpret_cast<void*>(logged_target_parameters),
        selected_parameters,
        target_slot,
        selected_slot,
        transition_local ? "transition_local" : "canonical",
        target.context,
        selected.context,
        target.name_symbol,
        selected.name_symbol,
        target_name.empty() ? "<unreadable>" : target_name.c_str(),
        transition_local
            ? "<transition_local_header_uninitialized>"
            : (selected_name.empty() ? "<unreadable>" :
                                       selected_name.c_str()),
        reinterpret_cast<void*>(transition_source.address),
        transition_source.context,
        transition_source.name_symbol,
        transition_source_name.empty() ? "<not_active_or_unreadable>" :
                                         transition_source_name.c_str(),
        static_cast<double>(transition_source_weight),
        static_cast<unsigned int>(update_eye_offset),
        static_cast<unsigned int>(looking_back),
        static_cast<unsigned int>(state_b5a),
        static_cast<unsigned int>(state_b5b),
        static_cast<unsigned int>(state_b5c),
        observed_parameter_source,
        static_cast<double>(observed_parameters.yaw_ang_vel_timespan),
        static_cast<double>(observed_parameters.look_offset_max),
        static_cast<double>(observed_parameters.look_offset_ang_vel_min),
        static_cast<double>(observed_parameters.look_offset_ang_vel_max),
        static_cast<double>(observed_parameters.pitch_offset_max),
        static_cast<double>(observed_parameters.pitch_offset_look_factor_up),
        static_cast<double>(observed_parameters.pitch_offset_look_factor_down),
        static_cast<double>(observed_parameters.pitch_offset_eye_factor_up),
        static_cast<double>(observed_parameters.pitch_offset_eye_factor_down),
        static_cast<double>(observed_parameters.centering_speed_min),
        static_cast<double>(observed_parameters.centering_speed_max),
        static_cast<double>(observed_parameters.reverse_speed),
        static_cast<double>(observed_parameters.orbit_speed),
        slot_read_mask,
        reinterpret_cast<void*>(slot_values[0]),
        reinterpret_cast<void*>(slot_values[1]),
        reinterpret_cast<void*>(slot_values[2]),
        reinterpret_cast<void*>(slot_values[3]),
        reinterpret_cast<void*>(slot_values[4]),
        reinterpret_cast<void*>(slot_values[5]),
        reinterpret_cast<void*>(slot_values[6]),
        reinterpret_cast<void*>(slot_values[7]),
        reinterpret_cast<void*>(slot_values[8]),
        reinterpret_cast<void*>(slot_values[9]),
        reinterpret_cast<void*>(slot_values[10]),
        reinterpret_cast<void*>(slot_values[11]),
        reinterpret_cast<void*>(slot_values[12]));
}

void DetourChaseCameraSetParameters(void* component,
                                    const void* parameters) {
    alignas(16) vehicle_camera::ParameterBlock adjusted_parameters{};
    DriverSideCameraDecision decision{};
    const bool mutation_applied = TryBuildDriverSideCameraParameters(
        component, parameters, adjusted_parameters, decision);
    vehicle_camera::InvokeSelectedParameterBlockOnce(
        [&](const void* selected_parameters) noexcept {
            g_chase_camera_set_parameters_original(component,
                                                   selected_parameters);
        },
        parameters,
        adjusted_parameters,
        mutation_applied);
    if (ChaseCameraUpdateProbeScope* scope =
            FindActiveChaseCameraUpdateProbeScope(component)) {
        ++scope->calls.setter_calls;
        scope->selected_parameters = parameters;
        scope->decision = decision;
        scope->mutation_applied = mutation_applied;
    }
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        (g_config.gta_iv_car_camera || g_config.gta_iv_bike_camera) &&
        g_config.enable_logging) {
        ObserveChaseCameraParameters(component,
                                     parameters,
                                     mutation_applied,
                                     decision);
    }
}

bool TryBuildGtaIvDesiredPose(
    ChaseCameraUpdateProbeScope& scope,
    float delta_seconds,
    const CameraVector3& stock_eye,
    const CameraVector3& stock_look,
    const CameraVector3& stock_up,
    CameraVector3& adjusted_eye,
    CameraVector3& adjusted_look,
    CameraVector3& adjusted_up) noexcept {
    using namespace vehicle_camera;

    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        scope.depth != 1 || !std::isfinite(delta_seconds)) {
        return false;
    }
    if (ShouldResetGtaIvManualPitchForUpdate(scope.depth, delta_seconds)) {
        ResetGtaIvVehicleCameraDynamicStateForComponent(scope.component);
        return false;
    }

    bool has_native_override = false;
    if (!IsGtaIvVehicleCameraDynamicDecisionEnabled(scope.decision) ||
        !TryReadNativeVehicleCameraOverride(scope.component,
                                            has_native_override) ||
        has_native_override || !scope.stock_mouse_eye_offset_readable ||
        !std::isfinite(scope.stock_mouse_eye_offset)) {
        ResetGtaIvVehicleCameraDynamicState();
        return false;
    }

    std::uint32_t input_flags = 0;
    if (!SafeRead(scope.decision.physics_mover +
                      kPhysicsMoverInputFlagsOffset,
                  input_flags) ||
        IsPhysicsMoverReversing(input_flags)) {
        ResetGtaIvVehicleCameraDynamicStateForComponent(scope.component);
        return false;
    }

    float vertical_input = 0.0f;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(scope.component) +
                      kLookupJoyInputOffset,
                  vertical_input) ||
        !std::isfinite(vertical_input)) {
        ResetGtaIvVehicleCameraDynamicState();
        return false;
    }

    GtaIvVehicleCameraDynamicState& state =
        g_gtaiv_vehicle_camera_dynamic_state;
    if (state.component != scope.component ||
        state.subject != scope.decision.subject) {
        state = {};
        state.component = scope.component;
        state.subject = scope.decision.subject;
    }

    const float applied_mouse_eye_offset = scope.stock_mouse_eye_offset;
    state.manual_pitch = StepGtaIvManualPitch(
        state.manual_pitch,
        ManualPitchFrameInput{
            delta_seconds,
            vertical_input,
        });
    const float manual_pitch_offset = ResolveGtaIvManualPitchOffset(
        state.manual_pitch.angle_radians, applied_mouse_eye_offset);
    scope.manual_pitch_offset_radians = manual_pitch_offset;

    adjusted_eye = stock_eye;
    adjusted_look = stock_look;
    adjusted_up = stock_up;
    adjusted_eye.z -= applied_mouse_eye_offset;

    const float radius_x = adjusted_eye.x - adjusted_look.x;
    const float radius_y = adjusted_eye.y - adjusted_look.y;
    const float radius_z = adjusted_eye.z - adjusted_look.z;
    const float horizontal_distance =
        std::sqrt(radius_x * radius_x + radius_y * radius_y);
    const float distance =
        std::sqrt(horizontal_distance * horizontal_distance +
                  radius_z * radius_z);
    if (!std::isfinite(horizontal_distance) || !std::isfinite(distance) ||
        horizontal_distance <= 0.0001f || distance <= 0.0001f) {
        return false;
    }

    const float stock_pitch = std::atan2(radius_z, horizontal_distance);
    const float applied_pitch = ClampCameraValue(
        stock_pitch + manual_pitch_offset,
        kGtaIvMinimumEyeOrbitPitchRadians,
        kGtaIvMaximumEyeOrbitPitchRadians);
    const float adjusted_horizontal_distance =
        std::cos(applied_pitch) * distance;
    const float horizontal_scale =
        adjusted_horizontal_distance / horizontal_distance;
    adjusted_eye.x = adjusted_look.x + radius_x * horizontal_scale;
    adjusted_eye.y = adjusted_look.y + radius_y * horizontal_scale;
    adjusted_eye.z = adjusted_look.z + std::sin(applied_pitch) * distance;
    if (!std::isfinite(adjusted_eye.x) ||
        !std::isfinite(adjusted_eye.y) ||
        !std::isfinite(adjusted_eye.z)) {
        return false;
    }

    return CameraAbs(manual_pitch_offset) > 0.0001f ||
           CameraAbs(applied_mouse_eye_offset) > 0.0001f;
}

void ObserveChaseCameraUpdate(
    const ChaseCameraUpdateProbeScope& scope,
    const ChaseCameraDynamicSnapshot& after) {
    using namespace vehicle_camera;

    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        !g_config.enable_logging ||
        (!g_config.gta_iv_car_camera && !g_config.gta_iv_bike_camera)) {
        return;
    }

    const unsigned long long sample =
        g_gtaiv_camera_dynamic_probe_sample_count.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    const float yaw_angular_velocity =
        ResolveObservedYawAngularVelocity(after);
    const DriverSideTargetProfile logged_target_profile =
        after.profile_readable ? after.target_profile
                               : scope.decision.target_profile;
    const VehicleClass logged_vehicle_class =
        after.vehicle_class != VehicleClass::Unknown
            ? after.vehicle_class
            : scope.decision.vehicle_class;
    log::InfoF(
        "gtaiv_vehicle_camera_dynamic event=update sample=%llu tid=%lu "
        "component=0x%p subject=0x%p physics_mover=0x%p "
        "mover_vtable_rva=0x%llX dt=%.6f depth=%u nested_updates=%u "
        "setter_calls=%u nested_setter_calls=%u pose_calls=%u "
        "nested_pose_calls=%u selected_parameters=0x%p "
        "policy_evaluated=%d base_drive_branch_readable=%d "
        "base_drive_branch_selected=%d target_profile=%s vehicle_class=%s "
        "profile_readable=%d target_parameters=0x%p "
        "target_slot_match_mask=0x%04X target_context=%u "
        "target_symbol=0x%08X source_weight=%.6f "
        "source_weight_valid=%d "
        "mutation=%d dynamics_mutation=%d manual_pitch_offset=%.6f "
        "before_readable=%d after_readable=%d "
        "input_pre=%.6f,%.6f,%.6f input_post=%.6f,%.6f,%.6f "
        "center_timer_pre=%.6f center_timer_post=%.6f "
        "manual_yaw_pre=%.6f manual_yaw_post=%.6f "
        "snap_yaw_pre=%.6f snap_yaw_post=%.6f "
        "follow_yaw=%.6f desired_yaw=%.6f yaw_rate_avg=%.6f "
        "target_pitch=%.6f pitch_target=%.6f "
        "signed_forward_speed_readable=%d signed_forward_speed_mps=%.6f "
        "input_state_readable=%d steering_input=%.6f "
        "input_flags=0x%08X in_reverse=%d handbrake=%d "
        "looking_around=%u looking_back=%u aim_or_focus=%u "
        "eye_locked=%u look_locked=%u "
        "centering_min=%.6f centering_max=%.6f "
        "reverse_threshold_mps=%.6f "
        "yaw_window_s=%.6f look_offset_max=%.6f "
        "look_yaw_rate_min=%.6f look_yaw_rate_max=%.6f "
        "pitch_offset_max=%.6f",
        sample,
        scope.thread_id,
        scope.component,
        reinterpret_cast<void*>(after.subject),
        reinterpret_cast<void*>(after.physics_mover),
        static_cast<unsigned long long>(after.physics_mover_vtable_rva),
        static_cast<double>(scope.delta_seconds),
        scope.depth,
        scope.calls.nested_updates,
        scope.calls.setter_calls,
        scope.calls.nested_setter_calls,
        scope.calls.desired_pose_calls,
        scope.calls.nested_desired_pose_calls,
        scope.selected_parameters,
        scope.decision.policy_evaluated ? 1 : 0,
        after.base_drive_branch_readable ? 1 : 0,
        after.base_drive_branch_selected ? 1 : 0,
        DriverSideTargetProfileName(logged_target_profile),
        VehicleClassName(logged_vehicle_class),
        after.profile_readable ? 1 : 0,
        reinterpret_cast<void*>(after.target_parameters),
        static_cast<unsigned int>(after.target_slot_match_mask),
        after.target_context,
        after.target_symbol,
        static_cast<double>(after.transition_source_weight),
        after.transition_source_weight_valid ? 1 : 0,
        scope.mutation_applied ? 1 : 0,
        scope.dynamics_mutation_applied ? 1 : 0,
        static_cast<double>(scope.manual_pitch_offset_radians),
        scope.before.readable ? 1 : 0,
        after.readable ? 1 : 0,
        static_cast<double>(scope.before.lookaround_joy_input),
        static_cast<double>(scope.before.lookup_joy_input),
        static_cast<double>(scope.before.lookup_mouse),
        static_cast<double>(after.lookaround_joy_input),
        static_cast<double>(after.lookup_joy_input),
        static_cast<double>(after.lookup_mouse),
        static_cast<double>(scope.before.lookaround_center_timer),
        static_cast<double>(after.lookaround_center_timer),
        static_cast<double>(scope.before.lookaround_angle),
        static_cast<double>(after.lookaround_angle),
        static_cast<double>(scope.before.lookaround_angle_desired),
        static_cast<double>(after.lookaround_angle_desired),
        static_cast<double>(after.forward_angle),
        static_cast<double>(after.forward_angle_desired),
        static_cast<double>(yaw_angular_velocity),
        static_cast<double>(after.target_pitch_position),
        static_cast<double>(after.target_pitch_target),
        after.forward_speed_readable ? 1 : 0,
        static_cast<double>(after.forward_speed),
        after.input_state_readable ? 1 : 0,
        static_cast<double>(after.steering_input),
        after.input_flags,
        (after.input_flags & kPhysicsMoverInReverseFlag) != 0 ? 1 : 0,
        (after.input_flags & kPhysicsMoverHandbrakeFlag) != 0 ? 1 : 0,
        static_cast<unsigned int>(after.is_looking_around),
        static_cast<unsigned int>(after.looking_back),
        static_cast<unsigned int>(after.aim_or_focus),
        static_cast<unsigned int>(after.eye_locked),
        static_cast<unsigned int>(after.look_locked),
        static_cast<double>(after.centering_speed_min),
        static_cast<double>(after.centering_speed_max),
        static_cast<double>(after.reverse_speed),
        static_cast<double>(after.yaw_ang_vel_timespan),
        static_cast<double>(after.look_offset_max),
        static_cast<double>(after.look_offset_ang_vel_min),
        static_cast<double>(after.look_offset_ang_vel_max),
        static_cast<double>(after.pitch_offset_max));

    if (!scope.desired_pose_readable) {
        return;
    }
    const float stock_delta_x = scope.desired_look.x - scope.desired_eye.x;
    const float stock_delta_y = scope.desired_look.y - scope.desired_eye.y;
    const float stock_delta_z = scope.desired_look.z - scope.desired_eye.z;
    const float stock_horizontal_distance =
        std::sqrt(stock_delta_x * stock_delta_x +
                  stock_delta_y * stock_delta_y);
    const float stock_pose_distance =
        std::sqrt(stock_horizontal_distance * stock_horizontal_distance +
                  stock_delta_z * stock_delta_z);
    const float stock_pose_yaw = std::atan2(stock_delta_y, stock_delta_x);
    const float stock_pose_pitch =
        std::atan2(stock_delta_z, stock_horizontal_distance);
    const float delta_x =
        scope.effective_desired_look.x - scope.effective_desired_eye.x;
    const float delta_y =
        scope.effective_desired_look.y - scope.effective_desired_eye.y;
    const float delta_z =
        scope.effective_desired_look.z - scope.effective_desired_eye.z;
    const float horizontal_distance =
        std::sqrt(delta_x * delta_x + delta_y * delta_y);
    const float pose_distance =
        std::sqrt(horizontal_distance * horizontal_distance +
                  delta_z * delta_z);
    const float pose_yaw = std::atan2(delta_y, delta_x);
    const float pose_pitch = std::atan2(delta_z, horizontal_distance);
    log::InfoF(
        "gtaiv_vehicle_camera_dynamic event=desired_pose sample=%llu "
        "component=0x%p collide=%d mutation=%d "
        "manual_pitch_offset=%.6f "
        "stock_eye=%.6f,%.6f,%.6f stock_look=%.6f,%.6f,%.6f "
        "stock_up=%.6f,%.6f,%.6f stock_distance=%.6f "
        "stock_yaw=%.6f stock_pitch=%.6f "
        "eye=%.6f,%.6f,%.6f look=%.6f,%.6f,%.6f "
        "up=%.6f,%.6f,%.6f distance=%.6f yaw=%.6f pitch=%.6f",
        sample,
        scope.component,
        scope.collide ? 1 : 0,
        scope.dynamics_mutation_applied ? 1 : 0,
        static_cast<double>(scope.manual_pitch_offset_radians),
        static_cast<double>(scope.desired_eye.x),
        static_cast<double>(scope.desired_eye.y),
        static_cast<double>(scope.desired_eye.z),
        static_cast<double>(scope.desired_look.x),
        static_cast<double>(scope.desired_look.y),
        static_cast<double>(scope.desired_look.z),
        static_cast<double>(scope.desired_up.x),
        static_cast<double>(scope.desired_up.y),
        static_cast<double>(scope.desired_up.z),
        static_cast<double>(stock_pose_distance),
        static_cast<double>(stock_pose_yaw),
        static_cast<double>(stock_pose_pitch),
        static_cast<double>(scope.effective_desired_eye.x),
        static_cast<double>(scope.effective_desired_eye.y),
        static_cast<double>(scope.effective_desired_eye.z),
        static_cast<double>(scope.effective_desired_look.x),
        static_cast<double>(scope.effective_desired_look.y),
        static_cast<double>(scope.effective_desired_look.z),
        static_cast<double>(scope.effective_desired_up.x),
        static_cast<double>(scope.effective_desired_up.y),
        static_cast<double>(scope.effective_desired_up.z),
        static_cast<double>(pose_distance),
        static_cast<double>(pose_yaw),
        static_cast<double>(pose_pitch));
}

void DetourGameCameraSetDesiredEyeLookUp(
    void* component,
    float delta_seconds,
    const CameraVector3* desired_eye,
    const CameraVector3* desired_look,
    const CameraVector3* desired_up,
    bool collide,
    CameraVector3* new_eye,
    CameraVector3* new_look) {
    CameraVector3 adjusted_eye{};
    CameraVector3 adjusted_look{};
    CameraVector3 adjusted_up{};
    CameraVector3 eye{};
    CameraVector3 look{};
    CameraVector3 up{};
    bool pose_adjusted = false;
    ChaseCameraUpdateProbeScope* scope =
        FindActiveChaseCameraUpdateProbeScope(component);
    if (scope != nullptr) {
        ++scope->calls.desired_pose_calls;
        scope->desired_pose_readable =
            SafeRead(reinterpret_cast<std::uintptr_t>(desired_eye), eye) &&
            SafeRead(reinterpret_cast<std::uintptr_t>(desired_look), look) &&
            SafeRead(reinterpret_cast<std::uintptr_t>(desired_up), up);
        if (scope->desired_pose_readable) {
            scope->desired_eye = eye;
            scope->desired_look = look;
            scope->desired_up = up;
            scope->collide = collide;
            pose_adjusted = TryBuildGtaIvDesiredPose(
                *scope,
                delta_seconds,
                eye,
                look,
                up,
                adjusted_eye,
                adjusted_look,
                adjusted_up);
        }
    }

    struct AlternateLookRestore {
        std::uintptr_t address = 0;
        unsigned char value = 0;
        bool armed = false;

        ~AlternateLookRestore() {
            if (armed) {
                (void)SafeWrite(address, value);
            }
        }
    } alternate_look_restore;

    unsigned char alternate_look_flag = 0;
    if (pose_adjusted) {
        alternate_look_restore.address =
            reinterpret_cast<std::uintptr_t>(component) +
            vehicle_camera::kAlternateLookFlagOffset;
        if (!SafeRead(alternate_look_restore.address, alternate_look_flag)) {
            pose_adjusted = false;
            ResetGtaIvVehicleCameraDynamicState();
        } else if (alternate_look_flag != 0) {
            const unsigned char disabled = 0;
            if (!SafeWrite(alternate_look_restore.address, disabled)) {
                pose_adjusted = false;
                ResetGtaIvVehicleCameraDynamicState();
            } else {
                alternate_look_restore.value = alternate_look_flag;
                alternate_look_restore.armed = true;
            }
        }
    }
    if (scope != nullptr && scope->desired_pose_readable) {
        scope->effective_desired_eye = pose_adjusted ? adjusted_eye : eye;
        scope->effective_desired_look = pose_adjusted ? adjusted_look : look;
        scope->effective_desired_up = pose_adjusted ? adjusted_up : up;
        scope->dynamics_mutation_applied |= pose_adjusted;
    }
    vehicle_camera::InvokeDesiredEyeLookUpOnce(
        g_game_camera_set_desired_eye_look_up_original,
        component,
        delta_seconds,
        pose_adjusted ? &adjusted_eye : desired_eye,
        pose_adjusted ? &adjusted_look : desired_look,
        pose_adjusted ? &adjusted_up : desired_up,
        collide,
        new_eye,
        new_look);
}

void DetourChaseCameraUpdate(void* component, float delta_seconds) {
    if (g_chase_camera_update_original == nullptr) {
        return;
    }
    const unsigned int index = g_chase_camera_update_probe_depth;
    const bool transaction_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const bool feature_enabled =
        g_config.gta_iv_car_camera || g_config.gta_iv_bike_camera;
    std::uintptr_t active_subject = 0;
    const bool has_active_subject =
        component != nullptr &&
        SafeRead(reinterpret_cast<std::uintptr_t>(component) +
                     vehicle_camera::kSubjectOffset,
                 active_subject) &&
        active_subject != 0;
    const bool should_attempt_outer_capture =
        vehicle_camera::ShouldAttemptOuterDynamicProbeCapture(
            index,
            transaction_ready,
            g_config.enable_logging,
            feature_enabled);
    const bool has_dynamic_probe_budget =
        g_gtaiv_camera_dynamic_probe_log_budget.load(
            std::memory_order_relaxed) != 0;
    const bool capture_outer_sample =
        should_attempt_outer_capture && has_active_subject &&
        has_dynamic_probe_budget &&
        ClaimDynamicCameraProbeSample() &&
        ClaimDynamicCameraProbeLogBudget();
    if (index == 0 && transaction_ready && feature_enabled &&
        component != nullptr && !has_active_subject &&
        g_gtaiv_vehicle_camera_dynamic_state.component == component) {
        ResetGtaIvVehicleCameraDynamicStateForComponent(component);
    }
    if (!vehicle_camera::ShouldEnterDynamicCameraScope(
            index,
            transaction_ready,
            feature_enabled,
            has_active_subject)) {
        vehicle_camera::InvokeChaseCameraUpdateOnce(
            g_chase_camera_update_original, component, delta_seconds);
        return;
    }
    if (index >= kChaseCameraUpdateProbeMaxDepth) {
        vehicle_camera::InvokeChaseCameraUpdateOnce(
            g_chase_camera_update_original, component, delta_seconds);
        return;
    }

    ChaseCameraUpdateProbeScope& scope =
        g_chase_camera_update_probe_stack[index];
    scope = {};
    scope.component = component;
    scope.delta_seconds = delta_seconds;
    scope.thread_id = GetCurrentThreadId();
    scope.depth = index + 1;
    scope.capture_sample = capture_outer_sample;
    scope.stock_mouse_eye_offset_readable = SafeRead(
        reinterpret_cast<std::uintptr_t>(component) +
            vehicle_camera::kLookupMouseOffset,
        scope.stock_mouse_eye_offset);
    if (scope.capture_sample) {
        (void)ReadChaseCameraDynamicSnapshot(component, scope.before);
    }
    ++g_chase_camera_update_probe_depth;
    vehicle_camera::InvokeChaseCameraUpdateOnce(
        g_chase_camera_update_original, component, delta_seconds);
    --g_chase_camera_update_probe_depth;

    if (index != 0) {
        ChaseCameraUpdateProbeScope& parent =
            g_chase_camera_update_probe_stack[index - 1];
        vehicle_camera::MergeNestedDynamicProbeCounts(parent.calls,
                                                       scope.calls);
        return;
    }
    if (scope.capture_sample) {
        ChaseCameraDynamicSnapshot after{};
        (void)ReadChaseCameraDynamicSnapshot(component, after);
        ObserveChaseCameraUpdate(scope, after);
    }
}

std::string ReadTaskName(std::uintptr_t task_header) {
    if (task_header == 0) {
        return {};
    }

    std::uintptr_t metadata = 0;
    if (!SafeRead(task_header + 0x30, metadata) || metadata == 0) {
        return {};
    }

    std::uintptr_t name_ptr = 0;
    if (!SafeRead(metadata + 0x38, name_ptr) || name_ptr == 0) {
        return {};
    }

    return ReadCString(name_ptr);
}

std::string ReadTaskNameNoThrow(std::uintptr_t task_header) noexcept {
    try {
        return ReadTaskName(task_header);
    } catch (...) {
        // Verbose task telemetry is optional; never let a diagnostic allocation
        // failure escape a worker/render detour.
        return {};
    }
}

#if !defined(SPATCH_FINAL_RELEASE)
void MaybeWriteSummary() {
    if (g_config.summary_interval_ms == 0) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    unsigned long long last = g_last_summary_tick.load();
    if (now - last < g_config.summary_interval_ms) {
        return;
    }

    if (!g_last_summary_tick.compare_exchange_strong(last, now)) {
        return;
    }

    const smaa::Stats smaa_stats = smaa::GetStats();
    const SummaryRuntimeFields fields{
        .snapshot =
            {
                .task_ready = g_task_ready_count.load(),
                .task_dispatch = g_task_dispatch_count.load(),
                .wait_helper = g_wait_helper_count.load(),
                .wait_task = g_wait_helper_nonzero_task_count.load(),
                .wait_gt16 = g_wait_over_16ms_count.load(),
                .wait_gt100 = g_wait_over_100ms_count.load(),
                .wait_gt1000 = g_wait_over_1000ms_count.load(),
                .wait_gt5000 = g_wait_over_5000ms_count.load(),
                .scaleform_time = g_scaleform_time_count.load(),
                .provider_non_null = g_scaleform_provider_non_null_count.load(),
                .scaleform_init = g_scaleform_init_count.load(),
                .nis_time = g_nis_set_play_time_count.load(),
                .nis_sync = g_nis_sync_scene_time_count.load(),
                .nis_dt0 = g_nis_delta_zero_count.load(),
                .nis_dt30 = g_nis_delta_30hz_count.load(),
                .nis_dt60 = g_nis_delta_60hz_count.load(),
                .nis_dt_other = g_nis_delta_other_count.load(),
                .nis_scene_fix = g_nis_scene_time_fix_count.load(),
                .nis_play = g_nis_play_count.load(),
                .nis_play_adv = g_nis_play_advanced_count.load(),
                .nis_play_repeat = g_nis_play_repeat_count.load(),
                .nis_play_multi = g_nis_play_multi_tick_count.load(),
                .nis_boot = g_nis_bootstrap_count.load(),
                .nis_boot_s1 = g_nis_bootstrap_state1_count.load(),
                .nis_boot_s2 = g_nis_bootstrap_state2_count.load(),
                .nis_boot_fail = g_nis_bootstrap_fail_count.load(),
                .nis_owner = g_nis_owner_count.load(),
                .nis_owner_dt0 = g_nis_owner_dt_zero_count.load(),
                .nis_owner_dt30 = g_nis_owner_dt_30hz_count.load(),
                .nis_owner_dt60 = g_nis_owner_dt_60hz_count.load(),
                .nis_owner_dt_other = g_nis_owner_dt_other_count.load(),
                .nis_owner_adv = g_nis_owner_advanced_count.load(),
                .nis_owner_repeat = g_nis_owner_repeat_count.load(),
                .nis_owner_multi = g_nis_owner_multi_tick_count.load(),
                .nis_actor_setup = g_nis_actor_setup_count.load(),
                .nis_actor_restore = g_nis_actor_restore_count.load(),
                .nis_actor_restore_untracked = g_nis_actor_restore_untracked_count.load(),
                .nis_actor_setup_duplicate = g_nis_actor_setup_duplicate_count.load(),
                .nis_actor_restore_duplicate = g_nis_actor_restore_duplicate_count.load(),
                .nis_actor_restore_never_seen = g_nis_actor_restore_never_seen_count.load(),
                .nis_actor_restore_suppressed = g_nis_actor_restore_suppressed_count.load(),
                .twitch_tick = g_twitch_state_tick_count.load(),
                .twitch_login_callback = g_twitch_login_callback_count.load(),
                .twitch_login_failure = g_twitch_login_failure_count.load(),
                .frameflow = g_frame_flow_count.load(),
                .frameflow_dt0 = g_frame_flow_dt_zero_count.load(),
                .frameflow_dt60 = g_frame_flow_dt_60hz_count.load(),
                .frameflow_dt_other = g_frame_flow_dt_other_count.load(),
                .frameflow_from_cutscene = g_frame_flow_from_cutscene_owner_count.load(),
                .cutscene_flow = g_cutscene_flow_owner_count.load(),
                .cutscene_flow_dt0 = g_cutscene_flow_owner_dt_zero_count.load(),
                .cutscene_flow_fwd0 = g_cutscene_flow_owner_forwarded_zero_count.load(),
                .cutscene_flow_fwd60 = g_cutscene_flow_owner_forwarded_60hz_count.load(),
                .cutscene_flow_fwd_other = g_cutscene_flow_owner_forwarded_other_count.load(),
                .cutscene_flow_fix = g_cutscene_zero_dt_override_count.load(),
                .cutscene_flow_fix_paused = g_cutscene_zero_dt_pause_override_count.load(),
                .fog_slicing = g_fog_slicing_mode_count.load(),
                .fog_clamps = g_fog_slicing_clamp_count.load(),
                .aa_owner = g_aa_owner_count.load(),
                .aa_skip = g_aa_skip_count.load(),
                .aa_main = g_aa_main_count.load(),
                .aa_hairblur0 = g_aa_hair_blur_zero_count.load(),
                .aa_state = g_last_aa_state_gate.load(),
                .aa_hair_gate = g_last_aa_hair_blur_gate.load(),
                .aa_variant = g_last_aa_variant_mode.load(),
                .aa_variant_apply = g_aa_variant_apply_count.load(),
                .aa_aux_mode = g_last_aa_aux_mode.load(),
                .aa_aux_apply = g_aa_aux_apply_count.load(),
                .aa_shader = g_last_aa_shader_uid.load(),
                .aa_raster = g_last_aa_raster_uid.load(),
                .aa_aux = g_last_aa_aux_uid.load(),
                .aa_material = g_last_aa_material.load(),
                .aa_target = g_last_aa_target.load(),
                .aa_src_a = g_last_aa_source_a.load(),
                .aa_src_b = g_last_aa_source_b.load(),
                .aa_fx = g_aa_fx_handoff_count.load(),
                .aa_fx_arg1 = g_last_aa_fx_arg1.load(),
                .aa_fx_arg2 = g_last_aa_fx_arg2.load(),
                .aa_fx_arg3 = g_last_aa_fx_arg3.load(),
                .rumble_override = g_rumble_override_apply_count.load(),
                .rumble_value = g_last_rumble_override_value.load(),
                .post_submit = g_post_material_submit_count.load(),
                .post_comp_lights = g_post_composite_lights_submit_count.load(),
                .post_comp_final = g_post_composite_final_submit_count.load(),
                .post_bloom = g_post_bloom_threshold_submit_count.load(),
                .post_lightshaft = g_post_lightshaft_submit_count.load(),
                .post_shadow_collector = g_post_shadow_collector_submit_count.load(),
                .post_final_chg = g_post_composite_final_snapshot_change_count.load(),
                .post_final_flags = g_last_post_final_flags.load(),
                .post_final_cmd = g_last_post_final_cmd.load(),
                .post_final_params = g_last_post_final_params.load(),
                .post_final_p0 = g_last_post_final_param0.load(),
                .post_final_p1 = g_last_post_final_param1.load(),
                .post_final_p2 = g_last_post_final_param2.load(),
                .post_final_p3 = g_last_post_final_param3.load(),
                .char_water = g_character_water_collision_count.load(),
                .char_fx_update = g_character_effects_update_count.load(),
                .char_fx_wet_updates = g_character_effects_wet_surface_count.load(),
                .char_fx_surface = g_last_character_active_surface_uid.load(),
                .char_fx_wet_surface = g_last_character_active_wet_surface_uid.load(),
                .char_fx_gate = g_last_character_wet_gate_counter.load(),
                .char_fx_onfire = g_last_character_is_on_fire.load(),
                .char_fx_smolder = g_last_character_is_smoldering.load(),
                .char_fx_attached = g_last_character_is_attached_to_player.load(),
                .char_health_apply = g_character_health_apply_damage_count.load(),
                .char_health_proj = g_character_health_apply_projectile_count.load(),
                .char_health_melee = g_character_health_apply_melee_count.load(),
                .char_health_anim_found = g_character_health_anim_found_count.load(),
                .char_health_hitreact_found = g_character_health_hitreact_found_count.load(),
                .char_health_damage = g_last_character_health_damage.load(),
                .char_health_projectile = g_last_character_health_projectile.load(),
                .char_health_component = g_last_character_health_component.load(),
                .char_health_anim = g_last_character_health_anim_component.load(),
                .char_health_hitreact = g_last_character_health_hitreact_component.load(),
                .char_health_attacker = g_last_character_health_attacker.load(),
                .char_health_hit = g_last_character_health_hit_record.load(),
                .char_wet_force = g_character_wet_force_count.load(),
                .char_wet_force_verify = g_character_wet_force_verify_count.load(),
                .char_charred_anim = g_character_charred_anim_count.load(),
                .char_charred_rig = g_character_charred_rig_count.load(),
                .char_dispatch_owner = g_character_effect_dispatch_owner_count.load(),
                .char_dispatch_consume = g_character_effect_dispatch_consume_count.load(),
                .char_dispatch_owner_ptr = g_last_character_effect_dispatch_owner.load(),
                .char_dispatch_component = g_last_character_effect_dispatch_component.load(),
                .char_queue_build = g_character_effect_queue_builder_count.load(),
                .char_queue_build_tracked = g_character_effect_queue_builder_tracked_count.load(),
                .char_queue_build_owner = g_last_character_effect_queue_builder_owner.load(),
                .char_queue_build_component = g_last_character_effect_queue_builder_component.load(),
                .char_queue_build_mode = g_last_character_effect_queue_builder_mode.load(),
                .char_paint_owner = g_character_paint_owner_count.load(),
                .char_paint_owner_ptr = g_last_character_paint_owner_ptr.load(),
                .char_paint_owner_component = g_last_character_paint_owner_component.load(),
                .char_paint_consumer = g_character_paint_consumer_count.load(),
                .char_paint_anim = g_character_paint_anim_count.load(),
                .char_paint_rig = g_character_paint_rig_count.load(),
                .char_paint_enable = g_last_character_paint_enable.load(),
                .char_damage_create = g_character_damage_rig_create_count.load(),
                .char_damage_reset = g_character_damage_rig_reset_count.load(),
                .post_last = g_last_post_material.load(),
                .unique_callbacks = g_unique_callback_count.load(),
                .scenery_prepare = g_scenery_prepare_count.load(),
                .scenery_setup = g_scenery_setup_count.load(),
                .render_scenery = g_render_scenery_builder_count.load(),
                .rasterize_bucket = g_rasterize_bucket_builder_count.load(),
                .scenery_prepare_ready = g_scenery_prepare_ready_total.load(),
                .scenery_setup_ready = g_scenery_setup_ready_total.load(),
                .render_scenery_ready = g_render_scenery_ready_total.load(),
                .rasterize_bucket_ready = g_rasterize_bucket_ready_total.load(),
                .scenery_setup_qdelta = g_scenery_setup_queue_delta_total.load(),
                .render_scenery_qdelta = g_render_scenery_queue_delta_total.load(),
                .rasterize_bucket_qdelta = g_rasterize_bucket_queue_delta_total.load(),
                .scenery_count0 = g_last_scenery_counter0.load(),
                .scenery_count1 = g_last_scenery_counter1.load(),
                .scenery_count2 = g_last_scenery_counter2.load(),
                .scenery_count3 = g_last_scenery_counter3.load(),
            },
        .nis_last_bits = g_last_nis_scene_time_bits.load(),
        .nis_last_delta_bits = g_last_nis_scene_delta_bits.load(),
        .nis_owner_last_dt_bits = g_last_nis_owner_dt_bits.load(),
        .frameflow_last_dt_bits = g_last_frame_flow_dt_bits.load(),
        .cutscene_flow_in_bits = g_last_cutscene_flow_input_dt_bits.load(),
        .cutscene_flow_fwd_bits = g_last_cutscene_flow_forwarded_dt_bits.load(),
        .char_water_speed_bits = g_last_character_water_speed_bits.load(),
        .char_fx_tod_weather_bits = g_last_character_timeofday_weather_bits.load(),
        .char_fx_tod_override_bits = g_last_character_timeofday_override_bits.load(),
        .char_fx_fire_time_bits = g_last_character_fire_extinguish_bits.load(),
        .char_fx_smolder_time_bits = g_last_character_smolder_extinguish_bits.load(),
        .char_fx_queued_damage_bits = g_last_character_queued_health_damage_bits.load(),
        .char_charred_amount_bits = g_last_character_charred_amount_bits.load(),
        .char_paint_r_bits = g_last_character_paint_r_bits.load(),
        .char_paint_g_bits = g_last_character_paint_g_bits.load(),
        .char_paint_b_bits = g_last_character_paint_b_bits.load(),
        .smaa_stats = smaa_stats,
        .provider_ptr = ReadProviderValue(),
    };

    try {
        const SummarySnapshot snapshot = BuildSummarySnapshot(fields);
        log::Info(FormatSummaryMessage(snapshot));
    } catch (...) {
        // Summary generation is optional telemetry and allocates a sizeable
        // formatted string.  A low-memory event must not escape a render/NIS
        // detour and terminate the game; the next interval can retry.
    }
}
#endif

void LogTaskState(const char* prefix,
                  void* owner_or_manager,
                  void* resolved_manager_ptr,
                  void* task_header,
                  unsigned int worker_index) {
    const auto manager = reinterpret_cast<std::uintptr_t>(resolved_manager_ptr);
    const auto owner = reinterpret_cast<std::uintptr_t>(owner_or_manager);
    const auto task = reinterpret_cast<std::uintptr_t>(task_header);

    int worker_count = 0;
    int active_workers_guess = 0;
    int queue_depth_counter = 0;
    int queued_total = 0;
    int completed_total = 0;
    unsigned short task_state = 0;
    unsigned short dependency_count = 0;
    std::uintptr_t callback = 0;
    std::string task_name;

    if (manager != 0) {
        SafeRead(manager + 0x8, worker_count);
        SafeRead(manager + 0x14, active_workers_guess);
        SafeRead(manager + 0x58, queue_depth_counter);
        SafeRead(manager + 0x218, queued_total);
        SafeRead(manager + 0x21C, completed_total);
    }

    if (task != 0) {
        SafeRead(task + 0x20, task_state);
        SafeRead(task + 0x22, dependency_count);
        callback = ReadTaskCallback(task);
        task_name = ReadTaskNameNoThrow(task);
    }

    log::InfoF(
        "%s owner=0x%p manager=0x%p task=0x%p worker_index=%u worker_count=%d "
        "active_workers_guess=%d queue_lock_depth=%d queued_total=%d completed_total=%d "
        "task_state=%hu deps=%hu callback=0x%p callback_rva=0x%llX task_name=%s provider=0x%p",
        prefix,
        reinterpret_cast<void*>(owner),
        reinterpret_cast<void*>(manager),
        reinterpret_cast<void*>(task),
        worker_index,
        worker_count,
        active_workers_guess,
        queue_depth_counter,
        queued_total,
        completed_total,
        task_state,
        dependency_count,
        reinterpret_cast<void*>(callback),
        static_cast<unsigned long long>(ToRva(callback)),
        task_name.empty() ? "<unnamed>" : task_name.c_str(),
        reinterpret_cast<void*>(ReadProviderValue()));
}

void DetourTaskReady(void* task_manager_owner, void* task_header) {
    g_task_ready_count.fetch_add(1);
    if (g_current_builder_scope != BuilderScope::None) {
        AddReadyCounterForScope(g_current_builder_scope);
    }

    std::uintptr_t resolved_manager = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(task_manager_owner), resolved_manager);

    if (ShouldLogVerbose(g_task_ready_verbose)) {
        LogTaskState("task_ready",
                     task_manager_owner,
                     reinterpret_cast<void*>(resolved_manager),
                     task_header,
                     0);
    }

    MaybeWriteSummary();
    g_task_ready_original(task_manager_owner, task_header);
}

void DetourTaskDispatch(void* task_manager, unsigned int worker_index) {
    g_task_dispatch_count.fetch_add(1);

#if !defined(SPATCH_FINAL_RELEASE)
    const std::uintptr_t queued_task = PeekQueuedTaskHeader(reinterpret_cast<std::uintptr_t>(task_manager));
    const std::uintptr_t callback = ReadTaskCallback(queued_task);

    if (RecordSeenCallback(callback)) {
        const std::string task_name = ReadTaskNameNoThrow(queued_task);
        unsigned short task_state = 0;
        unsigned short dependency_count = 0;
        SafeRead(queued_task + 0x20, task_state);
        SafeRead(queued_task + 0x22, dependency_count);

        log::InfoF(
            "task_callback_first_seen callback=0x%p callback_rva=0x%llX task=0x%p worker_index=%u "
            "task_state=%hu deps=%hu task_name=%s",
            reinterpret_cast<void*>(callback),
            static_cast<unsigned long long>(ToRva(callback)),
            reinterpret_cast<void*>(queued_task),
            worker_index,
            task_state,
            dependency_count,
            task_name.empty() ? "<unnamed>" : task_name.c_str());
    }

    if (ShouldLogVerbose(g_task_dispatch_verbose)) {
        LogTaskState("task_dispatch",
                     task_manager,
                     task_manager,
                     reinterpret_cast<void*>(queued_task),
                     worker_index);
    }
#else
    (void)worker_index;
#endif

    MaybeWriteSummary();
    g_task_dispatch_original(task_manager, worker_index);
}

void DetourWaitHelper(void* task_manager_owner, void* task_header) {
    g_wait_helper_count.fetch_add(1);
    if (task_header != nullptr) {
        g_wait_helper_nonzero_task_count.fetch_add(1);
    }

    std::uintptr_t resolved_manager = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(task_manager_owner), resolved_manager);

    const unsigned long long before = GetTickCount64();
    g_wait_helper_original(task_manager_owner, task_header);
    const unsigned long long elapsed = GetTickCount64() - before;

    if (elapsed >= kWaitLogThresholdMs) {
        g_wait_over_16ms_count.fetch_add(1);
    }
    if (elapsed >= kWaitLongThresholdMs) {
        g_wait_over_100ms_count.fetch_add(1);
    }
    if (elapsed >= kWaitVeryLongThresholdMs) {
        g_wait_over_1000ms_count.fetch_add(1);
    }
    if (elapsed >= 5000) {
        g_wait_over_5000ms_count.fetch_add(1);
    }

    if ((elapsed >= kWaitLogThresholdMs && ShouldLogVerbose(g_wait_helper_verbose)) ||
        elapsed >= kWaitVeryLongThresholdMs) {
        const std::uintptr_t callback = ReadTaskCallback(reinterpret_cast<std::uintptr_t>(task_header));
        log::InfoF(
            "wait_helper owner=0x%p manager=0x%p task=0x%p elapsed_ms=%llu task_state=%hu "
            "deps=%hu callback=0x%p callback_rva=0x%llX task_name=%s",
            task_manager_owner,
            reinterpret_cast<void*>(resolved_manager),
            task_header,
            elapsed,
            [&]() {
                unsigned short value = 0;
                SafeRead(reinterpret_cast<std::uintptr_t>(task_header) + 0x20, value);
                return value;
            }(),
            [&]() {
                unsigned short value = 0;
                SafeRead(reinterpret_cast<std::uintptr_t>(task_header) + 0x22, value);
                return value;
            }(),
            reinterpret_cast<void*>(callback),
            static_cast<unsigned long long>(ToRva(callback)),
            [&]() -> const char* {
                static thread_local std::string name;
                try {
                    name = ReadTaskName(reinterpret_cast<std::uintptr_t>(task_header));
                } catch (...) {
                    name.clear();
                }
                return name.empty() ? "<all_tasks>" : name.c_str();
            }());
    }

    MaybeWriteSummary();
}

void DetourSamplerBuilder(void* sampler_state, void* descriptor_source) {
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        texture_filtering::ShouldInstallWriter(
            g_config.anisotropic_filtering)) {
        const std::uintptr_t value_address =
            ResolveAddress(texture_filtering::kAnisotropyValueRva);
        int previous_anisotropy = 0;
        int applied_anisotropy = 0;
        const bool read_previous =
            SafeRead(value_address, previous_anisotropy);
        const bool wrote_requested =
            read_previous &&
            (previous_anisotropy == g_config.anisotropic_filtering ||
             SafeWrite(value_address, g_config.anisotropic_filtering));
        const bool verified =
            wrote_requested && SafeRead(value_address, applied_anisotropy) &&
            applied_anisotropy == g_config.anisotropic_filtering;
        bool expected = false;
        if (verified &&
            g_sampler_builder_first_verified_invocation_logged
                .compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed)) {
            log::InfoF(
                "texture_filtering sampler_builder_first_verified_invocation=1 "
                "previous_anisotropy=%d anisotropy=%d",
                previous_anisotropy, applied_anisotropy);
        } else if (!verified &&
                   g_sampler_builder_failure_logged.compare_exchange_strong(
                       expected, true, std::memory_order_relaxed)) {
            log::WarnF(
                "texture_filtering sampler_builder_apply_failed=1 "
                "read_previous=%d previous_anisotropy=%d requested=%d",
                read_previous ? 1 : 0, previous_anisotropy,
                g_config.anisotropic_filtering);
        }
    }

    g_sampler_builder_original(sampler_state, descriptor_source);
}

void DetourAnisotropyWriter(int stock_exponent) {
    const bool behavior_active =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const int effective_exponent = behavior_active
        ? texture_filtering::ResolveWriterExponent(
              stock_exponent, g_config.anisotropic_filtering)
        : stock_exponent;
    g_anisotropy_writer_original(effective_exponent);

    if (behavior_active &&
        texture_filtering::ShouldInstallWriter(
            g_config.anisotropic_filtering)) {
        int applied_anisotropy = 0;
        const bool verified = SafeRead(
            ResolveAddress(texture_filtering::kAnisotropyValueRva),
            applied_anisotropy) &&
            applied_anisotropy == g_config.anisotropic_filtering;
        bool expected = false;
        if (verified &&
            g_anisotropy_writer_first_verified_invocation_logged
                .compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed)) {
            log::InfoF(
                "texture_filtering writer_first_verified_invocation=1 "
                "stock_exponent=%d effective_exponent=%d anisotropy=%d",
                stock_exponent, effective_exponent, applied_anisotropy);
        }
    }
}

std::uint64_t DetourPcFileRead(void* device, void* file, void* buffer, std::uint64_t byte_count) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_read_original(device, file, buffer, byte_count);
    }
    return qfile_io::ReadFileDevice(device, file, buffer, byte_count);
}

bool DetourPcFileSeek(void* device, void* file, std::uint32_t origin, std::int64_t offset) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_seek_original(device, file, origin, offset);
    }
    return qfile_io::SeekFileDevice(device, file, origin, offset);
}

std::uint64_t DetourPcFileTell(void* device, void* file) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_tell_original(device, file);
    }
    return qfile_io::TellFileDevice(device, file);
}

std::uint64_t DetourPcFileSize(void* device, void* file) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_size_original(device, file);
    }
    return qfile_io::SizeFileDevice(device, file);
}

std::uint64_t DetourQFileReadAt(void* file, void* buffer, std::uint64_t byte_count,
                                std::int64_t offset, std::uint32_t origin) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_qfile_read_at_original(file, buffer, byte_count, offset, origin);
    }
    return qfile_io::ReadAt(g_qfile_ready, file, buffer, byte_count, offset, origin);
}

std::uint64_t DetourQFileWriteAt(void* file, const void* buffer, std::uint64_t byte_count,
                                 std::int64_t offset, std::uint32_t origin, bool* disk_full) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_qfile_write_at_original(file, buffer, byte_count, offset, origin, disk_full);
    }
    return qfile_io::WriteAt(g_qfile_ready, file, buffer, byte_count, offset, origin, disk_full);
}

// Engine-owned streaming buffers can become stale while an asynchronous load
// is being cancelled.  Keep the policy code allocation-free and zero-copy,
// but isolate every raw buffer access behind a non-inlined SEH boundary.  On
// x64 this adds no per-byte validation work to the valid path.
__declspec(noinline) qcmp::Result DecodeQcmpSafely(
    void* source,
    std::uint64_t source_size,
    void* destination,
    std::uint64_t destination_capacity,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    qcmp::Result result{};
    __try {
        result = qcmp::Decode(source, source_size, destination, destination_capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = GetExceptionCode();
        result = {qcmp::Error::BufferAccessException};
    }
    return result;
}

__declspec(noinline) bool ClearQcmpDestinationSafely(void* destination) noexcept {
    __try {
        *static_cast<std::byte*>(destination) = std::byte{0};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) chunk_stream::Result ValidateChunkStreamSafely(
    const void* buffer,
    std::size_t size,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    chunk_stream::Result result{};
    __try {
        result = chunk_stream::Validate(buffer, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = GetExceptionCode();
        result = {chunk_stream::Error::BufferAccessException};
    }
    return result;
}

std::uint64_t DetourQcmpDecompress(void* source,
                                   std::uint64_t source_size,
                                   void* destination,
                                   std::uint64_t destination_capacity,
                                   const char* label) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_qcmp_decompress_original(
            source, source_size, destination, destination_capacity, label);
    }
    unsigned long seh_code = 0;
    const qcmp::Result result = DecodeQcmpSafely(
        source, source_size, destination, destination_capacity, seh_code);
    if (!result.valid()) {
        const bool clear_attempted = destination != nullptr && destination_capacity != 0 &&
                                     destination_capacity != qcmp::kFailure;
        const bool clear_succeeded =
            !clear_attempted || ClearQcmpDestinationSafely(destination);
        if (!g_qcmp_rejection_logged.exchange(true, std::memory_order_relaxed)) {
            log::WarnF(
                "qcmp rejected reason=%s source_size=%llu destination_capacity=%llu "
                "data_offset=%u stream_end=%llu declared_output=%llu "
                "exception_code=0x%08lX destination_clear_attempted=%d "
                "destination_clear_succeeded=%d label_ptr=0x%p",
                qcmp::ErrorName(result.error),
                static_cast<unsigned long long>(source_size),
                static_cast<unsigned long long>(destination_capacity),
                result.data_offset,
                static_cast<unsigned long long>(result.stream_end),
                static_cast<unsigned long long>(result.declared_output_size),
                seh_code,
                clear_attempted ? 1 : 0,
                clear_succeeded ? 1 : 0,
                static_cast<const void*>(label));
        }
        return qcmp::kFailure;
    }
    return result.output_size;
}

void ResetArchiveEntryGuard() noexcept {
    for (ArchiveSizeCacheEntry& entry : g_archive_size_cache) {
        entry.inventory.store(0, std::memory_order_relaxed);
        entry.file.store(0, std::memory_order_relaxed);
        entry.native_handle.store(0, std::memory_order_relaxed);
        entry.resource_uid.store(0, std::memory_order_relaxed);
        entry.size.store(qfile_io::kOperationFailure, std::memory_order_relaxed);
    }
    for (std::atomic<std::uint64_t>& key : g_archive_rejection_keys) {
        key.store(0, std::memory_order_relaxed);
    }
}

std::uint64_t CachedArchiveSize(void* inventory, void* file, std::uint32_t resource_uid) noexcept {
    const HANDLE handle = qfile_io::NativeHandle(file);
    if (inventory == nullptr || file == nullptr || !qfile_io::IsUsableHandle(handle)) {
        return qfile_io::kOperationFailure;
    }

    const auto inventory_key = reinterpret_cast<std::uintptr_t>(inventory);
    const auto file_key = reinterpret_cast<std::uintptr_t>(file);
    const auto handle_key = reinterpret_cast<std::uintptr_t>(handle);
    for (const ArchiveSizeCacheEntry& entry : g_archive_size_cache) {
        if (entry.inventory.load(std::memory_order_acquire) == inventory_key &&
            entry.file.load(std::memory_order_relaxed) == file_key &&
            entry.native_handle.load(std::memory_order_relaxed) == handle_key &&
            entry.resource_uid.load(std::memory_order_relaxed) == resource_uid) {
            return entry.size.load(std::memory_order_relaxed);
        }
    }

    const std::uint64_t size = qfile_io::SizeFileDevice(nullptr, file);
    if (size == qfile_io::kOperationFailure) {
        return size;
    }

    for (ArchiveSizeCacheEntry& entry : g_archive_size_cache) {
        std::uintptr_t expected = 0;
        if (entry.inventory.compare_exchange_strong(expected, kArchiveSizeCacheClaimed,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
            entry.file.store(file_key, std::memory_order_relaxed);
            entry.native_handle.store(handle_key, std::memory_order_relaxed);
            entry.resource_uid.store(resource_uid, std::memory_order_relaxed);
            entry.size.store(size, std::memory_order_relaxed);
            entry.inventory.store(inventory_key, std::memory_order_release);
            break;
        }
    }
    return size;
}

bool MarkArchiveRejectionForLogging(std::uint32_t resource_uid, std::uint32_t entry_uid) noexcept {
    std::uint64_t key = (static_cast<std::uint64_t>(resource_uid) << 32) | entry_uid;
    if (key == 0) {
        key = 1;
    }
    const std::size_t start =
        static_cast<std::size_t>((key ^ (key >> 33)) % kArchiveSizeCacheCapacity);
    for (std::size_t probe = 0; probe < kArchiveSizeCacheCapacity; ++probe) {
        std::atomic<std::uint64_t>& slot =
            g_archive_rejection_keys[(start + probe) % kArchiveSizeCacheCapacity];
        std::uint64_t existing = slot.load(std::memory_order_relaxed);
        if (existing == key) {
            return false;
        }
        if (existing == 0 &&
            slot.compare_exchange_strong(existing, key, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void* DetourStreamFileOpen(const char* path, std::uint32_t access, bool allow_loose_file,
                           void* device, void* user_data) {
    void* const stream_file =
        g_stream_file_open_original(path, access, allow_loose_file, device, user_data);
    if (stream_file == nullptr || !g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return stream_file;
    }

    std::uint32_t kind = 0;
    void* inventory = nullptr;
    void* entry = nullptr;
    void* file = nullptr;
    std::uint32_t resource_uid = 0;
    archive_io::EntryDescriptor descriptor{};
    const std::uintptr_t stream_file_address = reinterpret_cast<std::uintptr_t>(stream_file);
    if (!SafeRead(stream_file_address + archive_io::kStreamFileKindOffset, kind) ||
        kind != archive_io::kArchiveStreamFileKind ||
        !SafeRead(stream_file_address + archive_io::kStreamFileInventoryOffset, inventory) ||
        !SafeRead(stream_file_address + archive_io::kStreamFileEntryOffset, entry) ||
        !SafeRead(stream_file_address + archive_io::kStreamFileQFileOffset, file) ||
        inventory == nullptr || entry == nullptr || file == nullptr ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(inventory) +
                      archive_io::kInventoryResourceUidOffset,
                  resource_uid) ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(entry), descriptor)) {
        return stream_file;
    }

    const std::uint64_t archive_size = CachedArchiveSize(inventory, file, resource_uid);
    if (archive_size == qfile_io::kOperationFailure) {
        return stream_file;
    }

    archive_io::ReadRange range{};
    if (archive_io::IsWithinArchive(descriptor, archive_size, &range)) {
        return stream_file;
    }

    if (MarkArchiveRejectionForLogging(resource_uid, descriptor.uid)) {
        log::WarnF("archive entry rejected path=%s inventory_uid=0x%08X entry_uid=0x%08X "
                   "offset=%llu read_size=%llu archive_size=%llu",
                   path != nullptr ? path : "<null>", resource_uid, descriptor.uid,
                   static_cast<unsigned long long>(range.offset),
                   static_cast<unsigned long long>(range.size),
                   static_cast<unsigned long long>(archive_size));
    }
    g_stream_file_close(stream_file);
    return nullptr;
}

void DetourResourceChunkDispatch(void* manager, void* buffer, std::uint32_t size) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_resource_chunk_dispatch_original(manager, buffer, size);
        return;
    }
    unsigned long seh_code = 0;
    const chunk_stream::Result result =
        ValidateChunkStreamSafely(buffer, size, seh_code);
    if (result.valid()) {
        g_resource_chunk_dispatch_original(manager, buffer, size);
        return;
    }

    if (!g_resource_chunk_rejection_logged.exchange(true, std::memory_order_relaxed)) {
        log::WarnF(
            "resource chunk stream rejected reason=%s offset=%zu total_size=%u "
            "chunk_id=0x%08X chunk_size=%d data_size=%d data_offset=%u "
            "big_file_reason=%s entry_count=%u entries_offset=%zu "
            "offending_index=%u exception_code=0x%08lX",
            chunk_stream::ErrorName(result.error),
            result.offset,
            size,
            result.header.id,
            result.header.chunk_size,
            result.header.data_size,
            result.header.data_offset,
            big_file_index::ErrorName(result.big_file_result.error),
            result.big_file_result.entry_count,
            result.big_file_result.entries_offset,
            result.big_file_result.offending_index,
            seh_code);
    }
}

void DetourScaleformTime() {
    g_scaleform_time_count.fetch_add(1);

    const std::uintptr_t provider = ReadProviderValue();
    if (provider != 0) {
        g_scaleform_provider_non_null_count.fetch_add(1);
    }

    if (provider != 0 && ShouldLogVerbose(g_scaleform_time_verbose)) {
        log::InfoF("scaleform_time provider=0x%p", reinterpret_cast<void*>(provider));
    }

    MaybeWriteSummary();
    g_scaleform_time_original();
}

void DetourScaleformInit(void* heap_desc, void* sys_alloc) {
    g_scaleform_init_count.fetch_add(1);
    g_scaleform_init_original(heap_desc, sys_alloc);

    const std::uintptr_t provider = ReadProviderValue();
    if (ShouldLogVerbose(g_scaleform_init_verbose)) {
        log::InfoF("scaleform_init heap_desc=0x%p sys_alloc=0x%p provider=0x%p",
                   heap_desc,
                   sys_alloc,
                   reinterpret_cast<void*>(provider));
    }

    MaybeWriteSummary();
}

void DetourNisSetPlayTime(void* nis_manager, float scene_time, bool sync_scene_time) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_set_play_time_original(nis_manager, scene_time, sync_scene_time);
        return;
    }
    g_nis_set_play_time_count.fetch_add(1);
    if (sync_scene_time) {
        g_nis_sync_scene_time_count.fetch_add(1);
    }

    const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
    const auto previous_manager = g_last_nis_manager_tls;
    const auto previous_instance = g_last_nis_instance_tls;
    const float previous_raw_scene_time = g_last_nis_raw_scene_time_tls;
    const float previous_applied_scene_time = g_last_nis_applied_scene_time_tls;
    const bool previous_history_valid = g_last_nis_scene_history_valid_tls;
    const CutsceneCadenceTracker previous_cadence_tracker =
        g_cutscene_cadence_tracker_tls;

    int state_before = 0;
    std::uintptr_t active_instance = 0;
    if (manager != 0) {
        SafeRead(manager + 0x44, state_before);
        SafeRead(manager + 0x10, active_instance);
    }

    const bool in_cutscene_scope = g_cutscene_flow_owner_scope_depth != 0 ||
                                   g_nis_owner_scope_depth != 0;
    const CutscenePauseState pause_state =
        g_config.fix_cutscene_scene_time_step && in_cutscene_scope
            ? ReadCutscenePauseStateSafely()
            : CutscenePauseState{};
    // History is per engine thread and per active NIS instance. A singleton
    // manager can be reused for another scene, so manager identity alone is
    // not a safe continuity signal.
    const bool scene_time_rewound =
        previous_history_valid && std::isfinite(scene_time) &&
        std::isfinite(previous_raw_scene_time) && scene_time < previous_raw_scene_time;
    const bool timeline_must_reset =
        sync_scene_time || pause_state.ui_paused || scene_time_rewound ||
        (previous_history_valid &&
         (!in_cutscene_scope || state_before != 4 || manager == 0 || active_instance == 0 ||
          previous_manager != manager || previous_instance != active_instance));
    if (timeline_must_reset) {
        ResetTrackedCutsceneTimeline();
    }
    const bool same_timeline = previous_history_valid && !timeline_must_reset;

    float current_dt = (g_nis_owner_scope_depth != 0 &&
                        g_current_nis_owner_manager == manager)
                           ? g_current_nis_owner_dt
                           : 0.0f;
    if (!IsSaneCutsceneDelta(current_dt)) {
        current_dt = g_current_cutscene_dt;
    }
    if (!IsSaneCutsceneDelta(current_dt)) {
        current_dt = g_cutscene_flow_input_dt;
    }
    if (!IsSaneCutsceneDelta(current_dt)) {
        // Keep the completed cadence thread-local.  The diagnostic atomics
        // are process-wide and may belong to another NIS worker thread.
        current_dt = g_last_cutscene_flow_completed_dt_tls;
    }
    if (timeline_must_reset && active_instance != 0 && in_cutscene_scope && state_before == 4 &&
        !pause_state.ui_paused && !sync_scene_time) {
        // The owner sample was observed before SetPlayTime exposed an identity
        // change or raw rewind. Re-seed after clearing the old timeline so the
        // new baseline retains this frame's cadence immediately.
        current_dt = TrackCutsceneCadence(current_dt);
    } else {
        current_dt = ResolveTrackedCutsceneBaseDelta(
            current_dt, g_cutscene_cadence_tracker_tls.stable_delta, g_config.cutscene_fps);
    }
    // Keep the scene-time resolver aware of a recognized 30/60-Hz candidate
    // while the cadence tracker is still inside its hysteresis window.  A
    // target such as 62 FPS is close enough to 60 that a proximity-only
    // live/scene comparison would otherwise suppress the correction before
    // the candidate is either adopted or rejected.
    const bool legacy_fallback_pending =
        IsLegacyFallbackPending(g_cutscene_cadence_tracker_tls, g_config.cutscene_fps);

    const auto scene_decision = ResolveCutsceneSceneTime(
        in_cutscene_scope && state_before == 4,
        pause_state.ui_paused,
        g_config.fix_cutscene_scene_time_step && same_timeline,
        sync_scene_time,
        previous_raw_scene_time,
        previous_applied_scene_time,
        scene_time,
        current_dt,
        g_config.cutscene_fps,
        legacy_fallback_pending);
    const float applied_scene_time = scene_decision.applied_scene_time;
    const bool scene_time_fixed = scene_decision.corrected;
    const bool timing_repaired = scene_decision.repaired_timing;
    if (timing_repaired) {
        g_nis_scene_time_fix_count.fetch_add(1);
    }
    const float outer_dt = current_dt;

    float delta = 0.0f;
    if (same_timeline && applied_scene_time >= previous_applied_scene_time) {
        delta = applied_scene_time - previous_applied_scene_time;
        if (delta >= 0.0f && delta < 0.0005f) {
            g_nis_delta_zero_count.fetch_add(1);
        } else if (delta > 0.0f && delta < 0.2f) {
            if (delta >= 0.030f && delta <= 0.036f) {
                g_nis_delta_30hz_count.fetch_add(1);
            } else if (IsApprox60HzDelta(delta)) {
                g_nis_delta_60hz_count.fetch_add(1);
            } else {
                g_nis_delta_other_count.fetch_add(1);
            }
        }
    }

    if (g_nis_play_scope_depth != 0) {
        ++g_nis_play_scope_calls;
        if (delta >= 0.0f && delta < 0.0005f && same_timeline) {
            ++g_nis_play_scope_zero_delta_calls;
        } else if (delta > 0.0f) {
            ++g_nis_play_scope_advanced_calls;
        }
    }

    if (g_nis_owner_scope_depth != 0) {
        ++g_nis_owner_scope_calls;
        if (delta >= 0.0f && delta < 0.0005f && same_timeline) {
            ++g_nis_owner_scope_zero_delta_calls;
        } else if (delta > 0.0f) {
            ++g_nis_owner_scope_advanced_calls;
        }
    }

    g_last_nis_scene_time_bits.store(FloatToBits(applied_scene_time));
    g_last_nis_scene_delta_bits.store(FloatToBits(delta));
    g_last_nis_manager.store(manager);
    g_last_nis_manager_tls = manager;
    g_last_nis_instance_tls = active_instance;
    g_last_nis_raw_scene_time_tls = scene_time;
    g_last_nis_applied_scene_time_tls = applied_scene_time;
    g_last_nis_scene_history_valid_tls =
        g_config.fix_cutscene_scene_time_step && in_cutscene_scope && state_before == 4 &&
        active_instance != 0 && !pause_state.ui_paused && !sync_scene_time &&
        std::isfinite(scene_time) && std::isfinite(applied_scene_time) &&
        CanRetainCutsceneTimelineHistory(
            same_timeline, previous_applied_scene_time, applied_scene_time);

    if (ShouldLogVerbose(g_nis_set_play_time_verbose)) {
        const char* bucket = ClassifySmallDelta(delta);

        log::InfoF(
            "nis_set_play_time manager=0x%p scene_time=%.6f applied_scene_time=%.6f delta=%.6f sync=%d state=%d active_instance=0x%p bucket=%s scene_fix=%d scene_repair=%d outer_dt=%.6f",
            nis_manager,
            scene_time,
            applied_scene_time,
            delta,
            sync_scene_time ? 1 : 0,
            state_before,
            reinterpret_cast<void*>(active_instance),
            bucket,
            scene_time_fixed ? 1 : 0,
            timing_repaired ? 1 : 0,
            outer_dt);
    }

    MaybeWriteSummary();
    bool original_completed = false;
    __try {
        g_nis_set_play_time_original(nis_manager, applied_scene_time, sync_scene_time);
        original_completed = true;
    } __finally {
        if (!original_completed) {
            g_last_nis_manager_tls = previous_manager;
            g_last_nis_instance_tls = previous_instance;
            g_last_nis_raw_scene_time_tls = previous_raw_scene_time;
            g_last_nis_applied_scene_time_tls = previous_applied_scene_time;
            g_last_nis_scene_history_valid_tls = previous_history_valid;
            g_cutscene_cadence_tracker_tls = previous_cadence_tracker;
        }
    }
}

void DetourNisPlay(void* nis_manager) {
    g_nis_play_count.fetch_add(1);

    int state_before = 0;
    int phase_before = 0;
    std::uintptr_t active_instance_before = 0;
    int active_mode_before = 0;
    if (nis_manager != nullptr) {
        const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
        SafeRead(manager + 0x44, state_before);
        SafeRead(manager + 0x5C, phase_before);
        SafeRead(manager + 0x10, active_instance_before);
        if (active_instance_before != 0) {
            SafeRead(active_instance_before + 0x28, active_mode_before);
        }
    }

    const unsigned int previous_depth = g_nis_play_scope_depth++;
    if (previous_depth == 0) {
        g_nis_play_scope_calls = 0;
        g_nis_play_scope_advanced_calls = 0;
        g_nis_play_scope_zero_delta_calls = 0;
    }

    bool original_completed = false;
    __try {
        g_nis_play_original(nis_manager);
        original_completed = true;
    } __finally {
        if (!original_completed) {
            // Do not let a structured exception in the stock NIS routine
            // strand this worker in a permanently nested timing scope.  The
            // exception continues through the engine, while the TLS state is
            // restored before the next cutscene callback on this thread.
            if (g_nis_play_scope_depth != 0) {
                --g_nis_play_scope_depth;
            }
            if (g_nis_play_scope_depth == 0) {
                g_nis_play_scope_calls = 0;
                g_nis_play_scope_advanced_calls = 0;
                g_nis_play_scope_zero_delta_calls = 0;
            }
        }
    }

    const unsigned int inner_calls = g_nis_play_scope_calls;
    const unsigned int advanced_calls = g_nis_play_scope_advanced_calls;
    const unsigned int zero_delta_calls = g_nis_play_scope_zero_delta_calls;
    --g_nis_play_scope_depth;
    if (g_nis_play_scope_depth == 0) {
        g_nis_play_scope_calls = 0;
        g_nis_play_scope_advanced_calls = 0;
        g_nis_play_scope_zero_delta_calls = 0;
    }

    if (advanced_calls != 0) {
        g_nis_play_advanced_count.fetch_add(1);
    } else if (inner_calls != 0) {
        g_nis_play_repeat_count.fetch_add(1);
    }

    if (inner_calls > 1) {
        g_nis_play_multi_tick_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_play_verbose) || zero_delta_calls != 0 || inner_calls == 0 ||
        inner_calls > 1) {
        int state_after = 0;
        int phase_after = 0;
        std::uintptr_t active_instance_after = 0;
        int active_mode_after = 0;
        if (nis_manager != nullptr) {
            const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
            SafeRead(manager + 0x44, state_after);
            SafeRead(manager + 0x5C, phase_after);
            SafeRead(manager + 0x10, active_instance_after);
            if (active_instance_after != 0) {
                SafeRead(active_instance_after + 0x28, active_mode_after);
            }
        }

        log::InfoF(
            "nis_play manager=0x%p inner_calls=%u advanced=%u zero_delta=%u "
            "state_before=%d state_after=%d phase_before=%d phase_after=%d "
            "active_before=0x%p mode_before=%d active_after=0x%p mode_after=%d",
            nis_manager,
            inner_calls,
            advanced_calls,
            zero_delta_calls,
            state_before,
            state_after,
            phase_before,
            phase_after,
            reinterpret_cast<void*>(active_instance_before),
            active_mode_before,
            reinterpret_cast<void*>(active_instance_after),
            active_mode_after);
    }

    MaybeWriteSummary();
}

unsigned char DetourNisBootstrap(void* nis_manager, void* scene_asset) {
    g_nis_bootstrap_count.fetch_add(1);

    const unsigned char result = g_nis_bootstrap_original(nis_manager, scene_asset);

    int state = 0;
    std::uint32_t timeout_bits = 0;
    std::uintptr_t active_instance = 0;
    if (nis_manager != nullptr) {
        const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
        SafeRead(manager + 0x44, state);
        SafeRead(manager + 0x48, timeout_bits);
        SafeRead(manager + 0x10, active_instance);
    }

    if (result == 0) {
        g_nis_bootstrap_fail_count.fetch_add(1);
    } else if (state == 1) {
        g_nis_bootstrap_state1_count.fetch_add(1);
    } else if (state == 2) {
        g_nis_bootstrap_state2_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_bootstrap_verbose) || result == 0) {
        log::InfoF(
            "nis_bootstrap manager=0x%p scene_asset=0x%p result=%u state=%d timeout=%.6f active_instance=0x%p",
            nis_manager,
            scene_asset,
            static_cast<unsigned int>(result),
            state,
            BitsToFloat(timeout_bits),
            reinterpret_cast<void*>(active_instance));
    }

    MaybeWriteSummary();
    return result;
}

void DetourNisOwner(void* nis_manager, float delta_seconds) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_owner_original(nis_manager, delta_seconds);
        return;
    }
    g_nis_owner_count.fetch_add(1);
    g_last_nis_owner_dt_bits.store(FloatToBits(delta_seconds));

    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_nis_owner_dt_zero_count.fetch_add(1);
    } else if (delta_seconds > 0.0f && delta_seconds < 0.2f) {
        if (delta_seconds >= 0.030f && delta_seconds <= 0.036f) {
            g_nis_owner_dt_30hz_count.fetch_add(1);
        } else if (delta_seconds >= 0.015f && delta_seconds <= 0.019f) {
            g_nis_owner_dt_60hz_count.fetch_add(1);
        } else {
            g_nis_owner_dt_other_count.fetch_add(1);
        }
    }

    int state_before = 0;
    int phase_before = 0;
    std::uintptr_t active_instance_before = 0;
    if (nis_manager != nullptr) {
        const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
        SafeRead(manager + 0x44, state_before);
        SafeRead(manager + 0x5C, phase_before);
        SafeRead(manager + 0x10, active_instance_before);
    }

    const unsigned int previous_depth = g_nis_owner_scope_depth++;
    const std::uintptr_t previous_nis_owner_manager =
        g_current_nis_owner_manager;
    g_current_nis_owner_manager = reinterpret_cast<std::uintptr_t>(nis_manager);
    if (previous_depth == 0) {
        g_nis_owner_scope_calls = 0;
        g_nis_owner_scope_advanced_calls = 0;
        g_nis_owner_scope_zero_delta_calls = 0;
    }

    const float previous_nis_owner_dt = g_current_nis_owner_dt;
    g_current_nis_owner_dt = delta_seconds;
    __try {
        g_nis_owner_original(nis_manager, delta_seconds);
    } __finally {
        // Detours can be entered by engine code that raises a structured
        // exception.  Always restore the TLS cadence so a later frame cannot
        // inherit a stale owner delta.
        g_current_nis_owner_dt = previous_nis_owner_dt;
        // The previous manager already lives in this recursive call's stack
        // frame, so restoring it does not need a fixed-size TLS side stack.
        // Deep but valid nesting can no longer overflow and lose its owner.
        g_current_nis_owner_manager = previous_nis_owner_manager;
        if (g_nis_owner_scope_depth != 0) {
            --g_nis_owner_scope_depth;
        }
    }

    const unsigned int inner_calls = g_nis_owner_scope_calls;
    const unsigned int advanced_calls = g_nis_owner_scope_advanced_calls;
    const unsigned int zero_delta_calls = g_nis_owner_scope_zero_delta_calls;
    if (g_nis_owner_scope_depth == 0) {
        g_nis_owner_scope_calls = 0;
        g_nis_owner_scope_advanced_calls = 0;
        g_nis_owner_scope_zero_delta_calls = 0;
    }

    if (advanced_calls != 0) {
        g_nis_owner_advanced_count.fetch_add(1);
    } else if (inner_calls != 0) {
        g_nis_owner_repeat_count.fetch_add(1);
    }

    if (inner_calls > 1) {
        g_nis_owner_multi_tick_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_owner_verbose)) {
        int state_after = 0;
        int phase_after = 0;
        std::uintptr_t active_instance_after = 0;
        if (nis_manager != nullptr) {
            const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
            SafeRead(manager + 0x44, state_after);
            SafeRead(manager + 0x5C, phase_after);
            SafeRead(manager + 0x10, active_instance_after);
        }

        log::InfoF(
            "nis_owner manager=0x%p dt=%.6f dt_bucket=%s inner_calls=%u advanced=%u zero_delta=%u "
            "state_before=%d state_after=%d phase_before=%d phase_after=%d active_before=0x%p active_after=0x%p",
            nis_manager,
            delta_seconds,
            ClassifySmallDelta(delta_seconds),
            inner_calls,
            advanced_calls,
            zero_delta_calls,
            state_before,
            state_after,
            phase_before,
            phase_after,
            reinterpret_cast<void*>(active_instance_before),
            reinterpret_cast<void*>(active_instance_after));
    }

    MaybeWriteSummary();
}

void DetourNisActorSetup(std::uintptr_t actor_state, std::uintptr_t actor_target) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_actor_setup_original(actor_state, actor_target);
        return;
    }
    g_nis_actor_setup_count.fetch_add(1);

    const nisprobe::SetupResult lifecycle = TrackNisActorSetupLocked(actor_state, actor_target);

    if (lifecycle.duplicate) {
        g_nis_actor_setup_duplicate_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_actor_state_verbose) || actor_state == 0 || actor_target == 0 ||
        lifecycle.duplicate) {
        log::InfoF("nis_actor_setup state=0x%p target=0x%p duplicate=%d active=%llu setup_count=%u "
                   "restore_count=%u last_target=0x%p",
                   reinterpret_cast<void*>(actor_state),
                   reinterpret_cast<void*>(actor_target),
                   lifecycle.duplicate ? 1 : 0,
                   lifecycle.active_count,
                   lifecycle.setup_count,
                   lifecycle.restore_count,
                   reinterpret_cast<void*>(lifecycle.last_target));
    }

    bool original_completed = false;
    __try {
        g_nis_actor_setup_original(actor_state, actor_target);
        original_completed = true;
    } __finally {
        if (!original_completed) {
            // Tracking is updated before the stock call so a nested restore can
            // see the actor.  If stock setup faults, discard the optional table
            // rather than leaving a phantom live actor that later suppresses a
            // legitimate restore.
            ResetNisActorTrackerLocked();
        }
    }
    MaybeWriteSummary();
}

void DetourNisActorRestore(std::uintptr_t actor_state) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_actor_restore_original(actor_state);
        return;
    }
    g_nis_actor_restore_count.fetch_add(1);

    const nisprobe::RestoreResult lifecycle = TrackNisActorRestoreLocked(actor_state);

    const bool tracked = lifecycle.disposition == nisprobe::RestoreDisposition::tracked;
    const bool duplicate = lifecycle.disposition == nisprobe::RestoreDisposition::duplicate;
    const bool never_seen = lifecycle.disposition == nisprobe::RestoreDisposition::never_seen;
    const auto forwarding = nisprobe::ResolveRestoreForwardingDecision(
        lifecycle.disposition, g_config.fix_nis_actor_restore_duplicates);

    if (duplicate) {
        g_nis_actor_restore_duplicate_count.fetch_add(1);
    }
    if (never_seen) {
        g_nis_actor_restore_never_seen_count.fetch_add(1);
    }
    if (!tracked) {
        g_nis_actor_restore_untracked_count.fetch_add(1);
    }
    if (forwarding.suppressed_duplicate) {
        g_nis_actor_restore_suppressed_count.fetch_add(1);
    }

    const bool should_log_restore = ShouldLogVerbose(g_nis_actor_state_verbose) || actor_state == 0 ||
                                    never_seen || (duplicate && g_config.hook_nis_actor_state);
    if (should_log_restore) {
        log::WarnF("nis_actor_restore state=0x%p disposition=%s tracked=%d duplicate=%d active=%llu "
                   "setup_count=%u restore_count=%u last_target=0x%p call_original=%d suppressed=%d",
                   reinterpret_cast<void*>(actor_state),
                   nisprobe::DescribeDisposition(lifecycle.disposition),
                   tracked ? 1 : 0,
                   duplicate ? 1 : 0,
                   lifecycle.active_count,
                   lifecycle.setup_count,
                   lifecycle.restore_count,
                   reinterpret_cast<void*>(lifecycle.last_target),
                   forwarding.call_original ? 1 : 0,
                   forwarding.suppressed_duplicate ? 1 : 0);
    }

    if (forwarding.call_original) {
        bool original_completed = false;
        __try {
            g_nis_actor_restore_original(actor_state);
            original_completed = true;
        } __finally {
            if (!original_completed) {
                // A failed stock restore must not make the next real restore
                // look like a duplicate.  Resetting the optional tracker is a
                // conservative fail-open rollback that is safe across threads.
                ResetNisActorTrackerLocked();
            }
        }
    }
    MaybeWriteSummary();
}

void DetourTwitchStateTick(std::uintptr_t owner) {
    g_twitch_state_tick_count.fetch_add(1);

    const int state_before = ReadTwitchState(owner);
    const std::uintptr_t handle_before = ReadTwitchHandle(owner);
    const unsigned int flag_754_before = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_before = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_before = ReadTwitchFlagByte(owner, 0x756);

    g_twitch_state_tick_original(owner);

    const int state_after = ReadTwitchState(owner);
    const std::uintptr_t handle_after = ReadTwitchHandle(owner);
    const unsigned int flag_754_after = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_after = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_after = ReadTwitchFlagByte(owner, 0x756);
    const bool interesting = owner == 0 || state_before != state_after || state_before == 3 ||
                             state_before == 5 || state_before == 7;

    if (interesting || ShouldLogVerbose(g_twitch_probe_verbose)) {
        log::InfoF(
            "twitch_tick owner=0x%p state_before=%d state_after=%d handle_before=0x%p handle_after=0x%p "
            "flags_before=%u/%u/%u flags_after=%u/%u/%u",
            reinterpret_cast<void*>(owner),
            state_before,
            state_after,
            reinterpret_cast<void*>(handle_before),
            reinterpret_cast<void*>(handle_after),
            flag_754_before,
            flag_755_before,
            flag_756_before,
            flag_754_after,
            flag_755_after,
            flag_756_after);
    }

    MaybeWriteSummary();
}

void DetourTwitchLoginCallback(int result, std::uintptr_t owner) {
    g_twitch_login_callback_count.fetch_add(1);
    const bool login_failure = result >= 1;
    if (login_failure) {
        g_twitch_login_failure_count.fetch_add(1);
    }

    const int state_before = ReadTwitchState(owner);
    const std::uintptr_t handle_before = ReadTwitchHandle(owner);
    const unsigned int flag_754_before = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_before = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_before = ReadTwitchFlagByte(owner, 0x756);

    g_twitch_login_callback_original(result, owner);

    const int state_after = ReadTwitchState(owner);
    const std::uintptr_t handle_after = ReadTwitchHandle(owner);
    const unsigned int flag_754_after = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_after = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_after = ReadTwitchFlagByte(owner, 0x756);

    if (login_failure || owner == 0 || state_before != state_after ||
        ShouldLogVerbose(g_twitch_probe_verbose)) {
        log::WarnF(
            "twitch_login_callback owner=0x%p result=%d failure=%d state_before=%d state_after=%d "
            "handle_before=0x%p handle_after=0x%p flags_before=%u/%u/%u flags_after=%u/%u/%u",
            reinterpret_cast<void*>(owner),
            result,
            login_failure ? 1 : 0,
            state_before,
            state_after,
            reinterpret_cast<void*>(handle_before),
            reinterpret_cast<void*>(handle_after),
            flag_754_before,
            flag_755_before,
            flag_756_before,
            flag_754_after,
            flag_755_after,
            flag_756_after);
    }

    MaybeWriteSummary();
}

bool ApplyOriginalFog(bool force_write) {
    if (!g_config.restore_original_fog || g_volumetric_fog_intensity == 0) {
        return false;
    }
    if (!force_write &&
        g_original_fog_frame_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    float previous_intensity = 0.0f;
    const bool read_previous = SafeRead(g_volumetric_fog_intensity, previous_intensity);
    const bool wrote_original =
        read_previous &&
        (previous_intensity == fog_restoration::kOriginalGameIntensity ||
         SafeWrite(g_volumetric_fog_intensity, fog_restoration::kOriginalGameIntensity));
    float verified_intensity = 0.0f;
    const bool verified =
        wrote_original && SafeRead(g_volumetric_fog_intensity, verified_intensity) &&
        verified_intensity == fog_restoration::kOriginalGameIntensity;
    if (!verified) {
        if (!g_original_fog_failure_logged.exchange(true, std::memory_order_acq_rel)) {
            log::WarnF("original_fog_and_neon_restore apply_failed target_rva=0x%llX",
                       static_cast<unsigned long long>(
                           fog_restoration::kIntensityRva));
        }
        return false;
    }

    if (!g_original_fog_applied.exchange(true, std::memory_order_acq_rel)) {
        log::InfoF("original_fog_and_neon_restore applied=1 previous_intensity=%.3f "
                   "restored_intensity=%.3f target_rva=0x%llX",
                   previous_intensity,
                   verified_intensity,
                   static_cast<unsigned long long>(fog_restoration::kIntensityRva));
    }
    if (!force_write) {
        g_original_fog_frame_initialized.store(true, std::memory_order_release);
    }
    return true;
}

void DetourVolumetricFogSetter(void* script_context) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        !g_config.restore_original_fog) {
        g_volumetric_fog_setter_original(script_context);
        return;
    }

    if (!g_original_fog_setter_logged.exchange(true, std::memory_order_acq_rel)) {
        log::Info("original_fog_and_neon_restore blocked_de_volumetric_fog_setter=1");
    }
    (void)ApplyOriginalFog(true);
}

void DetourFrameFlow(float delta_seconds, void* callback) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_frame_flow_original(delta_seconds, callback);
        return;
    }
    (void)ApplyOriginalFog(false);
    PollGraphicsDebugKeys();
    MaybeApplyRumbleOverride();

    g_frame_flow_count.fetch_add(1);
    g_last_frame_flow_dt_bits.store(FloatToBits(delta_seconds));

    const char* bucket = ClassifySmallDelta(delta_seconds);
    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_frame_flow_dt_zero_count.fetch_add(1);
    } else if (delta_seconds >= 0.015f && delta_seconds <= 0.019f) {
        g_frame_flow_dt_60hz_count.fetch_add(1);
    } else if (delta_seconds > 0.0f && delta_seconds < 0.2f) {
        g_frame_flow_dt_other_count.fetch_add(1);
    }

    const bool from_cutscene_scope = g_cutscene_flow_owner_scope_depth != 0;
    // UI/simtime queries are SEH-protected but still relatively expensive at
    // high refresh rates. They are only relevant while the cutscene owner is
    // active; ordinary gameplay keeps the fast path free of those calls.
    const CutscenePauseState pause_state =
        from_cutscene_scope ? ReadCutscenePauseStateSafely() : CutscenePauseState{};
    const bool game_paused = pause_state.ui_paused || pause_state.simtime_paused;
    if (from_cutscene_scope && pause_state.ui_paused) {
        ResetTrackedCutsceneTimeline();
    }
    if (from_cutscene_scope) {
        g_frame_flow_from_cutscene_owner_count.fetch_add(1);
    }

    const float tracked_cutscene_input_dt =
        from_cutscene_scope && !pause_state.ui_paused
            ? ResolveTrackedCutsceneBaseDelta(g_cutscene_flow_input_dt,
                                              g_cutscene_cadence_tracker_tls.stable_delta,
                                              g_config.cutscene_fps)
            : g_cutscene_flow_input_dt;

    const CutsceneFrameflowDecision cutscene_decision = ResolveCutsceneFrameflowDelta(
        from_cutscene_scope,
        pause_state,
        g_config.fix_cutscene_zero_dt,
        g_config.cutscene_fps,
        delta_seconds,
        tracked_cutscene_input_dt);
    float forwarded_dt = cutscene_decision.forwarded_dt;
    const bool applied_cutscene_zero_fix = cutscene_decision.applied_zero_dt_fix;
    if (applied_cutscene_zero_fix) {
        g_cutscene_zero_dt_override_count.fetch_add(1);
        if (cutscene_decision.applied_while_game_paused) {
            g_cutscene_zero_dt_pause_override_count.fetch_add(1);
        }
    }

    if (from_cutscene_scope) {
        g_cutscene_flow_forwarded = true;
        g_cutscene_flow_forwarded_dt = forwarded_dt;
        g_current_cutscene_dt = forwarded_dt;
        g_cutscene_flow_forwarded_callback = callback;
    }

    if (((from_cutscene_scope && applied_cutscene_zero_fix) ||
         (delta_seconds >= 0.0f && delta_seconds < 0.0005f)) &&
        ShouldLogVerbose(g_frame_flow_verbose)) {
        log::InfoF(
            "frame_flow dt=%.6f applied_dt=%.6f bucket=%s callback=0x%p callback_rva=0x%llX "
            "from_cutscene=%d paused=%d ui_paused=%d simtime_paused=%d "
            "cutscene_fix=%d cutscene_fix_paused=%d cutscene_fix_simtime=%d cutscene_input_dt=%.6f",
            delta_seconds,
            forwarded_dt,
            bucket,
            callback,
            static_cast<unsigned long long>(ToRva(reinterpret_cast<std::uintptr_t>(callback))),
            from_cutscene_scope ? 1 : 0,
            game_paused ? 1 : 0,
            pause_state.ui_paused ? 1 : 0,
            pause_state.simtime_paused ? 1 : 0,
            applied_cutscene_zero_fix ? 1 : 0,
            cutscene_decision.applied_while_game_paused ? 1 : 0,
            cutscene_decision.applied_while_simtime_paused ? 1 : 0,
            tracked_cutscene_input_dt);
    }

    g_frame_flow_original(forwarded_dt, callback);
    cut_content::OnGameThreadFrame();
    MaybeWriteSummary();
}

void DetourCutsceneFlowOwner(void* owner, float delta_seconds) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_cutscene_flow_owner_original(owner, delta_seconds);
        return;
    }
    g_cutscene_flow_owner_count.fetch_add(1);
    g_last_cutscene_flow_input_dt_bits.store(FloatToBits(delta_seconds));

    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_cutscene_flow_owner_dt_zero_count.fetch_add(1);
    }

    const auto owner_ptr = reinterpret_cast<std::uintptr_t>(owner);
    const int stage_before = ReadCutsceneFlowStage(owner_ptr);
    const unsigned int flags_before = ReadCutsceneFlowFlags(owner_ptr);

    const unsigned int depth_before = g_cutscene_flow_owner_scope_depth++;
    const bool outermost = depth_before == 0;
    const bool have_timing_slot = depth_before < g_cutscene_owner_timing_stack.size();
    if (outermost && g_last_cutscene_owner_tls != owner_ptr) {
        g_last_cutscene_owner_tls = owner_ptr;
        ResetTrackedCutsceneTimeline();
    }
    if (outermost) {
        TrackCutsceneCadence(delta_seconds);
    }
    if (have_timing_slot) {
        g_cutscene_owner_timing_stack[depth_before] = CutsceneOwnerTimingState{
            g_cutscene_flow_input_dt,
            g_current_cutscene_dt,
            g_cutscene_flow_forwarded,
            g_cutscene_flow_forwarded_dt,
            g_cutscene_flow_forwarded_callback};
    }
    if (outermost) {
        g_cutscene_flow_forwarded = false;
        g_cutscene_flow_forwarded_dt = 0.0f;
        g_cutscene_flow_forwarded_callback = nullptr;
        g_last_cutscene_flow_completed_dt_tls = 0.0f;
        // Do not let an early NIS call in a new scope consume the previous
        // owner's completed frame.
        g_last_cutscene_flow_forwarded_dt_bits.store(0);
    } else if (have_timing_slot) {
        // Isolate nested owner state.  Its result is merged into the parent
        // only after the nested original returns.
        g_cutscene_flow_forwarded = false;
        g_cutscene_flow_forwarded_dt = 0.0f;
        g_cutscene_flow_forwarded_callback = nullptr;
    }
    // Each owner invocation has its own live cadence.  The previous code kept
    // the outer owner's value during re-entry, which made nested NIS/frameflow
    // calls consume a stale frame delta.
    g_cutscene_flow_input_dt = delta_seconds;
    g_current_cutscene_dt = 0.0f;

    bool forwarded = false;
    float forwarded_dt = 0.0f;
    void* forwarded_callback = nullptr;
    __try {
        g_cutscene_flow_owner_original(owner, delta_seconds);
    } __finally {
        forwarded = g_cutscene_flow_forwarded;
        forwarded_dt = g_cutscene_flow_forwarded_dt;
        forwarded_callback = g_cutscene_flow_forwarded_callback;

        if (g_cutscene_flow_owner_scope_depth != 0) {
            --g_cutscene_flow_owner_scope_depth;
        }
        if (have_timing_slot) {
            const CutsceneOwnerTimingState previous = g_cutscene_owner_timing_stack[depth_before];
            g_cutscene_flow_input_dt = previous.input_dt;
            g_current_cutscene_dt = previous.current_dt;
            g_cutscene_flow_forwarded = previous.forwarded;
            g_cutscene_flow_forwarded_dt = previous.forwarded_dt;
            g_cutscene_flow_forwarded_callback = previous.forwarded_callback;

            if (!outermost && forwarded) {
                g_cutscene_flow_forwarded = true;
                g_cutscene_flow_forwarded_dt = forwarded_dt;
                g_cutscene_flow_forwarded_callback = forwarded_callback;
            }
        } else {
            // Defensive overflow path: keep the detour scope balanced and do
            // not leave a nested cadence active indefinitely.
            g_cutscene_flow_input_dt = 0.0f;
            g_current_cutscene_dt = 0.0f;
            g_cutscene_flow_forwarded = false;
            g_cutscene_flow_forwarded_dt = 0.0f;
            g_cutscene_flow_forwarded_callback = nullptr;
        }
        if (g_cutscene_flow_owner_scope_depth == 0) {
            g_last_cutscene_flow_completed_dt_tls = outermost && forwarded ? forwarded_dt : 0.0f;
            g_cutscene_flow_forwarded = false;
            g_cutscene_flow_forwarded_dt = 0.0f;
            g_cutscene_flow_forwarded_callback = nullptr;
            g_cutscene_flow_input_dt = 0.0f;
            g_current_cutscene_dt = 0.0f;
        }
    }

    const int stage_after = ReadCutsceneFlowStage(owner_ptr);
    const unsigned int flags_after = ReadCutsceneFlowFlags(owner_ptr);

    if (outermost) {
        // Publish zero as well as non-zero values; retaining the previous
        // frame here made NIS corrections fire during a paused/outro frame.
        g_last_cutscene_flow_forwarded_dt_bits.store(FloatToBits(forwarded ? forwarded_dt : 0.0f));
    }
    if (outermost && forwarded) {
        if (forwarded_dt >= 0.0f && forwarded_dt < 0.0005f) {
            g_cutscene_flow_owner_forwarded_zero_count.fetch_add(1);
        } else if (forwarded_dt >= 0.015f && forwarded_dt <= 0.019f) {
            g_cutscene_flow_owner_forwarded_60hz_count.fetch_add(1);
        } else {
            g_cutscene_flow_owner_forwarded_other_count.fetch_add(1);
        }
    }

    const bool verbose_cutscene_flow = ShouldLogVerbose(g_cutscene_flow_owner_verbose);
    if (verbose_cutscene_flow) {
        log::InfoF(
            "cutscene_flow_owner owner=0x%p input_dt=%.6f forwarded=%d forwarded_dt=%.6f "
            "forwarded_callback=0x%p forwarded_rva=0x%llX stage_before=%d stage_after=%d "
            "flags_before=0x%X flags_after=0x%X",
            owner,
            delta_seconds,
            outermost && forwarded ? 1 : 0,
            outermost && forwarded ? forwarded_dt : 0.0f,
            outermost ? forwarded_callback : nullptr,
            static_cast<unsigned long long>(
                ToRva(reinterpret_cast<std::uintptr_t>(outermost ? forwarded_callback : nullptr))),
            stage_before,
            stage_after,
            flags_before,
            flags_after);
    }

    MaybeWriteSummary();
}

void DetourFogSlicingMode(void* time_of_day_manager, int update_interval) {
    // The stock setter stores the interval and immediately divides by
    // (interval * 8).  MinHook can expose this detour briefly while the
    // bootstrap transaction is still committing, so the not-ready fast path
    // must receive the same divisor guard as the normal path.
    const int clamped_interval =
        ClampFogSlicingInterval(update_interval, g_config.min_fog_slicing_interval);
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_fog_slicing_mode_original(time_of_day_manager, clamped_interval);
        return;
    }
    g_fog_slicing_mode_count.fetch_add(1);

    if (clamped_interval != update_interval) {
        g_fog_slicing_clamp_count.fetch_add(1);
        bool expected = false;
        if (g_fog_clamp_warning_emitted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            log::WarnF(
                "fog_slicing_clamp manager=0x%p requested=%d applied=%d (further occurrences suppressed)",
                time_of_day_manager,
                update_interval,
                clamped_interval);
        }
    } else if (ShouldLogVerbose(g_fog_slicing_verbose)) {
        log::InfoF("fog_slicing_mode manager=0x%p interval=%d",
                   time_of_day_manager,
                   clamped_interval);
    }

    g_fog_slicing_mode_original(time_of_day_manager, clamped_interval);
    MaybeWriteSummary();
}

void DetourCharacterHandleWaterCollision(void* character_effects_component,
                                         const void* mat,
                                         const void* character_velocity) {
    g_character_water_collision_count.fetch_add(1);
    const float speed = ReadVector3Magnitude(character_velocity);
    g_last_character_wet_component.store(reinterpret_cast<std::uintptr_t>(character_effects_component));
    g_last_character_water_speed_bits.store(FloatToBits(speed));

    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        g_config.restore_character_wetness && character_effects_component != nullptr) {
        unsigned char is_attached_to_player = 0;
        const bool attachment_read = SafeRead(
            reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x1A0,
            is_attached_to_player);
        if (attachment_read && is_attached_to_player != 0) {
            g_player_water_collision_component.store(
                reinterpret_cast<std::uintptr_t>(character_effects_component),
                std::memory_order_release);
            g_player_water_collision_generation.fetch_add(1, std::memory_order_acq_rel);
        } else if (attachment_read) {
            MarkNpcCharacterWaterContact(
                reinterpret_cast<std::uintptr_t>(character_effects_component));
        }
    }

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_water_collision component=0x%p speed=%.3f velocity=0x%p mat=0x%p",
                   character_effects_component,
                   speed,
                   character_velocity,
                   mat);
    }

    g_character_handle_water_collision_original(character_effects_component, mat, character_velocity);
    MaybeWriteSummary();
}

void DetourCharacterEffectsUpdate(void* character_effects_component, float delta_seconds) {
    g_character_effects_update_count.fetch_add(1);

#if !defined(SPATCH_FINAL_RELEASE)
    // Keep this entry probe independent from component decoding.  It answers
    // whether the resolved detour is actually reached before any optional
    // field read can filter the diagnostic out.
    if (g_sweat_field_probe.load(std::memory_order_relaxed)) {
        const unsigned long long now = GetTickCount64();
        unsigned long long last =
            g_sweat_update_probe_last_log_tick.load(std::memory_order_relaxed);
        if (now - last >= 1000 &&
            g_sweat_update_probe_last_log_tick.compare_exchange_strong(
                last, now, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            log::InfoF("sweat_probe_tick component=0x%p dt=%.6f",
                       character_effects_component,
                       delta_seconds);
        }
    }
#endif

    g_character_effects_update_original(character_effects_component, delta_seconds);

    const bool behavior_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const std::uintptr_t component_ptr =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    const std::uintptr_t tracked_wet_component = g_last_character_wet_component.load();
    const bool is_tracked_wet_component =
        component_ptr != 0 && tracked_wet_component != 0 && component_ptr == tracked_wet_component;

    const bool runtime_effects_requested =
        behavior_ready &&
        (g_config.restore_character_wetness || g_config.restore_character_sweat);
    bool diagnostic_fields_requested =
        is_tracked_wet_component || g_config.summary_interval_ms != 0;
#if !defined(SPATCH_FINAL_RELEASE)
    diagnostic_fields_requested = diagnostic_fields_requested ||
        g_config.max_verbose_events != 0 ||
        g_force_wetness_field_probe.load(std::memory_order_relaxed) ||
        g_sweat_field_probe.load(std::memory_order_relaxed);
#endif
    if (!runtime_effects_requested && !diagnostic_fields_requested) {
        MaybeWriteSummary();
        return;
    }

    unsigned int active_surface_uid = 0;
    unsigned int active_wet_surface_uid = 0;
    unsigned char is_on_fire = 0;
    unsigned char is_smoldering = 0;
    unsigned char is_attached_to_player = 0;
    float fire_extinguish_time = 0.0f;
    float smolder_extinguish_time = 0.0f;
    float queued_health_damage = 0.0f;
    int wet_gate_counter = 0;
    float timeofday_weather_state = 0.0f;
    float timeofday_surface_wetness = 0.0f;
    float timeofday_override_surface_wetness = -1.0f;
    bool attachment_read = false;

    if (character_effects_component != nullptr) {
        attachment_read = SafeRead(
            reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x1A0,
            is_attached_to_player);
        if (diagnostic_fields_requested) {
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x164,
                     active_surface_uid);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x168,
                     active_wet_surface_uid);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x16C,
                     is_on_fire);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x17C,
                     is_smoldering);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x18C,
                     fire_extinguish_time);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x190,
                     smolder_extinguish_time);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x194,
                     queued_health_damage);
        } else if (behavior_ready && g_config.restore_character_wetness &&
                   attachment_read && is_attached_to_player == 0) {
            // NPC wetness needs only the active wet-surface UID. Avoid seven
            // unrelated diagnostic reads on every visible character.
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x168,
                     active_wet_surface_uid);
        }
    }

    if (diagnostic_fields_requested && g_wet_surface_block_counter != 0) {
        SafeRead(g_wet_surface_block_counter, wet_gate_counter);
    }

    const bool weather_requested = diagnostic_fields_requested ||
        (behavior_ready && g_config.restore_character_wetness && attachment_read);
    const std::uintptr_t time_of_day_manager =
        weather_requested ? ReadTimeOfDayManagerInstance() : 0;
    if (weather_requested && time_of_day_manager != 0) {
        // TimeOfDayManager::m_WeatherState is at +0x34.  The actual surface
        // wetness values consumed by rendering are m_WeatherSurfaceWetness at
        // +0x48 and m_OverrideSurfaceWetness at +0x74.
        SafeRead(time_of_day_manager + 0x34, timeofday_weather_state);
        SafeRead(time_of_day_manager + 0x48, timeofday_surface_wetness);
        SafeRead(time_of_day_manager + 0x74, timeofday_override_surface_wetness);
    }

    bool apply_wetness_restore = behavior_ready &&
                                 g_config.restore_character_wetness &&
                                 attachment_read &&
                                 is_attached_to_player != 0;
#if !defined(SPATCH_FINAL_RELEASE)
    apply_wetness_restore =
        apply_wetness_restore &&
        !g_force_wetness_field_probe.load(std::memory_order_relaxed);
#endif
    if (apply_wetness_restore) {
        ApplyCharacterWetnessRestore(character_effects_component,
                                     delta_seconds,
                                     timeofday_weather_state,
                                     timeofday_surface_wetness,
                                     timeofday_override_surface_wetness);
    }

    if (behavior_ready && attachment_read && is_attached_to_player == 0 &&
        (g_config.restore_character_wetness || g_config.restore_character_sweat)) {
        ApplyNpcCharacterEffectsRestore(character_effects_component,
                                        delta_seconds,
                                        active_wet_surface_uid,
                                        timeofday_weather_state,
                                        timeofday_surface_wetness,
                                        timeofday_override_surface_wetness);
    }

    if (behavior_ready && attachment_read && g_config.restore_character_sweat &&
        is_attached_to_player != 0) {
        ApplyCharacterSweatRestore(
            character_effects_component, delta_seconds, is_attached_to_player != 0);
    }

#if !defined(SPATCH_FINAL_RELEASE)
    if (g_force_wetness_field_probe.load(std::memory_order_relaxed) &&
        is_attached_to_player != 0 && component_ptr != 0) {
        if (ConsumeHotkeyPress(
                VK_F10, g_force_wetness_field_probe_toggle_tick, 300)) {
            const int previous =
                g_force_wetness_field_probe_mode.load(std::memory_order_relaxed);
            const int next = previous == 1 ? 0 : 1;
            g_force_wetness_field_probe_mode.store(next, std::memory_order_relaxed);
            log::InfoF("wetness_render_bridge_probe mode=%s",
                       next == 1 ? "live_bridge" : "stock");
        }

        std::uintptr_t sim_object = 0;
        std::uintptr_t character_look_component = 0;
        __try {
            sim_object = ReadSimObjectFromComponent(character_effects_component);
            character_look_component = ResolveSimObjectComponent(sim_object, 0xCC000001u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sim_object = 0;
            character_look_component = 0;
        }

        std::uintptr_t render_data = 0;
        if (character_look_component != 0 &&
            SafeRead(character_look_component + 0x78, render_data) && render_data != 0) {
            g_wetness_memcpy_watch_address.store(render_data + 0x24,
                                                 std::memory_order_release);
        }

        bool probe_log_expected = false;
        if (render_data != 0 &&
            g_force_wetness_field_probe_logged.compare_exchange_strong(
                probe_log_expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            float component_wetness = -1.0f;
            float bridge_source = -1.0f;
            float original_slot = -1.0f;
            SafeRead(character_look_component + 0xB8, component_wetness);
            SafeRead(render_data + 0x24, bridge_source);
            SafeRead(render_data + 0x38, original_slot);
            log::InfoF(
                "wetness_render_bridge_probe active=1 effects=0x%p sim=0x%p character_look=0x%p render_data=0x%p component=%.3f source=%.3f slot=%.3f",
                character_effects_component,
                reinterpret_cast<void*>(sim_object),
                reinterpret_cast<void*>(character_look_component),
                reinterpret_cast<void*>(render_data),
                component_wetness,
                bridge_source,
                original_slot);
        }

        if (g_wetness_memcpy_capture_ready.load(std::memory_order_acquire)) {
            bool capture_log_expected = false;
            if (g_wetness_memcpy_capture_logged.compare_exchange_strong(
                    capture_log_expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                const std::uintptr_t return_address =
                    g_wetness_memcpy_capture_return.load(std::memory_order_relaxed);
                const std::uintptr_t return_rva =
                    return_address >= g_module_base ? return_address - g_module_base : 0;
                log::InfoF(
                    "wetness_memcpy_capture return=0x%p rva=0x%llX destination=0x%p source=0x%p size=%zu thread=%lu watched=0x%p",
                    reinterpret_cast<void*>(return_address),
                    static_cast<unsigned long long>(return_rva),
                    reinterpret_cast<void*>(g_wetness_memcpy_capture_destination.load(
                        std::memory_order_relaxed)),
                    reinterpret_cast<void*>(
                        g_wetness_memcpy_capture_source.load(std::memory_order_relaxed)),
                    g_wetness_memcpy_capture_size.load(std::memory_order_relaxed),
                    g_wetness_memcpy_capture_thread.load(std::memory_order_relaxed),
                    reinterpret_cast<void*>(
                        g_wetness_memcpy_watch_address.load(std::memory_order_relaxed)));
            }
        }
    }

    const bool trace_sweat = g_sweat_field_probe.load(std::memory_order_relaxed);
    // The player flag is not stable during save loading and mission handoffs.
    // Keep this development-only probe broad enough to observe the component
    // while that flag is transitioning.  This probe is strictly read-only.
    if (trace_sweat && component_ptr != 0) {
        std::uintptr_t sim_object = 0;
        std::uintptr_t character_look_component = 0;
        __try {
            sim_object = ReadSimObjectFromComponent(character_effects_component);
            character_look_component = ResolveSimObjectComponent(sim_object, 0xCC000001u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sim_object = 0;
            character_look_component = 0;
        }

        std::uintptr_t render_data = 0;
        if (character_look_component != 0) {
            SafeRead(character_look_component + 0x78, render_data);
        }

        const unsigned long long now = GetTickCount64();
        auto& last_tick = is_attached_to_player != 0
                              ? g_sweat_field_probe_last_player_log_tick
                              : g_sweat_field_probe_last_npc_log_tick;
        unsigned long long last = last_tick.load(std::memory_order_relaxed);
        if (now - last >= 1000 &&
            last_tick.compare_exchange_strong(
                last, now, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            float component_sweat = -1.0f;
            float render_source_sweat = -1.0f;
            float horizontal_speed = 0.0f;
            bool in_combat = false;
            const bool velocity_valid = ReadPlayerHorizontalSpeed(sim_object, horizontal_speed);
            const bool combat_valid =
                is_attached_to_player != 0
                    ? ReadPlayerCombatState(in_combat)
                    : (in_combat = IsCharacterCombatActive(sim_object, now), true);
            const float policy_sweat =
                is_attached_to_player != 0
                    ? ReadCharacterSweatPolicyAmount()
                    : ReadNpcCharacterSweatPolicyAmount(component_ptr);
            if (character_look_component != 0 && render_data != 0) {
                SafeRead(character_look_component + 0xBC, component_sweat);
                SafeRead(render_data + 0x28, render_source_sweat);
            }
            log::InfoF(
                "sweat_probe scope=%s attached=%u velocity_valid=%u speed=%.3f combat_valid=%u combat=%u policy=%.3f surface=0x%08X wet_surface=0x%08X effects=0x%p sim=0x%p character_look=0x%p render_data=0x%p component=%.3f source=%.3f",
                is_attached_to_player != 0 ? "player" : "npc",
                is_attached_to_player != 0 ? 1u : 0u,
                velocity_valid ? 1u : 0u,
                horizontal_speed,
                combat_valid ? 1u : 0u,
                in_combat ? 1u : 0u,
                policy_sweat,
                active_surface_uid,
                active_wet_surface_uid,
                character_effects_component,
                reinterpret_cast<void*>(sim_object),
                reinterpret_cast<void*>(character_look_component),
                reinterpret_cast<void*>(render_data),
                component_sweat,
                render_source_sweat);
        }
    }
#endif

    if (is_tracked_wet_component) {
        g_last_character_active_surface_uid.store(active_surface_uid);
        g_last_character_active_wet_surface_uid.store(active_wet_surface_uid);
        g_last_character_wet_gate_counter.store(wet_gate_counter);
        g_last_character_is_on_fire.store(is_on_fire != 0 ? 1u : 0u);
        g_last_character_is_smoldering.store(is_smoldering != 0 ? 1u : 0u);
        g_last_character_is_attached_to_player.store(is_attached_to_player != 0 ? 1u : 0u);
        g_last_character_fire_extinguish_bits.store(FloatToBits(fire_extinguish_time));
        g_last_character_smolder_extinguish_bits.store(FloatToBits(smolder_extinguish_time));
        g_last_character_queued_health_damage_bits.store(FloatToBits(queued_health_damage));
        g_last_character_timeofday_weather_bits.store(FloatToBits(timeofday_surface_wetness));
        g_last_character_timeofday_override_bits.store(
            FloatToBits(timeofday_override_surface_wetness));
    }

    if (is_tracked_wet_component && active_wet_surface_uid != 0) {
        g_character_effects_wet_surface_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose) &&
        (active_surface_uid != 0 || active_wet_surface_uid != 0 || is_on_fire != 0 ||
         is_smoldering != 0 || wet_gate_counter != 0 ||
         !NearlyEqual(timeofday_surface_wetness, 0.0f) ||
         !NearlyEqual(timeofday_override_surface_wetness, -1.0f))) {
        log::InfoF(
            "character_effects_update component=0x%p dt=%.3f surface=0x%08X wet_surface=0x%08X wet_gate=%d tod_surface_wetness=%.3f tod_override_surface_wetness=%.3f on_fire=%u smolder=%u attached=%u fire_time=%.3f smolder_time=%.3f queued_damage=%.3f",
            character_effects_component,
            delta_seconds,
            active_surface_uid,
            active_wet_surface_uid,
            wet_gate_counter,
            timeofday_surface_wetness,
            timeofday_override_surface_wetness,
            is_on_fire != 0 ? 1u : 0u,
            is_smoldering != 0 ? 1u : 0u,
            is_attached_to_player != 0 ? 1u : 0u,
            fire_extinguish_time,
            smolder_extinguish_time,
            queued_health_damage);
    }

    MaybeWriteSummary();
}

bool DetourHealthApplyDamage(void* health_component,
                             int damage,
                             void* attacker,
                             void* hit_record,
                             bool projectile_damage) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_health_apply_damage_original(
            health_component, damage, attacker, hit_record, projectile_damage);
    }
    // Sweat-only installs this detour solely as an NPC melee-combat signal.
    // Keep animation, hit-react, and trace diagnostics off this path.
    if (g_sweat_only_health_fast_path) {
        if (!projectile_damage && damage > 0) {
            std::uintptr_t sim_object = 0;
            __try {
                sim_object = ReadSimObjectFromComponent(health_component);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                sim_object = 0;
            }
            MarkCharacterCombatActivity(sim_object);
        }
        return g_health_apply_damage_original(
            health_component, damage, attacker, hit_record, projectile_damage);
    }
    g_character_health_apply_damage_count.fetch_add(1);
    if (projectile_damage) {
        g_character_health_apply_projectile_count.fetch_add(1);
    } else {
        g_character_health_apply_melee_count.fetch_add(1);
    }

    g_last_character_health_component.store(reinterpret_cast<std::uintptr_t>(health_component));
    g_last_character_health_attacker.store(reinterpret_cast<std::uintptr_t>(attacker));
    g_last_character_health_hit_record.store(reinterpret_cast<std::uintptr_t>(hit_record));
    g_last_character_health_damage.store(damage);
    g_last_character_health_projectile.store(projectile_damage ? 1u : 0u);

    std::uintptr_t sim_object = 0;
    std::uintptr_t anim_component = 0;
    std::uintptr_t hitreact_component = 0;
    __try {
        sim_object = ReadSimObjectFromComponent(health_component);
        anim_component = ResolveSimObjectComponent(sim_object, 0xC6000003u);
        hitreact_component = ResolveSimObjectComponent(sim_object, 0xA8000001u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sim_object = 0;
        anim_component = 0;
        hitreact_component = 0;
    }
    if (!projectile_damage && damage > 0) {
        // The target's health callback is the only build-stable combat signal
        // available for NPCs. Keep a short activity window so stationary
        // attack/hit animations still build sweat without guessing from
        // arbitrary animation memory.
        MarkCharacterCombatActivity(sim_object);
    }
    g_last_character_health_anim_component.store(anim_component);
    g_last_character_health_hitreact_component.store(hitreact_component);
    if (anim_component != 0) {
        g_character_health_anim_found_count.fetch_add(1);
    }
    if (hitreact_component != 0) {
        g_character_health_hitreact_found_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF(
            "character_health_apply component=0x%p damage=%d projectile=%d attacker=0x%p hit=0x%p sim_object=0x%p anim=0x%p hitreact=0x%p",
            health_component,
            damage,
            projectile_damage ? 1 : 0,
            attacker,
            hit_record,
            reinterpret_cast<void*>(sim_object),
            reinterpret_cast<void*>(anim_component),
            reinterpret_cast<void*>(hitreact_component));
    }

    AppendCharacterTraceLine("enter",
                             health_component,
                             reinterpret_cast<void*>(anim_component),
                             reinterpret_cast<void*>(hitreact_component),
                             damage,
                             projectile_damage,
                             false);

    const bool result = g_health_apply_damage_original(
        health_component, damage, attacker, hit_record, projectile_damage);
    AppendCharacterTraceLine("after_orig",
                             health_component,
                             reinterpret_cast<void*>(anim_component),
                             reinterpret_cast<void*>(hitreact_component),
                             damage,
                             projectile_damage,
                             result);

    MaybeWriteSummary();
    return result;
}

void DetourCharacterAnimationCreateDamageRig(void* character_animation_component) {
    g_character_damage_rig_create_count.fetch_add(1);

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_damage_rig_create component=0x%p", character_animation_component);
    }

    MaybeWriteSummary();
    g_character_animation_create_damage_rig_original(character_animation_component);
}

void DetourCharacterDamageRigResetDamage(void* damage_rig) {
    g_character_damage_rig_reset_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_damage_rig_reset rig=0x%p", damage_rig);
    }

    MaybeWriteSummary();
    g_character_damage_rig_reset_damage_original(damage_rig);
}

void DetourAntiAliasOwner(std::uintptr_t render_context,
                          std::uintptr_t param_2,
                          std::uintptr_t param_3,
                          std::uintptr_t* param_4,
                          std::uintptr_t* param_5) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_antialias_owner_original(render_context, param_2, param_3, param_4, param_5);
        return;
    }
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }

    g_aa_owner_count.fetch_add(1);

    if (g_config.hook_smaa_present && g_config.smaa_disable_stock_aa &&
        smaa::CanReplaceStockAa()) {
        g_aa_skip_count.fetch_add(1);
        g_last_aa_state_gate.store(0);
        g_last_aa_hair_blur_gate.store(0);
        g_last_aa_variant_mode.store(g_aa_runtime_variant_mode.load());
        g_last_aa_material.store(0);
        g_last_aa_target.store(0);
        g_last_aa_source_a.store(0);
        g_last_aa_source_b.store(0);
        g_last_aa_shader_uid.store(0);
        g_last_aa_raster_uid.store(0);
        g_last_aa_aux_uid.store(0);
        MaybeWriteSummary();
        return;
    }

    const int variant_mode = ClampAaVariantMode(g_aa_runtime_variant_mode.load());
    int aa_state_gate = 0;
    int hair_blur_gate = 0;
    int branch_gate = 0;
    std::uintptr_t aa_material = 0;
    std::uintptr_t aa_target = 0;
    std::uintptr_t source_a = 0;
    std::uintptr_t source_b = 0;

    SafeRead(render_context + 0x49C, aa_state_gate);
    SafeRead(render_context + 0x148, hair_blur_gate);
    SafeRead(render_context + 0x178, branch_gate);
    SafeRead(render_context + 0x330, aa_material);
    SafeRead(render_context + 0x338, aa_target);
    if (param_4 != nullptr) {
        SafeRead(reinterpret_cast<std::uintptr_t>(param_4), source_a);
    }
    if (param_5 != nullptr) {
        SafeRead(reinterpret_cast<std::uintptr_t>(param_5), source_b);
    }

    g_last_aa_state_gate.store(aa_state_gate);
    g_last_aa_hair_blur_gate.store(hair_blur_gate);
    g_last_aa_variant_mode.store(variant_mode);
    g_last_aa_material.store(aa_material);
    g_last_aa_target.store(aa_target);
    g_last_aa_source_a.store(source_a);
    g_last_aa_source_b.store(source_b);

    if (aa_state_gate == 1) {
        g_aa_main_count.fetch_add(1);
    } else {
        g_aa_skip_count.fetch_add(1);
    }

    if (hair_blur_gate == 0) {
        g_aa_hair_blur_zero_count.fetch_add(1);
    }

    bool variant_applied = false;
    bool wrote_hair_gate = false;
    bool wrote_branch_gate = false;
    if (variant_mode != 0 && render_context != 0) {
        const int forced_hair_blur_gate = 1;
        const int forced_branch_gate = variant_mode == 2 ? 1 : 0;
        const bool hair_gate_needs_write = hair_blur_gate != forced_hair_blur_gate;
        const bool branch_gate_needs_write = branch_gate != forced_branch_gate;
        const bool hair_gate_ready =
            !hair_gate_needs_write || SafeWrite(render_context + 0x148, forced_hair_blur_gate);
        const bool branch_gate_ready =
            hair_gate_ready &&
            (!branch_gate_needs_write || SafeWrite(render_context + 0x178, forced_branch_gate));
        variant_applied = hair_gate_ready && branch_gate_ready;
        if (variant_applied) {
            wrote_hair_gate = hair_gate_needs_write;
            wrote_branch_gate = branch_gate_needs_write;
            g_aa_variant_apply_count.fetch_add(1);
        } else {
            // The stock routine must never observe half of a requested variant.
            // Roll the first field back immediately if the second write failed;
            // the __finally block below owns restoration only after full commit.
            if (branch_gate_needs_write && branch_gate_ready) {
                SafeWrite(render_context + 0x178, branch_gate);
            }
            if (hair_gate_needs_write && hair_gate_ready) {
                SafeWrite(render_context + 0x148, hair_blur_gate);
            }
        }
    }

    if (ShouldLogVerbose(g_aa_probe_verbose)) {
        log::InfoF(
            "aa_probe render_context=0x%p state_gate=%d hair_blur_gate=%d branch_gate=%d variant=%d applied=%d material=0x%p target=0x%p src_a=0x%p src_b=0x%p",
            reinterpret_cast<void*>(render_context),
            aa_state_gate,
            hair_blur_gate,
            branch_gate,
            variant_mode,
            variant_applied ? 1 : 0,
            reinterpret_cast<void*>(aa_material),
            reinterpret_cast<void*>(aa_target),
            reinterpret_cast<void*>(source_a),
            reinterpret_cast<void*>(source_b));
    }

    ++g_antialias_owner_scope_depth;
    __try {
        g_antialias_owner_original(render_context, param_2, param_3, param_4, param_5);
    } __finally {
        // Restore each field independently.  A partial SafeWrite must not
        // leave one stock-AA gate forced for every subsequent frame.
        if (wrote_hair_gate && hair_blur_gate != 1) {
            SafeWrite(render_context + 0x148, hair_blur_gate);
        }
        if (wrote_branch_gate && branch_gate != (variant_mode == 2 ? 1 : 0)) {
            SafeWrite(render_context + 0x178, branch_gate);
        }
        if (g_antialias_owner_scope_depth != 0) {
            --g_antialias_owner_scope_depth;
        }
    }

    std::uintptr_t resolved_aa_material = aa_material;
    SafeRead(render_context + 0x330, resolved_aa_material);

    unsigned int resolved_shader_uid = 0;
    unsigned int resolved_raster_uid = 0;
    unsigned int resolved_aux_uid = 0;
    if (resolved_aa_material != 0) {
        SafeRead(resolved_aa_material + 0xA8, resolved_shader_uid);
        SafeRead(resolved_aa_material + 0x150, resolved_raster_uid);
        SafeRead(resolved_aa_material + 0x1C0, resolved_aux_uid);
    }

    g_last_aa_material.store(resolved_aa_material);
    g_last_aa_shader_uid.store(resolved_shader_uid);
    g_last_aa_raster_uid.store(resolved_raster_uid);
    g_last_aa_aux_uid.store(resolved_aux_uid);

    MaybeWriteSummary();
}

void DetourAntiAliasFxHandoff(std::uintptr_t render_context,
                              std::uintptr_t arg1,
                              std::uintptr_t arg2,
                              std::uintptr_t arg3) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_antialias_fx_handoff_original(render_context, arg1, arg2, arg3);
        return;
    }
    g_aa_fx_handoff_count.fetch_add(1);
    g_last_aa_fx_arg1.store(arg1);
    g_last_aa_fx_arg2.store(arg2);
    g_last_aa_fx_arg3.store(arg3);

    if (ShouldLogVerbose(g_aa_fx_probe_verbose)) {
        log::InfoF(
            "aa_fx_probe render_context=0x%p arg1=0x%p arg2=0x%p arg3=0x%p aa_material=0x%p aa_target=0x%p",
            reinterpret_cast<void*>(render_context),
            reinterpret_cast<void*>(arg1),
            reinterpret_cast<void*>(arg2),
            reinterpret_cast<void*>(arg3),
            reinterpret_cast<void*>(g_last_aa_material.load()),
            reinterpret_cast<void*>(g_last_aa_target.load()));
    }

    if (g_config.hook_smaa_present && g_config.smaa_disable_stock_aa &&
        smaa::CanReplaceStockAa()) {
        MaybeWriteSummary();
        return;
    }

    g_antialias_fx_handoff_original(render_context, arg1, arg2, arg3);
    MaybeWriteSummary();
}

char DetourAntiAliasAuxStateQuery(void* state) {
    const char original = g_antialias_aux_state_query_original(state);
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return original;
    }

    const int aux_mode = ClampAaAuxMode(g_aa_runtime_aux_mode.load());
    g_last_aa_aux_mode.store(aux_mode);
    if (aux_mode == 0) {
        return original;
    }

    if (g_antialias_owner_scope_depth == 0) {
        return original;
    }

    g_aa_aux_apply_count.fetch_add(1);
    return aux_mode == 1 ? 0 : 1;
}

void DetourPresentBuffer(void* render_packet) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_present_buffer_original(render_packet);
        return;
    }
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }

    if (!g_config.hook_frameflow) {
        PollGraphicsDebugKeys();
    }

    g_present_buffer_original(render_packet);

    MaybeWriteSummary();
}

void DetourRenderSubmitMaterial(void* render_cmd,
                                void* submit_params,
                                void* material,
                                int flags) {
    g_post_material_submit_count.fetch_add(1);

    const std::uintptr_t material_ptr = reinterpret_cast<std::uintptr_t>(material);
    g_last_post_material.store(material_ptr);

    const std::uintptr_t render_context = ReadRenderContextInstance();
    const std::uintptr_t composite_lights = ReadRenderContextMaterial(render_context, 0x2B8);
    const std::uintptr_t composite_final = ReadRenderContextMaterial(render_context, 0x2C0);
    const std::uintptr_t shadow_collector = ReadRenderContextMaterial(render_context, 0x2C8);
    const std::uintptr_t bloom_threshold = ReadRenderContextMaterial(render_context, 0x328);
    const std::uintptr_t post_lightshaft = ReadRenderContextMaterial(render_context, 0x340);

    const char* matched = nullptr;
    if (material_ptr != 0) {
        if (material_ptr == composite_lights) {
            g_post_composite_lights_submit_count.fetch_add(1);
            matched = "composite_lights";
        } else if (material_ptr == composite_final) {
            g_post_composite_final_submit_count.fetch_add(1);
            matched = "composite_final";
            const std::uintptr_t render_cmd_ptr =
                reinterpret_cast<std::uintptr_t>(render_cmd);
            const std::uintptr_t submit_params_ptr =
                reinterpret_cast<std::uintptr_t>(submit_params);
            const std::uintptr_t param0 = ReadPointerField(submit_params_ptr, 0x00);
            const std::uintptr_t param1 = ReadPointerField(submit_params_ptr, 0x08);
            const std::uintptr_t param2 = ReadPointerField(submit_params_ptr, 0x10);
            const std::uintptr_t param3 = ReadPointerField(submit_params_ptr, 0x18);

            const bool snapshot_changed =
                g_last_post_final_flags.load() != flags ||
                g_last_post_final_cmd.load() != render_cmd_ptr ||
                g_last_post_final_params.load() != submit_params_ptr ||
                g_last_post_final_param0.load() != param0 ||
                g_last_post_final_param1.load() != param1 ||
                g_last_post_final_param2.load() != param2 ||
                g_last_post_final_param3.load() != param3;

            if (snapshot_changed) {
                g_post_composite_final_snapshot_change_count.fetch_add(1);
            }

            g_last_post_final_flags.store(flags);
            g_last_post_final_cmd.store(render_cmd_ptr);
            g_last_post_final_params.store(submit_params_ptr);
            g_last_post_final_param0.store(param0);
            g_last_post_final_param1.store(param1);
            g_last_post_final_param2.store(param2);
            g_last_post_final_param3.store(param3);

            if (snapshot_changed && ShouldLogVerbose(g_post_material_verbose)) {
                log::InfoF(
                    "post_composite_final_snapshot render_context=0x%p material=0x%p flags=%d cmd=0x%p params=0x%p p0=0x%p p1=0x%p p2=0x%p p3=0x%p",
                    reinterpret_cast<void*>(render_context),
                    material,
                    flags,
                    render_cmd,
                    submit_params,
                    reinterpret_cast<void*>(param0),
                    reinterpret_cast<void*>(param1),
                    reinterpret_cast<void*>(param2),
                    reinterpret_cast<void*>(param3));
            }
        } else if (material_ptr == bloom_threshold) {
            g_post_bloom_threshold_submit_count.fetch_add(1);
            matched = "bloom_threshold";
        } else if (material_ptr == post_lightshaft) {
            g_post_lightshaft_submit_count.fetch_add(1);
            matched = "post_lightshaft";
        } else if (material_ptr == shadow_collector) {
            g_post_shadow_collector_submit_count.fetch_add(1);
            matched = "shadow_collector";
        }
    }

    if (matched != nullptr && ShouldLogVerbose(g_post_material_verbose)) {
        log::InfoF(
            "post_material_submit match=%s render_context=0x%p material=0x%p flags=%d cmd=0x%p params=0x%p",
            matched,
            reinterpret_cast<void*>(render_context),
            material,
            flags,
            render_cmd,
            submit_params);
    }

    g_render_submit_material_original(render_cmd, submit_params, material, flags);
    MaybeWriteSummary();
}

void DetourSceneryPrepare(std::uintptr_t scenery_state) {
    g_scenery_prepare_count.fetch_add(1);
    const unsigned long long ready_before = ReadReadyCounterForScope(BuilderScope::SceneryPrepare);
    const SceneryCounters before_counters = ReadSceneryCounters();

    if (ShouldLogVerbose(g_scenery_prepare_verbose)) {
        log::InfoF("scenery_prepare state=0x%p counters_before=%u/%u/%u/%u",
                   reinterpret_cast<void*>(scenery_state),
                   before_counters.c0,
                   before_counters.c1,
                   before_counters.c2,
                   before_counters.c3);
    }

    {
        BuilderScopeGuard scope_guard(BuilderScope::SceneryPrepare);
        g_scenery_prepare_original(scenery_state);
    }

    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::SceneryPrepare) - ready_before;
    if (ShouldLogVerbose(g_scenery_prepare_verbose)) {
        log::InfoF("scenery_prepare_complete state=0x%p ready_delta=%llu counters_after=%u/%u/%u/%u",
                   reinterpret_cast<void*>(scenery_state),
                   ready_delta,
                   after_counters.c0,
                   after_counters.c1,
                   after_counters.c2,
                   after_counters.c3);
    }

    MaybeWriteSummary();
}

void DetourScenerySetup(std::uintptr_t query_context,
                        std::uint32_t query_kind,
                        void* param_3,
                        void* param_4,
                        float range_scale) {
    g_scenery_setup_count.fetch_add(1);

    const QueueSnapshot before_queue = ReadQueueSnapshot();
    const SceneryCounters before_counters = ReadSceneryCounters();
    const unsigned long long ready_before = ReadReadyCounterForScope(BuilderScope::ScenerySetup);

    {
        BuilderScopeGuard scope_guard(BuilderScope::ScenerySetup);
        g_scenery_setup_original(query_context, query_kind, param_3, param_4, range_scale);
    }

    const QueueSnapshot after_queue = ReadQueueSnapshot();
    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::ScenerySetup) - ready_before;

    const long long queued_delta =
        static_cast<long long>(after_queue.queued_total) - before_queue.queued_total;
    if (queued_delta > 0) {
        g_scenery_setup_queue_delta_total.fetch_add(
            static_cast<unsigned long long>(queued_delta));
    }

    if (ShouldLogVerbose(g_scenery_setup_verbose) || queued_delta > 0) {
        log::InfoF(
            "scenery_setup context=0x%p kind=%u range=%.3f queue_delta=%lld ready_delta=%llu "
            "counters_before=%u/%u/%u/%u counters_after=%u/%u/%u/%u",
            reinterpret_cast<void*>(query_context),
            query_kind,
            range_scale,
            queued_delta,
            ready_delta,
            before_counters.c0,
            before_counters.c1,
            before_counters.c2,
            before_counters.c3,
            after_counters.c0,
            after_counters.c1,
            after_counters.c2,
            after_counters.c3);
    }

    MaybeWriteSummary();
}

std::uintptr_t DetourRenderSceneryBuilder(std::uintptr_t param_1,
                                          std::uintptr_t param_2,
                                          void* param_3,
                                          std::uint32_t param_4,
                                          std::uint32_t param_5) {
    g_render_scenery_builder_count.fetch_add(1);

    const QueueSnapshot before_queue = ReadQueueSnapshot();
    const SceneryCounters before_counters = ReadSceneryCounters();
    const unsigned long long ready_before =
        ReadReadyCounterForScope(BuilderScope::RenderScenery);

    std::uintptr_t result = 0;
    {
        BuilderScopeGuard scope_guard(BuilderScope::RenderScenery);
        result = g_render_scenery_builder_original(param_1, param_2, param_3, param_4, param_5);
    }

    const QueueSnapshot after_queue = ReadQueueSnapshot();
    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::RenderScenery) - ready_before;

    const long long queued_delta =
        static_cast<long long>(after_queue.queued_total) - before_queue.queued_total;
    if (queued_delta > 0) {
        g_render_scenery_queue_delta_total.fetch_add(
            static_cast<unsigned long long>(queued_delta));
    }

    if (ShouldLogVerbose(g_render_scenery_builder_verbose) || queued_delta > 0) {
        log::InfoF(
            "render_scenery_builder state=0x%p scene=0x%p dependency=0x%p mode=%u flags=%u "
            "queue_delta=%lld ready_delta=%llu counters_before=%u/%u/%u/%u counters_after=%u/%u/%u/%u result=0x%p",
            reinterpret_cast<void*>(param_1),
            reinterpret_cast<void*>(param_2),
            param_3,
            param_4,
            param_5,
            queued_delta,
            ready_delta,
            before_counters.c0,
            before_counters.c1,
            before_counters.c2,
            before_counters.c3,
            after_counters.c0,
            after_counters.c1,
            after_counters.c2,
            after_counters.c3,
            reinterpret_cast<void*>(result));
    }

    MaybeWriteSummary();
    return result;
}

unsigned long long DetourRasterizeBucketBuilder(std::uintptr_t state,
                                                int bucket_index,
                                                void* dependency) {
    g_rasterize_bucket_builder_count.fetch_add(1);

    const QueueSnapshot before_queue = ReadQueueSnapshot();
    const SceneryCounters before_counters = ReadSceneryCounters();
    const unsigned long long ready_before =
        ReadReadyCounterForScope(BuilderScope::RasterizeBucket);

    unsigned long long result = 0;
    {
        BuilderScopeGuard scope_guard(BuilderScope::RasterizeBucket);
        result = g_rasterize_bucket_builder_original(state, bucket_index, dependency);
    }

    const QueueSnapshot after_queue = ReadQueueSnapshot();
    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::RasterizeBucket) - ready_before;
    const long long queued_delta =
        static_cast<long long>(after_queue.queued_total) - before_queue.queued_total;
    if (queued_delta > 0) {
        g_rasterize_bucket_queue_delta_total.fetch_add(
            static_cast<unsigned long long>(queued_delta));
    }

    if (ShouldLogVerbose(g_rasterize_bucket_builder_verbose) || queued_delta > 0) {
        log::InfoF(
            "rasterize_bucket_builder state=0x%p bucket=%d dependency=0x%p queue_delta=%lld "
            "ready_delta=%llu counters_before=%u/%u/%u/%u counters_after=%u/%u/%u/%u result=%llu",
            reinterpret_cast<void*>(state),
            bucket_index,
            dependency,
            queued_delta,
            ready_delta,
            before_counters.c0,
            before_counters.c1,
            before_counters.c2,
            before_counters.c3,
            after_counters.c0,
            after_counters.c1,
            after_counters.c2,
            after_counters.c3,
            result);
    }

    MaybeWriteSummary();
    return result;
}

void DetourPedestrianSpawnUpdate(void* manager, float delta_seconds) {
    if (g_pedestrian_spawn_update_original == nullptr) {
        return;
    }
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_pedestrian_spawn_update_original(manager, delta_seconds);
        return;
    }

    const bool previous_valid = g_pedestrian_update_delta_valid_tls;
    const float previous_delta = g_pedestrian_update_delta_tls;
    if (!previous_valid) {
        if (manager != g_pedestrian_manager_tls) {
            g_pedestrian_throttle_scheduler_tls.Reset();
            g_pedestrian_manager_tls = manager;
        }
    }
    g_pedestrian_update_delta_valid_tls = true;
    g_pedestrian_update_delta_tls = delta_seconds;

    __try {
        g_pedestrian_spawn_update_original(manager, delta_seconds);
    } __finally {
        g_pedestrian_update_delta_valid_tls = previous_valid;
        g_pedestrian_update_delta_tls = previous_delta;
    }
}

void DetourAverageWindowInitialize(void* window,
                                   float maximum_timespan,
                                   float assumed_sample_rate) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_average_window_initialize_original(window, maximum_timespan, assumed_sample_rate);
        return;
    }
    g_average_window_initialize_count.fetch_add(1, std::memory_order_relaxed);
    const average_window::CapacityDecision decision =
        average_window::ResolveCapacity(maximum_timespan, assumed_sample_rate);
    if (decision.expanded) {
        assumed_sample_rate = decision.effective_sample_rate;
        g_average_window_expanded_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        bool expected = false;
        if (g_average_window_fallback_logged.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            log::WarnF("average_window_capacity unchanged max_timespan=%.6f "
                       "sample_rate=%.6f recognized=%d entry_count=%zu",
                       maximum_timespan,
                       assumed_sample_rate,
                       decision.recognized_stock_rate ? 1 : 0,
                       decision.entry_count);
        }
    }

    g_average_window_initialize_original(window, maximum_timespan, assumed_sample_rate);
}

void DetourMaterialOnLoad(void* material) {
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        g_config.restore_original_eye_reflections && material != nullptr) {
        const std::uintptr_t material_address =
            reinterpret_cast<std::uintptr_t>(material);
        std::uint32_t material_uid = 0;
        if (SafeRead(material_address + character_eye::kMaterialNameUidOffset, material_uid) &&
            character_eye::IsTargetMaterialUid(material_uid)) {
            const std::uint32_t original_diffuse =
                character_eye::OriginalDiffuseForMaterial(material_uid);
            const std::uint32_t log_bit = character_eye::LogBitForMaterial(material_uid);
            std::array<std::byte, character_eye::kRequiredMaterialBytes> snapshot{};
            character_eye::RestoreResult result =
                character_eye::RestoreResult::InvalidLayout;
            if (SafeRead(material_address, snapshot)) {
                result = character_eye::ApplyOriginalWeiEyeDiffuse(snapshot);
            }

            if (result == character_eye::RestoreResult::Applied) {
                const std::uintptr_t diffuse_address =
                    material_address + character_eye::kDiffuseResourceUidOffset;
                std::uint32_t current_diffuse = 0;
                if (SafeRead(diffuse_address, current_diffuse) &&
                    current_diffuse == character_eye::kDefinitiveFallbackDiffuseUid &&
                    SafeWrite(diffuse_address, original_diffuse)) {
                    const std::uint32_t previous =
                        g_character_eye_restore_applied_mask.fetch_or(
                            log_bit, std::memory_order_acq_rel);
                    if ((previous & log_bit) == 0) {
                        log::InfoF(
                            "character_eye_restore applied=1 material_uid=0x%08X "
                            "stock_diffuse=0x%08X original_diffuse=0x%08X",
                            material_uid,
                            character_eye::kDefinitiveFallbackDiffuseUid,
                            original_diffuse);
                    }
                } else if (current_diffuse == original_diffuse) {
                    result = character_eye::RestoreResult::AlreadyApplied;
                } else {
                    result = character_eye::RestoreResult::InvalidLayout;
                }
            }

            if (result == character_eye::RestoreResult::AlreadyApplied) {
                const std::uint32_t previous =
                    g_character_eye_restore_already_present_mask.fetch_or(
                        log_bit, std::memory_order_acq_rel);
                if ((previous & log_bit) == 0) {
                    log::InfoF(
                        "character_eye_restore already_present=1 material_uid=0x%08X "
                        "diffuse=0x%08X",
                        material_uid,
                        original_diffuse);
                }
            } else if (result == character_eye::RestoreResult::InvalidLayout) {
                const std::uint32_t previous =
                    g_character_eye_restore_failure_mask.fetch_or(
                        log_bit, std::memory_order_acq_rel);
                if ((previous & log_bit) == 0) {
                    log::WarnF(
                        "character_eye_restore skipped material_uid=0x%08X "
                        "reason=unexpected_layout",
                        material_uid);
                }
            }
        }
    }

    g_material_on_load_original(material);
}

void DetourPedestrianFrameRateThrottle() {
    if (g_pedestrian_frame_rate_throttle_original == nullptr) {
        return;
    }
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_pedestrian_frame_rate_throttle_original();
        return;
    }
    if (!g_pedestrian_update_delta_valid_tls) {
        // Preserve calls made from an owner that has not been mapped. The
        // verified PedSpawnManager update supplies a delta through the TLS
        // scope above, while unknown callers retain stock behavior.
        g_pedestrian_frame_rate_throttle_original();
        g_pedestrian_throttle_stock_call_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const pedestrian_timing::StepResult step =
        g_pedestrian_throttle_scheduler_tls.Advance(g_pedestrian_update_delta_tls);
    g_pedestrian_throttle_frame_count.fetch_add(1, std::memory_order_relaxed);
    if (step.frame_delta_clamped) {
        g_pedestrian_throttle_clamped_frame_count.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::uint32_t index = 0; index < step.steps; ++index) {
        g_pedestrian_frame_rate_throttle_original();
        g_pedestrian_throttle_stock_call_count.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ValidateHookSignature(std::uintptr_t address,
                           std::span<const std::uint8_t> signature,
                           const char* name) {
    if (runtime_patch::MatchesBytes(address, signature)) {
        return true;
    }
    log::WarnF("hook disabled name=%s reason=unexpected_bytes target=0x%p",
               name,
               reinterpret_cast<void*>(address));
    g_hook_creation_transaction_open = false;
    return false;
}

bool PreflightRequiredHookTarget(
    std::uintptr_t address,
    std::span<const std::uint8_t> signature,
    const char* name) {
    if (!runtime_patch::MatchesBytes(address, signature)) {
        log::ErrorF(
            "required hook preflight failed name=%s reason=unexpected_bytes "
            "target=0x%p",
            name,
            reinterpret_cast<void*>(address));
        g_hook_creation_transaction_open = false;
        return false;
    }
    const hook_guard::Result guard_result = g_hook_target_guard.Verify(
        reinterpret_cast<const void*>(address),
        hook_guard::kMinHookTargetVerificationBytes);
    if (guard_result.verified()) {
        return true;
    }
    log::ErrorF(
        "required hook preflight failed name=%s reason=target_rejected "
        "target=0x%p "
        "status=%s pe_status=%s rva=0x%X mismatch=%zu",
        name,
        reinterpret_cast<void*>(address),
        hook_guard::StatusName(guard_result.status),
        hook_guard::PeStatusName(guard_result.pe_status),
        guard_result.target_rva,
        guard_result.mismatch_offset);
    g_hook_creation_transaction_open = false;
    return false;
}

bool CreateHookDetour(std::uintptr_t address, void* detour, void** original, const char* name) {
    if (!g_hook_creation_transaction_open) {
        return false;
    }
    const hook_guard::Result guard_result = g_hook_target_guard.Verify(
        reinterpret_cast<const void*>(address), hook_guard::kMinHookTargetVerificationBytes);
    if (!guard_result.verified()) {
        log::ErrorF("hook target rejected name=%s target=0x%p status=%s pe_status=%s "
                    "rva=0x%X mismatch=%zu",
                    name,
                    reinterpret_cast<void*>(address),
                    hook_guard::StatusName(guard_result.status),
                    hook_guard::PeStatusName(guard_result.pe_status),
                    guard_result.target_rva,
                    guard_result.mismatch_offset);
        g_hook_creation_transaction_open = false;
        return false;
    }
    // Reserve the process-lifetime hook record before MinHook mutates
    // executable code. If allocation fails, no detour has been created and
    // stack unwinding is safe.
    try {
        g_created_hook_targets.push_back(address);
    } catch (...) {
        log::ErrorF("hook registry allocation failed before creating %s at 0x%p",
                    name,
                    reinterpret_cast<void*>(address));
        g_hook_creation_transaction_open = false;
        return false;
    }
    const MH_STATUS create_status =
        MH_CreateHook(reinterpret_cast<void*>(address), detour, original);
    if (create_status != MH_OK) {
        g_created_hook_targets.pop_back();
        log::ErrorF("MH_CreateHook failed for %s at 0x%p: %d",
                    name,
                    reinterpret_cast<void*>(address),
                    static_cast<int>(create_status));
        g_hook_creation_transaction_open = false;
        return false;
    }

    // The SDmodding MinHook build enables a hook as part of MH_CreateHook.
    // Keep one authoritative registry so a later failure can retain every
    // earlier detour as a transparent pass-through without losing ownership.
    log::InfoF("hook attached name=%s target=0x%p behavior=pending_commit",
               name,
               reinterpret_cast<void*>(address));
    return true;
}

bool RetainCreatedHooks() {
    if (g_created_hook_targets.empty()) {
        return true;
    }
    const bool was_retained =
        g_minhook_retained_process_lifetime.exchange(true, std::memory_order_acq_rel);
    if (!was_retained) {
        log::InfoF("minhook_hooks_retained_process_lifetime count=%zu",
                   g_created_hook_targets.size());
    }
    // Do not call MH_RemoveHook.  The bundled fork frees each original
    // trampoline there, while another thread can already be executing through
    // that pointer.  Keeping the registry also prevents a later retry from
    // accidentally creating a second hook for the same target.
    return true;
}

}  // namespace

bool Initialize(const Config& config,
                const std::filesystem::path& config_path,
                const std::filesystem::path& display_settings_path,
                std::string_view build_id,
                const bool hook_layout_supported) {
    std::lock_guard<std::mutex> lifecycle_lock(g_hook_lifecycle_mutex);
    if (g_cleanup_pending.load(std::memory_order_acquire)) {
        log::Error("hook initialization blocked while a previous teardown is pending");
        return false;
    }
    if (g_hooks_initialized.load(std::memory_order_acquire)) {
        if (g_behavior_transaction_ready.load(std::memory_order_acquire)) {
            return true;
        }
        log::Error("hook initialization blocked: retained detours belong to a closed transaction");
        return false;
    }
    if (g_minhook_retained_process_lifetime.load(std::memory_order_acquire)) {
        log::Error("hook initialization blocked: process-lifetime MinHook state is retained");
        return false;
    }

    g_behavior_transaction_ready.store(false, std::memory_order_release);
    g_config = config;
    g_config_path = config_path;
    g_player_water_collision_component.store(0, std::memory_order_relaxed);
    g_player_water_collision_generation.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> wetness_lock(g_character_wetness_mutex);
        g_character_wetness_state = {};
    }
    {
        std::lock_guard<std::mutex> sweat_lock(g_character_sweat_mutex);
        g_character_sweat_state = {};
    }
    ResetNpcCharacterEffectsStates();
    g_character_physics_get_velocity = nullptr;
    g_ui_is_player_in_combat = nullptr;
    g_character_sweat_active_component.store(0, std::memory_order_relaxed);
#if !defined(SPATCH_FINAL_RELEASE)
    g_force_wetness_field_probe.store(
        FieldProbeRequested(config_path.parent_path() / L"SPatch.wetness.probe",
                            L"SPATCH_FORCE_WETNESS"),
        std::memory_order_relaxed);
    g_sweat_field_probe.store(
        FieldProbeRequested(config_path.parent_path() / L"SPatch.sweat.probe",
                            L"SPATCH_TRACE_SWEAT"),
        std::memory_order_relaxed);
    g_sweat_field_probe_last_player_log_tick.store(0, std::memory_order_relaxed);
    g_sweat_field_probe_last_npc_log_tick.store(0, std::memory_order_relaxed);
    g_sweat_update_probe_last_log_tick.store(0, std::memory_order_relaxed);
    g_force_wetness_field_probe_logged.store(false, std::memory_order_relaxed);
    g_force_wetness_field_probe_mode.store(1, std::memory_order_relaxed);
    g_wetness_memcpy_watch_address.store(0, std::memory_order_relaxed);
    g_wetness_memcpy_capture_claimed.store(false, std::memory_order_relaxed);
    g_wetness_memcpy_capture_ready.store(false, std::memory_order_relaxed);
    g_wetness_memcpy_capture_logged.store(false, std::memory_order_relaxed);
    g_wetness_memcpy_capture_return.store(0, std::memory_order_relaxed);
    g_wetness_memcpy_capture_destination.store(0, std::memory_order_relaxed);
    g_wetness_memcpy_capture_source.store(0, std::memory_order_relaxed);
    g_wetness_memcpy_capture_size.store(0, std::memory_order_relaxed);
    g_wetness_memcpy_capture_thread.store(0, std::memory_order_relaxed);
    if (g_force_wetness_field_probe.load(std::memory_order_relaxed)) {
        log::Warn("diagnostic wetness field probe enabled by an explicit test trigger");
    }
    if (g_sweat_field_probe.load(std::memory_order_relaxed)) {
        log::Warn("diagnostic sweat field probe enabled by an explicit test trigger");
    }
#endif
    // Behaviour fixes own the minimal hook dependencies they need. Keeping
    // these as separate user-editable switches used to let a harmless-looking
    // probe toggle silently disable the shipped cutscene fix (and FinalRelease
    // disabled the same hooks unconditionally).
    if (g_config.fix_cutscene_scene_time_step) {
        g_config.hook_nis_timing = true;
        g_config.hook_nis_owner = true;
    }
    if (g_config.fix_cutscene_zero_dt) {
        g_config.hook_frameflow = true;
    }
    g_use_latest_steam_layout = (build_id == "latest_steam");
    // Trace-file writes are synchronous. Keep them strictly diagnostic so a
    // normal end-user install never opens or flushes a file on the damage thread.
    g_character_trace_budget.store(
        (g_use_latest_steam_layout && config.hook_character_regression_probe) ? 128 : 0);
    g_module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_module_base == 0) {
        log::Error("hook initialization failed: process image base unavailable");
        return false;
    }
    // ResolveAddress is used throughout the hook transaction, so establish a
    // bounded image end without dereferencing an untrusted e_lfanew/PE header
    // directly.  GetModuleHandle normally returns a valid image, but keeping
    // this path SEH-guarded makes safe-compatibility and malformed-loader
    // cases fail closed instead of crashing the bootstrap worker.
    g_module_end = 0;
    IMAGE_DOS_HEADER dos_header{};
    if (SafeRead(g_module_base, dos_header) && dos_header.e_magic == IMAGE_DOS_SIGNATURE &&
        dos_header.e_lfanew >= static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        const std::uintptr_t nt_offset = static_cast<std::uintptr_t>(dos_header.e_lfanew);
        constexpr std::uintptr_t max_address =
            (std::numeric_limits<std::uintptr_t>::max)();
        if (nt_offset <= max_address - g_module_base &&
            sizeof(IMAGE_NT_HEADERS64) <=
                max_address - (g_module_base + nt_offset)) {
            IMAGE_NT_HEADERS64 nt_headers{};
            const std::uintptr_t nt_address = g_module_base + nt_offset;
            if (SafeRead(nt_address, nt_headers) &&
                nt_headers.Signature == IMAGE_NT_SIGNATURE &&
                nt_headers.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                const std::uintptr_t image_size = nt_headers.OptionalHeader.SizeOfImage;
                if (image_size != 0 && g_module_base <= max_address - image_size) {
                    g_module_end = g_module_base + image_size;
                }
            }
        }
    }
    hook_guard::Result hook_guard_result;
    if (!g_hook_target_guard.Initialize(
            reinterpret_cast<void*>(g_module_base), &hook_guard_result)) {
        log::ErrorF("hook target guard initialization failed status=%s pe_status=%s win32=%u",
                    hook_guard::StatusName(hook_guard_result.status),
                    hook_guard::PeStatusName(hook_guard_result.pe_status),
                    hook_guard_result.win32_error);
        return false;
    }
    struct HookTargetGuardReset {
        ~HookTargetGuardReset() { g_hook_target_guard.Reset(); }
    } hook_target_guard_reset;

    if (hook_layout_supported) {
        cut_content::Initialize(g_module_base,
                                g_use_latest_steam_layout,
                                config_path.parent_path() / L"SPatch.cutcontent.probe");
    }
    g_provider_slot = ResolveAddress(kScaleformTimeProviderRva);
    g_render_task_manager = ResolveAddress(kRenderTaskManagerRva);
    g_render_context_instance_slot = ResolveAddress(kRenderContextInstanceRva);
    g_scenery_counter0 = ResolveAddress(kSceneryCounter0Rva);
    g_scenery_counter1 = ResolveAddress(kSceneryCounter1Rva);
    g_scenery_counter2 = ResolveAddress(kSceneryCounter2Rva);
    g_scenery_counter3 = ResolveAddress(kSceneryCounter3Rva);
    g_d3d_device_slot = ResolveAddress(kD3DDeviceSlotRva);
    g_d3d_context_slot = ResolveAddress(kD3DContextSlotRva);
    g_dxgi_swapchain_slot = ResolveAddress(kDxgiSwapChainSlotRva);
    g_present_rtv_slot = ResolveAddress(kPresentRtvSlotRva);
    g_rumble_apply_object = ResolveAddress(kRumbleApplyObjectRva);
    g_wet_surface_block_counter = ResolveAddress(kWetSurfaceBlockCounterRva);
    g_volumetric_fog_intensity = ResolveAddress(fog_restoration::kIntensityRva);
    g_rumble_apply_helper =
        reinterpret_cast<RumbleApplyHelperFn>(ResolveAddress(kRumbleApplyHelperRva));
    g_simtime_is_paused =
        g_use_latest_steam_layout ? nullptr
                                  : reinterpret_cast<SimTimeIsPausedFn>(
                                        ResolveAddress(kSimTimeIsPausedRva));
    const std::uintptr_t ui_pause_address = ResolveAddress(kUiIsGamePausedRva);
    g_ui_is_game_paused =
        runtime_patch::MatchesBytes(ui_pause_address, kUiIsGamePausedSignature)
            ? reinterpret_cast<UiIsGamePausedFn>(ui_pause_address)
            : nullptr;
    if (!g_use_latest_steam_layout) {
        const std::uintptr_t velocity_address = ResolveAddress(kCharacterPhysicsGetVelocityRva);
        if (runtime_patch::MatchesBytes(velocity_address,
                                       kCharacterPhysicsGetVelocitySignature)) {
            g_character_physics_get_velocity =
                reinterpret_cast<CharacterPhysicsGetVelocityFn>(velocity_address);
        }
        const std::uintptr_t combat_address = ResolveAddress(kUiIsPlayerInCombatRva);
        if (runtime_patch::MatchesBytes(combat_address, kUiIsPlayerInCombatSignature)) {
            g_ui_is_player_in_combat =
                reinterpret_cast<UiIsPlayerInCombatFn>(combat_address);
        }
    }
    g_simobject_get_component =
        reinterpret_cast<SimObjectGetComponentFn>(ResolveAddress(kSimObjectGetComponentRva));
    g_time_of_day_accessor =
        reinterpret_cast<TimeOfDayAccessorFn>(ResolveAddress(kTimeOfDayAccessorRva));
    g_qfile_ready = reinterpret_cast<qfile_io::QFileReadyFn>(ResolveAddress(kQFileReadyRva));
    ResetArchiveEntryGuard();
    g_sampler_builder_first_verified_invocation_logged.store(
        false, std::memory_order_relaxed);
    g_sampler_builder_failure_logged.store(false, std::memory_order_relaxed);
    g_anisotropy_writer_first_verified_invocation_logged.store(
        false, std::memory_order_relaxed);
    g_chase_camera_set_parameters_original = nullptr;
    g_chase_camera_update_original = nullptr;
    g_angular_approach_original = nullptr;
    g_game_camera_set_desired_eye_look_up_original = nullptr;
    g_chase_camera_update_probe_depth = 0;
    g_chase_camera_update_probe_stack = {};
    ResetGtaIvVehicleCameraDynamicState();
    g_gtaiv_camera_probe_sample_count.store(0,
                                                 std::memory_order_relaxed);
    g_gtaiv_camera_probe_log_budget.store(
        (config.gta_iv_car_camera || config.gta_iv_bike_camera) &&
                config.enable_logging
            ? 32u
            : 0u,
        std::memory_order_relaxed);
    g_gtaiv_camera_probe_next_sample_tick.store(0,
                                                std::memory_order_relaxed);
    g_gtaiv_camera_dynamic_probe_sample_count.store(
        0, std::memory_order_relaxed);
    g_gtaiv_camera_dynamic_probe_log_budget.store(
        (config.gta_iv_car_camera || config.gta_iv_bike_camera) &&
                config.enable_logging
            ? 2400u
            : 0u,
        std::memory_order_relaxed);
    g_gtaiv_camera_dynamic_probe_next_sample_tick.store(
        0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_gtaiv_camera_probe_state_mutex);
        g_gtaiv_camera_probe_last_state =
            (std::numeric_limits<unsigned long long>::max)();
        g_gtaiv_camera_probe_last_subject =
            (std::numeric_limits<std::uintptr_t>::max)();
    }
    g_qcmp_rejection_logged.store(false, std::memory_order_relaxed);
    g_resource_chunk_rejection_logged.store(false, std::memory_order_relaxed);
    g_rumble_override_attempted = false;
    g_last_rumble_override_value.store(-1);
    g_aa_runtime_variant_mode.store(ClampAaVariantMode(config.aa_variant_mode));
    g_aa_runtime_aux_mode.store(ClampAaAuxMode(config.aa_aux_mode));

    g_hook_creation_transaction_open = true;
    g_created_hook_targets.clear();
    // Allocate the complete process-lifetime hook registry before SMAA or any
    // other hook can mutate executable code. A low-memory failure remains a
    // clean, transparent initialization failure.
    try {
        g_created_hook_targets.reserve(128);
    } catch (...) {
        g_hook_creation_transaction_open = false;
        log::Error("hook initialization failed: hook registry allocation failed");
        return false;
    }

    bool smaa_initialized = true;
    smaa::SetEnabled(config.hook_smaa_present && config.smaa_enable);
    smaa::SetDebugKeysEnabled(config.hook_smaa_present && config.smaa_debug_keys);
    if (config.hook_smaa_present) {
        smaa::SetPreset(config.smaa_preset);
        smaa_initialized =
            smaa::Initialize(g_module_base,
                             g_d3d_device_slot,
                             g_d3d_context_slot,
                             g_dxgi_swapchain_slot,
                             g_present_rtv_slot);
    }
    StoreSceneryCounters(ReadSceneryCounters());
#if !defined(SPATCH_FINAL_RELEASE)
    g_last_summary_tick.store(GetTickCount64());
#endif
    ResetNisActorTrackerLocked();

    bool ok = !config.hook_smaa_present || smaa_initialized;
    if ((config.fix_cutscene_zero_dt || config.fix_cutscene_scene_time_step) &&
        g_ui_is_game_paused == nullptr) {
        log::Error("cutscene pause function signature mismatch");
        ok = false;
    }
    const bool need_nis_actor_state_hooks =
        config.hook_nis_actor_state || config.fix_nis_actor_restore_duplicates;
#if !defined(SPATCH_FINAL_RELEASE)
    const bool need_wetness_field_probe =
        g_force_wetness_field_probe.load(std::memory_order_relaxed);
    const bool need_sweat_field_probe =
        g_sweat_field_probe.load(std::memory_order_relaxed);
#else
    constexpr bool need_wetness_field_probe = false;
    constexpr bool need_sweat_field_probe = false;
#endif
    const character_hooks::HookPlan character_hook_plan = character_hooks::BuildHookPlan({
        .restore_wetness = config.restore_character_wetness,
        .restore_sweat = config.restore_character_sweat,
        .regression_probe = config.hook_character_regression_probe,
        .wetness_field_probe = need_wetness_field_probe,
        .sweat_field_probe = need_sweat_field_probe,
    });
    const bool need_character_regression_probe =
        character_hook_plan.install_regression_hooks;
    const bool need_character_effects_hooks = character_hook_plan.install_effects_hooks;
    const bool need_character_damage_hooks =
        character_hook_plan.require_simobject_component;
    g_sweat_only_health_fast_path =
        character_hook_plan.use_sweat_only_health_fast_path;

    if (config.hook_nis_actor_state && g_use_latest_steam_layout) {
        log::Warn(
            "nis_actor_state probe requested on latest_steam, but only legacy_researched is mapped; probe disabled");
    }
    if (config.fix_nis_actor_restore_duplicates && g_use_latest_steam_layout) {
        log::Warn(
            "fix_nis_actor_restore_duplicates requested on latest_steam, but only legacy_researched is mapped; fix disabled");
    }
    if (config.hook_twitch_probe && g_use_latest_steam_layout) {
        log::Warn(
            "twitch_probe requested on latest_steam, but only legacy_researched is mapped; probe disabled");
    }
    if (config.override_rumble_enabled >= 0 &&
        (g_rumble_apply_helper == nullptr || g_rumble_apply_object == 0)) {
        log::Error("rumble override resolution failed");
        ok = false;
    }
    if (need_character_damage_hooks && g_simobject_get_component == nullptr) {
        log::Error("simobject_get_component resolution failed");
        ok = false;
    }
    if (config.restore_character_sweat && g_character_physics_get_velocity == nullptr &&
        g_ui_is_player_in_combat == nullptr) {
        log::Warn(
            "character sweat restoration disabled: exertion inputs are not mapped for this executable");
    }
    if (config.hook_smaa_present &&
        (g_d3d_device_slot == 0 || g_d3d_context_slot == 0 || g_dxgi_swapchain_slot == 0 ||
         g_present_rtv_slot == 0)) {
        log::Error("smaa slot resolution failed");
        ok = false;
    }
    if (config.restore_original_fog &&
        (g_volumetric_fog_intensity < g_module_base ||
         g_module_end < g_volumetric_fog_intensity + sizeof(float))) {
        log::Error("original atmosphere intensity resolution failed");
        ok = false;
    }
    if (!ok) {
        g_hook_creation_transaction_open = false;
    }
    g_pedestrian_throttle_frame_count.store(0, std::memory_order_relaxed);
    g_pedestrian_throttle_stock_call_count.store(0, std::memory_order_relaxed);
    g_pedestrian_throttle_clamped_frame_count.store(0, std::memory_order_relaxed);
    g_average_window_initialize_count.store(0, std::memory_order_relaxed);
    g_average_window_expanded_count.store(0, std::memory_order_relaxed);
    g_average_window_fallback_logged.store(false, std::memory_order_relaxed);
    g_fog_clamp_warning_emitted.store(false, std::memory_order_relaxed);
    g_original_fog_applied.store(false, std::memory_order_relaxed);
    g_original_fog_frame_initialized.store(false, std::memory_order_relaxed);
    g_original_fog_failure_logged.store(false, std::memory_order_relaxed);
    g_original_fog_setter_logged.store(false, std::memory_order_relaxed);
    g_character_eye_restore_applied_mask.store(0, std::memory_order_relaxed);
    g_character_eye_restore_already_present_mask.store(0, std::memory_order_relaxed);
    g_character_eye_restore_failure_mask.store(0, std::memory_order_relaxed);
    log::InfoF("engine_fixes pedestrian_density=%d spherical_reflections=%d "
               "original_fog_and_neon=%d original_eye_reflections=%d remove_120_fps_cap=%d "
               "resource_loading=%d corrupt_save_guard=%d thread_failure_guard=%d",
               config.fix_pedestrian_density_at_high_fps ? 1 : 0,
               config.improve_spherical_reflections ? 1 : 0,
               config.restore_original_fog ? 1 : 0,
               config.restore_original_eye_reflections ? 1 : 0,
               config.remove_hidden_120_fps_cap ? 1 : 0,
               config.fix_resource_loading ? 1 : 0,
               config.fix_corrupt_save_handling ? 1 : 0,
               config.fix_thread_creation_failure ? 1 : 0);

    const bool anisotropy_override_requested =
        texture_filtering::ShouldInstallWriter(
            config.anisotropic_filtering);
    const texture_filtering::AddressProfile texture_filtering_addresses =
        texture_filtering::SelectAddresses(g_use_latest_steam_layout);
    const std::uintptr_t sampler_builder_address =
        anisotropy_override_requested && hook_layout_supported
            ? ResolveAddress(
                  texture_filtering_addresses.sampler_builder_rva)
            : 0;
    if (anisotropy_override_requested) {
        const std::uintptr_t writer_address =
            hook_layout_supported
                ? ResolveAddress(
                      texture_filtering_addresses.anisotropy_writer_rva)
                : 0;
        bool installed = false;
        if (writer_address == 0) {
            log::Warn(
                "texture_filtering anisotropy_writer disabled=1 "
                "reason=unsupported_hook_layout");
        } else if (ValidateHookSignature(
                       writer_address,
                       texture_filtering::kAnisotropyWriterSignature,
                       "anisotropy_writer")) {
            installed = CreateHookDetour(
                writer_address,
                reinterpret_cast<void*>(&DetourAnisotropyWriter),
                reinterpret_cast<void**>(&g_anisotropy_writer_original),
                "anisotropy_writer");
        }
        ok &= installed;
        log::InfoF(
            "texture_filtering anisotropy_writer requested=%d installed=%d "
            "layout=%s target=0x%p behavior=pending_commit",
            config.anisotropic_filtering,
            installed ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" : "legacy_researched",
            reinterpret_cast<void*>(writer_address));
    }

    if (config.fix_resource_loading) {
        const std::uintptr_t pc_file_read_address = ResolveAddress(kPcFileReadRva);
        const std::uintptr_t pc_file_seek_address = ResolveAddress(kPcFileSeekRva);
        const std::uintptr_t pc_file_tell_address = ResolveAddress(kPcFileTellRva);
        const std::uintptr_t pc_file_size_address = ResolveAddress(kPcFileSizeRva);
        const std::uintptr_t qfile_read_at_address = ResolveAddress(kQFileReadAtRva);
        const std::uintptr_t qfile_write_at_address = ResolveAddress(kQFileWriteAtRva);
        const std::uintptr_t qfile_ready_address = ResolveAddress(kQFileReadyRva);
        const std::uintptr_t qcmp_decompress_address = ResolveAddress(kQcmpDecompressRva);
        const std::uintptr_t stream_file_open_address = ResolveAddress(kStreamFileOpenRva);
        const std::uintptr_t stream_file_close_address = ResolveAddress(kStreamFileCloseRva);
        const std::uintptr_t resource_chunk_dispatch_address =
            ResolveAddress(kResourceChunkDispatchRva);
        const bool qfile_targets_valid =
        ValidateHookSignature(pc_file_read_address, kPcFileReadSignature, "pc_file_read") &&
        ValidateHookSignature(pc_file_seek_address, kPcFileSeekSignature, "pc_file_seek") &&
        ValidateHookSignature(pc_file_tell_address, kPcFileTellAndSizeSignature, "pc_file_tell") &&
        ValidateHookSignature(pc_file_size_address, kPcFileTellAndSizeSignature, "pc_file_size") &&
        ValidateHookSignature(qfile_read_at_address, kQFileReadAtSignature, "qfile_read_at") &&
        ValidateHookSignature(qfile_write_at_address, kQFileWriteAtSignature, "qfile_write_at") &&
        ValidateHookSignature(qfile_ready_address, kQFileReadySignature, "qfile_ready") &&
        ValidateHookSignature(qcmp_decompress_address, kQcmpDecompressSignature,
                              "qcmp_decompress") &&
        ValidateHookSignature(stream_file_open_address, kStreamFileOpenSignature,
                              "stream_file_open") &&
        ValidateHookSignature(stream_file_close_address, kStreamFileCloseSignature,
                              "stream_file_close") &&
        ValidateHookSignature(resource_chunk_dispatch_address,
                              kResourceChunkDispatchSignature,
                              "resource_chunk_dispatch");
        if (qfile_targets_valid) {
            g_stream_file_close = reinterpret_cast<StreamFileCloseFn>(stream_file_close_address);
            const bool read_installed =
            CreateHookDetour(pc_file_read_address, reinterpret_cast<void*>(&DetourPcFileRead),
                             reinterpret_cast<void**>(&g_pc_file_read_original), "pc_file_read");
            const bool seek_installed =
            CreateHookDetour(pc_file_seek_address, reinterpret_cast<void*>(&DetourPcFileSeek),
                             reinterpret_cast<void**>(&g_pc_file_seek_original), "pc_file_seek");
            const bool tell_installed =
            CreateHookDetour(pc_file_tell_address, reinterpret_cast<void*>(&DetourPcFileTell),
                             reinterpret_cast<void**>(&g_pc_file_tell_original), "pc_file_tell");
            const bool size_installed =
            CreateHookDetour(pc_file_size_address, reinterpret_cast<void*>(&DetourPcFileSize),
                             reinterpret_cast<void**>(&g_pc_file_size_original), "pc_file_size");
            const bool read_at_installed =
            CreateHookDetour(qfile_read_at_address, reinterpret_cast<void*>(&DetourQFileReadAt),
                             reinterpret_cast<void**>(&g_qfile_read_at_original), "qfile_read_at");
            const bool write_at_installed = CreateHookDetour(
            qfile_write_at_address, reinterpret_cast<void*>(&DetourQFileWriteAt),
            reinterpret_cast<void**>(&g_qfile_write_at_original), "qfile_write_at");
            const bool qcmp_guard_installed = CreateHookDetour(
            qcmp_decompress_address, reinterpret_cast<void*>(&DetourQcmpDecompress),
            reinterpret_cast<void**>(&g_qcmp_decompress_original), "qcmp_decompress");
            const bool archive_guard_installed = CreateHookDetour(
            stream_file_open_address, reinterpret_cast<void*>(&DetourStreamFileOpen),
            reinterpret_cast<void**>(&g_stream_file_open_original), "stream_file_open");
            const bool resource_chunk_guard_installed = CreateHookDetour(
            resource_chunk_dispatch_address,
            reinterpret_cast<void*>(&DetourResourceChunkDispatch),
            reinterpret_cast<void**>(&g_resource_chunk_dispatch_original),
            "resource_chunk_dispatch");
            const bool qfile_io_installed =
                read_installed && seek_installed && tell_installed && size_installed &&
                read_at_installed && write_at_installed && qcmp_guard_installed &&
                archive_guard_installed && resource_chunk_guard_installed;
            ok &= qfile_io_installed;
            if (qfile_io_installed) {
                log::Info(
                    "engine_fix requested=1 hooks_installed=1 behavior=pending_commit "
                    "qfile_io_error_propagation=1 archive_entry_bounds=1 qcmp_bounds=1 "
                    "resource_chunk_bounds=1 big_file_index_bounds=1");
            }
        } else {
            ok = false;
        }
    }

    if (config.fix_high_fps_average_windows) {
        const std::uintptr_t address = ResolveAddress(kAverageWindowInitializeRva);
        if (ValidateHookSignature(address, kAverageWindowInitializeSignature,
                                  "average_window_initialize")) {
            const bool installed =
                CreateHookDetour(address, reinterpret_cast<void*>(&DetourAverageWindowInitialize),
                                 reinterpret_cast<void**>(&g_average_window_initialize_original),
                                 "average_window_initialize");
            ok &= installed;
            if (installed) {
                log::InfoF("average_window_fix requested=1 hooks_installed=1 "
                           "behavior=pending_commit sample_rate=%.0f max_entries=%zu",
                           average_window::kExpandedSampleRate,
                           average_window::kMaximumEntries);
            }
        }
    }

    if (config.fix_pedestrian_density_at_high_fps) {
        const std::uintptr_t update_address = ResolveAddress(kPedestrianSpawnUpdateRva);
        const std::uintptr_t throttle_address =
            ResolveAddress(kPedestrianFrameRateThrottleRva);
        const auto& throttle_signature = g_use_latest_steam_layout
                                             ? kLatestPedestrianThrottleSignature
                                             : kLegacyPedestrianThrottleSignature;
        if (ValidateHookSignature(
                update_address, kPedestrianSpawnUpdateSignature, "pedestrian_spawn_update") &&
            ValidateHookSignature(throttle_address,
                                  throttle_signature,
                                  "pedestrian_frame_rate_throttle")) {
            const bool update_installed = CreateHookDetour(
                update_address,
                reinterpret_cast<void*>(&DetourPedestrianSpawnUpdate),
                reinterpret_cast<void**>(&g_pedestrian_spawn_update_original),
                "pedestrian_spawn_update");
            const bool throttle_installed = CreateHookDetour(
                throttle_address,
                reinterpret_cast<void*>(&DetourPedestrianFrameRateThrottle),
                reinterpret_cast<void**>(&g_pedestrian_frame_rate_throttle_original),
                "pedestrian_frame_rate_throttle");
            ok &= update_installed && throttle_installed;
            if (update_installed && throttle_installed) {
                log::InfoF(
                    "pedestrian_density_fix requested=1 hooks_installed=1 "
                    "behavior=pending_commit cadence_hz=%.1f max_steps_per_frame=%u",
                    pedestrian_timing::kStockThrottleHz,
                    pedestrian_timing::kMaximumStepsPerFrame);
            }
        }
    }

    if (config.hook_queue_ready) {
        ok &= CreateHookDetour(ResolveAddress(kTaskReadyRva),
                               reinterpret_cast<void*>(&DetourTaskReady),
                               reinterpret_cast<void**>(&g_task_ready_original), "task_ready");
    }

    if (config.hook_task_dispatch) {
        ok &= CreateHookDetour(
            ResolveAddress(kTaskDispatchRva), reinterpret_cast<void*>(&DetourTaskDispatch),
            reinterpret_cast<void**>(&g_task_dispatch_original), "task_dispatch");
    }

    if (config.hook_wait_helper) {
        ok &= CreateHookDetour(ResolveAddress(kWaitHelperRva),
                               reinterpret_cast<void*>(&DetourWaitHelper),
                               reinterpret_cast<void**>(&g_wait_helper_original), "wait_helper");
    }

    if (config.hook_scaleform_time) {
        ok &= CreateHookDetour(
            ResolveAddress(kScaleformTimeRva), reinterpret_cast<void*>(&DetourScaleformTime),
            reinterpret_cast<void**>(&g_scaleform_time_original), "scaleform_time");
    }

    if (config.hook_scaleform_init) {
        ok &= CreateHookDetour(
            ResolveAddress(kScaleformInitRva), reinterpret_cast<void*>(&DetourScaleformInit),
            reinterpret_cast<void**>(&g_scaleform_init_original), "scaleform_init");
    }

    const bool need_nis_timing_hook = config.hook_nis_timing || config.fix_cutscene_scene_time_step;
    const bool need_nis_owner_hook = config.hook_nis_owner || config.fix_cutscene_scene_time_step;
    // Scene-time repair uses the owner cadence tracker to distinguish a brief
    // legacy 30/60-Hz leak from a genuine sustained slowdown. It therefore
    // owns CutsceneFlowOwner even when zero-dt/frameflow repair is disabled.
    const bool need_cutscene_frameflow_hook = config.hook_frameflow ||
                                              config.fix_cutscene_zero_dt ||
                                              config.fix_cutscene_scene_time_step;

    if (need_nis_timing_hook) {
        ok &= CreateHookDetour(
            ResolveAddress(kNisSetPlayTimeRva), reinterpret_cast<void*>(&DetourNisSetPlayTime),
            reinterpret_cast<void**>(&g_nis_set_play_time_original), "nis_set_play_time");
    }

    if (config.hook_nis_runtime) {
        ok &= CreateHookDetour(ResolveAddress(kNisPlayRva), reinterpret_cast<void*>(&DetourNisPlay),
                               reinterpret_cast<void**>(&g_nis_play_original), "nis_play");
        ok &= CreateHookDetour(
            ResolveAddress(kNisBootstrapRva), reinterpret_cast<void*>(&DetourNisBootstrap),
            reinterpret_cast<void**>(&g_nis_bootstrap_original), "nis_bootstrap");
    }

    if (need_nis_owner_hook) {
        ok &=
            CreateHookDetour(ResolveAddress(kNisOwnerRva), reinterpret_cast<void*>(&DetourNisOwner),
                             reinterpret_cast<void**>(&g_nis_owner_original), "nis_owner");
    }
    if (need_nis_actor_state_hooks && !g_use_latest_steam_layout) {
        ok &= CreateHookDetour(
            ResolveAddress(kNisActorSetupRva), reinterpret_cast<void*>(&DetourNisActorSetup),
            reinterpret_cast<void**>(&g_nis_actor_setup_original), "nis_actor_setup");
        ok &= CreateHookDetour(
            ResolveAddress(kNisActorRestoreRva), reinterpret_cast<void*>(&DetourNisActorRestore),
            reinterpret_cast<void**>(&g_nis_actor_restore_original), "nis_actor_restore");
    }
    if (config.hook_twitch_probe && !g_use_latest_steam_layout) {
        ok &= CreateHookDetour(
            ResolveAddress(kTwitchStateTickRva), reinterpret_cast<void*>(&DetourTwitchStateTick),
            reinterpret_cast<void**>(&g_twitch_state_tick_original), "twitch_tick");
        ok &= CreateHookDetour(ResolveAddress(kTwitchLoginCallbackRva),
                               reinterpret_cast<void*>(&DetourTwitchLoginCallback),
                               reinterpret_cast<void**>(&g_twitch_login_callback_original),
                               "twitch_login_callback");
    }

    if (config.restore_original_fog) {
        const std::uintptr_t setter_address =
            ResolveAddress(fog_restoration::kLegacySetterRva);
        const auto setter_signature =
            fog_restoration::SetterSignature(g_use_latest_steam_layout);
        if (ValidateHookSignature(
                setter_address, setter_signature, "volumetric_fog_setter")) {
            ok &= CreateHookDetour(
                setter_address,
                reinterpret_cast<void*>(&DetourVolumetricFogSetter),
                reinterpret_cast<void**>(&g_volumetric_fog_setter_original),
                "volumetric_fog_setter");
        }
    }

    const bool need_frameflow_hook = config.hook_frameflow || config.fix_cutscene_zero_dt ||
                                     config.restore_original_fog ||
                                     config.override_rumble_enabled >= 0 ||
                                     cut_content::IsArmed();
    if (need_frameflow_hook) {
        ok &= CreateHookDetour(ResolveAddress(kFrameFlowRva),
                               reinterpret_cast<void*>(&DetourFrameFlow),
                               reinterpret_cast<void**>(&g_frame_flow_original), "frame_flow");
    }
    if (need_cutscene_frameflow_hook) {
        ok &= CreateHookDetour(ResolveAddress(kCutsceneFlowOwnerRva),
                               reinterpret_cast<void*>(&DetourCutsceneFlowOwner),
                               reinterpret_cast<void**>(&g_cutscene_flow_owner_original),
                               "cutscene_flow_owner");
    }

    if (config.gta_iv_car_camera || config.gta_iv_bike_camera) {
        const vehicle_camera::AddressProfile camera_addresses =
            vehicle_camera::SelectAddresses(g_use_latest_steam_layout);
        const vehicle_camera::DynamicAddressProfile dynamic_addresses =
            vehicle_camera::SelectDynamicAddresses(
                g_use_latest_steam_layout);
        const std::uintptr_t setter_address =
            hook_layout_supported
                ? ResolveAddress(camera_addresses.parameter_setter_rva)
                : 0;
        const std::uintptr_t update_address =
            hook_layout_supported
                ? ResolveAddress(dynamic_addresses.update_rva)
                : 0;
        const std::uintptr_t desired_pose_address =
            hook_layout_supported
                ? ResolveAddress(dynamic_addresses.desired_eye_look_up_rva)
                : 0;
        const std::uintptr_t angular_approach_address =
            hook_layout_supported
                ? ResolveAddress(dynamic_addresses.angular_approach_rva)
                : 0;
        const auto angular_approach_signature =
            vehicle_camera::SelectAngularApproachSignature(
                g_use_latest_steam_layout);
        bool setter_installed = false;
        bool update_installed = false;
        bool desired_pose_installed = false;
        bool angular_approach_installed = false;
        if (!hook_layout_supported) {
            log::WarnF(
                "gtaiv_vehicle_camera car_requested=%d bike_requested=%d "
                "installed=0 reason=unsupported_hook_layout",
                config.gta_iv_car_camera ? 1 : 0,
                config.gta_iv_bike_camera ? 1 : 0);
        } else {
            const bool camera_preflight =
                PreflightRequiredHookTarget(
                    setter_address,
                    vehicle_camera::kParameterSetterSignature,
                    "chase_camera_set_parameters") &&
                PreflightRequiredHookTarget(
                    update_address,
                    vehicle_camera::kChaseUpdateSignature,
                    "chase_camera_update") &&
                PreflightRequiredHookTarget(
                    desired_pose_address,
                    vehicle_camera::kDesiredEyeLookUpSignature,
                    "game_camera_desired_pose") &&
                PreflightRequiredHookTarget(
                    angular_approach_address,
                    angular_approach_signature,
                    "angular_approach");
            if (!camera_preflight) {
                ok = false;
            } else {
                setter_installed = CreateHookDetour(
                    setter_address,
                    reinterpret_cast<void*>(
                        &DetourChaseCameraSetParameters),
                    reinterpret_cast<void**>(
                        &g_chase_camera_set_parameters_original),
                    "chase_camera_set_parameters");
                ok &= setter_installed;
                if (setter_installed) {
                    update_installed = CreateHookDetour(
                        update_address,
                        reinterpret_cast<void*>(&DetourChaseCameraUpdate),
                        reinterpret_cast<void**>(
                            &g_chase_camera_update_original),
                        "chase_camera_update");
                }
                if (update_installed) {
                    desired_pose_installed = CreateHookDetour(
                        desired_pose_address,
                        reinterpret_cast<void*>(
                            &DetourGameCameraSetDesiredEyeLookUp),
                        reinterpret_cast<void**>(
                            &g_game_camera_set_desired_eye_look_up_original),
                        "game_camera_desired_pose");
                }
                if (desired_pose_installed) {
                    angular_approach_installed = CreateHookDetour(
                        angular_approach_address,
                        reinterpret_cast<void*>(&DetourAngularApproach),
                        reinterpret_cast<void**>(&g_angular_approach_original),
                        "angular_approach");
                }
            }
        }
        const bool installed = setter_installed && update_installed &&
                               desired_pose_installed &&
                               angular_approach_installed;
        if (hook_layout_supported) {
            ok &= installed;
        }
        log::InfoF(
            "gtaiv_vehicle_camera car_requested=%d bike_requested=%d "
            "installed=%d setter_installed=%d update_installed=%d "
            "desired_pose_installed=%d angular_approach_installed=%d "
            "layout=%s target=0x%p update_target=0x%p "
            "desired_pose_target=0x%p angular_approach_target=0x%p "
            "mode=%s mutation=%d dynamics_mutation=%d "
            "car_behavior=road_vehicle_drive_and_flee "
            "heavy_drive_alias_guard=base_selector_replay "
            "bike_behavior=motorcycle_drive_block direction=right "
            "lateral_offset_m=%.3f",
            config.gta_iv_car_camera ? 1 : 0,
            config.gta_iv_bike_camera ? 1 : 0,
            installed ? 1 : 0,
            setter_installed ? 1 : 0,
            update_installed ? 1 : 0,
            desired_pose_installed ? 1 : 0,
            angular_approach_installed ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" :
                                        "legacy_researched",
            reinterpret_cast<void*>(setter_address),
            reinterpret_cast<void*>(update_address),
            reinterpret_cast<void*>(desired_pose_address),
            reinterpret_cast<void*>(angular_approach_address),
            installed ? "active" : "disabled",
            installed ? 1 : 0,
            installed ? 1 : 0,
            static_cast<double>(
                vehicle_camera::kDriverSideLateralOffsetMeters));
    }

    if (config.hook_fog_slicing_guard) {
        ok &= CreateHookDetour(
            ResolveAddress(kFogSlicingModeRva), reinterpret_cast<void*>(&DetourFogSlicingMode),
            reinterpret_cast<void**>(&g_fog_slicing_mode_original), "fog_slicing_mode");
    }

    // Stock-AA suppression is a behavioral dependency of SMAA, not a
    // developer-probe dependency.  The latest Steam profile has no verified
    // AntiAlias RVAs, so leave suppression off there instead of patching an
    // unvalidated address.
    const bool need_stock_aa_suppression =
        config.hook_smaa_present && config.smaa_disable_stock_aa &&
        (config.smaa_enable || config.smaa_debug_keys) && !g_use_latest_steam_layout;
    const bool need_aa_owner_hook =
        !g_use_latest_steam_layout && (config.hook_aa_probe || need_stock_aa_suppression);
    const bool need_aa_fx_hook =
        !g_use_latest_steam_layout && (config.hook_aa_fx_probe || need_stock_aa_suppression);
    if ((config.hook_aa_probe || config.hook_aa_fx_probe) && g_use_latest_steam_layout) {
        log::Warn("AA probes requested on latest_steam, but AntiAlias targets are not mapped; "
                  "probes disabled");
    }
    if (config.hook_smaa_present && config.smaa_disable_stock_aa && g_use_latest_steam_layout) {
        log::Warn("smaa_disable_stock_aa requested on latest_steam, but AntiAlias targets are not "
                  "mapped; stock AA remains active");
    }

    if (need_aa_owner_hook) {
        const bool installed = CreateHookDetour(
            ResolveAddress(kAntiAliasOwnerRva), reinterpret_cast<void*>(&DetourAntiAliasOwner),
            reinterpret_cast<void**>(&g_antialias_owner_original), "antialias_owner");
        ok &= installed;
        log::InfoF("aa_owner_hook requested=1 installed=%d probe=%d stock_suppression=%d",
                   installed ? 1 : 0,
                   config.hook_aa_probe ? 1 : 0,
                   need_stock_aa_suppression ? 1 : 0);
    }

    if (need_aa_fx_hook) {
        const bool installed = CreateHookDetour(
            ResolveAddress(kAntiAliasFxHandoffRva),
            reinterpret_cast<void*>(&DetourAntiAliasFxHandoff),
            reinterpret_cast<void**>(&g_antialias_fx_handoff_original),
            "antialias_fx_handoff");
        ok &= installed;
        log::InfoF("aa_fx_hook requested=1 installed=%d probe=%d stock_suppression=%d",
                   installed ? 1 : 0,
                   config.hook_aa_fx_probe ? 1 : 0,
                   need_stock_aa_suppression ? 1 : 0);
    }
    const bool need_aa_aux_hook =
        !g_use_latest_steam_layout && (config.hook_aa_probe || config.hook_aa_fx_probe ||
                                       config.aa_aux_mode != 0 || config.aa_aux_debug_keys);
    if (need_aa_aux_hook) {
        ok &= CreateHookDetour(ResolveAddress(kAntiAliasAuxStateQueryRva),
                               reinterpret_cast<void*>(&DetourAntiAliasAuxStateQuery),
                               reinterpret_cast<void**>(&g_antialias_aux_state_query_original),
                               "antialias_aux_state_query");
    }
    if (config.hook_aa_probe || config.hook_aa_fx_probe) {
        log::InfoF("aa_variant mode=%d debug_keys=%d key_down=F6 key_up=F7",
                   g_aa_runtime_variant_mode.load(), config.aa_variant_debug_keys ? 1 : 0);
        log::InfoF("aa_aux mode=%d debug_keys=%d key_down=F4 key_up=F5",
                   g_aa_runtime_aux_mode.load(), config.aa_aux_debug_keys ? 1 : 0);
    }
    if (config.hook_smaa_present) {
        log::InfoF("smaa requested=%d debug_keys=%d preset=%d toggle=F2 device_slot=0x%p "
                   "context_slot=0x%p swapchain_slot=0x%p rtv_slot=0x%p",
                   config.smaa_enable ? 1 : 0, config.smaa_debug_keys ? 1 : 0, config.smaa_preset,
                   reinterpret_cast<void*>(g_d3d_device_slot),
                   reinterpret_cast<void*>(g_d3d_context_slot),
                   reinterpret_cast<void*>(g_dxgi_swapchain_slot),
                   reinterpret_cast<void*>(g_present_rtv_slot));
    }

    if (config.hook_post_material_submit) {
        ok &= CreateHookDetour(ResolveAddress(kRenderSubmitMaterialRva),
                               reinterpret_cast<void*>(&DetourRenderSubmitMaterial),
                               reinterpret_cast<void**>(&g_render_submit_material_original),
                               "render_submit_material");
    }

    if (config.hook_smaa_present) {
        ok &= CreateHookDetour(ResolveAddress(kPresentBufferRva),
                               reinterpret_cast<void*>(&DetourPresentBuffer),
                               reinterpret_cast<void**>(&g_present_buffer_original),
                               "present_buffer");
    }

#if !defined(SPATCH_FINAL_RELEASE)
    if (need_wetness_field_probe) {
        if (g_use_latest_steam_layout) {
            log::Warn("wetness memcpy diagnostic is unavailable for the latest Steam layout");
        } else {
            const std::uintptr_t memcpy_address = ResolveAddress(kLegacyGameMemcpyRva);
            const bool signature_valid = ValidateHookSignature(
                memcpy_address, kLegacyGameMemcpySignature, "wetness_memcpy_diagnostic");
            ok &= signature_valid;
            if (signature_valid) {
                ok &= CreateHookDetour(memcpy_address,
                                       reinterpret_cast<void*>(&DetourGameMemcpy),
                                       reinterpret_cast<void**>(&g_game_memcpy_original),
                                       "wetness_memcpy_diagnostic");
            }
        }
    }
#endif

    bool character_effects_hooks_installed = !need_character_effects_hooks;
    if (need_character_effects_hooks) {
        const bool water_hook_installed = CreateHookDetour(
            ResolveAddress(kCharacterHandleWaterCollisionRva),
            reinterpret_cast<void*>(&DetourCharacterHandleWaterCollision),
            reinterpret_cast<void**>(&g_character_handle_water_collision_original),
            "character_handle_water_collision");
        const bool update_hook_installed = CreateHookDetour(
            ResolveAddress(kCharacterEffectsUpdateRva),
            reinterpret_cast<void*>(&DetourCharacterEffectsUpdate),
            reinterpret_cast<void**>(&g_character_effects_update_original),
            "character_effects_update");
        character_effects_hooks_installed =
            water_hook_installed && update_hook_installed;
        ok &= character_effects_hooks_installed;
    }

    bool health_damage_hook_installed =
        !character_hook_plan.install_health_damage_hook;
    bool character_regression_hooks_installed =
        !need_character_regression_probe;
    if (need_character_regression_probe) {
        character_regression_hooks_installed = CreateHookDetour(
            ResolveAddress(kHealthApplyDamageRva),
            reinterpret_cast<void*>(&DetourHealthApplyDamage),
            reinterpret_cast<void**>(&g_health_apply_damage_original),
            "health_apply_damage");
        health_damage_hook_installed = character_regression_hooks_installed;
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterPaintOwnerRva),
            reinterpret_cast<void*>(&DetourCharacterPaintOwner),
            reinterpret_cast<void**>(&g_character_paint_owner_original),
            "character_paint_owner");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterPaintConsumerRva),
            reinterpret_cast<void*>(&DetourCharacterPaintConsumer),
            reinterpret_cast<void**>(&g_character_paint_consumer_original),
            "character_paint_consumer");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterEffectDispatchOwnerRva),
            reinterpret_cast<void*>(&DetourCharacterEffectDispatchOwner),
            reinterpret_cast<void**>(&g_character_effect_dispatch_owner_original),
            "character_effect_dispatch_owner");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterEffectDispatchConsumeRva),
            reinterpret_cast<void*>(&DetourCharacterEffectDispatchConsume),
            reinterpret_cast<void**>(&g_character_effect_dispatch_consume_original),
            "character_effect_dispatch_consume");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterEffectQueueBuilderRva),
            reinterpret_cast<void*>(&DetourCharacterEffectQueueBuilder),
            reinterpret_cast<void**>(&g_character_effect_queue_builder_original),
            "character_effect_queue_builder");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterAnimationApplyCharredEffectRva),
            reinterpret_cast<void*>(&DetourCharacterAnimationApplyCharredEffect),
            reinterpret_cast<void**>(&g_character_animation_apply_charred_effect_original),
            "character_animation_apply_charred_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterAnimationApplyPaintEffectRva),
            reinterpret_cast<void*>(&DetourCharacterAnimationApplyPaintEffect),
            reinterpret_cast<void**>(&g_character_animation_apply_paint_effect_original),
            "character_animation_apply_paint_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterAnimationCreateDamageRigRva),
            reinterpret_cast<void*>(&DetourCharacterAnimationCreateDamageRig),
            reinterpret_cast<void**>(&g_character_animation_create_damage_rig_original),
            "character_animation_create_damage_rig");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kDamageRigApplyCharredEffectRva),
            reinterpret_cast<void*>(&DetourDamageRigApplyCharredEffect),
            reinterpret_cast<void**>(&g_damage_rig_apply_charred_effect_original),
            "damage_rig_apply_charred_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kDamageRigApplyPaintEffectRva),
            reinterpret_cast<void*>(&DetourDamageRigApplyPaintEffect),
            reinterpret_cast<void**>(&g_damage_rig_apply_paint_effect_original),
            "damage_rig_apply_paint_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterDamageRigResetDamageRva),
            reinterpret_cast<void*>(&DetourCharacterDamageRigResetDamage),
            reinterpret_cast<void**>(&g_character_damage_rig_reset_damage_original),
            "character_damage_rig_reset_damage");
        ok &= character_regression_hooks_installed;
    } else if (character_hook_plan.install_health_damage_hook) {
        health_damage_hook_installed = CreateHookDetour(
            ResolveAddress(kHealthApplyDamageRva),
            reinterpret_cast<void*>(&DetourHealthApplyDamage),
            reinterpret_cast<void**>(&g_health_apply_damage_original),
            "health_apply_damage");
        ok &= health_damage_hook_installed;
    }

    if (config.restore_character_wetness) {
        log::InfoF(
            "character_wetness_restore requested=1 hooks_installed=%d scope=player_npc full_time_seconds=%d fade_time_seconds=%d",
            character_effects_hooks_installed ? 1 : 0,
            config.wetness_full_time_seconds,
            config.wetness_fade_time_seconds);
    }
    if (config.restore_character_sweat) {
        const bool sweat_hooks_installed =
            character_effects_hooks_installed && health_damage_hook_installed;
        log::InfoF(
            "character_sweat_restore requested=1 hooks_installed=%d scope=player_npc build_seconds=%d fade_seconds=%d onset_seconds=%d run_speed=%.2f combat_time=%d velocity_input=%d combat_input=%d",
            sweat_hooks_installed ? 1 : 0,
            config.sweat_build_time_seconds,
            config.sweat_fade_time_seconds,
            config.sweat_onset_time_seconds,
            config.sweat_run_speed,
            config.sweat_combat_time_seconds,
            g_character_physics_get_velocity != nullptr ? 1 : 0,
            g_ui_is_player_in_combat != nullptr ? 1 : 0);
    }
    if (need_character_regression_probe) {
        const bool probe_hooks_installed =
            character_effects_hooks_installed &&
            character_regression_hooks_installed;
        log::InfoF(
            "character_regression_probe requested=1 hooks_installed=%d "
            "wetness=HandleWaterCollision/CharacterEffects::Update/DispatchOwner/DispatchConsume/"
            "CharacterPaintConsumer material=ApplyCharredEffect/ApplyPaintEffect "
            "damage=ApplyHealthDamage/CreateDamageRig/ResetDamage",
            probe_hooks_installed ? 1 : 0);
    }

    if (config.override_rumble_enabled >= 0) {
        log::InfoF("rumble_override configured=%d object=0x%p helper=0x%p",
                   config.override_rumble_enabled, reinterpret_cast<void*>(g_rumble_apply_object),
                   reinterpret_cast<void*>(g_rumble_apply_helper));
    }
    if (config.hook_scenery_builders) {
        ok &= CreateHookDetour(
            ResolveAddress(kSceneryPrepareRva), reinterpret_cast<void*>(&DetourSceneryPrepare),
            reinterpret_cast<void**>(&g_scenery_prepare_original), "scenery_prepare");
        ok &= CreateHookDetour(
            ResolveAddress(kScenerySetupRva), reinterpret_cast<void*>(&DetourScenerySetup),
            reinterpret_cast<void**>(&g_scenery_setup_original), "scenery_setup");
        ok &= CreateHookDetour(ResolveAddress(kRenderSceneryBuilderRva),
                               reinterpret_cast<void*>(&DetourRenderSceneryBuilder),
                               reinterpret_cast<void**>(&g_render_scenery_builder_original),
                               "render_scenery_builder");
        ok &= CreateHookDetour(ResolveAddress(kRasterizeBucketBuilderRva),
                               reinterpret_cast<void*>(&DetourRasterizeBucketBuilder),
                               reinterpret_cast<void**>(&g_rasterize_bucket_builder_original),
                               "rasterize_bucket_builder");
    }

    // Install this narrow resource-load hook last. It avoids the confirmed
    // render-submit hot path and minimizes the pre-commit window in which a
    // streamed material could pass through while behavior is still gated.
    if (config.restore_original_eye_reflections) {
        const std::uintptr_t material_on_load_address = ResolveAddress(kMaterialOnLoadRva);
        if (ValidateHookSignature(
                material_on_load_address, kMaterialOnLoadSignature, "material_on_load")) {
            const bool installed = CreateHookDetour(
                material_on_load_address,
                reinterpret_cast<void*>(&DetourMaterialOnLoad),
                reinterpret_cast<void**>(&g_material_on_load_original),
                "material_on_load");
            ok &= installed;
            if (installed) {
                log::Info(
                    "character_eye_restore requested=1 hooks_installed=1 "
                    "behavior=pending_commit scope=wei_eye_materials variants=3");
            }
        }
    }

    bool sampler_builder_prevalidated = false;
    if (ok && anisotropy_override_requested) {
        bool installed = false;
        if (sampler_builder_address == 0) {
            log::Warn(
                "texture_filtering sampler_builder disabled=1 "
                "reason=unsupported_hook_layout");
        } else if (ValidateHookSignature(
                       sampler_builder_address,
                       texture_filtering::kSamplerBuilderPrologue,
                       "sampler_builder")) {
            installed = CreateHookDetour(
                sampler_builder_address,
                reinterpret_cast<void*>(&DetourSamplerBuilder),
                reinterpret_cast<void**>(&g_sampler_builder_original),
                "sampler_builder");
        }
        ok &= installed;
        sampler_builder_prevalidated = installed;
        log::InfoF(
            "texture_filtering sampler_builder requested=%d installed=%d "
            "layout=%s target=0x%p behavior=pending_commit",
            config.anisotropic_filtering,
            installed ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" :
                                        "legacy_researched",
            reinterpret_cast<void*>(sampler_builder_address));
    }

    ok = ok && g_hook_creation_transaction_open;
    if (ok) {
        // Static byte patches commit last. All MinHook detours are already
        // installed but remain transparent behind the global behavior gate,
        // eliminating the former long window where engine bytes were live
        // while later hook creation could still fail and roll them back.
        ok = engine_fixes::InitializeStaticPatches(
            config,
            g_module_base,
            g_use_latest_steam_layout,
            sampler_builder_prevalidated,
            display_settings_path);
    }
    g_hook_creation_transaction_open = false;

    log::InfoF("hook_layout selected=%s",
               g_use_latest_steam_layout ? "latest_steam" : "legacy_researched");
    if (!ok) {
        // The bundled MinHook variant enables hooks during creation, but its
        // only teardown API frees original trampolines.  A failed late probe
        // therefore retains every successfully-created detour for process
        // lifetime instead of attempting an unsafe rollback.  SMAA is gated
        // fail-open and static byte patches still get their normal verified
        // restore attempt.
        (void)RetainCreatedHooks();
        const bool smaa_quiesced = smaa::Shutdown();
        const bool static_patches_restored = engine_fixes::ShutdownStaticPatches();
        const bool smaa_hooks_retained =
            smaa::GetStats().any_hook_retained;
        if (smaa_hooks_retained) {
            g_minhook_retained_process_lifetime.store(true, std::memory_order_release);
        }
        const bool hooks_retained =
            !g_created_hook_targets.empty() || smaa_hooks_retained;
        if (!smaa_quiesced) {
            log::Error("SMAA teardown could not quiesce; D3D resources retained");
        }
        if (!static_patches_restored) {
            log::Error("static patch restore incomplete; patch ownership retained");
        }
        g_hooks_initialized.store(hooks_retained, std::memory_order_release);
        g_cleanup_pending.store(!static_patches_restored, std::memory_order_release);
        log::WarnF("hook_initialization_partial behavior=pass_through "
                   "retained_hook_count=%zu smaa_hook_retained=%d static_restore=%d",
                   g_created_hook_targets.size(),
                   smaa_hooks_retained ? 1 : 0,
                   static_patches_restored ? 1 : 0);
        return false;
    }

    g_cleanup_pending.store(false, std::memory_order_release);
    if (config.hook_smaa_present) {
        // Initialize only prepared CPU state. Open DXGI discovery after every
        // game hook and static patch has committed, while mutating parent
        // detours are still behind the global pass-through gate.
        smaa::Activate();
    }
    // Publish the global gate last. Until this release every mutating detour is
    // pass-through, so it cannot observe half-published feature readiness.
    g_behavior_transaction_ready.store(true, std::memory_order_release);
    g_hooks_initialized.store(true);
    log::InfoF("hook_transaction_committed behavior=active installed_hook_count=%zu",
               g_created_hook_targets.size());
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(g_hook_lifecycle_mutex);
    cut_content::Shutdown();
    if (!g_cleanup_pending.load(std::memory_order_acquire) &&
        !g_hooks_initialized.load() && g_created_hook_targets.empty() &&
        !engine_fixes::IsInitialized() &&
        !smaa::GetStats().any_hook_retained &&
        !g_minhook_retained_process_lifetime.load(std::memory_order_acquire)) {
        return;
    }

    // Retained detours must become transparent before any companion D3D or
    // static state starts tearing down.
    g_hook_creation_transaction_open = false;
    g_behavior_transaction_ready.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> wetness_lock(g_character_wetness_mutex);
        RestoreOwnedCharacterWetness(g_character_wetness_state);
        g_character_wetness_state = {};
    }
    {
        std::lock_guard<std::mutex> sweat_lock(g_character_sweat_mutex);
        RestoreOwnedCharacterSweat(g_character_sweat_state);
    }
    ResetNpcCharacterEffectsStates();

    // Gate SMAA and release only its D3D resources after active calls drain.
    // MinHook detours themselves are process-lifetime objects: MH_RemoveHook
    // frees their trampolines and can race a thread that has already loaded an
    // original pointer.  Main.cpp pins this module, so retaining the hooks is
    // both safer and bounded (the OS reclaims them at process exit).
    if (!smaa::Shutdown()) {
        log::Error("hook shutdown deferred until SMAA calls quiesce");
        g_cleanup_pending.store(true, std::memory_order_release);
        return;
    }
    (void)RetainCreatedHooks();
    if (!engine_fixes::ShutdownStaticPatches()) {
        log::Error("hook shutdown deferred because static patch restoration failed");
        g_cleanup_pending.store(true, std::memory_order_release);
        return;
    }
    ResetNisActorTrackerLocked();
    log::InfoF("pedestrian_density_summary frames=%llu stock_calls=%llu clamped_frames=%llu",
               g_pedestrian_throttle_frame_count.load(std::memory_order_relaxed),
               g_pedestrian_throttle_stock_call_count.load(std::memory_order_relaxed),
               g_pedestrian_throttle_clamped_frame_count.load(std::memory_order_relaxed));
    log::InfoF("average_window_summary initialized=%llu expanded=%llu",
               g_average_window_initialize_count.load(std::memory_order_relaxed),
               g_average_window_expanded_count.load(std::memory_order_relaxed));
    if (!g_created_hook_targets.empty() ||
        smaa::GetStats().any_hook_retained) {
        g_minhook_retained_process_lifetime.store(true, std::memory_order_release);
        g_hooks_initialized.store(true, std::memory_order_release);
    } else {
        g_hooks_initialized.store(false, std::memory_order_release);
    }
    g_cleanup_pending.store(false, std::memory_order_release);
}

}  // namespace spatch::hooks
