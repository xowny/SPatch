#include "Hooks.h"

#include "Logger.h"
#include "SmaaRuntime.h"

#include <Windows.h>
#include <dxgi.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace spatch::hooks {
namespace {

constexpr std::uintptr_t kTaskReadyRva = 0x00A390C0;
constexpr std::uintptr_t kTaskDispatchRva = 0x00A37EC0;
constexpr std::uintptr_t kWaitHelperRva = 0x00A395C0;
constexpr std::uintptr_t kScaleformTimeRva = 0x0098DC90;
constexpr std::uintptr_t kScaleformInitRva = 0x009928B0;
constexpr std::uintptr_t kNisSetPlayTimeRva = 0x003E51B0;
constexpr std::uintptr_t kNisPlayRva = 0x003E3C40;
constexpr std::uintptr_t kNisBootstrapRva = 0x003E3B60;
constexpr std::uintptr_t kNisOwnerRva = 0x003E6CE0;
constexpr std::uintptr_t kFrameFlowRva = 0x00590AD0;
constexpr std::uintptr_t kCutsceneFlowOwnerRva = 0x00412240;
constexpr std::uintptr_t kScaleformTimeProviderRva = 0x02441150;
constexpr std::uintptr_t kFogSlicingModeRva = 0x0006BA60;
constexpr std::uintptr_t kAmbientOcclusionStageRva = 0x00035370;
constexpr std::uintptr_t kAmbientOcclusionMaterialRva = 0x00036140;
constexpr std::uintptr_t kAmbientOcclusionComputeNodeRva = 0x000359C0;
constexpr std::uintptr_t kAmbientOcclusionComputeCompositeRva = 0x00034590;
constexpr std::uintptr_t kAntiAliasOwnerRva = 0x00053230;
constexpr std::uintptr_t kAntiAliasFxHandoffRva = 0x000554E0;
constexpr std::uintptr_t kAntiAliasAuxStateQueryRva = 0x00008A00;
constexpr std::uintptr_t kRenderSubmitMaterialRva = 0x0000E880;
constexpr std::uintptr_t kFinalCompositionRva = 0x00049D30;
constexpr std::uintptr_t kPresentBufferRva = 0x00053A30;
constexpr std::uintptr_t kDisplayCurveBuilderRva = 0x006A23C0;
constexpr std::uintptr_t kDxgiAdapterInitRva = 0x0069F270;
constexpr std::uintptr_t kRumbleApplyHelperRva = 0x0014C170;
constexpr std::uintptr_t kRumbleApplyObjectRva = 0x02175EB0;
#if !defined(SPATCH_FINAL_RELEASE)
constexpr std::uintptr_t kProgressionTrackerInstanceRva = 0x0240A0E0;
constexpr std::uintptr_t kProgressionFindRva = 0x004A0380;
constexpr std::uintptr_t kProgressionForceSliceChangeRva = 0x004A1390;
#endif
constexpr std::uintptr_t kCharacterHandleWaterCollisionRva = 0x00535140;
constexpr std::uintptr_t kCharacterEffectsUpdateRva = 0x00559AF0;
constexpr std::uintptr_t kHealthApplyDamageRva = 0x00521F60;
constexpr std::uintptr_t kCharacterPaintOwnerRva = 0x004CF1E0;
constexpr std::uintptr_t kCharacterPaintConsumerRva = 0x00509200;
constexpr std::uintptr_t kCharacterEffectDispatchOwnerRva = 0x00400930;
constexpr std::uintptr_t kCharacterEffectDispatchConsumeRva = 0x00402CB0;
constexpr std::uintptr_t kCharacterEffectQueueBuilderRva = 0x003FE840;
constexpr std::uintptr_t kCharacterAnimationApplyCharredEffectRva = 0x0057F270;
constexpr std::uintptr_t kCharacterAnimationApplyPaintEffectRva = 0x0057F280;
constexpr std::uintptr_t kCharacterAnimationCreateDamageRigRva = 0x00580570;
constexpr std::uintptr_t kCharacterAnimationSetVisualDamageRva = 0x0058BBC0;
constexpr std::uintptr_t kDamageRigApplyCharredEffectRva = 0x003A1EA0;
constexpr std::uintptr_t kDamageRigApplyPaintEffectRva = 0x003A2430;
constexpr std::uintptr_t kCharacterDamageRigSetVisualDamageRva = 0x003AF920;
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
constexpr std::uintptr_t kFinalCurveGammaRva = 0x02021984;
constexpr std::uintptr_t kFinalCurveWindowedScaleRva = 0x02021988;
constexpr std::uintptr_t kFinalCurveOffsetScaleRva = 0x02021B30;
constexpr std::uintptr_t kFinalCurveCutoffRva = 0x01677F3C;
constexpr std::uintptr_t kFinalCurveCoeffARva = 0x01677F88;
constexpr std::uintptr_t kFinalCurveCoeffBRva = 0x01677F78;
constexpr std::uintptr_t kFinalCurveCoeffCRva = 0x01677F80;
constexpr std::uintptr_t kFinalCurveCoeffDRva = 0x01677F70;
constexpr std::uintptr_t kDisplayCurveModeRva = 0x02093441;
constexpr std::uintptr_t kDisplayCurveDirtyRva = 0x02439A91;
constexpr std::uintptr_t kDxgiFactorySlotRva = 0x02439AF0;
constexpr std::uintptr_t kDxgiAdapterSlotRva = 0x02439AF8;
constexpr std::uintptr_t kD3DDeviceSlotRva = 0x02439AE0;
constexpr std::uintptr_t kD3DContextSlotRva = 0x02439AE8;
constexpr std::uintptr_t kDxgiSwapChainSlotRva = 0x02439B10;
constexpr std::uintptr_t kPresentRtvSlotRva = 0x02439B18;
constexpr unsigned long kWaitLogThresholdMs = 16;
constexpr unsigned long kWaitLongThresholdMs = 100;
constexpr unsigned long kWaitVeryLongThresholdMs = 1000;

constexpr std::uintptr_t kLatestSteamNisSetPlayTimeRva = 0x003E50C0;
constexpr std::uintptr_t kLatestSteamNisOwnerRva = 0x003E6BF0;
constexpr std::uintptr_t kLatestSteamFrameFlowRva = 0x00590D30;
constexpr std::uintptr_t kLatestSteamCutsceneFlowOwnerRva = 0x00412490;
constexpr std::uintptr_t kLatestSteamFogSlicingModeRva = 0x0006BD70;
constexpr std::uintptr_t kLatestSteamFinalCompositionRva = 0x0004A040;
constexpr std::uintptr_t kLatestSteamPresentBufferRva = 0x00053D40;
constexpr std::uintptr_t kLatestSteamDisplayCurveBuilderRva = 0x006A2380;
constexpr std::uintptr_t kLatestSteamDxgiAdapterInitRva = 0x0069F240;
constexpr std::uintptr_t kLatestSteamHealthApplyDamageRva = 0x005221C0;
constexpr std::uintptr_t kLatestSteamSimObjectGetComponentRva = 0x00190A60;
constexpr std::uintptr_t kLatestSteamCharacterAnimationSetVisualDamageRva = 0x004DD040;
constexpr std::uintptr_t kLatestSteamTimeOfDayAccessorRva = 0x0006AD80;
constexpr std::uintptr_t kLatestSteamCharacterHandleWaterCollisionRva = 0x005353A0;
constexpr std::uintptr_t kLatestSteamCharacterEffectsUpdateRva = 0x00559D50;
constexpr std::uintptr_t kLatestSteamCharacterDamageRigSetVisualDamageRva = 0x003AF740;
constexpr std::uintptr_t kLatestSteamCharacterEffectDispatchConsumeRva = 0x00402E30;
constexpr std::uintptr_t kLatestSteamCharacterEffectQueueBuilderRva = 0x003FE900;
constexpr std::uintptr_t kLatestSteamDamageRigApplyPaintEffectRva = 0x003A2220;
constexpr std::uintptr_t kLatestSteamCharacterDamageRigResetDamageRva = 0x003ADF30;
constexpr std::uintptr_t kLatestSteamTaskReadyRva = 0x00A39060;
constexpr std::uintptr_t kLatestSteamTaskDispatchRva = 0x00A37E60;
constexpr std::uintptr_t kLatestSteamFinalCurveCutoffRva = 0x016780AC;
constexpr std::uintptr_t kLatestSteamFinalCurveCoeffARva = 0x016780F8;
constexpr std::uintptr_t kLatestSteamFinalCurveCoeffBRva = 0x016780E8;
constexpr std::uintptr_t kLatestSteamFinalCurveCoeffCRva = 0x016780F0;
constexpr std::uintptr_t kLatestSteamFinalCurveCoeffDRva = 0x016780E0;

using TaskReadyFn = void (*)(void* task_manager_owner, void* task_header);
using TaskDispatchFn = void (*)(void* task_manager, unsigned int worker_index);
using WaitHelperFn = void (*)(void* task_manager_owner, void* task_header);
using ScaleformTimeFn = void (*)();
using ScaleformInitFn = void (*)(void* heap_desc, void* sys_alloc);
using NisSetPlayTimeFn = void (*)(void* nis_manager, float scene_time, bool sync_scene_time);
using NisPlayFn = void (*)(void* nis_manager);
using NisBootstrapFn = unsigned char (*)(void* nis_manager, void* scene_asset);
using NisOwnerFn = void (*)(void* nis_manager, float delta_seconds);
using FrameFlowFn = void (*)(float delta_seconds, void* callback);
using CutsceneFlowOwnerFn = void (*)(void* owner, float delta_seconds);
using FogSlicingModeFn = void (*)(void* time_of_day_manager, int update_interval);
using AmbientOcclusionStageFn = void (*)(char* ao_state, std::uintptr_t render_context, long long* cmd_list);
using AmbientOcclusionMaterialFn = void (*)(std::uintptr_t ao_state,
                                            std::uintptr_t render_context,
                                            long long* cmd_list);
using AmbientOcclusionComputeNodeFn = void* (*)(char* ao_state,
                                                std::uintptr_t render_context,
                                                int pass_index);
using AmbientOcclusionComputeCompositeFn = void (*)(char* ao_state,
                                                    std::uintptr_t render_context,
                                                    long long* cmd_list,
                                                    int pass_index);
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
using FinalCompositionFn = void (*)(std::uintptr_t render_context,
                                    void* render_packet,
                                    std::uintptr_t back_buffer_desc);
using PresentBufferFn = void (*)(void* render_packet);
using DisplayCurveBuilderFn = void (*)();
using DxgiAdapterInitFn = void (*)();
using RumbleApplyHelperFn = void (*)(std::uintptr_t rumble_object, bool enabled);
struct RawQSymbol {
    std::uint32_t mUID;
};

#if !defined(SPATCH_FINAL_RELEASE)
using ProgressionFindFn = void* (*)(void* tracker,
                                    const RawQSymbol& qsymbol,
                                    bool search_disabled_slices);
using ProgressionForceSliceChangeFn = void (*)(void* tracker,
                                               void* game_slice,
                                               bool simulate_rewards);
#endif
using CharacterHandleWaterCollisionFn = void (*)(void* character_effects_component,
                                                 const void* mat,
                                                 const void* character_velocity);
using CharacterEffectsUpdateFn = void (*)(void* character_effects_component, float delta_seconds);
using HealthApplyDamageFn = bool (*)(void* health_component,
                                     int damage,
                                     void* attacker,
                                     void* hit_record,
                                     bool projectile_damage);
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
using CharacterAnimationSetVisualDamageFn = void (*)(void* character_animation_component,
                                                     std::uint32_t bone_uid,
                                                     float damage_amount,
                                                     float duration);
using DamageRigApplyCharredEffectFn = void (*)(void* damage_rig, float amount);
using DamageRigApplyPaintEffectFn = void (*)(void* damage_rig,
                                             bool enable,
                                             float r,
                                             float g,
                                             float b);
using CharacterDamageRigSetVisualDamageFn = void (*)(void* damage_rig,
                                                     std::uint32_t bone_uid,
                                                     float damage_amount,
                                                     float duration);
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
FrameFlowFn g_frame_flow_original = nullptr;
CutsceneFlowOwnerFn g_cutscene_flow_owner_original = nullptr;
FogSlicingModeFn g_fog_slicing_mode_original = nullptr;
AmbientOcclusionStageFn g_ao_stage_original = nullptr;
AmbientOcclusionMaterialFn g_ao_material_original = nullptr;
AmbientOcclusionComputeNodeFn g_ao_compute_node_original = nullptr;
AmbientOcclusionComputeCompositeFn g_ao_compute_composite_original = nullptr;
AntiAliasOwnerFn g_antialias_owner_original = nullptr;
AntiAliasFxHandoffFn g_antialias_fx_handoff_original = nullptr;
AntiAliasAuxStateQueryFn g_antialias_aux_state_query_original = nullptr;
RenderSubmitMaterialFn g_render_submit_material_original = nullptr;
FinalCompositionFn g_final_composition_original = nullptr;
PresentBufferFn g_present_buffer_original = nullptr;
DisplayCurveBuilderFn g_display_curve_builder_original = nullptr;
DxgiAdapterInitFn g_dxgi_adapter_init_original = nullptr;
RumbleApplyHelperFn g_rumble_apply_helper = nullptr;
#if !defined(SPATCH_FINAL_RELEASE)
ProgressionFindFn g_progression_find = nullptr;
ProgressionForceSliceChangeFn g_progression_force_slice_change = nullptr;
#endif
CharacterHandleWaterCollisionFn g_character_handle_water_collision_original = nullptr;
CharacterEffectsUpdateFn g_character_effects_update_original = nullptr;
HealthApplyDamageFn g_health_apply_damage_original = nullptr;
CharacterPaintOwnerFn g_character_paint_owner_original = nullptr;
CharacterPaintConsumerFn g_character_paint_consumer_original = nullptr;
CharacterEffectDispatchOwnerFn g_character_effect_dispatch_owner_original = nullptr;
CharacterEffectDispatchConsumeFn g_character_effect_dispatch_consume_original = nullptr;
CharacterEffectQueueBuilderFn g_character_effect_queue_builder_original = nullptr;
CharacterAnimationApplyCharredEffectFn g_character_animation_apply_charred_effect_original = nullptr;
CharacterAnimationApplyPaintEffectFn g_character_animation_apply_paint_effect_original = nullptr;
CharacterAnimationCreateDamageRigFn g_character_animation_create_damage_rig_original = nullptr;
CharacterAnimationSetVisualDamageFn g_character_animation_set_visual_damage_original = nullptr;
DamageRigApplyCharredEffectFn g_damage_rig_apply_charred_effect_original = nullptr;
DamageRigApplyPaintEffectFn g_damage_rig_apply_paint_effect_original = nullptr;
CharacterDamageRigSetVisualDamageFn g_character_damage_rig_set_visual_damage_original = nullptr;
CharacterDamageRigResetDamageFn g_character_damage_rig_reset_damage_original = nullptr;
SimObjectGetComponentFn g_simobject_get_component = nullptr;
SceneryPrepareFn g_scenery_prepare_original = nullptr;
ScenerySetupFn g_scenery_setup_original = nullptr;
RenderSceneryBuilderFn g_render_scenery_builder_original = nullptr;
RasterizeBucketBuilderFn g_rasterize_bucket_builder_original = nullptr;
TimeOfDayAccessorFn g_time_of_day_accessor = nullptr;

Config g_config{};
bool g_use_latest_steam_layout = false;
std::uintptr_t g_module_base = 0;
std::uintptr_t g_module_end = 0;
std::uintptr_t g_provider_slot = 0;
std::uintptr_t g_render_task_manager = 0;
std::uintptr_t g_render_context_instance_slot = 0;
std::uintptr_t g_scenery_counter0 = 0;
std::uintptr_t g_scenery_counter1 = 0;
std::uintptr_t g_scenery_counter2 = 0;
std::uintptr_t g_scenery_counter3 = 0;
std::uintptr_t g_final_curve_gamma = 0;
std::uintptr_t g_final_curve_windowed_scale = 0;
std::uintptr_t g_final_curve_offset_scale = 0;
std::uintptr_t g_final_curve_cutoff = 0;
std::uintptr_t g_final_curve_coeff_a = 0;
std::uintptr_t g_final_curve_coeff_b = 0;
std::uintptr_t g_final_curve_coeff_c = 0;
std::uintptr_t g_final_curve_coeff_d = 0;
std::uintptr_t g_display_curve_mode = 0;
std::uintptr_t g_display_curve_dirty = 0;
std::uintptr_t g_dxgi_factory_slot = 0;
std::uintptr_t g_dxgi_adapter_slot = 0;
std::uintptr_t g_d3d_device_slot = 0;
std::uintptr_t g_d3d_context_slot = 0;
std::uintptr_t g_dxgi_swapchain_slot = 0;
std::uintptr_t g_present_rtv_slot = 0;
std::uintptr_t g_rumble_apply_object = 0;
#if !defined(SPATCH_FINAL_RELEASE)
std::uintptr_t g_progression_tracker_instance_slot = 0;
#endif
std::uintptr_t g_wet_surface_block_counter = 0;
std::filesystem::path g_config_path;
std::atomic<unsigned long long> g_task_ready_count = 0;
std::atomic<unsigned long long> g_task_dispatch_count = 0;
std::atomic<unsigned long long> g_wait_helper_count = 0;
std::atomic<unsigned long long> g_wait_helper_nonzero_task_count = 0;
std::atomic<unsigned long long> g_wait_over_16ms_count = 0;
std::atomic<unsigned long long> g_wait_over_100ms_count = 0;
std::atomic<unsigned long long> g_wait_over_1000ms_count = 0;
std::atomic<unsigned long long> g_wait_over_5000ms_count = 0;
std::atomic<unsigned long long> g_scaleform_time_count = 0;
std::atomic<unsigned long long> g_scaleform_provider_non_null_count = 0;
std::atomic<unsigned long long> g_scaleform_init_count = 0;
std::atomic<unsigned long long> g_rumble_override_apply_count = 0;
std::atomic<unsigned long long> g_nis_set_play_time_count = 0;
std::atomic<unsigned long long> g_nis_sync_scene_time_count = 0;
std::atomic<unsigned long long> g_nis_delta_zero_count = 0;
std::atomic<unsigned long long> g_nis_delta_30hz_count = 0;
std::atomic<unsigned long long> g_nis_delta_60hz_count = 0;
std::atomic<unsigned long long> g_nis_delta_other_count = 0;
std::atomic<unsigned long long> g_nis_scene_time_fix_count = 0;
std::atomic<unsigned long long> g_nis_play_count = 0;
std::atomic<unsigned long long> g_nis_play_advanced_count = 0;
std::atomic<unsigned long long> g_nis_play_repeat_count = 0;
std::atomic<unsigned long long> g_nis_play_multi_tick_count = 0;
std::atomic<unsigned long long> g_nis_bootstrap_count = 0;
std::atomic<unsigned long long> g_nis_bootstrap_state1_count = 0;
std::atomic<unsigned long long> g_nis_bootstrap_state2_count = 0;
std::atomic<unsigned long long> g_nis_bootstrap_fail_count = 0;
std::atomic<unsigned long long> g_nis_owner_count = 0;
std::atomic<unsigned long long> g_nis_owner_dt_zero_count = 0;
std::atomic<unsigned long long> g_nis_owner_dt_30hz_count = 0;
std::atomic<unsigned long long> g_nis_owner_dt_60hz_count = 0;
std::atomic<unsigned long long> g_nis_owner_dt_other_count = 0;
std::atomic<unsigned long long> g_nis_owner_advanced_count = 0;
std::atomic<unsigned long long> g_nis_owner_repeat_count = 0;
std::atomic<unsigned long long> g_nis_owner_multi_tick_count = 0;
std::atomic<unsigned long long> g_frame_flow_count = 0;
std::atomic<unsigned long long> g_frame_flow_dt_zero_count = 0;
std::atomic<unsigned long long> g_frame_flow_dt_60hz_count = 0;
std::atomic<unsigned long long> g_frame_flow_dt_other_count = 0;
std::atomic<unsigned long long> g_frame_flow_from_cutscene_owner_count = 0;
std::atomic<unsigned long long> g_cutscene_flow_owner_count = 0;
std::atomic<unsigned long long> g_cutscene_flow_owner_dt_zero_count = 0;
std::atomic<unsigned long long> g_cutscene_flow_owner_forwarded_zero_count = 0;
std::atomic<unsigned long long> g_cutscene_flow_owner_forwarded_60hz_count = 0;
std::atomic<unsigned long long> g_cutscene_flow_owner_forwarded_other_count = 0;
std::atomic<unsigned long long> g_cutscene_zero_dt_override_count = 0;
std::atomic<unsigned long long> g_fog_slicing_mode_count = 0;
std::atomic<unsigned long long> g_fog_slicing_clamp_count = 0;
std::atomic<unsigned long long> g_ao_stage_count = 0;
std::atomic<unsigned long long> g_ao_skip_count = 0;
std::atomic<unsigned long long> g_ao_none_path_count = 0;
std::atomic<unsigned long long> g_ao_main_path_count = 0;
std::atomic<unsigned long long> g_ao_material_count = 0;
std::atomic<unsigned long long> g_ao_compute_node_count = 0;
std::atomic<unsigned long long> g_ao_compute_pass0_count = 0;
std::atomic<unsigned long long> g_ao_compute_pass1_count = 0;
std::atomic<unsigned long long> g_ao_compute_composite_count = 0;
std::atomic<unsigned long long> g_ao_compute_composite_pass0_count = 0;
std::atomic<unsigned long long> g_ao_compute_composite_pass1_count = 0;
std::atomic<unsigned long long> g_aa_owner_count = 0;
std::atomic<unsigned long long> g_aa_skip_count = 0;
std::atomic<unsigned long long> g_aa_main_count = 0;
std::atomic<unsigned long long> g_aa_hair_blur_zero_count = 0;
std::atomic<unsigned long long> g_aa_fx_handoff_count = 0;
std::atomic<unsigned long long> g_aa_variant_apply_count = 0;
std::atomic<unsigned long long> g_aa_aux_apply_count = 0;
std::atomic<unsigned long long> g_post_material_submit_count = 0;
std::atomic<unsigned long long> g_post_composite_lights_submit_count = 0;
std::atomic<unsigned long long> g_post_composite_final_submit_count = 0;
std::atomic<unsigned long long> g_post_bloom_threshold_submit_count = 0;
std::atomic<unsigned long long> g_post_lightshaft_submit_count = 0;
std::atomic<unsigned long long> g_post_shadow_collector_submit_count = 0;
std::atomic<unsigned long long> g_post_composite_final_snapshot_change_count = 0;
std::atomic<unsigned long long> g_vram_probe_count = 0;
std::atomic<unsigned long long> g_vram_adapter_override_count = 0;
std::atomic<unsigned long long> g_character_water_collision_count = 0;
std::atomic<unsigned long long> g_character_effects_update_count = 0;
std::atomic<unsigned long long> g_character_effects_wet_surface_count = 0;
std::atomic<unsigned long long> g_character_health_apply_damage_count = 0;
std::atomic<unsigned long long> g_character_health_apply_projectile_count = 0;
std::atomic<unsigned long long> g_character_health_apply_melee_count = 0;
std::atomic<unsigned long long> g_character_health_anim_found_count = 0;
std::atomic<unsigned long long> g_character_health_hitreact_found_count = 0;
std::atomic<unsigned long long> g_character_paint_owner_count = 0;
std::atomic<unsigned long long> g_character_paint_consumer_count = 0;
std::atomic<unsigned long long> g_character_effect_dispatch_owner_count = 0;
std::atomic<unsigned long long> g_character_effect_dispatch_consume_count = 0;
std::atomic<unsigned long long> g_character_effect_queue_builder_count = 0;
std::atomic<unsigned long long> g_character_effect_queue_builder_tracked_count = 0;
std::atomic<unsigned long long> g_character_charred_anim_count = 0;
std::atomic<unsigned long long> g_character_paint_anim_count = 0;
std::atomic<unsigned long long> g_character_damage_rig_create_count = 0;
std::atomic<unsigned long long> g_character_charred_rig_count = 0;
std::atomic<unsigned long long> g_character_paint_rig_count = 0;
std::atomic<unsigned long long> g_character_visual_damage_anim_count = 0;
std::atomic<unsigned long long> g_character_visual_damage_rig_count = 0;
std::atomic<unsigned long long> g_character_visual_damage_restore_count = 0;
std::atomic<unsigned long long> g_character_damage_rig_reset_count = 0;
std::atomic<unsigned long long> g_character_wet_force_count = 0;
std::atomic<unsigned long long> g_character_wet_force_verify_count = 0;
std::atomic<unsigned long long> g_aces_final_count = 0;
std::atomic<unsigned long long> g_aces_apply_count = 0;
std::atomic<unsigned long long> g_aces_presentbuffer_count = 0;
std::atomic<unsigned long long> g_aces_presentbuffer_neutralize_count = 0;
std::atomic<unsigned long long> g_aces_display_curve_count = 0;
std::atomic<unsigned long long> g_aces_display_curve_linearize_count = 0;
std::atomic<unsigned long long> g_aces_toggle_count = 0;
std::atomic<unsigned long long> g_aces_adjust_count = 0;
std::atomic<unsigned long long> g_unique_callback_count = 0;
std::atomic<unsigned long long> g_scenery_prepare_count = 0;
std::atomic<unsigned long long> g_scenery_setup_count = 0;
std::atomic<unsigned long long> g_render_scenery_builder_count = 0;
std::atomic<unsigned long long> g_rasterize_bucket_builder_count = 0;
std::atomic<unsigned long long> g_render_scenery_queue_delta_total = 0;
std::atomic<unsigned long long> g_rasterize_bucket_queue_delta_total = 0;
std::atomic<unsigned long long> g_scenery_setup_queue_delta_total = 0;
std::atomic<unsigned long long> g_scenery_prepare_ready_total = 0;
std::atomic<unsigned long long> g_scenery_setup_ready_total = 0;
std::atomic<unsigned long long> g_render_scenery_ready_total = 0;
std::atomic<unsigned long long> g_rasterize_bucket_ready_total = 0;
std::atomic<unsigned int> g_last_scenery_counter0 = 0;
std::atomic<unsigned int> g_last_scenery_counter1 = 0;
std::atomic<unsigned int> g_last_scenery_counter2 = 0;
std::atomic<unsigned int> g_last_scenery_counter3 = 0;
std::atomic<std::uint32_t> g_last_nis_owner_dt_bits = 0;
std::atomic<std::uint32_t> g_last_frame_flow_dt_bits = 0;
std::atomic<std::uint32_t> g_last_cutscene_flow_input_dt_bits = 0;
std::atomic<std::uint32_t> g_last_cutscene_flow_forwarded_dt_bits = 0;
std::atomic<std::uint32_t> g_last_ao_opacity_bits = 0;
std::atomic<int> g_last_ao_ssao_setting = 0;
std::atomic<unsigned int> g_last_ao_feature_flag = 0;
std::atomic<unsigned int> g_last_ao_stage_flag = 0;
std::atomic<int> g_last_aa_state_gate = 0;
std::atomic<int> g_last_aa_hair_blur_gate = 0;
std::atomic<int> g_last_aa_variant_mode = 0;
std::atomic<int> g_last_aa_aux_mode = 0;
std::atomic<unsigned int> g_last_aa_shader_uid = 0;
std::atomic<unsigned int> g_last_aa_raster_uid = 0;
std::atomic<unsigned int> g_last_aa_aux_uid = 0;
std::atomic<std::uintptr_t> g_last_aa_material = 0;
std::atomic<std::uintptr_t> g_last_aa_target = 0;
std::atomic<std::uintptr_t> g_last_aa_source_a = 0;
std::atomic<std::uintptr_t> g_last_aa_source_b = 0;
std::atomic<std::uintptr_t> g_last_aa_fx_arg1 = 0;
std::atomic<std::uintptr_t> g_last_aa_fx_arg2 = 0;
std::atomic<std::uintptr_t> g_last_aa_fx_arg3 = 0;
std::atomic<std::uintptr_t> g_last_ao_material_slot0 = 0;
std::atomic<std::uintptr_t> g_last_ao_material_slot1 = 0;
std::atomic<std::uintptr_t> g_last_ao_material_slot2 = 0;
std::atomic<std::uintptr_t> g_last_ao_material_slot3 = 0;
std::atomic<std::uintptr_t> g_last_ao_material_selected = 0;
std::atomic<int> g_last_ao_material_selected_slot = -1;
std::atomic<int> g_last_ao_compute_pass = -1;
std::atomic<int> g_last_ao_compute_composite_pass = -1;
std::atomic<std::uintptr_t> g_last_post_material = 0;
std::atomic<int> g_last_post_final_flags = 0;
std::atomic<std::uintptr_t> g_last_post_final_cmd = 0;
std::atomic<std::uintptr_t> g_last_post_final_params = 0;
std::atomic<std::uintptr_t> g_last_post_final_param0 = 0;
std::atomic<std::uintptr_t> g_last_post_final_param1 = 0;
std::atomic<std::uintptr_t> g_last_post_final_param2 = 0;
std::atomic<std::uintptr_t> g_last_post_final_param3 = 0;
std::atomic<std::uint32_t> g_last_aces_source_exposure_bits = 0;
std::atomic<std::uint32_t> g_last_aces_remapped_exposure_bits = 0;
std::atomic<std::uint32_t> g_last_aces_stock_curve_bits = 0;
std::atomic<std::uint32_t> g_last_aces_target_curve_bits = 0;
std::atomic<std::uint32_t> g_last_aces_presentbuffer_offset_bits = 0;
std::atomic<unsigned int> g_last_aces_display_curve_mode = 0;
std::atomic<unsigned long long> g_last_vram_selected_mb = 0;
std::atomic<unsigned long long> g_last_vram_best_mb = 0;
std::atomic<unsigned long long> g_last_vram_effective_mb = 0;
std::atomic<int> g_last_rumble_override_value = -1;
std::atomic<std::uintptr_t> g_last_character_wet_component = 0;
std::atomic<std::uint32_t> g_last_character_water_speed_bits = 0;
std::atomic<unsigned int> g_last_character_active_surface_uid = 0;
std::atomic<unsigned int> g_last_character_active_wet_surface_uid = 0;
std::atomic<int> g_last_character_wet_gate_counter = 0;
std::atomic<unsigned int> g_last_character_is_on_fire = 0;
std::atomic<unsigned int> g_last_character_is_smoldering = 0;
std::atomic<unsigned int> g_last_character_is_attached_to_player = 0;
std::atomic<std::uint32_t> g_last_character_fire_extinguish_bits = 0;
std::atomic<std::uint32_t> g_last_character_smolder_extinguish_bits = 0;
std::atomic<std::uint32_t> g_last_character_queued_health_damage_bits = 0;
std::atomic<std::uint32_t> g_last_character_timeofday_weather_bits = 0;
std::atomic<std::uint32_t> g_last_character_timeofday_override_bits = 0;
std::atomic<std::uintptr_t> g_last_character_health_component = 0;
std::atomic<std::uintptr_t> g_last_character_health_anim_component = 0;
std::atomic<std::uintptr_t> g_last_character_health_hitreact_component = 0;
std::atomic<std::uintptr_t> g_last_character_health_attacker = 0;
std::atomic<std::uintptr_t> g_last_character_health_hit_record = 0;
std::atomic<int> g_last_character_health_damage = 0;
std::atomic<unsigned int> g_last_character_health_projectile = 0;
std::atomic<std::uint32_t> g_last_character_charred_amount_bits = 0;
std::atomic<unsigned int> g_last_character_paint_enable = 0;
std::atomic<std::uint32_t> g_last_character_paint_r_bits = 0;
std::atomic<std::uint32_t> g_last_character_paint_g_bits = 0;
std::atomic<std::uint32_t> g_last_character_paint_b_bits = 0;
std::atomic<std::uintptr_t> g_last_character_paint_consumer_component = 0;
std::atomic<std::uintptr_t> g_last_character_paint_consumer_block = 0;
std::atomic<std::uintptr_t> g_last_character_paint_owner_ptr = 0;
std::atomic<std::uintptr_t> g_last_character_paint_owner_component = 0;
std::atomic<std::uintptr_t> g_last_character_effect_dispatch_owner = 0;
std::atomic<std::uintptr_t> g_last_character_effect_dispatch_component = 0;
std::atomic<std::uintptr_t> g_last_character_effect_queue_builder_owner = 0;
std::atomic<std::uintptr_t> g_last_character_effect_queue_builder_component = 0;
std::atomic<std::uint32_t> g_last_character_effect_queue_builder_mode = 0;
std::atomic<std::uintptr_t> g_last_character_damage_anim_component = 0;
std::atomic<std::uintptr_t> g_last_character_damage_rig = 0;
std::atomic<unsigned int> g_last_character_damage_bone = 0;
std::atomic<std::uint32_t> g_last_character_damage_amount_bits = 0;
std::atomic<std::uint32_t> g_last_character_damage_duration_bits = 0;
std::atomic<unsigned int> g_last_character_damage_restored = 0;
constexpr char kMenuAcesLabel[] = "ACES Tonemapping";
constexpr char kMenuSmaaLabel[] = "SMAA";
constexpr char kMenuSmaaValue[] = "SMAA =";
std::atomic<int> g_aa_runtime_aux_mode = 0;
std::atomic<unsigned long> g_last_aa_aux_up_key_tick = 0;
std::atomic<unsigned long> g_last_aa_aux_down_key_tick = 0;
std::atomic<unsigned long> g_last_smaa_toggle_key_tick = 0;
std::atomic<unsigned long> g_last_character_water_collision_tick = 0;
std::atomic<long> g_character_trace_budget = 0;

std::atomic<unsigned long> g_task_ready_verbose = 0;
std::atomic<unsigned long> g_task_dispatch_verbose = 0;
std::atomic<unsigned long> g_wait_helper_verbose = 0;
std::atomic<unsigned long> g_scaleform_time_verbose = 0;
std::atomic<unsigned long> g_scaleform_init_verbose = 0;
std::atomic<unsigned long> g_nis_set_play_time_verbose = 0;
std::atomic<unsigned long> g_nis_play_verbose = 0;
std::atomic<unsigned long> g_nis_bootstrap_verbose = 0;
std::atomic<unsigned long> g_nis_owner_verbose = 0;
std::atomic<unsigned long> g_frame_flow_verbose = 0;
std::atomic<unsigned long> g_cutscene_flow_owner_verbose = 0;
std::atomic<unsigned long> g_fog_slicing_verbose = 0;
std::atomic<unsigned long> g_ao_stage_verbose = 0;
std::atomic<unsigned long> g_ao_material_verbose = 0;
std::atomic<unsigned long> g_ao_compute_verbose = 0;
std::atomic<unsigned long> g_aa_probe_verbose = 0;
std::atomic<unsigned long> g_aa_fx_probe_verbose = 0;
std::atomic<unsigned long> g_post_material_verbose = 0;
std::atomic<int> g_aa_runtime_variant_mode = 0;
std::atomic<unsigned long> g_last_aa_variant_up_key_tick = 0;
std::atomic<unsigned long> g_last_aa_variant_down_key_tick = 0;
std::atomic<bool> g_aces_runtime_enabled = false;
std::atomic<int> g_aces_runtime_strength_percent = 100;
std::atomic<int> g_aces_runtime_exposure_scale_percent = 100;
std::atomic<unsigned long> g_last_aces_toggle_key_tick = 0;
std::atomic<unsigned long> g_last_aces_strength_up_key_tick = 0;
std::atomic<unsigned long> g_last_aces_strength_down_key_tick = 0;
std::atomic<unsigned long> g_last_aces_scale_up_key_tick = 0;
std::atomic<unsigned long> g_last_aces_scale_down_key_tick = 0;
std::atomic<unsigned long> g_scenery_prepare_verbose = 0;
std::atomic<unsigned long> g_scenery_setup_verbose = 0;
std::atomic<unsigned long> g_render_scenery_builder_verbose = 0;
std::atomic<unsigned long> g_rasterize_bucket_builder_verbose = 0;
std::atomic<unsigned long> g_character_regression_verbose = 0;

std::atomic<unsigned long> g_last_summary_tick = 0;
std::atomic<std::uintptr_t> g_last_nis_manager = 0;
std::atomic<std::uint32_t> g_last_nis_scene_time_bits = 0;
std::atomic<std::uint32_t> g_last_nis_scene_delta_bits = 0;
std::atomic<bool> g_hooks_initialized = false;
bool g_rumble_override_attempted = false;
#if !defined(SPATCH_FINAL_RELEASE)
bool g_cut_content_probe_attempted = false;
bool g_cut_content_force_pending = false;
bool g_cut_content_force_applied = false;
#endif
std::mutex g_seen_callback_mutex;
std::vector<std::uintptr_t> g_seen_callbacks;

#if !defined(SPATCH_FINAL_RELEASE)
struct CutContentCandidate {
    const char* label;
    const char* symbol;
    std::uint32_t uid;
};

constexpr std::array<CutContentCandidate, 4> kCutContentCandidates{{
    {"Confrontation", "M_Confrontation", 0xD8CDDB22u},
    {"Triad Highway", "E_TriadHighway", 0x8EFCB580u},
    {"Death By A Thousand Cuts", "E_DeathByThousandCuts", 0xDA5C5BD2u},
    {"Broken Nose Call", "E_BrokenNoseCall", 0x3679F77Cu},
}};

void* g_cut_content_force_tracker = nullptr;
void* g_cut_content_force_slice = nullptr;
int g_cut_content_force_candidate_index = -1;
#endif

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
thread_local unsigned int g_cutscene_flow_owner_scope_depth = 0;
thread_local unsigned int g_antialias_owner_scope_depth = 0;
thread_local bool g_cutscene_flow_forwarded = false;
thread_local float g_cutscene_flow_forwarded_dt = 0.0f;
thread_local void* g_cutscene_flow_forwarded_callback = nullptr;
thread_local float g_cutscene_flow_input_dt = 0.0f;

template <typename T>
bool SafeRead(std::uintptr_t address, T& out_value) {
    __try {
        out_value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_value = T{};
        return false;
    }
}

template <typename T>
bool SafeWrite(std::uintptr_t address, const T& value) {
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
                              bool result,
                              unsigned int bone_uid) {
    if (!g_use_latest_steam_layout) {
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
                                        "%s hc=%p anim=%p hitreact=%p damage=%d projectile=%d result=%d bone=%u\r\n",
                                        stage,
                                        health_component,
                                        anim_component,
                                        hitreact_component,
                                        damage,
                                        projectile_damage ? 1 : 0,
                                        result ? 1 : 0,
                                        bone_uid);
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
}

void AppendCharacterHitRecordTraceLine(const char* stage,
                                       void* hit_record,
                                       void* damage_rig) {
    if (!g_use_latest_steam_layout) {
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

    const std::uintptr_t hit = reinterpret_cast<std::uintptr_t>(hit_record);
    std::uint32_t hr_3c = 0;
    std::uint32_t hr_f0 = 0;
    std::uint32_t hr_f4 = 0;
    std::uint32_t hr_f8 = 0;
    std::uint32_t hr_fc = 0;
    std::uint32_t hr_100 = 0;
    std::uint32_t hr_104 = 0;
    if (hit != 0) {
        SafeRead(hit + 0x3C, hr_3c);
        SafeRead(hit + 0xF0, hr_f0);
        SafeRead(hit + 0xF4, hr_f4);
        SafeRead(hit + 0xF8, hr_f8);
        SafeRead(hit + 0xFC, hr_fc);
        SafeRead(hit + 0x100, hr_100);
        SafeRead(hit + 0x104, hr_104);
    }

    char line[512]{};
    const int line_length =
        _snprintf_s(line,
                    sizeof(line),
                    _TRUNCATE,
                    "%s hit=%p rig=%p hr3c=%08X hrf0=%08X hrf4=%08X hrf8=%08X hrfc=%08X hr100=%08X hr104=%08X\r\n",
                    stage,
                    hit_record,
                    damage_rig,
                    hr_3c,
                    hr_f0,
                    hr_f4,
                    hr_f8,
                    hr_fc,
                    hr_100,
                    hr_104);
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
}

bool ShouldLogVerbose(std::atomic<unsigned long>& counter) {
    const unsigned long current = counter.fetch_add(1) + 1;
    return current <= g_config.max_verbose_events;
}

std::uint32_t FloatToBits(float value);
void MaybeWriteSummary();

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

int ReadHitRecordEffectBone(void* hit_record) {
    if (hit_record == nullptr) {
        return 0;
    }

    // HitRecord is POD in the SDK. With the published sizes:
    // bool flags + attackType + damage + qSafePointer + timers + MeleeInfo + ProjectileInfo +
    // CollisionInfo => mEffectBone lands at +0xF8.
    int effect_bone = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(hit_record) + 0xF8, effect_bone);
    return effect_bone;
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
        case kFogSlicingModeRva:
            return kLatestSteamFogSlicingModeRva;
        case kFinalCompositionRva:
            return kLatestSteamFinalCompositionRva;
        case kPresentBufferRva:
            return kLatestSteamPresentBufferRva;
        case kDisplayCurveBuilderRva:
            return kLatestSteamDisplayCurveBuilderRva;
        case kDxgiAdapterInitRva:
            return kLatestSteamDxgiAdapterInitRva;
        case kHealthApplyDamageRva:
            return kLatestSteamHealthApplyDamageRva;
        case kSimObjectGetComponentRva:
            return kLatestSteamSimObjectGetComponentRva;
        case kCharacterAnimationSetVisualDamageRva:
            return kLatestSteamCharacterAnimationSetVisualDamageRva;
        case kTimeOfDayAccessorRva:
            return kLatestSteamTimeOfDayAccessorRva;
        case kCharacterHandleWaterCollisionRva:
            return kLatestSteamCharacterHandleWaterCollisionRva;
        case kCharacterEffectsUpdateRva:
            return kLatestSteamCharacterEffectsUpdateRva;
        case kCharacterDamageRigSetVisualDamageRva:
            return kLatestSteamCharacterDamageRigSetVisualDamageRva;
        case kCharacterEffectDispatchConsumeRva:
            return kLatestSteamCharacterEffectDispatchConsumeRva;
        case kCharacterEffectQueueBuilderRva:
            return kLatestSteamCharacterEffectQueueBuilderRva;
        case kDamageRigApplyPaintEffectRva:
            return kLatestSteamDamageRigApplyPaintEffectRva;
        case kCharacterDamageRigResetDamageRva:
            return kLatestSteamCharacterDamageRigResetDamageRva;
        case kFinalCurveCutoffRva:
            return kLatestSteamFinalCurveCutoffRva;
        case kFinalCurveCoeffARva:
            return kLatestSteamFinalCurveCoeffARva;
        case kFinalCurveCoeffBRva:
            return kLatestSteamFinalCurveCoeffBRva;
        case kFinalCurveCoeffCRva:
            return kLatestSteamFinalCurveCoeffCRva;
        case kFinalCurveCoeffDRva:
            return kLatestSteamFinalCurveCoeffDRva;
        default:
            return rva;
    }
}

