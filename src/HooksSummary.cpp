#include "HooksSummary.h"

#if !defined(SPATCH_FINAL_RELEASE)

#include <cstdio>
#include <cstring>
#include <string>

namespace spatch::hooks {
namespace {

constexpr const char* kSummaryFormat =
    "summary task_ready=%llu task_dispatch=%llu wait_helper=%llu wait_task=%llu "
    "wait_gt16=%llu wait_gt100=%llu wait_gt1000=%llu wait_gt5000=%llu "
    "scaleform_time=%llu provider_non_null=%llu scaleform_init=%llu "
    "nis_time=%llu nis_sync=%llu nis_dt0=%llu nis_dt30=%llu nis_dt60=%llu nis_dt_other=%llu nis_scene_fix=%llu "
    "nis_play=%llu nis_play_adv=%llu nis_play_repeat=%llu nis_play_multi=%llu "
    "nis_boot=%llu nis_boot_s1=%llu nis_boot_s2=%llu nis_boot_fail=%llu "
    "nis_owner=%llu nis_owner_dt0=%llu nis_owner_dt30=%llu nis_owner_dt60=%llu nis_owner_dt_other=%llu "
    "nis_owner_adv=%llu nis_owner_repeat=%llu nis_owner_multi=%llu "
    "nis_actor_setup=%llu nis_actor_restore=%llu nis_actor_restore_untracked=%llu "
    "nis_actor_setup_duplicate=%llu nis_actor_restore_duplicate=%llu nis_actor_restore_never_seen=%llu "
    "nis_actor_restore_suppressed=%llu "
    "twitch_tick=%llu twitch_login_callback=%llu twitch_login_failure=%llu "
    "nis_last=%.6f nis_last_delta=%.6f nis_owner_last_dt=%.6f "
    "frameflow=%llu frameflow_dt0=%llu frameflow_dt60=%llu frameflow_dt_other=%llu "
    "frameflow_from_cutscene=%llu frameflow_last_dt=%.6f "
    "cutscene_flow=%llu cutscene_flow_dt0=%llu cutscene_flow_fwd0=%llu "
    "cutscene_flow_fwd60=%llu cutscene_flow_fwd_other=%llu cutscene_flow_fix=%llu cutscene_flow_fix_paused=%llu cutscene_flow_in=%.6f cutscene_flow_fwd=%.6f "
    "fog_slicing=%llu fog_clamps=%llu "
    "aa_owner=%llu aa_skip=%llu aa_main=%llu aa_hairblur0=%llu aa_state=%d aa_hair_gate=%d aa_variant=%d aa_variant_apply=%llu aa_aux_mode=%d aa_aux_apply=%llu aa_shader=0x%08X aa_raster=0x%08X aa_aux=0x%08X aa_material=0x%p aa_target=0x%p aa_src_a=0x%p aa_src_b=0x%p aa_fx=%llu aa_fx_arg1=0x%p aa_fx_arg2=0x%p aa_fx_arg3=0x%p "
    "smaa_enabled=%d smaa_any_hook_retained=%d smaa_ready=%d smaa_present=%llu smaa_apply=%llu smaa_fail=%llu smaa_resize=%llu smaa_size=%ux%u "
    "rumble_override=%llu rumble_value=%d "
    "post_submit=%llu post_comp_lights=%llu post_comp_final=%llu post_bloom=%llu post_lightshaft=%llu post_shadow_collector=%llu "
    "post_final_chg=%llu post_final_flags=%d post_final_cmd=0x%p post_final_params=0x%p "
    "post_final_p0=0x%p post_final_p1=0x%p post_final_p2=0x%p post_final_p3=0x%p "
    "char_water=%llu char_water_speed=%.3f char_fx_update=%llu char_fx_wet_updates=%llu char_fx_surface=0x%08X char_fx_wet_surface=0x%08X char_fx_gate=%d char_fx_tod_weather=%.3f char_fx_tod_override=%.3f char_fx_onfire=%u char_fx_smolder=%u char_fx_attached=%u char_fx_fire_time=%.3f char_fx_smolder_time=%.3f char_fx_queued_damage=%.3f char_health_apply=%llu char_health_proj=%llu char_health_melee=%llu char_health_anim_found=%llu char_health_hitreact_found=%llu char_health_damage=%d char_health_projectile=%u char_health_component=0x%p char_health_anim=0x%p char_health_hitreact=0x%p char_health_attacker=0x%p char_health_hit=0x%p char_wet_force=%llu char_wet_force_verify=%llu char_charred_anim=%llu char_charred_rig=%llu char_charred_amount=%.3f char_dispatch_owner=%llu char_dispatch_consume=%llu char_dispatch_owner_ptr=0x%p char_dispatch_component=0x%p char_queue_build=%llu char_queue_build_tracked=%llu char_queue_build_owner=0x%p char_queue_build_component=0x%p char_queue_build_mode=%u char_paint_owner=%llu char_paint_owner_ptr=0x%p char_paint_owner_component=0x%p char_paint_consumer=%llu char_paint_anim=%llu char_paint_rig=%llu char_paint_enable=%u char_paint_rgb=%.3f/%.3f/%.3f char_damage_create=%llu char_damage_reset=%llu "
    "post_last=0x%p unique_callbacks=%llu "
    "scenery_prepare=%llu scenery_setup=%llu render_scenery=%llu rasterize_bucket=%llu "
    "scenery_prepare_ready=%llu scenery_setup_ready=%llu render_scenery_ready=%llu "
    "rasterize_bucket_ready=%llu "
    "scenery_setup_qdelta=%llu render_scenery_qdelta=%llu rasterize_bucket_qdelta=%llu "
    "scenery_counts=%u/%u/%u/%u "
    "provider_ptr=0x%p";

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

SummarySnapshot BuildSummarySnapshot(const SummaryRuntimeFields& fields) {
    SummarySnapshot snapshot = fields.snapshot;

    snapshot.nis_last = BitsToFloat(fields.nis_last_bits);
    snapshot.nis_last_delta = BitsToFloat(fields.nis_last_delta_bits);
    snapshot.nis_owner_last_dt = BitsToFloat(fields.nis_owner_last_dt_bits);
    snapshot.frameflow_last_dt = BitsToFloat(fields.frameflow_last_dt_bits);
    snapshot.cutscene_flow_in = BitsToFloat(fields.cutscene_flow_in_bits);
    snapshot.cutscene_flow_fwd = BitsToFloat(fields.cutscene_flow_fwd_bits);
    snapshot.char_water_speed = BitsToFloat(fields.char_water_speed_bits);
    snapshot.char_fx_tod_weather = BitsToFloat(fields.char_fx_tod_weather_bits);
    snapshot.char_fx_tod_override = BitsToFloat(fields.char_fx_tod_override_bits);
    snapshot.char_fx_fire_time = BitsToFloat(fields.char_fx_fire_time_bits);
    snapshot.char_fx_smolder_time = BitsToFloat(fields.char_fx_smolder_time_bits);
    snapshot.char_fx_queued_damage = BitsToFloat(fields.char_fx_queued_damage_bits);
    snapshot.char_charred_amount = BitsToFloat(fields.char_charred_amount_bits);
    snapshot.char_paint_r = BitsToFloat(fields.char_paint_r_bits);
    snapshot.char_paint_g = BitsToFloat(fields.char_paint_g_bits);
    snapshot.char_paint_b = BitsToFloat(fields.char_paint_b_bits);
    snapshot.smaa_enabled = fields.smaa_stats.enabled ? 1 : 0;
    snapshot.smaa_any_hook_retained =
        fields.smaa_stats.any_hook_retained ? 1 : 0;
    snapshot.smaa_ready = fields.smaa_stats.resources_ready ? 1 : 0;
    snapshot.smaa_present = fields.smaa_stats.present_count;
    snapshot.smaa_apply = fields.smaa_stats.apply_count;
    snapshot.smaa_fail = fields.smaa_stats.fail_count;
    snapshot.smaa_resize = fields.smaa_stats.resize_count;
    snapshot.smaa_width = fields.smaa_stats.width;
    snapshot.smaa_height = fields.smaa_stats.height;
    snapshot.provider_ptr = fields.provider_ptr;

    return snapshot;
}

std::string FormatSummaryMessage(const SummarySnapshot& snapshot) {
    std::string buffer(4608, '\0');

    for (;;) {
        const int length = std::snprintf(
            buffer.data(),
            buffer.size(),
            kSummaryFormat,
        snapshot.task_ready,
        snapshot.task_dispatch,
        snapshot.wait_helper,
        snapshot.wait_task,
        snapshot.wait_gt16,
        snapshot.wait_gt100,
        snapshot.wait_gt1000,
        snapshot.wait_gt5000,
        snapshot.scaleform_time,
        snapshot.provider_non_null,
        snapshot.scaleform_init,
        snapshot.nis_time,
        snapshot.nis_sync,
        snapshot.nis_dt0,
        snapshot.nis_dt30,
        snapshot.nis_dt60,
        snapshot.nis_dt_other,
        snapshot.nis_scene_fix,
        snapshot.nis_play,
        snapshot.nis_play_adv,
        snapshot.nis_play_repeat,
        snapshot.nis_play_multi,
        snapshot.nis_boot,
        snapshot.nis_boot_s1,
        snapshot.nis_boot_s2,
        snapshot.nis_boot_fail,
        snapshot.nis_owner,
        snapshot.nis_owner_dt0,
        snapshot.nis_owner_dt30,
        snapshot.nis_owner_dt60,
        snapshot.nis_owner_dt_other,
        snapshot.nis_owner_adv,
        snapshot.nis_owner_repeat,
        snapshot.nis_owner_multi,
        snapshot.nis_actor_setup,
        snapshot.nis_actor_restore,
        snapshot.nis_actor_restore_untracked,
        snapshot.nis_actor_setup_duplicate,
        snapshot.nis_actor_restore_duplicate,
        snapshot.nis_actor_restore_never_seen,
        snapshot.nis_actor_restore_suppressed,
        snapshot.twitch_tick,
        snapshot.twitch_login_callback,
        snapshot.twitch_login_failure,
        snapshot.nis_last,
        snapshot.nis_last_delta,
        snapshot.nis_owner_last_dt,
        snapshot.frameflow,
        snapshot.frameflow_dt0,
        snapshot.frameflow_dt60,
        snapshot.frameflow_dt_other,
        snapshot.frameflow_from_cutscene,
        snapshot.frameflow_last_dt,
        snapshot.cutscene_flow,
        snapshot.cutscene_flow_dt0,
        snapshot.cutscene_flow_fwd0,
        snapshot.cutscene_flow_fwd60,
        snapshot.cutscene_flow_fwd_other,
        snapshot.cutscene_flow_fix,
        snapshot.cutscene_flow_fix_paused,
        snapshot.cutscene_flow_in,
        snapshot.cutscene_flow_fwd,
        snapshot.fog_slicing,
        snapshot.fog_clamps,
        snapshot.aa_owner,
        snapshot.aa_skip,
        snapshot.aa_main,
        snapshot.aa_hairblur0,
        snapshot.aa_state,
        snapshot.aa_hair_gate,
        snapshot.aa_variant,
        snapshot.aa_variant_apply,
        snapshot.aa_aux_mode,
        snapshot.aa_aux_apply,
        snapshot.aa_shader,
        snapshot.aa_raster,
        snapshot.aa_aux,
        reinterpret_cast<void*>(snapshot.aa_material),
        reinterpret_cast<void*>(snapshot.aa_target),
        reinterpret_cast<void*>(snapshot.aa_src_a),
        reinterpret_cast<void*>(snapshot.aa_src_b),
        snapshot.aa_fx,
        reinterpret_cast<void*>(snapshot.aa_fx_arg1),
        reinterpret_cast<void*>(snapshot.aa_fx_arg2),
        reinterpret_cast<void*>(snapshot.aa_fx_arg3),
        snapshot.smaa_enabled,
        snapshot.smaa_any_hook_retained,
        snapshot.smaa_ready,
        snapshot.smaa_present,
        snapshot.smaa_apply,
        snapshot.smaa_fail,
        snapshot.smaa_resize,
        snapshot.smaa_width,
        snapshot.smaa_height,
        snapshot.rumble_override,
        snapshot.rumble_value,
        snapshot.post_submit,
        snapshot.post_comp_lights,
        snapshot.post_comp_final,
        snapshot.post_bloom,
        snapshot.post_lightshaft,
        snapshot.post_shadow_collector,
        snapshot.post_final_chg,
        snapshot.post_final_flags,
        reinterpret_cast<void*>(snapshot.post_final_cmd),
        reinterpret_cast<void*>(snapshot.post_final_params),
        reinterpret_cast<void*>(snapshot.post_final_p0),
        reinterpret_cast<void*>(snapshot.post_final_p1),
        reinterpret_cast<void*>(snapshot.post_final_p2),
        reinterpret_cast<void*>(snapshot.post_final_p3),
        snapshot.char_water,
        snapshot.char_water_speed,
        snapshot.char_fx_update,
        snapshot.char_fx_wet_updates,
        snapshot.char_fx_surface,
        snapshot.char_fx_wet_surface,
        snapshot.char_fx_gate,
        snapshot.char_fx_tod_weather,
        snapshot.char_fx_tod_override,
        snapshot.char_fx_onfire,
        snapshot.char_fx_smolder,
        snapshot.char_fx_attached,
        snapshot.char_fx_fire_time,
        snapshot.char_fx_smolder_time,
        snapshot.char_fx_queued_damage,
        snapshot.char_health_apply,
        snapshot.char_health_proj,
        snapshot.char_health_melee,
        snapshot.char_health_anim_found,
        snapshot.char_health_hitreact_found,
        snapshot.char_health_damage,
        snapshot.char_health_projectile,
        reinterpret_cast<void*>(snapshot.char_health_component),
        reinterpret_cast<void*>(snapshot.char_health_anim),
        reinterpret_cast<void*>(snapshot.char_health_hitreact),
        reinterpret_cast<void*>(snapshot.char_health_attacker),
        reinterpret_cast<void*>(snapshot.char_health_hit),
        snapshot.char_wet_force,
        snapshot.char_wet_force_verify,
        snapshot.char_charred_anim,
        snapshot.char_charred_rig,
        snapshot.char_charred_amount,
        snapshot.char_dispatch_owner,
        snapshot.char_dispatch_consume,
        reinterpret_cast<void*>(snapshot.char_dispatch_owner_ptr),
        reinterpret_cast<void*>(snapshot.char_dispatch_component),
        snapshot.char_queue_build,
        snapshot.char_queue_build_tracked,
        reinterpret_cast<void*>(snapshot.char_queue_build_owner),
        reinterpret_cast<void*>(snapshot.char_queue_build_component),
        snapshot.char_queue_build_mode,
        snapshot.char_paint_owner,
        reinterpret_cast<void*>(snapshot.char_paint_owner_ptr),
        reinterpret_cast<void*>(snapshot.char_paint_owner_component),
        snapshot.char_paint_consumer,
        snapshot.char_paint_anim,
        snapshot.char_paint_rig,
        snapshot.char_paint_enable,
        snapshot.char_paint_r,
        snapshot.char_paint_g,
        snapshot.char_paint_b,
        snapshot.char_damage_create,
        snapshot.char_damage_reset,
        reinterpret_cast<void*>(snapshot.post_last),
        snapshot.unique_callbacks,
        snapshot.scenery_prepare,
        snapshot.scenery_setup,
        snapshot.render_scenery,
        snapshot.rasterize_bucket,
        snapshot.scenery_prepare_ready,
        snapshot.scenery_setup_ready,
        snapshot.render_scenery_ready,
        snapshot.rasterize_bucket_ready,
        snapshot.scenery_setup_qdelta,
        snapshot.render_scenery_qdelta,
        snapshot.rasterize_bucket_qdelta,
        snapshot.scenery_count0,
        snapshot.scenery_count1,
        snapshot.scenery_count2,
        snapshot.scenery_count3,
        reinterpret_cast<void*>(snapshot.provider_ptr));

        if (length < 0) {
            return {};
        }

        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return buffer;
        }

        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

}  // namespace spatch::hooks

#endif
