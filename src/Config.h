#pragma once

#include <filesystem>

namespace spatch {

struct Config {
    // Core
    bool enabled = true;
    bool allow_unsupported_build = false;
    bool write_minidumps = true;

    // Runtime diagnostics / probes
    bool hook_queue_ready = false;
    bool hook_task_dispatch = false;
    bool hook_wait_helper = false;
    bool hook_scaleform_time = false;
    bool hook_scaleform_init = false;
    bool hook_nis_timing = true;
    bool hook_nis_runtime = false;
    bool hook_nis_owner = true;
    bool hook_frameflow = true;
    bool hook_scenery_builders = false;
    bool hook_fog_slicing_guard = true;
    bool hook_ao_stage = false;
    bool hook_ao_material = false;
    bool hook_ao_compute = false;
    bool hook_aa_probe = false;
    bool hook_aa_fx_probe = false;
    bool aa_variant_debug_keys = false;
    bool aa_aux_debug_keys = false;
    bool hook_smaa_present = true;
    bool smaa_disable_stock_aa = true;
    bool hook_post_material_submit = false;
    bool hook_aces_final = true;
    bool hook_aces_presentbuffer = true;
    bool hook_aces_display_curve = true;
    bool hook_vram_adapter_fix = true;
    bool hook_character_regression_probe = false;

    // Stable fixes / restorations
    bool fix_cutscene_zero_dt = true;
    bool fix_cutscene_scene_time_step = true;
    bool disable_time_step_smoothing = false;
    bool smaa_enable = true;
    bool smaa_debug_keys = true;
    bool restore_character_visual_damage = true;
    bool aces_enable = true;
    bool aces_debug_keys = true;
    bool warn_low_res_buffer = true;
    bool probe_cut_content_slices = false;
    bool force_cut_content_simulate_rewards = true;

    // Numeric tuning
    int min_fog_slicing_interval = 1;
    int time_step_smoothing_frames = 2;
    int override_low_res_buffer = 0;
    int override_shadow_filter = 1;
    int override_ssao = 1;
    int override_fps_limiter = -1;
    int override_texture_detail_level = 2;
    int override_world_density = -1;
    int override_rumble_enabled = -1;
    int vram_override_mb = 0;
    int force_cut_content_candidate = -1;
    int aa_variant_mode = 0;
    int aa_aux_mode = 0;
    int smaa_preset = 2;
    int aces_strength_percent = 100;
    int aces_exposure_scale_percent = 175;
    unsigned long summary_interval_ms = 2000;
    unsigned long max_verbose_events = 8;
    unsigned long max_unique_callbacks = 64;
};

Config LoadConfig(const std::filesystem::path& path);
bool WriteBoolValue(const std::filesystem::path& path, const wchar_t* key, bool value);
bool WriteIntValue(const std::filesystem::path& path, const wchar_t* key, int value);

}  // namespace spatch