std::uintptr_t ResolveAddress(std::uintptr_t rva) {
    return g_module_base + RemapRvaForCurrentBuild(rva);
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

float ReadFloatValue(std::uintptr_t address, float fallback) {
    float value = fallback;
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

unsigned char ReadByteValue(std::uintptr_t address, unsigned char fallback) {
    unsigned char value = fallback;
    SafeRead(address, value);
    return value;
}

double ReadDoubleValue(std::uintptr_t address, double fallback) {
    double value = fallback;
    SafeRead(address, value);
    return value;
}

bool IsApprox60HzDelta(float value) {
    return value >= 0.015f && value <= 0.019f;
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
    if (address < g_module_base || address >= g_module_end) {
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

unsigned long long BytesToMegabytes(SIZE_T bytes) {
    return static_cast<unsigned long long>(bytes / (1024ull * 1024ull));
}

std::string AdapterDescriptionUtf8(const DXGI_ADAPTER_DESC& desc) {
    return log::ToUtf8(desc.Description);
}

bool IsSoftwareAdapter(const DXGI_ADAPTER_DESC& desc) {
    return std::wcscmp(desc.Description, L"Microsoft Basic Render Driver") == 0;
}

bool ProbeAndMaybeFixAdapterSelection(const char* reason) {
    IDXGIFactory* factory = reinterpret_cast<IDXGIFactory*>(ReadPointerSlot(g_dxgi_factory_slot));
    if (factory == nullptr || g_dxgi_adapter_slot == 0) {
        log::WarnF("vram_probe_skip reason=%s factory=0x%p adapter_slot=0x%p",
                   reason != nullptr ? reason : "unknown",
                   factory,
                   reinterpret_cast<void*>(g_dxgi_adapter_slot));
        return false;
    }

    IDXGIAdapter* current_adapter =
        reinterpret_cast<IDXGIAdapter*>(ReadPointerSlot(g_dxgi_adapter_slot));
    DXGI_ADAPTER_DESC current_desc{};
    bool current_desc_ok = current_adapter != nullptr && SUCCEEDED(current_adapter->GetDesc(&current_desc));
    unsigned long long current_mb =
        current_desc_ok ? BytesToMegabytes(current_desc.DedicatedVideoMemory) : 0ull;

    IDXGIAdapter* best_adapter = current_adapter;
    DXGI_ADAPTER_DESC best_desc = current_desc;
    bool best_desc_ok = current_desc_ok;
    unsigned long long best_mb = current_mb;

    for (UINT adapter_index = 0;; ++adapter_index) {
        IDXGIAdapter* adapter = nullptr;
        const HRESULT enum_hr = factory->EnumAdapters(adapter_index, &adapter);
        if (enum_hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enum_hr) || adapter == nullptr) {
            continue;
        }

        DXGI_ADAPTER_DESC desc{};
        const HRESULT desc_hr = adapter->GetDesc(&desc);
        if (SUCCEEDED(desc_hr)) {
            const unsigned long long dedicated_mb = BytesToMegabytes(desc.DedicatedVideoMemory);
            const unsigned long long shared_mb = BytesToMegabytes(desc.SharedSystemMemory);
            log::InfoF("vram_adapter index=%u desc=\"%s\" vendor=0x%04X device=0x%04X dedicated_mb=%llu shared_mb=%llu software=%d",
                       adapter_index,
                       AdapterDescriptionUtf8(desc).c_str(),
                       static_cast<unsigned int>(desc.VendorId),
                       static_cast<unsigned int>(desc.DeviceId),
                       dedicated_mb,
                       shared_mb,
                       IsSoftwareAdapter(desc) ? 1 : 0);

            const bool candidate_is_better =
                !IsSoftwareAdapter(desc) &&
                (!best_desc_ok ||
                 dedicated_mb > best_mb ||
                 (dedicated_mb == best_mb &&
                  current_desc_ok &&
                  std::wcscmp(desc.Description, current_desc.Description) != 0 &&
                  IsSoftwareAdapter(best_desc)));
            if (candidate_is_better) {
                if (best_adapter != nullptr && best_adapter != current_adapter) {
                    best_adapter->Release();
                }
                best_adapter = adapter;
                best_desc = desc;
                best_desc_ok = true;
                best_mb = dedicated_mb;
                continue;
            }
        }

        adapter->Release();
    }

    unsigned long long effective_mb = best_mb;
    if (g_config.vram_override_mb > 0) {
        effective_mb = static_cast<unsigned long long>(g_config.vram_override_mb);
    }

    g_vram_probe_count.fetch_add(1);
    g_last_vram_selected_mb.store(current_mb);
    g_last_vram_best_mb.store(best_mb);
    g_last_vram_effective_mb.store(effective_mb);

    if (g_config.hook_vram_adapter_fix && best_adapter != nullptr && best_adapter != current_adapter) {
        SafeWrite(g_dxgi_adapter_slot, reinterpret_cast<std::uintptr_t>(best_adapter));
        g_vram_adapter_override_count.fetch_add(1);
        log::InfoF("vram_adapter_override reason=%s from=\"%s\" from_mb=%llu to=\"%s\" to_mb=%llu effective_mb=%llu",
                   reason != nullptr ? reason : "unknown",
                   current_desc_ok ? AdapterDescriptionUtf8(current_desc).c_str() : "<unknown>",
                   current_mb,
                   best_desc_ok ? AdapterDescriptionUtf8(best_desc).c_str() : "<unknown>",
                   best_mb,
                   effective_mb);
    } else {
        if (best_adapter != nullptr && best_adapter != current_adapter) {
            best_adapter->Release();
        }
        log::InfoF("vram_probe reason=%s selected=\"%s\" selected_mb=%llu best=\"%s\" best_mb=%llu effective_mb=%llu override_mb=%d",
                   reason != nullptr ? reason : "unknown",
                   current_desc_ok ? AdapterDescriptionUtf8(current_desc).c_str() : "<unknown>",
                   current_mb,
                   best_desc_ok ? AdapterDescriptionUtf8(best_desc).c_str() : "<unknown>",
                   best_mb,
                   effective_mb,
                   g_config.vram_override_mb);
    }

    return true;
}

struct StockCurveCoefficients {
    float cutoff = 0.0f;
    double coeff_a = 0.0;
    double coeff_b = 0.0;
    double coeff_c = 1.0;
    double coeff_d = 1.0;
};

StockCurveCoefficients ReadStockCurveCoefficients() {
    StockCurveCoefficients coefficients;
    coefficients.cutoff = ReadFloatValue(g_final_curve_cutoff, 0.0f);
    coefficients.coeff_a = ReadDoubleValue(g_final_curve_coeff_a, 0.0);
    coefficients.coeff_b = ReadDoubleValue(g_final_curve_coeff_b, 0.0);
    coefficients.coeff_c = ReadDoubleValue(g_final_curve_coeff_c, 1.0);
    coefficients.coeff_d = ReadDoubleValue(g_final_curve_coeff_d, 1.0);
    return coefficients;
}

float EvaluateStockFinalCurve(float exposure, const StockCurveCoefficients& coefficients) {
    const float cutoff = coefficients.cutoff;
    const double coeff_a = coefficients.coeff_a;
    const double coeff_b = coefficients.coeff_b;
    const double coeff_c = coefficients.coeff_c;
    const double coeff_d = coefficients.coeff_d;

    float adjusted = exposure - cutoff;
    if (adjusted < 0.0f) {
        adjusted = 0.0f;
    }

    const double x = adjusted;
    const double numerator = ((x * coeff_a) + coeff_b) * x;
    const double denominator = (((x * coeff_a) + coeff_c) * x) + coeff_d;
    if (denominator <= 1e-6) {
        return 0.0f;
    }

    const double curve = numerator / denominator;
    if (!std::isfinite(curve)) {
        return 0.0f;
    }

    if (curve <= 0.0) {
        return 0.0f;
    }
    if (curve >= 1.0) {
        return 1.0f;
    }
    return static_cast<float>(curve);
}

float EvaluateAcesFitted(float exposure) {
    const float x = exposure < 0.0f ? 0.0f : exposure;
    const float numerator = x * ((2.51f * x) + 0.03f);
    const float denominator = x * ((2.43f * x) + 0.59f) + 0.14f;
    if (denominator <= 1e-6f) {
        return 0.0f;
    }

    const float curve = numerator / denominator;
    if (!std::isfinite(curve) || curve <= 0.0f) {
        return 0.0f;
    }
    if (curve >= 1.0f) {
        return 1.0f;
    }
    return curve;
}

float ComputeStockSharedOffset(float gamma_setting) {
    const float offset_scale = ReadFloatValue(g_final_curve_offset_scale, 0.0f);
    return (gamma_setting - 1.0f) * offset_scale;
}

void MarkDisplayCurveDirty() {
    if (g_display_curve_dirty != 0) {
        SafeWrite(g_display_curve_dirty, static_cast<unsigned char>(1));
    }
}

void PersistAcesConfig() {
    if (g_config_path.empty()) {
        return;
    }

    WriteBoolValue(g_config_path, L"aces_enable", g_aces_runtime_enabled.load());
    WriteIntValue(g_config_path, L"aces_strength_percent", g_aces_runtime_strength_percent.load());
    WriteIntValue(g_config_path,
                  L"aces_exposure_scale_percent",
                  g_aces_runtime_exposure_scale_percent.load());
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

#if !defined(SPATCH_FINAL_RELEASE)
void* ReadProgressionTrackerInstance() {
    if (g_progression_tracker_instance_slot == 0) {
        return nullptr;
    }

    std::uintptr_t tracker = 0;
    if (!SafeRead(g_progression_tracker_instance_slot, tracker) || tracker == 0) {
        return nullptr;
    }

    return reinterpret_cast<void*>(tracker);
}

const CutContentCandidate* GetCutContentCandidate(int candidate_index) {
    if (candidate_index < 0 ||
        candidate_index >= static_cast<int>(kCutContentCandidates.size())) {
        return nullptr;
    }

    return &kCutContentCandidates[static_cast<std::size_t>(candidate_index)];
}

void MaybeRunCutContentProbe() {
    if (g_cut_content_force_pending && !g_cut_content_force_applied) {
        g_cut_content_force_applied = true;
        g_cut_content_force_pending = false;

        if (g_progression_force_slice_change != nullptr && g_cut_content_force_tracker != nullptr &&
            g_cut_content_force_slice != nullptr) {
            const auto* candidate = GetCutContentCandidate(g_cut_content_force_candidate_index);
            g_progression_force_slice_change(
                g_cut_content_force_tracker,
                g_cut_content_force_slice,
                g_config.force_cut_content_simulate_rewards);
            log::InfoF(
                "cut_content_force candidate=%d label=\"%s\" symbol=\"%s\" slice=0x%p simulate_rewards=%d",
                g_cut_content_force_candidate_index,
                candidate != nullptr ? candidate->label : "<unknown>",
                candidate != nullptr ? candidate->symbol : "<unknown>",
                g_cut_content_force_slice,
                g_config.force_cut_content_simulate_rewards ? 1 : 0);
        } else {
            log::Warn("cut_content_force_skipped reason=missing_runtime_state");
        }
    }

    if (g_cut_content_probe_attempted) {
        return;
    }

    if (!g_config.probe_cut_content_slices && g_config.force_cut_content_candidate < 0) {
        return;
    }

    if (g_progression_find == nullptr || g_progression_tracker_instance_slot == 0) {
        g_cut_content_probe_attempted = true;
        log::Warn("cut_content_probe skipped because progression pointers were unresolved");
        return;
    }

    void* tracker = ReadProgressionTrackerInstance();
    if (tracker == nullptr) {
        return;
    }

    g_cut_content_probe_attempted = true;

    log::InfoF("cut_content_probe_start tracker=0x%p probe=%d force_candidate=%d simulate_rewards=%d",
               tracker,
               g_config.probe_cut_content_slices ? 1 : 0,
               g_config.force_cut_content_candidate,
               g_config.force_cut_content_simulate_rewards ? 1 : 0);

    for (int i = 0; i < static_cast<int>(kCutContentCandidates.size()); ++i) {
        const auto& candidate = kCutContentCandidates[static_cast<std::size_t>(i)];
        const RawQSymbol symbol{candidate.uid};
        void* slice = g_progression_find(tracker, symbol, true);
        log::InfoF(
            "cut_content_probe candidate=%d label=\"%s\" symbol=\"%s\" uid=0x%08X found=%d slice=0x%p",
            i,
            candidate.label,
            candidate.symbol,
            candidate.uid,
            slice != nullptr ? 1 : 0,
            slice);

        if (i != g_config.force_cut_content_candidate) {
            continue;
        }

        if (slice == nullptr) {
            log::WarnF("cut_content_force_missing candidate=%d label=\"%s\" symbol=\"%s\" uid=0x%08X",
                       i,
                       candidate.label,
                       candidate.symbol,
                       candidate.uid);
            continue;
        }

        g_cut_content_force_pending = true;
        g_cut_content_force_tracker = tracker;
        g_cut_content_force_slice = slice;
        g_cut_content_force_candidate_index = i;
        log::InfoF(
            "cut_content_force_arm candidate=%d label=\"%s\" symbol=\"%s\" uid=0x%08X slice=0x%p simulate_rewards=%d",
            i,
            candidate.label,
            candidate.symbol,
            candidate.uid,
            slice,
            g_config.force_cut_content_simulate_rewards ? 1 : 0);
    }
}

void PersistAaConfig() {
#else
void PersistAaConfig() {
#endif
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

float InvertStockFinalCurve(float target_curve,
                            float source_exposure,
                            const StockCurveCoefficients& coefficients) {
    float target = target_curve;
    if (target <= 0.0f) {
        return 0.0f;
    }
    if (target >= 1.0f) {
        target = 0.9995f;
    }

    float low = 0.0f;
    float high = source_exposure > 1.0f ? source_exposure : 1.0f;
    while (EvaluateStockFinalCurve(high, coefficients) < target && high < 256.0f) {
        high *= 2.0f;
    }
    if (high > 256.0f) {
        high = 256.0f;
    }

    for (int i = 0; i < 24; ++i) {
        const float mid = (low + high) * 0.5f;
        if (EvaluateStockFinalCurve(mid, coefficients) < target) {
            low = mid;
        } else {
            high = mid;
        }
    }

    return high;
}

bool ConsumeHotkeyPress(int virtual_key,
                        std::atomic<unsigned long>& last_tick,
                        unsigned long debounce_ms) {
    if ((GetAsyncKeyState(virtual_key) & 1) == 0) {
        return false;
    }

    const unsigned long now = GetTickCount();
    const unsigned long last = last_tick.load();
    if (now - last < debounce_ms) {
        return false;
    }

    last_tick.store(now);
    return true;
}

void PollAcesDebugKeys() {
    if (!g_config.aces_debug_keys) {
        if (!g_config.aa_variant_debug_keys && !g_config.aa_aux_debug_keys &&
            !g_config.smaa_debug_keys) {
            return;
        }
    } else {
        if (ConsumeHotkeyPress('P', g_last_aces_toggle_key_tick, 200)) {
            SetAcesEnabled(!g_aces_runtime_enabled.load());
        }

        if (ConsumeHotkeyPress(VK_F11, g_last_aces_strength_up_key_tick, 150)) {
            int strength = g_aces_runtime_strength_percent.load();
            strength += 10;
            if (strength > 100) {
                strength = 100;
            }
            SetAcesStrengthPercent(strength);
        }

        if (ConsumeHotkeyPress(VK_F9, g_last_aces_strength_down_key_tick, 150)) {
            int strength = g_aces_runtime_strength_percent.load();
            strength -= 10;
            if (strength < 0) {
                strength = 0;
            }
            SetAcesStrengthPercent(strength);
        }

        if (ConsumeHotkeyPress(VK_F8, g_last_aces_scale_down_key_tick, 150)) {
            int scale = g_aces_runtime_exposure_scale_percent.load();
            scale -= 25;
            if (scale < 25) {
                scale = 25;
            }
            SetAcesExposureScalePercent(scale);
        }

        if (ConsumeHotkeyPress(VK_F12, g_last_aces_scale_up_key_tick, 150)) {
            int scale = g_aces_runtime_exposure_scale_percent.load();
            scale += 25;
            if (scale > 400) {
                scale = 400;
            }
            SetAcesExposureScalePercent(scale);
        }
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

unsigned int ReadAoFeatureFlag(std::uintptr_t render_context) {
    unsigned char value = 0;
    if (render_context != 0) {
        SafeRead(render_context + 0x35C, value);
    }
    return value;
}

unsigned int ReadAoStageFlag(std::uintptr_t render_context) {
    unsigned char value = 0;
    if (render_context != 0) {
        SafeRead(render_context + 0x364, value);
    }
    return value;
}

int ReadAoSsaoSetting(std::uintptr_t render_context) {
    int value = 0;
    if (render_context != 0) {
        SafeRead(render_context + 0x15C, value);
    }
    return value;
}

float ReadAoOpacity(std::uintptr_t render_context) {
    std::uint32_t bits = 0;
    if (render_context != 0) {
        SafeRead(render_context + 0x34C, bits);
    }
    return BitsToFloat(bits);
}

std::uintptr_t ReadAoMaterialSlot(std::uintptr_t render_context, std::uintptr_t offset) {
    std::uintptr_t material = 0;
    if (render_context != 0) {
        SafeRead(render_context + offset, material);
    }
    return material;
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

void ReadTargetDimensions(std::uintptr_t target, unsigned int& width, unsigned int& height) {
    width = 0;
    height = 0;
    if (target == 0) {
        return;
    }

    int signed_width = 0;
    int signed_height = 0;
    SafeRead(target + 0x0, signed_width);
    SafeRead(target + 0x4, signed_height);
    if (signed_width > 0) {
        width = static_cast<unsigned int>(signed_width);
    }
    if (signed_height > 0) {
        height = static_cast<unsigned int>(signed_height);
    }
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

int SelectAoMaterialSlot(int ssao_setting,
                         std::uintptr_t slot0,
                         std::uintptr_t slot1,
                         std::uintptr_t slot2,
                         std::uintptr_t slot3) {
    if (ssao_setting == 1) {
        if (slot1 != 0) {
            return 1;
        }
        if (slot2 != 0) {
            return 2;
        }
    } else if (ssao_setting == 0 && slot0 != 0) {
        return 0;
    }

    if (slot0 != 0) {
        return 0;
    }
    if (slot1 != 0) {
        return 1;
    }
    if (slot2 != 0) {
        return 2;
    }
    if (slot3 != 0) {
        return 3;
    }

    return -1;
}

std::uintptr_t ResolveAoMaterialBySlot(int slot,
                                       std::uintptr_t slot0,
                                       std::uintptr_t slot1,
                                       std::uintptr_t slot2,
                                       std::uintptr_t slot3) {
    switch (slot) {
    case 0:
        return slot0;
    case 1:
        return slot1;
    case 2:
        return slot2;
    case 3:
        return slot3;
    default:
        return 0;
    }
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

    std::string text;
    text.reserve(max_length);

    for (std::size_t index = 0; index < max_length; ++index) {
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

bool IsInterestingCallback(std::uintptr_t callback) {
    if (callback == 0 || callback == static_cast<std::uintptr_t>(-1) ||
        callback == static_cast<std::uintptr_t>(-2)) {
        return false;
    }

    return callback >= g_module_base && callback < g_module_end;
}

bool RecordSeenCallback(std::uintptr_t callback) {
    if (!IsInterestingCallback(callback)) {
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

    g_seen_callbacks.push_back(callback);
    g_unique_callback_count.fetch_add(1);
    return true;
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

void MaybeWriteSummary() {
    const unsigned long now = GetTickCount();
    unsigned long last = g_last_summary_tick.load();
    if (now - last < g_config.summary_interval_ms) {
        return;
    }

    if (!g_last_summary_tick.compare_exchange_strong(last, now)) {
        return;
    }

    const smaa::Stats smaa_stats = smaa::GetStats();

    log::InfoF(
        "summary task_ready=%llu task_dispatch=%llu wait_helper=%llu wait_task=%llu "
        "wait_gt16=%llu wait_gt100=%llu wait_gt1000=%llu wait_gt5000=%llu "
        "scaleform_time=%llu provider_non_null=%llu scaleform_init=%llu "
        "nis_time=%llu nis_sync=%llu nis_dt0=%llu nis_dt30=%llu nis_dt60=%llu nis_dt_other=%llu nis_scene_fix=%llu "
        "nis_play=%llu nis_play_adv=%llu nis_play_repeat=%llu nis_play_multi=%llu "
        "nis_boot=%llu nis_boot_s1=%llu nis_boot_s2=%llu nis_boot_fail=%llu "
        "nis_owner=%llu nis_owner_dt0=%llu nis_owner_dt30=%llu nis_owner_dt60=%llu nis_owner_dt_other=%llu "
        "nis_owner_adv=%llu nis_owner_repeat=%llu nis_owner_multi=%llu "
        "nis_last=%.6f nis_last_delta=%.6f nis_owner_last_dt=%.6f "
        "frameflow=%llu frameflow_dt0=%llu frameflow_dt60=%llu frameflow_dt_other=%llu "
        "frameflow_from_cutscene=%llu frameflow_last_dt=%.6f "
        "cutscene_flow=%llu cutscene_flow_dt0=%llu cutscene_flow_fwd0=%llu "
        "cutscene_flow_fwd60=%llu cutscene_flow_fwd_other=%llu cutscene_flow_fix=%llu cutscene_flow_in=%.6f cutscene_flow_fwd=%.6f "
        "fog_slicing=%llu fog_clamps=%llu ao_stage=%llu ao_skip=%llu ao_none=%llu ao_main=%llu ao_feature=%u ao_state=%u ao_ssao=%d ao_opacity=%.6f "
        "ao_material=%llu ao_mat_sel=%d ao_mat0=0x%p ao_mat1=0x%p ao_mat2=0x%p ao_mat3=0x%p ao_mat_selected=0x%p "
        "ao_compute=%llu ao_compute_p0=%llu ao_compute_p1=%llu ao_compute_comp=%llu ao_compute_comp_p0=%llu ao_compute_comp_p1=%llu ao_compute_last=%d ao_compute_comp_last=%d "
        "aa_owner=%llu aa_skip=%llu aa_main=%llu aa_hairblur0=%llu aa_state=%d aa_hair_gate=%d aa_variant=%d aa_variant_apply=%llu aa_aux_mode=%d aa_aux_apply=%llu aa_shader=0x%08X aa_raster=0x%08X aa_aux=0x%08X aa_material=0x%p aa_target=0x%p aa_src_a=0x%p aa_src_b=0x%p aa_fx=%llu aa_fx_arg1=0x%p aa_fx_arg2=0x%p aa_fx_arg3=0x%p "
        "smaa_enabled=%d smaa_hooked=%d smaa_ready=%d smaa_present=%llu smaa_apply=%llu smaa_fail=%llu smaa_resize=%llu smaa_size=%ux%u "
        "rumble_override=%llu rumble_value=%d "
        "post_submit=%llu post_comp_lights=%llu post_comp_final=%llu post_bloom=%llu post_lightshaft=%llu post_shadow_collector=%llu "
        "post_final_chg=%llu post_final_flags=%d post_final_cmd=0x%p post_final_params=0x%p "
        "post_final_p0=0x%p post_final_p1=0x%p post_final_p2=0x%p post_final_p3=0x%p "
        "vram_probe=%llu vram_override=%llu vram_selected_mb=%llu vram_best_mb=%llu vram_effective_mb=%llu "
        "char_water=%llu char_water_speed=%.3f char_fx_update=%llu char_fx_wet_updates=%llu char_fx_surface=0x%08X char_fx_wet_surface=0x%08X char_fx_gate=%d char_fx_tod_weather=%.3f char_fx_tod_override=%.3f char_fx_onfire=%u char_fx_smolder=%u char_fx_attached=%u char_fx_fire_time=%.3f char_fx_smolder_time=%.3f char_fx_queued_damage=%.3f char_health_apply=%llu char_health_proj=%llu char_health_melee=%llu char_health_anim_found=%llu char_health_hitreact_found=%llu char_health_damage=%d char_health_projectile=%u char_health_component=0x%p char_health_anim=0x%p char_health_hitreact=0x%p char_health_attacker=0x%p char_health_hit=0x%p char_wet_force=%llu char_wet_force_verify=%llu char_charred_anim=%llu char_charred_rig=%llu char_charred_amount=%.3f char_dispatch_owner=%llu char_dispatch_consume=%llu char_dispatch_owner_ptr=0x%p char_dispatch_component=0x%p char_queue_build=%llu char_queue_build_tracked=%llu char_queue_build_owner=0x%p char_queue_build_component=0x%p char_queue_build_mode=%u char_paint_owner=%llu char_paint_owner_ptr=0x%p char_paint_owner_component=0x%p char_paint_consumer=%llu char_paint_anim=%llu char_paint_rig=%llu char_paint_enable=%u char_paint_rgb=%.3f/%.3f/%.3f char_damage_create=%llu char_visual_anim=%llu char_visual_rig=%llu char_visual_restore=%llu char_damage_reset=%llu char_damage_bone=0x%08X char_damage_amount=%.3f char_damage_duration=%.3f char_damage_restored=%u "
        "aces_final=%llu aces_apply=%llu aces_present=%llu aces_present_neutralize=%llu aces_display=%llu aces_display_linear=%llu aces_display_mode=%u aces_enabled=%d aces_strength=%d aces_scale=%d "
        "aces_toggle=%llu aces_adjust=%llu aces_src=%.6f aces_remap=%.6f aces_stock=%.6f aces_target=%.6f "
        "aces_present_offset=%.6f "
        "post_last=0x%p unique_callbacks=%llu "
        "scenery_prepare=%llu scenery_setup=%llu render_scenery=%llu rasterize_bucket=%llu "
        "scenery_prepare_ready=%llu scenery_setup_ready=%llu render_scenery_ready=%llu "
        "rasterize_bucket_ready=%llu "
        "scenery_setup_qdelta=%llu render_scenery_qdelta=%llu rasterize_bucket_qdelta=%llu "
        "scenery_counts=%u/%u/%u/%u "
        "provider_ptr=0x%p",
        g_task_ready_count.load(),
        g_task_dispatch_count.load(),
        g_wait_helper_count.load(),
        g_wait_helper_nonzero_task_count.load(),
        g_wait_over_16ms_count.load(),
        g_wait_over_100ms_count.load(),
        g_wait_over_1000ms_count.load(),
        g_wait_over_5000ms_count.load(),
        g_scaleform_time_count.load(),
        g_scaleform_provider_non_null_count.load(),
        g_scaleform_init_count.load(),
        g_nis_set_play_time_count.load(),
        g_nis_sync_scene_time_count.load(),
        g_nis_delta_zero_count.load(),
        g_nis_delta_30hz_count.load(),
        g_nis_delta_60hz_count.load(),
        g_nis_delta_other_count.load(),
        g_nis_scene_time_fix_count.load(),
        g_nis_play_count.load(),
        g_nis_play_advanced_count.load(),
        g_nis_play_repeat_count.load(),
        g_nis_play_multi_tick_count.load(),
        g_nis_bootstrap_count.load(),
        g_nis_bootstrap_state1_count.load(),
        g_nis_bootstrap_state2_count.load(),
        g_nis_bootstrap_fail_count.load(),
        g_nis_owner_count.load(),
        g_nis_owner_dt_zero_count.load(),
        g_nis_owner_dt_30hz_count.load(),
        g_nis_owner_dt_60hz_count.load(),
        g_nis_owner_dt_other_count.load(),
        g_nis_owner_advanced_count.load(),
        g_nis_owner_repeat_count.load(),
        g_nis_owner_multi_tick_count.load(),
        BitsToFloat(g_last_nis_scene_time_bits.load()),
        BitsToFloat(g_last_nis_scene_delta_bits.load()),
        BitsToFloat(g_last_nis_owner_dt_bits.load()),
        g_frame_flow_count.load(),
        g_frame_flow_dt_zero_count.load(),
        g_frame_flow_dt_60hz_count.load(),
        g_frame_flow_dt_other_count.load(),
        g_frame_flow_from_cutscene_owner_count.load(),
        BitsToFloat(g_last_frame_flow_dt_bits.load()),
        g_cutscene_flow_owner_count.load(),
        g_cutscene_flow_owner_dt_zero_count.load(),
        g_cutscene_flow_owner_forwarded_zero_count.load(),
        g_cutscene_flow_owner_forwarded_60hz_count.load(),
        g_cutscene_flow_owner_forwarded_other_count.load(),
        g_cutscene_zero_dt_override_count.load(),
        BitsToFloat(g_last_cutscene_flow_input_dt_bits.load()),
        BitsToFloat(g_last_cutscene_flow_forwarded_dt_bits.load()),
        g_fog_slicing_mode_count.load(),
        g_fog_slicing_clamp_count.load(),
        g_ao_stage_count.load(),
        g_ao_skip_count.load(),
        g_ao_none_path_count.load(),
        g_ao_main_path_count.load(),
        g_last_ao_feature_flag.load(),
        g_last_ao_stage_flag.load(),
        g_last_ao_ssao_setting.load(),
        BitsToFloat(g_last_ao_opacity_bits.load()),
        g_ao_material_count.load(),
        g_last_ao_material_selected_slot.load(),
        reinterpret_cast<void*>(g_last_ao_material_slot0.load()),
        reinterpret_cast<void*>(g_last_ao_material_slot1.load()),
        reinterpret_cast<void*>(g_last_ao_material_slot2.load()),
        reinterpret_cast<void*>(g_last_ao_material_slot3.load()),
        reinterpret_cast<void*>(g_last_ao_material_selected.load()),
        g_ao_compute_node_count.load(),
        g_ao_compute_pass0_count.load(),
        g_ao_compute_pass1_count.load(),
        g_ao_compute_composite_count.load(),
        g_ao_compute_composite_pass0_count.load(),
        g_ao_compute_composite_pass1_count.load(),
        g_last_ao_compute_pass.load(),
        g_last_ao_compute_composite_pass.load(),
        g_aa_owner_count.load(),
        g_aa_skip_count.load(),
        g_aa_main_count.load(),
        g_aa_hair_blur_zero_count.load(),
        g_last_aa_state_gate.load(),
        g_last_aa_hair_blur_gate.load(),
        g_last_aa_variant_mode.load(),
        g_aa_variant_apply_count.load(),
        g_last_aa_aux_mode.load(),
        g_aa_aux_apply_count.load(),
        g_last_aa_shader_uid.load(),
        g_last_aa_raster_uid.load(),
        g_last_aa_aux_uid.load(),
        reinterpret_cast<void*>(g_last_aa_material.load()),
        reinterpret_cast<void*>(g_last_aa_target.load()),
        reinterpret_cast<void*>(g_last_aa_source_a.load()),
        reinterpret_cast<void*>(g_last_aa_source_b.load()),
        g_aa_fx_handoff_count.load(),
        reinterpret_cast<void*>(g_last_aa_fx_arg1.load()),
        reinterpret_cast<void*>(g_last_aa_fx_arg2.load()),
        reinterpret_cast<void*>(g_last_aa_fx_arg3.load()),
        smaa_stats.enabled ? 1 : 0,
        smaa_stats.hooks_installed ? 1 : 0,
        smaa_stats.resources_ready ? 1 : 0,
        smaa_stats.present_count,
        smaa_stats.apply_count,
        smaa_stats.fail_count,
        smaa_stats.resize_count,
        smaa_stats.width,
        smaa_stats.height,
        g_rumble_override_apply_count.load(),
        g_last_rumble_override_value.load(),
        g_post_material_submit_count.load(),
        g_post_composite_lights_submit_count.load(),
        g_post_composite_final_submit_count.load(),
        g_post_bloom_threshold_submit_count.load(),
        g_post_lightshaft_submit_count.load(),
        g_post_shadow_collector_submit_count.load(),
        g_post_composite_final_snapshot_change_count.load(),
        g_last_post_final_flags.load(),
        reinterpret_cast<void*>(g_last_post_final_cmd.load()),
        reinterpret_cast<void*>(g_last_post_final_params.load()),
        reinterpret_cast<void*>(g_last_post_final_param0.load()),
        reinterpret_cast<void*>(g_last_post_final_param1.load()),
        reinterpret_cast<void*>(g_last_post_final_param2.load()),
        reinterpret_cast<void*>(g_last_post_final_param3.load()),
        g_vram_probe_count.load(),
        g_vram_adapter_override_count.load(),
        g_last_vram_selected_mb.load(),
        g_last_vram_best_mb.load(),
        g_last_vram_effective_mb.load(),
        g_character_water_collision_count.load(),
        BitsToFloat(g_last_character_water_speed_bits.load()),
        g_character_effects_update_count.load(),
        g_character_effects_wet_surface_count.load(),
        g_last_character_active_surface_uid.load(),
        g_last_character_active_wet_surface_uid.load(),
        g_last_character_wet_gate_counter.load(),
        BitsToFloat(g_last_character_timeofday_weather_bits.load()),
        BitsToFloat(g_last_character_timeofday_override_bits.load()),
        g_last_character_is_on_fire.load(),
        g_last_character_is_smoldering.load(),
        g_last_character_is_attached_to_player.load(),
        BitsToFloat(g_last_character_fire_extinguish_bits.load()),
        BitsToFloat(g_last_character_smolder_extinguish_bits.load()),
        BitsToFloat(g_last_character_queued_health_damage_bits.load()),
        g_character_health_apply_damage_count.load(),
        g_character_health_apply_projectile_count.load(),
        g_character_health_apply_melee_count.load(),
        g_character_health_anim_found_count.load(),
        g_character_health_hitreact_found_count.load(),
        g_last_character_health_damage.load(),
        g_last_character_health_projectile.load(),
        reinterpret_cast<void*>(g_last_character_health_component.load()),
        reinterpret_cast<void*>(g_last_character_health_anim_component.load()),
        reinterpret_cast<void*>(g_last_character_health_hitreact_component.load()),
        reinterpret_cast<void*>(g_last_character_health_attacker.load()),
        reinterpret_cast<void*>(g_last_character_health_hit_record.load()),
        g_character_wet_force_count.load(),
        g_character_wet_force_verify_count.load(),
        g_character_charred_anim_count.load(),
        g_character_charred_rig_count.load(),
        BitsToFloat(g_last_character_charred_amount_bits.load()),
        g_character_effect_dispatch_owner_count.load(),
        g_character_effect_dispatch_consume_count.load(),
        reinterpret_cast<void*>(g_last_character_effect_dispatch_owner.load()),
        reinterpret_cast<void*>(g_last_character_effect_dispatch_component.load()),
        g_character_effect_queue_builder_count.load(),
        g_character_effect_queue_builder_tracked_count.load(),
        reinterpret_cast<void*>(g_last_character_effect_queue_builder_owner.load()),
        reinterpret_cast<void*>(g_last_character_effect_queue_builder_component.load()),
        g_last_character_effect_queue_builder_mode.load(),
        g_character_paint_owner_count.load(),
        reinterpret_cast<void*>(g_last_character_paint_owner_ptr.load()),
        reinterpret_cast<void*>(g_last_character_paint_owner_component.load()),
        g_character_paint_consumer_count.load(),
        g_character_paint_anim_count.load(),
        g_character_paint_rig_count.load(),
        g_last_character_paint_enable.load(),
        BitsToFloat(g_last_character_paint_r_bits.load()),
        BitsToFloat(g_last_character_paint_g_bits.load()),
        BitsToFloat(g_last_character_paint_b_bits.load()),
        g_character_damage_rig_create_count.load(),
        g_character_visual_damage_anim_count.load(),
        g_character_visual_damage_rig_count.load(),
        g_character_visual_damage_restore_count.load(),
        g_character_damage_rig_reset_count.load(),
        g_last_character_damage_bone.load(),
        BitsToFloat(g_last_character_damage_amount_bits.load()),
        BitsToFloat(g_last_character_damage_duration_bits.load()),
        g_last_character_damage_restored.load(),
        g_aces_final_count.load(),
        g_aces_apply_count.load(),
        g_aces_presentbuffer_count.load(),
        g_aces_presentbuffer_neutralize_count.load(),
        g_aces_display_curve_count.load(),
        g_aces_display_curve_linearize_count.load(),
        g_last_aces_display_curve_mode.load(),
        g_aces_runtime_enabled.load() ? 1 : 0,
        g_aces_runtime_strength_percent.load(),
        g_aces_runtime_exposure_scale_percent.load(),
        g_aces_toggle_count.load(),
        g_aces_adjust_count.load(),
        BitsToFloat(g_last_aces_source_exposure_bits.load()),
        BitsToFloat(g_last_aces_remapped_exposure_bits.load()),
        BitsToFloat(g_last_aces_stock_curve_bits.load()),
        BitsToFloat(g_last_aces_target_curve_bits.load()),
        BitsToFloat(g_last_aces_presentbuffer_offset_bits.load()),
        reinterpret_cast<void*>(g_last_post_material.load()),
        g_unique_callback_count.load(),
        g_scenery_prepare_count.load(),
        g_scenery_setup_count.load(),
        g_render_scenery_builder_count.load(),
        g_rasterize_bucket_builder_count.load(),
        g_scenery_prepare_ready_total.load(),
        g_scenery_setup_ready_total.load(),
        g_render_scenery_ready_total.load(),
        g_rasterize_bucket_ready_total.load(),
        g_scenery_setup_queue_delta_total.load(),
        g_render_scenery_queue_delta_total.load(),
        g_rasterize_bucket_queue_delta_total.load(),
        g_last_scenery_counter0.load(),
        g_last_scenery_counter1.load(),
        g_last_scenery_counter2.load(),
        g_last_scenery_counter3.load(),
        reinterpret_cast<void*>(ReadProviderValue()));
}

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
        task_name = ReadTaskName(task);
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

    const std::uintptr_t queued_task = PeekQueuedTaskHeader(reinterpret_cast<std::uintptr_t>(task_manager));
    const std::uintptr_t callback = ReadTaskCallback(queued_task);

    if (RecordSeenCallback(callback)) {
        const std::string task_name = ReadTaskName(queued_task);
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
                name = ReadTaskName(reinterpret_cast<std::uintptr_t>(task_header));
                return name.empty() ? "<all_tasks>" : name.c_str();
            }());
    }

    MaybeWriteSummary();
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
    g_nis_set_play_time_count.fetch_add(1);
    if (sync_scene_time) {
        g_nis_sync_scene_time_count.fetch_add(1);
    }

    const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
    const auto previous_manager = g_last_nis_manager.exchange(manager);
    const float previous_scene_time = BitsToFloat(g_last_nis_scene_time_bits.load());
    const float outer_dt = BitsToFloat(g_last_cutscene_flow_forwarded_dt_bits.load());
    const bool inner_fix_candidate =
        g_config.fix_cutscene_scene_time_step &&
        previous_manager == manager &&
        !sync_scene_time &&
        IsApprox60HzDelta(outer_dt);

    float applied_scene_time = scene_time;
    bool scene_time_fixed = false;
    if (inner_fix_candidate && scene_time >= previous_scene_time) {
        const float original_delta = scene_time - previous_scene_time;
        if (original_delta >= 0.0f && original_delta < 0.050f) {
            const float normalized_scene_time = previous_scene_time + outer_dt;
            if ((original_delta >= 0.0205f && original_delta <= 0.0235f) ||
                (original_delta >= 0.0f && original_delta < 0.0005f) ||
                (original_delta >= 0.003f && original_delta <= 0.0075f) ||
                !IsApprox60HzDelta(original_delta)) {
                applied_scene_time = normalized_scene_time;
                if (applied_scene_time != scene_time) {
                    scene_time_fixed = true;
                    g_nis_scene_time_fix_count.fetch_add(1);
                }
            }
        }
    }

    float delta = 0.0f;
    if (previous_manager == manager && applied_scene_time >= previous_scene_time) {
        delta = applied_scene_time - previous_scene_time;
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
        if (delta >= 0.0f && delta < 0.0005f && previous_manager == manager) {
            ++g_nis_play_scope_zero_delta_calls;
        } else if (delta > 0.0f) {
            ++g_nis_play_scope_advanced_calls;
        }
    }

    if (g_nis_owner_scope_depth != 0) {
        ++g_nis_owner_scope_calls;
        if (delta >= 0.0f && delta < 0.0005f && previous_manager == manager) {
            ++g_nis_owner_scope_zero_delta_calls;
        } else if (delta > 0.0f) {
            ++g_nis_owner_scope_advanced_calls;
        }
    }

    g_last_nis_scene_time_bits.store(FloatToBits(applied_scene_time));
    g_last_nis_scene_delta_bits.store(FloatToBits(delta));

    if (ShouldLogVerbose(g_nis_set_play_time_verbose) || delta >= 0.030f || scene_time_fixed) {
        int state = 0;
        std::uintptr_t active_instance = 0;
        if (manager != 0) {
            SafeRead(manager + 0x44, state);
            SafeRead(manager + 0x10, active_instance);
        }

        const char* bucket = ClassifySmallDelta(delta);

        log::InfoF(
            "nis_set_play_time manager=0x%p scene_time=%.6f applied_scene_time=%.6f delta=%.6f sync=%d state=%d active_instance=0x%p bucket=%s scene_fix=%d outer_dt=%.6f",
            nis_manager,
            scene_time,
            applied_scene_time,
            delta,
            sync_scene_time ? 1 : 0,
            state,
            reinterpret_cast<void*>(active_instance),
            bucket,
            scene_time_fixed ? 1 : 0,
            outer_dt);
    }

    MaybeWriteSummary();
    g_nis_set_play_time_original(nis_manager, applied_scene_time, sync_scene_time);
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

    g_nis_play_original(nis_manager);

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
    if (previous_depth == 0) {
        g_nis_owner_scope_calls = 0;
        g_nis_owner_scope_advanced_calls = 0;
        g_nis_owner_scope_zero_delta_calls = 0;
    }

    g_nis_owner_original(nis_manager, delta_seconds);

    const unsigned int inner_calls = g_nis_owner_scope_calls;
    const unsigned int advanced_calls = g_nis_owner_scope_advanced_calls;
    const unsigned int zero_delta_calls = g_nis_owner_scope_zero_delta_calls;
    if (g_nis_owner_scope_depth != 0) {
        --g_nis_owner_scope_depth;
    }
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

    if (ShouldLogVerbose(g_nis_owner_verbose) || zero_delta_calls != 0 || inner_calls == 0 ||
        inner_calls > 1 || delta_seconds >= 0.030f) {
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

void DetourFrameFlow(float delta_seconds, void* callback) {
    PollAcesDebugKeys();
    MaybeApplyRumbleOverride();
#if !defined(SPATCH_FINAL_RELEASE)
    MaybeRunCutContentProbe();
#endif

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
    if (from_cutscene_scope) {
        g_frame_flow_from_cutscene_owner_count.fetch_add(1);
    }

    float forwarded_dt = delta_seconds;
    bool applied_cutscene_zero_fix = false;
    if (from_cutscene_scope && g_config.fix_cutscene_zero_dt &&
        delta_seconds >= 0.0f && delta_seconds < 0.0005f &&
        g_cutscene_flow_input_dt >= 0.015f && g_cutscene_flow_input_dt <= 0.019f) {
        forwarded_dt = g_cutscene_flow_input_dt;
        applied_cutscene_zero_fix = true;
        g_cutscene_zero_dt_override_count.fetch_add(1);
    }

    if (from_cutscene_scope) {
        g_cutscene_flow_forwarded = true;
        g_cutscene_flow_forwarded_dt = forwarded_dt;
        g_cutscene_flow_forwarded_callback = callback;
    }

    if (((from_cutscene_scope && applied_cutscene_zero_fix) ||
         (delta_seconds >= 0.0f && delta_seconds < 0.0005f)) &&
        ShouldLogVerbose(g_frame_flow_verbose)) {
        log::InfoF(
            "frame_flow dt=%.6f applied_dt=%.6f bucket=%s callback=0x%p callback_rva=0x%llX "
            "from_cutscene=%d cutscene_fix=%d cutscene_input_dt=%.6f",
            delta_seconds,
            forwarded_dt,
            bucket,
            callback,
            static_cast<unsigned long long>(ToRva(reinterpret_cast<std::uintptr_t>(callback))),
            from_cutscene_scope ? 1 : 0,
            applied_cutscene_zero_fix ? 1 : 0,
            g_cutscene_flow_input_dt);
    }

    g_frame_flow_original(forwarded_dt, callback);
    MaybeWriteSummary();
}

void DetourCutsceneFlowOwner(void* owner, float delta_seconds) {
    g_cutscene_flow_owner_count.fetch_add(1);
    g_last_cutscene_flow_input_dt_bits.store(FloatToBits(delta_seconds));

    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_cutscene_flow_owner_dt_zero_count.fetch_add(1);
    }

    const auto owner_ptr = reinterpret_cast<std::uintptr_t>(owner);
    const int stage_before = ReadCutsceneFlowStage(owner_ptr);
    const unsigned int flags_before = ReadCutsceneFlowFlags(owner_ptr);

    const unsigned int depth_before = g_cutscene_flow_owner_scope_depth++;
    if (depth_before == 0) {
        g_cutscene_flow_forwarded = false;
        g_cutscene_flow_forwarded_dt = 0.0f;
        g_cutscene_flow_forwarded_callback = nullptr;
        g_cutscene_flow_input_dt = delta_seconds;
    }

    g_cutscene_flow_owner_original(owner, delta_seconds);

    const bool outermost = depth_before == 0;
    const bool forwarded = g_cutscene_flow_forwarded;
    const float forwarded_dt = g_cutscene_flow_forwarded_dt;
    void* forwarded_callback = g_cutscene_flow_forwarded_callback;
    if (g_cutscene_flow_owner_scope_depth != 0) {
        --g_cutscene_flow_owner_scope_depth;
    }
    if (g_cutscene_flow_owner_scope_depth == 0) {
        g_cutscene_flow_forwarded = false;
        g_cutscene_flow_forwarded_dt = 0.0f;
        g_cutscene_flow_forwarded_callback = nullptr;
        g_cutscene_flow_input_dt = 0.0f;
    }

    const int stage_after = ReadCutsceneFlowStage(owner_ptr);
    const unsigned int flags_after = ReadCutsceneFlowFlags(owner_ptr);

    if (outermost && forwarded) {
        g_last_cutscene_flow_forwarded_dt_bits.store(FloatToBits(forwarded_dt));
        if (forwarded_dt >= 0.0f && forwarded_dt < 0.0005f) {
            g_cutscene_flow_owner_forwarded_zero_count.fetch_add(1);
        } else if (forwarded_dt >= 0.015f && forwarded_dt <= 0.019f) {
            g_cutscene_flow_owner_forwarded_60hz_count.fetch_add(1);
        } else {
            g_cutscene_flow_owner_forwarded_other_count.fetch_add(1);
        }
    }

    const bool verbose_cutscene_flow = ShouldLogVerbose(g_cutscene_flow_owner_verbose);
    const bool zero_forwarded = outermost && forwarded &&
                                (forwarded_dt >= 0.0f && forwarded_dt < 0.0005f);
    if (verbose_cutscene_flow || delta_seconds < 0.0005f ||
        (zero_forwarded && verbose_cutscene_flow) || flags_before != flags_after) {
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
    g_fog_slicing_mode_count.fetch_add(1);

    int clamped_interval = update_interval;
    if (clamped_interval < g_config.min_fog_slicing_interval) {
        clamped_interval = g_config.min_fog_slicing_interval;
        g_fog_slicing_clamp_count.fetch_add(1);
        log::WarnF(
            "fog_slicing_clamp manager=0x%p requested=%d applied=%d",
            time_of_day_manager,
            update_interval,
            clamped_interval);
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
    g_last_character_water_collision_tick.store(GetTickCount());

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

    g_character_effects_update_original(character_effects_component, delta_seconds);

    const std::uintptr_t component_ptr =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    const std::uintptr_t tracked_wet_component = g_last_character_wet_component.load();
    const bool is_tracked_wet_component =
        component_ptr != 0 && tracked_wet_component != 0 && component_ptr == tracked_wet_component;

    unsigned int active_surface_uid = 0;
    unsigned int active_wet_surface_uid = 0;
    unsigned char is_on_fire = 0;
    unsigned char is_smoldering = 0;
    unsigned char is_attached_to_player = 0;
    float fire_extinguish_time = 0.0f;
    float smolder_extinguish_time = 0.0f;
    float queued_health_damage = 0.0f;
    int wet_gate_counter = 0;
    float timeofday_weather_wetness = 0.0f;
    float timeofday_override_wetness = -1.0f;

    if (character_effects_component != nullptr) {
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x164,
                 active_surface_uid);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x168,
                 active_wet_surface_uid);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x16C, is_on_fire);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x17C,
                 is_smoldering);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x18C,
                 fire_extinguish_time);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x190,
                 smolder_extinguish_time);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x194,
                 queued_health_damage);
        SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x1A0,
                 is_attached_to_player);
    }

    if (g_wet_surface_block_counter != 0) {
        SafeRead(g_wet_surface_block_counter, wet_gate_counter);
    }

    const std::uintptr_t time_of_day_manager = ReadTimeOfDayManagerInstance();
    if (time_of_day_manager != 0) {
        SafeRead(time_of_day_manager + 0x34, timeofday_weather_wetness);
        SafeRead(time_of_day_manager + 0x74, timeofday_override_wetness);
    }

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
        g_last_character_timeofday_weather_bits.store(FloatToBits(timeofday_weather_wetness));
        g_last_character_timeofday_override_bits.store(FloatToBits(timeofday_override_wetness));
    }

    if (is_tracked_wet_component && active_wet_surface_uid != 0) {
        g_character_effects_wet_surface_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose) &&
        (active_surface_uid != 0 || active_wet_surface_uid != 0 || is_on_fire != 0 ||
         is_smoldering != 0 || wet_gate_counter != 0 ||
         !NearlyEqual(timeofday_weather_wetness, 0.0f) ||
         !NearlyEqual(timeofday_override_wetness, -1.0f))) {
        log::InfoF(
            "character_effects_update component=0x%p dt=%.3f surface=0x%08X wet_surface=0x%08X wet_gate=%d tod_weather=%.3f tod_override=%.3f on_fire=%u smolder=%u attached=%u fire_time=%.3f smolder_time=%.3f queued_damage=%.3f",
            character_effects_component,
            delta_seconds,
            active_surface_uid,
            active_wet_surface_uid,
            wet_gate_counter,
            timeofday_weather_wetness,
            timeofday_override_wetness,
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
    int effect_bone = 0;
    __try {
        sim_object = ReadSimObjectFromComponent(health_component);
        anim_component = ResolveSimObjectComponent(sim_object, 0xC6000003u);
        hitreact_component = ResolveSimObjectComponent(sim_object, 0xA8000001u);
        effect_bone = ReadHitRecordEffectBone(hit_record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sim_object = 0;
        anim_component = 0;
        hitreact_component = 0;
        effect_bone = 0;
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
            "character_health_apply component=0x%p damage=%d projectile=%d attacker=0x%p hit=0x%p sim_object=0x%p anim=0x%p hitreact=0x%p effect_bone=%d",
            health_component,
            damage,
            projectile_damage ? 1 : 0,
            attacker,
            hit_record,
            reinterpret_cast<void*>(sim_object),
            reinterpret_cast<void*>(anim_component),
            reinterpret_cast<void*>(hitreact_component),
            effect_bone);
    }

    AppendCharacterTraceLine("enter",
                             health_component,
                             reinterpret_cast<void*>(anim_component),
                             reinterpret_cast<void*>(hitreact_component),
                             damage,
                             projectile_damage,
                             false,
                             0u);

    const auto visual_anim_before = g_character_visual_damage_anim_count.load();
    const auto visual_rig_before = g_character_visual_damage_rig_count.load();

    const bool result = g_health_apply_damage_original(
        health_component, damage, attacker, hit_record, projectile_damage);
    AppendCharacterTraceLine("after_orig",
                             health_component,
                             reinterpret_cast<void*>(anim_component),
                             reinterpret_cast<void*>(hitreact_component),
                             damage,
                             projectile_damage,
                             result,
                             0u);

    const bool stock_visual_fired =
        g_character_visual_damage_anim_count.load() != visual_anim_before ||
        g_character_visual_damage_rig_count.load() != visual_rig_before;

    bool restored = false;
    if (g_config.restore_character_visual_damage && result && !projectile_damage && damage > 0 &&
        anim_component != 0 && !stock_visual_fired) {
        const unsigned int bone_uid =
            (effect_bone > 0 && effect_bone <= 0x1A) ? static_cast<unsigned int>(effect_bone) : 0u;
        const float damage_amount =
            std::clamp(0.15f + (static_cast<float>(damage) * 0.03f), 0.20f, 0.85f);
        const float duration = 12.0f;
        AppendCharacterTraceLine("pre_restore",
                                 health_component,
                                 reinterpret_cast<void*>(anim_component),
                                 reinterpret_cast<void*>(hitreact_component),
                                 damage,
                                 projectile_damage,
                                 result,
                                 bone_uid);

        __try {
            if (g_use_latest_steam_layout) {
                std::uintptr_t damage_rig = 0;
                SafeRead(anim_component + 0xE0, damage_rig);
                g_last_character_damage_rig.store(damage_rig);
                AppendCharacterHitRecordTraceLine(
                    "latest_hitctx", hit_record, reinterpret_cast<void*>(damage_rig));
                if (damage_rig != 0 && g_character_damage_rig_set_visual_damage_original != nullptr) {
                    g_character_damage_rig_set_visual_damage_original(
                        reinterpret_cast<void*>(damage_rig), bone_uid, damage_amount, duration);
                } else {
                    AppendCharacterTraceLine("restore_skip_no_rig",
                                             health_component,
                                             reinterpret_cast<void*>(anim_component),
                                             reinterpret_cast<void*>(hitreact_component),
                                             damage,
                                             projectile_damage,
                                             result,
                                             bone_uid);
                    MaybeWriteSummary();
                    return result;
                }
            } else {
                g_character_animation_set_visual_damage_original(
                    reinterpret_cast<void*>(anim_component), bone_uid, damage_amount, duration);
            }
            g_character_visual_damage_restore_count.fetch_add(1);
            g_last_character_damage_bone.store(bone_uid);
            g_last_character_damage_amount_bits.store(FloatToBits(damage_amount));
            g_last_character_damage_duration_bits.store(FloatToBits(duration));
            g_last_character_damage_restored.store(1u);
            restored = true;
            AppendCharacterTraceLine("post_restore",
                                     health_component,
                                     reinterpret_cast<void*>(anim_component),
                                     reinterpret_cast<void*>(hitreact_component),
                                     damage,
                                     projectile_damage,
                                     result,
                                     bone_uid);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_last_character_damage_restored.store(0u);
            restored = false;
            AppendCharacterTraceLine("restore_except",
                                     health_component,
                                     reinterpret_cast<void*>(anim_component),
                                     reinterpret_cast<void*>(hitreact_component),
                                     damage,
                                     projectile_damage,
                                     result,
                                     bone_uid);
        }

        if (ShouldLogVerbose(g_character_regression_verbose)) {
            log::InfoF(
                "character_visual_damage_restore anim=0x%p bone=0x%08X damage=%.3f duration=%.3f raw_damage=%d",
                reinterpret_cast<void*>(anim_component),
                bone_uid,
                damage_amount,
                duration,
                damage);
        }
    } else {
        g_last_character_damage_restored.store(0u);
    }

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

void DetourCharacterAnimationSetVisualDamage(void* character_animation_component,
                                             std::uint32_t bone_uid,
                                             float damage_amount,
                                             float duration) {
    g_character_visual_damage_anim_count.fetch_add(1);
    g_last_character_damage_anim_component.store(reinterpret_cast<std::uintptr_t>(character_animation_component));
    g_last_character_damage_bone.store(bone_uid);
    g_last_character_damage_amount_bits.store(FloatToBits(damage_amount));
    g_last_character_damage_duration_bits.store(FloatToBits(duration));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_visual_damage_anim component=0x%p bone=0x%08X damage=%.3f duration=%.3f",
                   character_animation_component,
                   bone_uid,
                   damage_amount,
                   duration);
    }

    MaybeWriteSummary();
    g_character_animation_set_visual_damage_original(character_animation_component,
                                                     bone_uid,
                                                     damage_amount,
                                                     duration);
}

void DetourCharacterDamageRigSetVisualDamage(void* damage_rig,
                                             std::uint32_t bone_uid,
                                             float damage_amount,
                                             float duration) {
    g_character_visual_damage_rig_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));
    g_last_character_damage_bone.store(bone_uid);
    g_last_character_damage_amount_bits.store(FloatToBits(damage_amount));
    g_last_character_damage_duration_bits.store(FloatToBits(duration));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_visual_damage_rig rig=0x%p bone=0x%08X damage=%.3f duration=%.3f",
                   damage_rig,
                   bone_uid,
                   damage_amount,
                   duration);
    }

    MaybeWriteSummary();
    g_character_damage_rig_set_visual_damage_original(damage_rig, bone_uid, damage_amount, duration);
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

void DetourAmbientOcclusionStage(char* ao_state, std::uintptr_t render_context, long long* cmd_list) {
    g_ao_stage_count.fetch_add(1);

    const unsigned int ao_feature = ReadAoFeatureFlag(render_context);
    const unsigned int ao_stage_flag = ReadAoStageFlag(render_context);
    const int ssao_setting = ReadAoSsaoSetting(render_context);
    const float ao_opacity = ReadAoOpacity(render_context);

    g_last_ao_feature_flag.store(ao_feature);
    g_last_ao_stage_flag.store(ao_stage_flag);
    g_last_ao_ssao_setting.store(ssao_setting);
    g_last_ao_opacity_bits.store(FloatToBits(ao_opacity));

    if (ao_feature == 0) {
        g_ao_skip_count.fetch_add(1);
    } else if (ao_stage_flag == 0) {
        g_ao_none_path_count.fetch_add(1);
    } else {
        g_ao_main_path_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_ao_stage_verbose) || ao_feature == 0 || ao_stage_flag == 0) {
        log::InfoF(
            "ao_stage state=0x%p render_context=0x%p ao_feature=%u ao_state=%u ssao=%d ao_opacity=%.6f",
            ao_state,
            reinterpret_cast<void*>(render_context),
            ao_feature,
            ao_stage_flag,
            ssao_setting,
            ao_opacity);
    }

    g_ao_stage_original(ao_state, render_context, cmd_list);
    MaybeWriteSummary();
}

void DetourAmbientOcclusionMaterial(std::uintptr_t ao_state,
                                    std::uintptr_t render_context,
                                    long long* cmd_list) {
    g_ao_material_count.fetch_add(1);

    const int ssao_setting = ReadAoSsaoSetting(render_context);
    const std::uintptr_t slot0 = ReadAoMaterialSlot(render_context, 0x300);
    const std::uintptr_t slot1 = ReadAoMaterialSlot(render_context, 0x308);
    const std::uintptr_t slot2 = ReadAoMaterialSlot(render_context, 0x310);
    const std::uintptr_t slot3 = ReadAoMaterialSlot(render_context, 0x318);
    const int selected_slot = SelectAoMaterialSlot(ssao_setting, slot0, slot1, slot2, slot3);
    const std::uintptr_t selected_ptr =
        ResolveAoMaterialBySlot(selected_slot, slot0, slot1, slot2, slot3);

    g_last_ao_ssao_setting.store(ssao_setting);
    g_last_ao_material_slot0.store(slot0);
    g_last_ao_material_slot1.store(slot1);
    g_last_ao_material_slot2.store(slot2);
    g_last_ao_material_slot3.store(slot3);
    g_last_ao_material_selected_slot.store(selected_slot);
    g_last_ao_material_selected.store(selected_ptr);

    if (ShouldLogVerbose(g_ao_material_verbose) || selected_slot < 0) {
        log::InfoF(
            "ao_material ao_state=0x%p render_context=0x%p ssao=%d selected_slot=%d slot0=0x%p slot1=0x%p slot2=0x%p slot3=0x%p selected=0x%p",
            reinterpret_cast<void*>(ao_state),
            reinterpret_cast<void*>(render_context),
            ssao_setting,
            selected_slot,
            reinterpret_cast<void*>(slot0),
            reinterpret_cast<void*>(slot1),
            reinterpret_cast<void*>(slot2),
            reinterpret_cast<void*>(slot3),
            reinterpret_cast<void*>(selected_ptr));
    }

    g_ao_material_original(ao_state, render_context, cmd_list);
    MaybeWriteSummary();
}

void* DetourAmbientOcclusionComputeNode(char* ao_state,
                                        std::uintptr_t render_context,
                                        int pass_index) {
    g_ao_compute_node_count.fetch_add(1);
    g_last_ao_compute_pass.store(pass_index);

    if (pass_index == 0) {
        g_ao_compute_pass0_count.fetch_add(1);
    } else if (pass_index == 1) {
        g_ao_compute_pass1_count.fetch_add(1);
    }

    const int ssao_setting = ReadAoSsaoSetting(render_context);
    const std::uintptr_t result =
        reinterpret_cast<std::uintptr_t>(g_ao_compute_node_original(ao_state, render_context, pass_index));

    if (ShouldLogVerbose(g_ao_compute_verbose)) {
        log::InfoF(
            "ao_compute_node ao_state=0x%p render_context=0x%p pass=%d ssao=%d result=0x%p",
            ao_state,
            reinterpret_cast<void*>(render_context),
            pass_index,
            ssao_setting,
            reinterpret_cast<void*>(result));
    }

    MaybeWriteSummary();
    return reinterpret_cast<void*>(result);
}

void DetourAmbientOcclusionComputeComposite(char* ao_state,
                                            std::uintptr_t render_context,
                                            long long* cmd_list,
                                            int pass_index) {
    g_ao_compute_composite_count.fetch_add(1);
    g_last_ao_compute_composite_pass.store(pass_index);

    if (pass_index == 0) {
        g_ao_compute_composite_pass0_count.fetch_add(1);
    } else if (pass_index == 1) {
        g_ao_compute_composite_pass1_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_ao_compute_verbose)) {
        log::InfoF(
            "ao_compute_composite ao_state=0x%p render_context=0x%p cmd_list=0x%p pass=%d",
            ao_state,
            reinterpret_cast<void*>(render_context),
            cmd_list,
            pass_index);
    }

    g_ao_compute_composite_original(ao_state, render_context, cmd_list, pass_index);
    MaybeWriteSummary();
}

