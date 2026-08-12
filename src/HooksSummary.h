#pragma once

#if !defined(SPATCH_FINAL_RELEASE)
#include "SmaaRuntime.h"

#include <cstdint>
#include <string>
#endif

namespace spatch::hooks {

#if !defined(SPATCH_FINAL_RELEASE)

struct SummarySnapshot {
    unsigned long long task_ready = 0;
    unsigned long long task_dispatch = 0;
    unsigned long long wait_helper = 0;
    unsigned long long wait_task = 0;
    unsigned long long wait_gt16 = 0;
    unsigned long long wait_gt100 = 0;
    unsigned long long wait_gt1000 = 0;
    unsigned long long wait_gt5000 = 0;
    unsigned long long scaleform_time = 0;
    unsigned long long provider_non_null = 0;
    unsigned long long scaleform_init = 0;

    unsigned long long nis_time = 0;
    unsigned long long nis_sync = 0;
    unsigned long long nis_dt0 = 0;
    unsigned long long nis_dt30 = 0;
    unsigned long long nis_dt60 = 0;
    unsigned long long nis_dt_other = 0;
    unsigned long long nis_scene_fix = 0;
    unsigned long long nis_play = 0;
    unsigned long long nis_play_adv = 0;
    unsigned long long nis_play_repeat = 0;
    unsigned long long nis_play_multi = 0;
    unsigned long long nis_boot = 0;
    unsigned long long nis_boot_s1 = 0;
    unsigned long long nis_boot_s2 = 0;
    unsigned long long nis_boot_fail = 0;
    unsigned long long nis_owner = 0;
    unsigned long long nis_owner_dt0 = 0;
    unsigned long long nis_owner_dt30 = 0;
    unsigned long long nis_owner_dt60 = 0;
    unsigned long long nis_owner_dt_other = 0;
    unsigned long long nis_owner_adv = 0;
    unsigned long long nis_owner_repeat = 0;
    unsigned long long nis_owner_multi = 0;
    unsigned long long nis_actor_setup = 0;
    unsigned long long nis_actor_restore = 0;
    unsigned long long nis_actor_restore_untracked = 0;
    unsigned long long nis_actor_setup_duplicate = 0;
    unsigned long long nis_actor_restore_duplicate = 0;
    unsigned long long nis_actor_restore_never_seen = 0;
    unsigned long long nis_actor_restore_suppressed = 0;
    unsigned long long twitch_tick = 0;
    unsigned long long twitch_login_callback = 0;
    unsigned long long twitch_login_failure = 0;
    float nis_last = 0.0f;
    float nis_last_delta = 0.0f;
    float nis_owner_last_dt = 0.0f;

    unsigned long long frameflow = 0;
    unsigned long long frameflow_dt0 = 0;
    unsigned long long frameflow_dt60 = 0;
    unsigned long long frameflow_dt_other = 0;
    unsigned long long frameflow_from_cutscene = 0;
    float frameflow_last_dt = 0.0f;
    unsigned long long cutscene_flow = 0;
    unsigned long long cutscene_flow_dt0 = 0;
    unsigned long long cutscene_flow_fwd0 = 0;
    unsigned long long cutscene_flow_fwd60 = 0;
    unsigned long long cutscene_flow_fwd_other = 0;
    unsigned long long cutscene_flow_fix = 0;
    unsigned long long cutscene_flow_fix_paused = 0;
    float cutscene_flow_in = 0.0f;
    float cutscene_flow_fwd = 0.0f;

    unsigned long long fog_slicing = 0;
    unsigned long long fog_clamps = 0;

    unsigned long long aa_owner = 0;
    unsigned long long aa_skip = 0;
    unsigned long long aa_main = 0;
    unsigned long long aa_hairblur0 = 0;
    int aa_state = 0;
    int aa_hair_gate = 0;
    int aa_variant = 0;
    unsigned long long aa_variant_apply = 0;
    int aa_aux_mode = 0;
    unsigned long long aa_aux_apply = 0;
    unsigned int aa_shader = 0;
    unsigned int aa_raster = 0;
    unsigned int aa_aux = 0;
    std::uintptr_t aa_material = 0;
    std::uintptr_t aa_target = 0;
    std::uintptr_t aa_src_a = 0;
    std::uintptr_t aa_src_b = 0;
    unsigned long long aa_fx = 0;
    std::uintptr_t aa_fx_arg1 = 0;
    std::uintptr_t aa_fx_arg2 = 0;
    std::uintptr_t aa_fx_arg3 = 0;
    int smaa_enabled = 0;
    int smaa_any_hook_retained = 0;
    int smaa_ready = 0;
    unsigned long long smaa_present = 0;
    unsigned long long smaa_apply = 0;
    unsigned long long smaa_fail = 0;
    unsigned long long smaa_resize = 0;
    unsigned int smaa_width = 0;
    unsigned int smaa_height = 0;
    unsigned long long rumble_override = 0;
    int rumble_value = -1;