void DetourAntiAliasOwner(std::uintptr_t render_context,
                          std::uintptr_t param_2,
                          std::uintptr_t param_3,
                          std::uintptr_t* param_4,
                          std::uintptr_t* param_5) {
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }

    g_aa_owner_count.fetch_add(1);

    if (g_config.hook_smaa_present && g_config.smaa_disable_stock_aa && smaa::GetEnabled()) {
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
    if (variant_mode != 0 && render_context != 0) {
        const int forced_hair_blur_gate = 1;
        const int forced_branch_gate = variant_mode == 2 ? 1 : 0;
        bool wrote_hair_gate = true;
        bool wrote_branch_gate = true;
        if (hair_blur_gate != forced_hair_blur_gate) {
            wrote_hair_gate = SafeWrite(render_context + 0x148, forced_hair_blur_gate);
        }
        if (branch_gate != forced_branch_gate) {
            wrote_branch_gate = SafeWrite(render_context + 0x178, forced_branch_gate);
        }
        variant_applied = wrote_hair_gate && wrote_branch_gate;
        if (variant_applied) {
            g_aa_variant_apply_count.fetch_add(1);
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
    g_antialias_owner_original(render_context, param_2, param_3, param_4, param_5);
    if (g_antialias_owner_scope_depth != 0) {
        --g_antialias_owner_scope_depth;
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

    if (variant_applied) {
        SafeWrite(render_context + 0x148, hair_blur_gate);
        SafeWrite(render_context + 0x178, branch_gate);
    }

    MaybeWriteSummary();
}

void DetourAntiAliasFxHandoff(std::uintptr_t render_context,
                              std::uintptr_t arg1,
                              std::uintptr_t arg2,
                              std::uintptr_t arg3) {
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

    if (g_config.hook_smaa_present && g_config.smaa_disable_stock_aa && smaa::GetEnabled()) {
        MaybeWriteSummary();
        return;
    }

    g_antialias_fx_handoff_original(render_context, arg1, arg2, arg3);
    MaybeWriteSummary();
}

char DetourAntiAliasAuxStateQuery(void* state) {
    const char original = g_antialias_aux_state_query_original(state);

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

void DetourDisplayCurveBuilder() {
    if (!g_config.hook_frameflow) {
        PollAcesDebugKeys();
    }

    g_aces_display_curve_count.fetch_add(1);

    const unsigned char original_mode = ReadByteValue(g_display_curve_mode, 1);
    g_last_aces_display_curve_mode.store(original_mode);

    bool forced_linear = false;
    if (g_aces_runtime_enabled.load()) {
        forced_linear = SafeWrite(g_display_curve_mode, static_cast<unsigned char>(0));
        if (forced_linear) {
            g_aces_display_curve_linearize_count.fetch_add(1);
        }
    }

    __try {
        g_display_curve_builder_original();
    } __finally {
        if (forced_linear) {
            SafeWrite(g_display_curve_mode, original_mode);
        }
    }

    MaybeWriteSummary();
}

void DetourDxgiAdapterInit() {
    g_dxgi_adapter_init_original();
    ProbeAndMaybeFixAdapterSelection("dxgi_adapter_init");
}

void DetourPresentBuffer(void* render_packet) {
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }

    if (!g_config.hook_frameflow) {
        PollAcesDebugKeys();
    }

    g_aces_presentbuffer_count.fetch_add(1);

    const float original_gamma = ReadFloatValue(g_final_curve_gamma, 1.0f);
    const float original_offset = ComputeStockSharedOffset(original_gamma);
    g_last_aces_presentbuffer_offset_bits.store(FloatToBits(original_offset));

    const bool aces_enabled = g_aces_runtime_enabled.load();
    bool neutralized = false;
    if (aces_enabled) {
        neutralized = SafeWrite(g_final_curve_gamma, 1.0f);
        if (neutralized) {
            g_aces_presentbuffer_neutralize_count.fetch_add(1);
        }
    }

    g_present_buffer_original(render_packet);

    if (neutralized) {
        SafeWrite(g_final_curve_gamma, original_gamma);
    }

    MaybeWriteSummary();
}

void DetourFinalComposition(std::uintptr_t render_context,
                            void* render_packet,
                            std::uintptr_t back_buffer_desc) {
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }

    if (!g_config.hook_frameflow) {
        PollAcesDebugKeys();
    }

    g_aces_final_count.fetch_add(1);

    if (!g_aces_runtime_enabled.load() || g_time_of_day_accessor == nullptr) {
        g_final_composition_original(render_context, render_packet, back_buffer_desc);
        MaybeWriteSummary();
        return;
    }

    const std::uintptr_t time_of_day = g_time_of_day_accessor();
    if (time_of_day == 0) {
        g_final_composition_original(render_context, render_packet, back_buffer_desc);
        MaybeWriteSummary();
        return;
    }

    float exposure0 = 0.0f;
    float exposure8 = 0.0f;
    if (!SafeRead(time_of_day + 0x0, exposure0) || !SafeRead(time_of_day + 0x8, exposure8)) {
        g_final_composition_original(render_context, render_packet, back_buffer_desc);
        MaybeWriteSummary();
        return;
    }

    const float source_exposure = exposure0 >= exposure8 ? exposure0 : exposure8;
    if (!std::isfinite(source_exposure) || source_exposure < 0.0f || source_exposure > 256.0f) {
        g_final_composition_original(render_context, render_packet, back_buffer_desc);
        MaybeWriteSummary();
        return;
    }

    const int strength_percent = g_aces_runtime_strength_percent.load();
    const int exposure_scale_percent = g_aces_runtime_exposure_scale_percent.load();
    const float strength = static_cast<float>(strength_percent) / 100.0f;
    const float exposure_scale = static_cast<float>(exposure_scale_percent) / 100.0f;
    const StockCurveCoefficients stock_curve_coefficients = ReadStockCurveCoefficients();
    const float stock_curve = EvaluateStockFinalCurve(source_exposure, stock_curve_coefficients);
    const float aces_curve = EvaluateAcesFitted(source_exposure * exposure_scale);
    const float target_curve = stock_curve + (aces_curve - stock_curve) * strength;
    const float remapped_exposure =
        InvertStockFinalCurve(target_curve,
                              source_exposure > 0.25f ? source_exposure : 0.25f,
                              stock_curve_coefficients);

    g_last_aces_source_exposure_bits.store(FloatToBits(source_exposure));
    g_last_aces_remapped_exposure_bits.store(FloatToBits(remapped_exposure));
    g_last_aces_stock_curve_bits.store(FloatToBits(stock_curve));
    g_last_aces_target_curve_bits.store(FloatToBits(target_curve));

    if (!std::isfinite(remapped_exposure) || remapped_exposure < 0.0f ||
        remapped_exposure >= 256.0f || strength_percent <= 0) {
        g_final_composition_original(render_context, render_packet, back_buffer_desc);
        MaybeWriteSummary();
        return;
    }

    const bool patch_first = exposure0 >= exposure8;
    const std::uintptr_t target_address = time_of_day + (patch_first ? 0x0 : 0x8);
    const std::uintptr_t mirror_address = time_of_day + (patch_first ? 0x8 : 0x0);
    const float original_gamma = ReadFloatValue(g_final_curve_gamma, 1.0f);
    g_last_aces_presentbuffer_offset_bits.store(FloatToBits(ComputeStockSharedOffset(original_gamma)));

    const float original_target = patch_first ? exposure0 : exposure8;
    const float original_mirror = patch_first ? exposure8 : exposure0;
    const bool mirror_equal = std::fabs(exposure0 - exposure8) < 0.00001f;

    const bool wrote_target = SafeWrite(target_address, remapped_exposure);
    const bool wrote_gamma = SafeWrite(g_final_curve_gamma, 1.0f);
    bool wrote_mirror = false;
    if (wrote_target && mirror_equal) {
        wrote_mirror = SafeWrite(mirror_address, remapped_exposure);
    }

    if (wrote_target) {
        g_aces_apply_count.fetch_add(1);
    }

    __try {
        g_final_composition_original(render_context, render_packet, back_buffer_desc);
    } __finally {
        if (wrote_target) {
            SafeWrite(target_address, original_target);
            if (wrote_mirror) {
                SafeWrite(mirror_address, original_mirror);
            }
        }
        if (wrote_gamma) {
            SafeWrite(g_final_curve_gamma, original_gamma);
        }
    }

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

bool CreateHookDetour(std::uintptr_t address, void* detour, void** original, const char* name) {
    const MH_STATUS create_status =
        MH_CreateHook(reinterpret_cast<void*>(address), detour, original);
    if (create_status != MH_OK) {
        log::ErrorF("MH_CreateHook failed for %s at 0x%p: %d",
                    name,
                    reinterpret_cast<void*>(address),
                    static_cast<int>(create_status));
        return false;
    }

    log::InfoF("hook enabled name=%s target=0x%p", name, reinterpret_cast<void*>(address));
    return true;
}

}  // namespace

bool Initialize(const Config& config,
                const std::filesystem::path& config_path,
                std::string_view build_id) {
    if (g_hooks_initialized.load()) {
        return true;
    }

    g_config = config;
    g_config_path = config_path;
    g_use_latest_steam_layout = (build_id == "latest_steam");
    g_character_trace_budget.store(
        (g_use_latest_steam_layout && config.restore_character_visual_damage) ? 128 : 0);
    g_module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_module_base != 0) {
        const auto* dos_header =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(g_module_base);
        if (dos_header->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                g_module_base + dos_header->e_lfanew);
            if (nt_headers->Signature == IMAGE_NT_SIGNATURE) {
                g_module_end = g_module_base + nt_headers->OptionalHeader.SizeOfImage;
            }
        }
    }
    g_provider_slot = ResolveAddress(kScaleformTimeProviderRva);
    g_render_task_manager = ResolveAddress(kRenderTaskManagerRva);
    g_render_context_instance_slot = ResolveAddress(kRenderContextInstanceRva);
    g_scenery_counter0 = ResolveAddress(kSceneryCounter0Rva);
    g_scenery_counter1 = ResolveAddress(kSceneryCounter1Rva);
    g_scenery_counter2 = ResolveAddress(kSceneryCounter2Rva);
    g_scenery_counter3 = ResolveAddress(kSceneryCounter3Rva);
    g_final_curve_gamma = ResolveAddress(kFinalCurveGammaRva);
    g_final_curve_windowed_scale = ResolveAddress(kFinalCurveWindowedScaleRva);
    g_final_curve_offset_scale = ResolveAddress(kFinalCurveOffsetScaleRva);
    g_final_curve_cutoff = ResolveAddress(kFinalCurveCutoffRva);
    g_final_curve_coeff_a = ResolveAddress(kFinalCurveCoeffARva);
    g_final_curve_coeff_b = ResolveAddress(kFinalCurveCoeffBRva);
    g_final_curve_coeff_c = ResolveAddress(kFinalCurveCoeffCRva);
    g_final_curve_coeff_d = ResolveAddress(kFinalCurveCoeffDRva);
    g_display_curve_mode = ResolveAddress(kDisplayCurveModeRva);
    g_display_curve_dirty = ResolveAddress(kDisplayCurveDirtyRva);
    g_dxgi_factory_slot = ResolveAddress(kDxgiFactorySlotRva);
    g_dxgi_adapter_slot = ResolveAddress(kDxgiAdapterSlotRva);
    g_d3d_device_slot = ResolveAddress(kD3DDeviceSlotRva);
    g_d3d_context_slot = ResolveAddress(kD3DContextSlotRva);
    g_dxgi_swapchain_slot = ResolveAddress(kDxgiSwapChainSlotRva);
    g_present_rtv_slot = ResolveAddress(kPresentRtvSlotRva);
    g_rumble_apply_object = ResolveAddress(kRumbleApplyObjectRva);
#if !defined(SPATCH_FINAL_RELEASE)
    g_progression_tracker_instance_slot = ResolveAddress(kProgressionTrackerInstanceRva);
#endif
    g_wet_surface_block_counter = ResolveAddress(kWetSurfaceBlockCounterRva);
    g_rumble_apply_helper =
        reinterpret_cast<RumbleApplyHelperFn>(ResolveAddress(kRumbleApplyHelperRva));
#if !defined(SPATCH_FINAL_RELEASE)
    g_progression_find =
        reinterpret_cast<ProgressionFindFn>(ResolveAddress(kProgressionFindRva));
    g_progression_force_slice_change =
        reinterpret_cast<ProgressionForceSliceChangeFn>(ResolveAddress(kProgressionForceSliceChangeRva));
#endif
    g_simobject_get_component =
        reinterpret_cast<SimObjectGetComponentFn>(ResolveAddress(kSimObjectGetComponentRva));
    g_time_of_day_accessor =
        reinterpret_cast<TimeOfDayAccessorFn>(ResolveAddress(kTimeOfDayAccessorRva));
    g_aces_runtime_enabled.store(config.aces_enable);
    g_rumble_override_attempted = false;
#if !defined(SPATCH_FINAL_RELEASE)
    g_cut_content_probe_attempted = false;
    g_cut_content_force_pending = false;
    g_cut_content_force_applied = false;
    g_cut_content_force_tracker = nullptr;
    g_cut_content_force_slice = nullptr;
    g_cut_content_force_candidate_index = -1;
#endif
    g_last_rumble_override_value.store(-1);
    g_aa_runtime_variant_mode.store(ClampAaVariantMode(config.aa_variant_mode));
    g_aa_runtime_aux_mode.store(ClampAaAuxMode(config.aa_aux_mode));
    g_aces_runtime_strength_percent.store(config.aces_strength_percent);
    g_aces_runtime_exposure_scale_percent.store(config.aces_exposure_scale_percent);
    smaa::Initialize(g_module_base,
                     g_d3d_device_slot,
                     g_d3d_context_slot,
                     g_dxgi_swapchain_slot,
                     g_present_rtv_slot);
    smaa::SetEnabled(config.smaa_enable);
    smaa::SetDebugKeysEnabled(config.smaa_debug_keys);
    smaa::SetPreset(config.smaa_preset);
    if (config.hook_aces_display_curve && config.aces_enable) {
        MarkDisplayCurveDirty();
    }
    StoreSceneryCounters(ReadSceneryCounters());
    g_last_summary_tick.store(GetTickCount());

    bool ok = true;

    if (config.hook_aces_final && g_time_of_day_accessor == nullptr) {
        log::Error("time_of_day_accessor resolution failed");
        ok = false;
    }
    if (config.hook_aces_display_curve && g_display_curve_mode == 0) {
        log::Error("display_curve_mode resolution failed");
        ok = false;
    }
    if (config.hook_vram_adapter_fix && (g_dxgi_factory_slot == 0 || g_dxgi_adapter_slot == 0)) {
        log::Error("vram adapter slot resolution failed");
        ok = false;
    }
    if (config.override_rumble_enabled >= 0 &&
        (g_rumble_apply_helper == nullptr || g_rumble_apply_object == 0)) {
        log::Error("rumble override resolution failed");
        ok = false;
    }
#if !defined(SPATCH_FINAL_RELEASE)
    if ((config.probe_cut_content_slices || config.force_cut_content_candidate >= 0) &&
        (g_progression_tracker_instance_slot == 0 || g_progression_find == nullptr)) {
        log::Error("cut-content progression resolution failed");
        ok = false;
    }
    if (config.force_cut_content_candidate >= 0 && g_progression_force_slice_change == nullptr) {
        log::Error("cut-content force helper resolution failed");
        ok = false;
    }
#endif
    const bool need_character_regression_probe = config.hook_character_regression_probe;
    const bool need_visual_damage_restore = config.restore_character_visual_damage;
    const bool need_character_damage_hooks =
        need_character_regression_probe || need_visual_damage_restore;

    if (need_character_damage_hooks && g_simobject_get_component == nullptr) {
        log::Error("simobject_get_component resolution failed");
        ok = false;
    }
    if (config.hook_smaa_present &&
        (g_d3d_device_slot == 0 || g_d3d_context_slot == 0 || g_dxgi_swapchain_slot == 0 ||
         g_present_rtv_slot == 0)) {
        log::Error("smaa slot resolution failed");
        ok = false;
    }
    if (config.hook_queue_ready) {
    ok &= CreateHookDetour(ResolveAddress(kTaskReadyRva),
                                  reinterpret_cast<void*>(&DetourTaskReady),
                                  reinterpret_cast<void**>(&g_task_ready_original),
                                  "task_ready");
    }

    if (config.hook_task_dispatch) {
    ok &= CreateHookDetour(ResolveAddress(kTaskDispatchRva),
                                  reinterpret_cast<void*>(&DetourTaskDispatch),
                                  reinterpret_cast<void**>(&g_task_dispatch_original),
                                  "task_dispatch");
    }

    if (config.hook_wait_helper) {
    ok &= CreateHookDetour(ResolveAddress(kWaitHelperRva),
                                  reinterpret_cast<void*>(&DetourWaitHelper),
                                  reinterpret_cast<void**>(&g_wait_helper_original),
                                  "wait_helper");
    }

    if (config.hook_scaleform_time) {
    ok &= CreateHookDetour(ResolveAddress(kScaleformTimeRva),
                                  reinterpret_cast<void*>(&DetourScaleformTime),
                                  reinterpret_cast<void**>(&g_scaleform_time_original),
                                  "scaleform_time");
    }

    if (config.hook_scaleform_init) {
    ok &= CreateHookDetour(ResolveAddress(kScaleformInitRva),
                                  reinterpret_cast<void*>(&DetourScaleformInit),
                                  reinterpret_cast<void**>(&g_scaleform_init_original),
                                  "scaleform_init");
    }

    if (config.hook_nis_timing) {
    ok &= CreateHookDetour(ResolveAddress(kNisSetPlayTimeRva),
                                  reinterpret_cast<void*>(&DetourNisSetPlayTime),
                                  reinterpret_cast<void**>(&g_nis_set_play_time_original),
                                  "nis_set_play_time");
    }

    if (config.hook_nis_runtime) {
    ok &= CreateHookDetour(ResolveAddress(kNisPlayRva),
                                  reinterpret_cast<void*>(&DetourNisPlay),
                                  reinterpret_cast<void**>(&g_nis_play_original),
                                  "nis_play");
    ok &= CreateHookDetour(ResolveAddress(kNisBootstrapRva),
                                  reinterpret_cast<void*>(&DetourNisBootstrap),
                                  reinterpret_cast<void**>(&g_nis_bootstrap_original),
                                  "nis_bootstrap");
    }

    if (config.hook_nis_owner) {
    ok &= CreateHookDetour(ResolveAddress(kNisOwnerRva),
                                  reinterpret_cast<void*>(&DetourNisOwner),
                                  reinterpret_cast<void**>(&g_nis_owner_original),
                                  "nis_owner");
    }

    const bool need_frameflow_hook = config.hook_frameflow || config.override_rumble_enabled >= 0;
    if (need_frameflow_hook) {
    ok &= CreateHookDetour(ResolveAddress(kFrameFlowRva),
                                  reinterpret_cast<void*>(&DetourFrameFlow),
                                  reinterpret_cast<void**>(&g_frame_flow_original),
                                  "frame_flow");
    }
    if (config.hook_frameflow) {
    ok &= CreateHookDetour(ResolveAddress(kCutsceneFlowOwnerRva),
                                  reinterpret_cast<void*>(&DetourCutsceneFlowOwner),
                                  reinterpret_cast<void**>(&g_cutscene_flow_owner_original),
                                  "cutscene_flow_owner");
    }

    if (config.hook_fog_slicing_guard) {
    ok &= CreateHookDetour(ResolveAddress(kFogSlicingModeRva),
                                  reinterpret_cast<void*>(&DetourFogSlicingMode),
                                  reinterpret_cast<void**>(&g_fog_slicing_mode_original),
                                  "fog_slicing_mode");
    }

    if (config.hook_ao_stage) {
    ok &= CreateHookDetour(ResolveAddress(kAmbientOcclusionStageRva),
                                  reinterpret_cast<void*>(&DetourAmbientOcclusionStage),
                                  reinterpret_cast<void**>(&g_ao_stage_original),
                                  "ao_stage");
    }

    if (config.hook_ao_material) {
    ok &= CreateHookDetour(ResolveAddress(kAmbientOcclusionMaterialRva),
                                  reinterpret_cast<void*>(&DetourAmbientOcclusionMaterial),
                                  reinterpret_cast<void**>(&g_ao_material_original),
                                  "ao_material");
    }

    if (config.hook_ao_compute) {
    ok &= CreateHookDetour(ResolveAddress(kAmbientOcclusionComputeNodeRva),
                                  reinterpret_cast<void*>(&DetourAmbientOcclusionComputeNode),
                                  reinterpret_cast<void**>(&g_ao_compute_node_original),
                                  "ao_compute_node");
    ok &= CreateHookDetour(ResolveAddress(kAmbientOcclusionComputeCompositeRva),
                                  reinterpret_cast<void*>(&DetourAmbientOcclusionComputeComposite),
                                  reinterpret_cast<void**>(&g_ao_compute_composite_original),
                                  "ao_compute_composite");
    }

    if (config.hook_aa_probe) {
    ok &= CreateHookDetour(ResolveAddress(kAntiAliasOwnerRva),
                                  reinterpret_cast<void*>(&DetourAntiAliasOwner),
                                  reinterpret_cast<void**>(&g_antialias_owner_original),
                                  "antialias_owner");
        log::Info("aa_probe enabled=1 target=FUN_140053230");
    }

    if (config.hook_aa_fx_probe) {
    ok &= CreateHookDetour(ResolveAddress(kAntiAliasFxHandoffRva),
                                  reinterpret_cast<void*>(&DetourAntiAliasFxHandoff),
                                  reinterpret_cast<void**>(&g_antialias_fx_handoff_original),
                                  "antialias_fx_handoff");
        log::Info("aa_fx_probe enabled=1 target=FUN_1400554E0");
    }
    if (config.hook_aa_probe || config.hook_aa_fx_probe || config.aa_aux_mode != 0 ||
        config.aa_aux_debug_keys) {
    ok &= CreateHookDetour(ResolveAddress(kAntiAliasAuxStateQueryRva),
                                  reinterpret_cast<void*>(&DetourAntiAliasAuxStateQuery),
                                  reinterpret_cast<void**>(&g_antialias_aux_state_query_original),
                                  "antialias_aux_state_query");
    }
    if (config.hook_aa_probe || config.hook_aa_fx_probe) {
        log::InfoF("aa_variant mode=%d debug_keys=%d key_down=F6 key_up=F7",
                   g_aa_runtime_variant_mode.load(),
                   config.aa_variant_debug_keys ? 1 : 0);
        log::InfoF("aa_aux mode=%d debug_keys=%d key_down=F4 key_up=F5",
                   g_aa_runtime_aux_mode.load(),
                   config.aa_aux_debug_keys ? 1 : 0);
    }
    if (config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
        log::InfoF(
            "smaa enabled=%d debug_keys=%d preset=%d toggle=F2 device_slot=0x%p context_slot=0x%p swapchain_slot=0x%p rtv_slot=0x%p",
            config.smaa_enable ? 1 : 0,
            config.smaa_debug_keys ? 1 : 0,
            config.smaa_preset,
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

    if (config.hook_aces_presentbuffer) {
    ok &= CreateHookDetour(ResolveAddress(kPresentBufferRva),
                                  reinterpret_cast<void*>(&DetourPresentBuffer),
                                  reinterpret_cast<void**>(&g_present_buffer_original),
                                  "present_buffer");
    }

    if (config.hook_aces_display_curve) {
    ok &= CreateHookDetour(ResolveAddress(kDisplayCurveBuilderRva),
                                  reinterpret_cast<void*>(&DetourDisplayCurveBuilder),
                                  reinterpret_cast<void**>(&g_display_curve_builder_original),
                                  "display_curve_builder");
    }

    if (config.hook_vram_adapter_fix) {
    ok &= CreateHookDetour(ResolveAddress(kDxgiAdapterInitRva),
                                  reinterpret_cast<void*>(&DetourDxgiAdapterInit),
                                  reinterpret_cast<void**>(&g_dxgi_adapter_init_original),
                                  "dxgi_adapter_init");
    }

    if (need_character_regression_probe) {
        ok &= CreateHookDetour(ResolveAddress(kCharacterHandleWaterCollisionRva),
                               reinterpret_cast<void*>(&DetourCharacterHandleWaterCollision),
                               reinterpret_cast<void**>(&g_character_handle_water_collision_original),
                               "character_handle_water_collision");
        ok &= CreateHookDetour(ResolveAddress(kCharacterEffectsUpdateRva),
                               reinterpret_cast<void*>(&DetourCharacterEffectsUpdate),
                               reinterpret_cast<void**>(&g_character_effects_update_original),
                               "character_effects_update");
        ok &= CreateHookDetour(ResolveAddress(kHealthApplyDamageRva),
                               reinterpret_cast<void*>(&DetourHealthApplyDamage),
                               reinterpret_cast<void**>(&g_health_apply_damage_original),
                               "health_apply_damage");
        ok &= CreateHookDetour(ResolveAddress(kCharacterPaintOwnerRva),
                               reinterpret_cast<void*>(&DetourCharacterPaintOwner),
                               reinterpret_cast<void**>(&g_character_paint_owner_original),
                               "character_paint_owner");
        ok &= CreateHookDetour(ResolveAddress(kCharacterPaintConsumerRva),
                               reinterpret_cast<void*>(&DetourCharacterPaintConsumer),
                               reinterpret_cast<void**>(&g_character_paint_consumer_original),
                               "character_paint_consumer");
        ok &= CreateHookDetour(ResolveAddress(kCharacterEffectDispatchOwnerRva),
                               reinterpret_cast<void*>(&DetourCharacterEffectDispatchOwner),
                               reinterpret_cast<void**>(&g_character_effect_dispatch_owner_original),
                               "character_effect_dispatch_owner");
        ok &= CreateHookDetour(ResolveAddress(kCharacterEffectDispatchConsumeRva),
                               reinterpret_cast<void*>(&DetourCharacterEffectDispatchConsume),
                               reinterpret_cast<void**>(&g_character_effect_dispatch_consume_original),
                               "character_effect_dispatch_consume");
        ok &= CreateHookDetour(ResolveAddress(kCharacterEffectQueueBuilderRva),
                               reinterpret_cast<void*>(&DetourCharacterEffectQueueBuilder),
                               reinterpret_cast<void**>(&g_character_effect_queue_builder_original),
                               "character_effect_queue_builder");
        ok &= CreateHookDetour(ResolveAddress(kCharacterAnimationApplyCharredEffectRva),
                               reinterpret_cast<void*>(&DetourCharacterAnimationApplyCharredEffect),
                               reinterpret_cast<void**>(&g_character_animation_apply_charred_effect_original),
                               "character_animation_apply_charred_effect");
        ok &= CreateHookDetour(ResolveAddress(kCharacterAnimationApplyPaintEffectRva),
                               reinterpret_cast<void*>(&DetourCharacterAnimationApplyPaintEffect),
                               reinterpret_cast<void**>(&g_character_animation_apply_paint_effect_original),
                               "character_animation_apply_paint_effect");
        ok &= CreateHookDetour(ResolveAddress(kCharacterAnimationCreateDamageRigRva),
                               reinterpret_cast<void*>(&DetourCharacterAnimationCreateDamageRig),
                               reinterpret_cast<void**>(&g_character_animation_create_damage_rig_original),
                               "character_animation_create_damage_rig");
        ok &= CreateHookDetour(ResolveAddress(kDamageRigApplyCharredEffectRva),
                               reinterpret_cast<void*>(&DetourDamageRigApplyCharredEffect),
                               reinterpret_cast<void**>(&g_damage_rig_apply_charred_effect_original),
                               "damage_rig_apply_charred_effect");
        ok &= CreateHookDetour(ResolveAddress(kDamageRigApplyPaintEffectRva),
                               reinterpret_cast<void*>(&DetourDamageRigApplyPaintEffect),
                               reinterpret_cast<void**>(&g_damage_rig_apply_paint_effect_original),
                               "damage_rig_apply_paint_effect");
        ok &= CreateHookDetour(ResolveAddress(kCharacterAnimationSetVisualDamageRva),
                               reinterpret_cast<void*>(&DetourCharacterAnimationSetVisualDamage),
                               reinterpret_cast<void**>(&g_character_animation_set_visual_damage_original),
                               "character_animation_set_visual_damage");
        ok &= CreateHookDetour(ResolveAddress(kCharacterDamageRigSetVisualDamageRva),
                               reinterpret_cast<void*>(&DetourCharacterDamageRigSetVisualDamage),
                               reinterpret_cast<void**>(&g_character_damage_rig_set_visual_damage_original),
                               "character_damage_rig_set_visual_damage");
        ok &= CreateHookDetour(ResolveAddress(kCharacterDamageRigResetDamageRva),
                               reinterpret_cast<void*>(&DetourCharacterDamageRigResetDamage),
                               reinterpret_cast<void**>(&g_character_damage_rig_reset_damage_original),
                               "character_damage_rig_reset_damage");
        log::InfoF(
            "character_regression_probe enabled=1 wetness=HandleWaterCollision/CharacterEffects::Update/DispatchOwner/DispatchConsume/CharacterPaintConsumer material=ApplyCharredEffect/ApplyPaintEffect damage=ApplyHealthDamage/CreateDamageRig/SetVisualDamage/ResetDamage restore_visual_damage=%d",
            config.restore_character_visual_damage ? 1 : 0);
    } else if (need_visual_damage_restore) {
        ok &= CreateHookDetour(ResolveAddress(kHealthApplyDamageRva),
                               reinterpret_cast<void*>(&DetourHealthApplyDamage),
                               reinterpret_cast<void**>(&g_health_apply_damage_original),
                               "health_apply_damage");
        if (g_use_latest_steam_layout) {
            g_character_animation_set_visual_damage_original =
                reinterpret_cast<CharacterAnimationSetVisualDamageFn>(
                    ResolveAddress(kCharacterAnimationSetVisualDamageRva));
            g_character_damage_rig_set_visual_damage_original =
                reinterpret_cast<CharacterDamageRigSetVisualDamageFn>(
                    ResolveAddress(kCharacterDamageRigSetVisualDamageRva));
            g_character_damage_rig_reset_damage_original =
                reinterpret_cast<CharacterDamageRigResetDamageFn>(
                    ResolveAddress(kCharacterDamageRigResetDamageRva));
            if (g_character_animation_set_visual_damage_original == nullptr) {
                log::Error("latest_steam character_animation_set_visual_damage resolution failed");
                ok = false;
            }
            log::Info("character_visual_damage_restore enabled=1 mode=latest_steam_slim");
        } else {
            ok &= CreateHookDetour(ResolveAddress(kCharacterAnimationSetVisualDamageRva),
                                   reinterpret_cast<void*>(&DetourCharacterAnimationSetVisualDamage),
                                   reinterpret_cast<void**>(&g_character_animation_set_visual_damage_original),
                                   "character_animation_set_visual_damage");
            ok &= CreateHookDetour(ResolveAddress(kCharacterDamageRigSetVisualDamageRva),
                                   reinterpret_cast<void*>(&DetourCharacterDamageRigSetVisualDamage),
                                   reinterpret_cast<void**>(&g_character_damage_rig_set_visual_damage_original),
                                   "character_damage_rig_set_visual_damage");
            ok &= CreateHookDetour(ResolveAddress(kCharacterDamageRigResetDamageRva),
                                   reinterpret_cast<void*>(&DetourCharacterDamageRigResetDamage),
                                   reinterpret_cast<void**>(&g_character_damage_rig_reset_damage_original),
                                   "character_damage_rig_reset_damage");
            log::Info("character_visual_damage_restore enabled=1 mode=full_hooks");
        }
    }

    if (config.hook_aces_final) {
    ok &= CreateHookDetour(ResolveAddress(kFinalCompositionRva),
                                  reinterpret_cast<void*>(&DetourFinalComposition),
                                  reinterpret_cast<void**>(&g_final_composition_original),
                                  "final_composition");
        log::InfoF("aces_keys toggle=P strength_down=F9 strength_up=F11 scale_down=F8 scale_up=F12 display_curve=%d enabled=%d strength_percent=%d exposure_scale_percent=%d",
                   config.hook_aces_display_curve ? 1 : 0,
                   config.aces_enable ? 1 : 0,
                   config.aces_strength_percent,
                   config.aces_exposure_scale_percent);
    }

    if (config.hook_vram_adapter_fix) {
        log::InfoF("vram_adapter_fix enabled=1 adapter_slot=0x%p factory_slot=0x%p override_mb=%d",
                   reinterpret_cast<void*>(g_dxgi_adapter_slot),
                   reinterpret_cast<void*>(g_dxgi_factory_slot),
                   config.vram_override_mb);
    }
    if (config.override_rumble_enabled >= 0) {
        log::InfoF("rumble_override configured=%d object=0x%p helper=0x%p",
                   config.override_rumble_enabled,
                   reinterpret_cast<void*>(g_rumble_apply_object),
                   reinterpret_cast<void*>(g_rumble_apply_helper));
    }
#if !defined(SPATCH_FINAL_RELEASE)
    if (config.probe_cut_content_slices || config.force_cut_content_candidate >= 0) {
        log::InfoF(
            "cut_content_probe configured=%d force_candidate=%d simulate_rewards=%d tracker_slot=0x%p find=0x%p force=0x%p",
            config.probe_cut_content_slices ? 1 : 0,
            config.force_cut_content_candidate,
            config.force_cut_content_simulate_rewards ? 1 : 0,
            reinterpret_cast<void*>(g_progression_tracker_instance_slot),
            reinterpret_cast<void*>(g_progression_find),
            reinterpret_cast<void*>(g_progression_force_slice_change));
    }
#endif

    if (config.hook_scenery_builders) {
    ok &= CreateHookDetour(ResolveAddress(kSceneryPrepareRva),
                                  reinterpret_cast<void*>(&DetourSceneryPrepare),
                                  reinterpret_cast<void**>(&g_scenery_prepare_original),
                                  "scenery_prepare");
    ok &= CreateHookDetour(ResolveAddress(kScenerySetupRva),
                                  reinterpret_cast<void*>(&DetourScenerySetup),
                                  reinterpret_cast<void**>(&g_scenery_setup_original),
                                  "scenery_setup");
    ok &= CreateHookDetour(ResolveAddress(kRenderSceneryBuilderRva),
                                  reinterpret_cast<void*>(&DetourRenderSceneryBuilder),
                                  reinterpret_cast<void**>(&g_render_scenery_builder_original),
                                  "render_scenery_builder");
    ok &= CreateHookDetour(ResolveAddress(kRasterizeBucketBuilderRva),
                                  reinterpret_cast<void*>(&DetourRasterizeBucketBuilder),
                                  reinterpret_cast<void**>(&g_rasterize_bucket_builder_original),
                                  "rasterize_bucket_builder");
    }

    log::InfoF("hook_layout selected=%s", g_use_latest_steam_layout ? "latest_steam" : "legacy_researched");
    g_hooks_initialized.store(ok);
    return ok;
}

void Shutdown() {
    if (!g_hooks_initialized.load()) {
        return;
    }

    if (g_config.hook_queue_ready) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kTaskReadyRva)));
    }
    if (g_config.hook_task_dispatch) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kTaskDispatchRva)));
    }
    if (g_config.hook_wait_helper) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kWaitHelperRva)));
    }
    if (g_config.hook_scaleform_time) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kScaleformTimeRva)));
    }
    if (g_config.hook_scaleform_init) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kScaleformInitRva)));
    }
    if (g_config.hook_nis_timing) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kNisSetPlayTimeRva)));
    }
    if (g_config.hook_nis_runtime) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kNisPlayRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kNisBootstrapRva)));
    }
    if (g_config.hook_nis_owner) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kNisOwnerRva)));
    }
    if (g_config.hook_frameflow || g_config.override_rumble_enabled >= 0) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kFrameFlowRva)));
    }
    if (g_config.hook_frameflow) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCutsceneFlowOwnerRva)));
    }
    if (g_config.hook_fog_slicing_guard) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kFogSlicingModeRva)));
    }
    if (g_config.hook_ao_stage) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAmbientOcclusionStageRva)));
    }
    if (g_config.hook_ao_material) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAmbientOcclusionMaterialRva)));
    }
    if (g_config.hook_ao_compute) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAmbientOcclusionComputeNodeRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAmbientOcclusionComputeCompositeRva)));
    }
    if (g_config.hook_aa_probe) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAntiAliasOwnerRva)));
    }
    if (g_config.hook_aa_fx_probe) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAntiAliasFxHandoffRva)));
    }
    if (g_config.hook_aa_probe || g_config.hook_aa_fx_probe || g_config.aa_aux_mode != 0 ||
        g_config.aa_aux_debug_keys) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kAntiAliasAuxStateQueryRva)));
    }
    if (g_config.hook_post_material_submit) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kRenderSubmitMaterialRva)));
    }
    if (g_config.hook_aces_presentbuffer) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kPresentBufferRva)));
    }
    if (g_config.hook_aces_display_curve) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kDisplayCurveBuilderRva)));
    }
    if (g_config.hook_vram_adapter_fix) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kDxgiAdapterInitRva)));
    }
    if (g_config.hook_character_regression_probe) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterHandleWaterCollisionRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterEffectsUpdateRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kHealthApplyDamageRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterPaintOwnerRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterPaintConsumerRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterEffectDispatchOwnerRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterEffectDispatchConsumeRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterEffectQueueBuilderRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterAnimationApplyCharredEffectRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterAnimationApplyPaintEffectRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterAnimationCreateDamageRigRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kDamageRigApplyCharredEffectRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kDamageRigApplyPaintEffectRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterAnimationSetVisualDamageRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterDamageRigSetVisualDamageRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterDamageRigResetDamageRva)));
    } else if (g_config.restore_character_visual_damage) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kHealthApplyDamageRva)));
        if (!g_use_latest_steam_layout) {
            MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterAnimationSetVisualDamageRva)));
            MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterDamageRigSetVisualDamageRva)));
            MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kCharacterDamageRigResetDamageRva)));
        }
    }
    if (g_config.hook_aces_final) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kFinalCompositionRva)));
    }
    if (g_config.hook_scenery_builders) {
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kSceneryPrepareRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kScenerySetupRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kRenderSceneryBuilderRva)));
        MH_RemoveHook(reinterpret_cast<void*>(ResolveAddress(kRasterizeBucketBuilderRva)));
    }
    smaa::Shutdown();
    MH_Uninitialize();
    g_hooks_initialized.store(false);
}

bool GetAcesEnabled() {
    return g_aces_runtime_enabled.load();
}

void SetAcesEnabled(bool enabled) {
    const bool previous = g_aces_runtime_enabled.exchange(enabled);
    if (previous == enabled) {
        return;
    }

    g_aces_toggle_count.fetch_add(1);
    MarkDisplayCurveDirty();
    PersistAcesConfig();
    log::InfoF("aces_toggle enabled=%d strength_percent=%d exposure_scale_percent=%d",
               enabled ? 1 : 0,
               g_aces_runtime_strength_percent.load(),
               g_aces_runtime_exposure_scale_percent.load());
}

int GetAcesStrengthPercent() {
    return g_aces_runtime_strength_percent.load();
}

void SetAcesStrengthPercent(int strength_percent) {
    if (strength_percent < 0) {
        strength_percent = 0;
    } else if (strength_percent > 100) {
        strength_percent = 100;
    }

    const int previous = g_aces_runtime_strength_percent.exchange(strength_percent);
    if (previous == strength_percent) {
        return;
    }

    g_aces_adjust_count.fetch_add(1);
    PersistAcesConfig();
    log::InfoF("aces_strength strength_percent=%d", strength_percent);
}

int GetAcesExposureScalePercent() {
    return g_aces_runtime_exposure_scale_percent.load();
}

void SetAcesExposureScalePercent(int exposure_scale_percent) {
    if (exposure_scale_percent < 25) {
        exposure_scale_percent = 25;
    } else if (exposure_scale_percent > 400) {
        exposure_scale_percent = 400;
    }

    const int previous = g_aces_runtime_exposure_scale_percent.exchange(exposure_scale_percent);
    if (previous == exposure_scale_percent) {
        return;
    }

    g_aces_adjust_count.fetch_add(1);
    PersistAcesConfig();
    log::InfoF("aces_scale exposure_scale_percent=%d", exposure_scale_percent);
}

}  // namespace spatch::hooks