    unsigned long long post_submit = 0;
    unsigned long long post_comp_lights = 0;
    unsigned long long post_comp_final = 0;
    unsigned long long post_bloom = 0;
    unsigned long long post_lightshaft = 0;
    unsigned long long post_shadow_collector = 0;
    unsigned long long post_final_chg = 0;
    int post_final_flags = 0;
    std::uintptr_t post_final_cmd = 0;
    std::uintptr_t post_final_params = 0;
    std::uintptr_t post_final_p0 = 0;
    std::uintptr_t post_final_p1 = 0;
    std::uintptr_t post_final_p2 = 0;
    std::uintptr_t post_final_p3 = 0;

    unsigned long long char_water = 0;
    float char_water_speed = 0.0f;
    unsigned long long char_fx_update = 0;
    unsigned long long char_fx_wet_updates = 0;
    unsigned int char_fx_surface = 0;
    unsigned int char_fx_wet_surface = 0;
    int char_fx_gate = 0;
    float char_fx_tod_weather = 0.0f;
    float char_fx_tod_override = 0.0f;
    unsigned int char_fx_onfire = 0;
    unsigned int char_fx_smolder = 0;
    unsigned int char_fx_attached = 0;
    float char_fx_fire_time = 0.0f;
    float char_fx_smolder_time = 0.0f;
    float char_fx_queued_damage = 0.0f;
    unsigned long long char_health_apply = 0;
    unsigned long long char_health_proj = 0;
    unsigned long long char_health_melee = 0;
    unsigned long long char_health_anim_found = 0;
    unsigned long long char_health_hitreact_found = 0;
    int char_health_damage = 0;
    unsigned int char_health_projectile = 0;
    std::uintptr_t char_health_component = 0;
    std::uintptr_t char_health_anim = 0;
    std::uintptr_t char_health_hitreact = 0;
    std::uintptr_t char_health_attacker = 0;
    std::uintptr_t char_health_hit = 0;
    unsigned long long char_wet_force = 0;
    unsigned long long char_wet_force_verify = 0;
    unsigned long long char_charred_anim = 0;
    unsigned long long char_charred_rig = 0;
    float char_charred_amount = 0.0f;
    unsigned long long char_dispatch_owner = 0;
    unsigned long long char_dispatch_consume = 0;
    std::uintptr_t char_dispatch_owner_ptr = 0;
    std::uintptr_t char_dispatch_component = 0;
    unsigned long long char_queue_build = 0;
    unsigned long long char_queue_build_tracked = 0;
    std::uintptr_t char_queue_build_owner = 0;
    std::uintptr_t char_queue_build_component = 0;
    unsigned int char_queue_build_mode = 0;
    unsigned long long char_paint_owner = 0;
    std::uintptr_t char_paint_owner_ptr = 0;
    std::uintptr_t char_paint_owner_component = 0;
    unsigned long long char_paint_consumer = 0;
    unsigned long long char_paint_anim = 0;
    unsigned long long char_paint_rig = 0;
    unsigned int char_paint_enable = 0;
    float char_paint_r = 0.0f;
    float char_paint_g = 0.0f;
    float char_paint_b = 0.0f;
    unsigned long long char_damage_create = 0;
    unsigned long long char_damage_reset = 0;

    std::uintptr_t post_last = 0;
    unsigned long long unique_callbacks = 0;
    unsigned long long scenery_prepare = 0;
    unsigned long long scenery_setup = 0;
    unsigned long long render_scenery = 0;
    unsigned long long rasterize_bucket = 0;
    unsigned long long scenery_prepare_ready = 0;
    unsigned long long scenery_setup_ready = 0;
    unsigned long long render_scenery_ready = 0;
    unsigned long long rasterize_bucket_ready = 0;
    unsigned long long scenery_setup_qdelta = 0;
    unsigned long long render_scenery_qdelta = 0;
    unsigned long long rasterize_bucket_qdelta = 0;
    unsigned int scenery_count0 = 0;
    unsigned int scenery_count1 = 0;
    unsigned int scenery_count2 = 0;
    unsigned int scenery_count3 = 0;
    std::uintptr_t provider_ptr = 0;
};

struct SummaryRuntimeFields {
    SummarySnapshot snapshot{};
    std::uint32_t nis_last_bits = 0;
    std::uint32_t nis_last_delta_bits = 0;
    std::uint32_t nis_owner_last_dt_bits = 0;
    std::uint32_t frameflow_last_dt_bits = 0;
    std::uint32_t cutscene_flow_in_bits = 0;
    std::uint32_t cutscene_flow_fwd_bits = 0;
    std::uint32_t char_water_speed_bits = 0;
    std::uint32_t char_fx_tod_weather_bits = 0;
    std::uint32_t char_fx_tod_override_bits = 0;
    std::uint32_t char_fx_fire_time_bits = 0;
    std::uint32_t char_fx_smolder_time_bits = 0;
    std::uint32_t char_fx_queued_damage_bits = 0;
    std::uint32_t char_charred_amount_bits = 0;
    std::uint32_t char_paint_r_bits = 0;
    std::uint32_t char_paint_g_bits = 0;
    std::uint32_t char_paint_b_bits = 0;
    smaa::Stats smaa_stats{};
    std::uintptr_t provider_ptr = 0;
};

SummarySnapshot BuildSummarySnapshot(const SummaryRuntimeFields& fields);
std::string FormatSummaryMessage(const SummarySnapshot& snapshot);

#endif

}  // namespace spatch::hooks
