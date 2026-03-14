#include "Config.h"

#include <Windows.h>

#include <fstream>
#include <string>

namespace spatch {
namespace {

constexpr wchar_t kSection[] = L"SPatch";

void WriteDefaultConfig(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    stream
        << "; SPatch final release configuration\n"
        << "[SPatch]\n"
        << "\n"
        << "; Core\n"
        << "enabled=1\n"
        << "allow_unsupported_build=0\n"
        << "write_minidumps=1\n"
        << "\n"
        << "; Fixes and restorations\n"
        << "fix_cutscene_zero_dt=1\n"
        << "fix_cutscene_scene_time_step=1\n"
        << "; 1 disables the stock smoothing path entirely.\n"
        << "disable_time_step_smoothing=0\n"
        << "hook_fog_slicing_guard=1\n"
        << "warn_low_res_buffer=1\n"
        << "restore_character_visual_damage=1\n"
        << "\n"
        << "; Stock option overrides\n"
        << "; -1 keeps the game's own value.\n"
        << "time_step_smoothing_frames=2\n"
        << "override_low_res_buffer=0\n"
        << "override_shadow_filter=1\n"
        << "override_ssao=1\n"
        << "override_fps_limiter=-1\n"
        << "override_texture_detail_level=2\n"
        << "override_world_density=-1\n"
        << "override_rumble_enabled=-1\n"
        << "\n"
        << "; Adapter / memory\n"
        << "hook_vram_adapter_fix=1\n"
        << "vram_override_mb=0\n"
        << "\n"
        << "; Anti-aliasing\n"
        << "hook_smaa_present=1\n"
        << "smaa_enable=1\n"
        << "smaa_disable_stock_aa=1\n"
        << "smaa_preset=2\n"
        << "\n"
        << "; Tonemapping\n"
        << "hook_aces_final=1\n"
        << "hook_aces_presentbuffer=1\n"
        << "hook_aces_display_curve=1\n"
        << "aces_enable=1\n"
        << "aces_strength_percent=100\n"
        << "aces_exposure_scale_percent=175\n";
}

bool ReadBool(const std::filesystem::path& path, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(
               kSection,
               key,
               fallback ? 1 : 0,
               path.c_str()) != 0;
}

int ReadInt(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(kSection, key, fallback, path.c_str());
}

unsigned long ReadUInt(const std::filesystem::path& path,
                       const wchar_t* key,
                       unsigned long fallback) {
    wchar_t buffer[64]{};
    const std::wstring fallback_text = std::to_wstring(fallback);
    GetPrivateProfileStringW(
        kSection, key, fallback_text.c_str(), buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());

    try {
        return static_cast<unsigned long>(std::stoul(buffer));
    } catch (...) {
        return fallback;
    }
}

}  // namespace

Config LoadConfig(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        WriteDefaultConfig(path);
    }

    Config config;
    config.enabled = ReadBool(path, L"enabled", config.enabled);
    config.allow_unsupported_build =
        ReadBool(path, L"allow_unsupported_build", config.allow_unsupported_build);
    config.write_minidumps = ReadBool(path, L"write_minidumps", config.write_minidumps);
    config.hook_queue_ready = ReadBool(path, L"hook_queue_ready", config.hook_queue_ready);
    config.hook_task_dispatch =
        ReadBool(path, L"hook_task_dispatch", config.hook_task_dispatch);
    config.hook_wait_helper = ReadBool(path, L"hook_wait_helper", config.hook_wait_helper);
    config.hook_scaleform_time =
        ReadBool(path, L"hook_scaleform_time", config.hook_scaleform_time);
    config.hook_scaleform_init =
        ReadBool(path, L"hook_scaleform_init", config.hook_scaleform_init);
    config.hook_nis_timing =
        ReadBool(path, L"hook_nis_timing", config.hook_nis_timing);
    config.hook_nis_runtime =
        ReadBool(path, L"hook_nis_runtime", config.hook_nis_runtime);
    config.hook_nis_owner =
        ReadBool(path, L"hook_nis_owner", config.hook_nis_owner);
    config.hook_frameflow =
        ReadBool(path, L"hook_frameflow", config.hook_frameflow);
    config.fix_cutscene_zero_dt =
        ReadBool(path, L"fix_cutscene_zero_dt", config.fix_cutscene_zero_dt);
    config.fix_cutscene_scene_time_step =
        ReadBool(path, L"fix_cutscene_scene_time_step", config.fix_cutscene_scene_time_step);
    config.disable_time_step_smoothing =
        ReadBool(path, L"disable_time_step_smoothing", config.disable_time_step_smoothing);
    config.hook_scenery_builders =
        ReadBool(path, L"hook_scenery_builders", config.hook_scenery_builders);
    config.hook_fog_slicing_guard =
        ReadBool(path, L"hook_fog_slicing_guard", config.hook_fog_slicing_guard);
    config.hook_ao_stage =
        ReadBool(path, L"hook_ao_stage", config.hook_ao_stage);
    config.hook_ao_material =
        ReadBool(path, L"hook_ao_material", config.hook_ao_material);
    config.hook_ao_compute =
        ReadBool(path, L"hook_ao_compute", config.hook_ao_compute);
    config.hook_aa_probe =
        ReadBool(path, L"hook_aa_probe", config.hook_aa_probe);
    config.hook_aa_fx_probe =
        ReadBool(path, L"hook_aa_fx_probe", config.hook_aa_fx_probe);
    config.aa_variant_debug_keys =
        ReadBool(path, L"aa_variant_debug_keys", config.aa_variant_debug_keys);
    config.aa_aux_debug_keys =
        ReadBool(path, L"aa_aux_debug_keys", config.aa_aux_debug_keys);
    config.hook_smaa_present =
        ReadBool(path, L"hook_smaa_present", config.hook_smaa_present);
    config.smaa_enable =
        ReadBool(path, L"smaa_enable", config.smaa_enable);
    config.smaa_debug_keys =
        ReadBool(path, L"smaa_debug_keys", config.smaa_debug_keys);
    config.smaa_disable_stock_aa =
        ReadBool(path, L"smaa_disable_stock_aa", config.smaa_disable_stock_aa);
    config.hook_post_material_submit =
        ReadBool(path, L"hook_post_material_submit", config.hook_post_material_submit);
    config.hook_aces_final =
        ReadBool(path, L"hook_aces_final", config.hook_aces_final);
    config.hook_aces_presentbuffer =
        ReadBool(path, L"hook_aces_presentbuffer", config.hook_aces_presentbuffer);
    config.hook_aces_display_curve =
        ReadBool(path, L"hook_aces_display_curve", config.hook_aces_display_curve);
    config.hook_vram_adapter_fix =
        ReadBool(path, L"hook_vram_adapter_fix", config.hook_vram_adapter_fix);
    config.hook_character_regression_probe =
        ReadBool(path, L"hook_character_regression_probe", config.hook_character_regression_probe);
    config.restore_character_visual_damage =
        ReadBool(path, L"restore_character_visual_damage", config.restore_character_visual_damage);
    config.aces_enable =
        ReadBool(path, L"aces_enable", config.aces_enable);
    config.aces_debug_keys =
        ReadBool(path, L"aces_debug_keys", config.aces_debug_keys);
    config.warn_low_res_buffer =
        ReadBool(path, L"warn_low_res_buffer", config.warn_low_res_buffer);
    config.probe_cut_content_slices =
        ReadBool(path, L"probe_cut_content_slices", config.probe_cut_content_slices);
    config.force_cut_content_simulate_rewards =
        ReadBool(path, L"force_cut_content_simulate_rewards", config.force_cut_content_simulate_rewards);
    config.min_fog_slicing_interval =
        ReadInt(path, L"min_fog_slicing_interval", config.min_fog_slicing_interval);
    config.time_step_smoothing_frames =
        ReadInt(path, L"time_step_smoothing_frames", config.time_step_smoothing_frames);
    config.override_low_res_buffer =
        ReadInt(path, L"override_low_res_buffer", config.override_low_res_buffer);
    config.override_shadow_filter =
        ReadInt(path, L"override_shadow_filter", config.override_shadow_filter);
    config.override_ssao =
        ReadInt(path, L"override_ssao", config.override_ssao);
    config.override_fps_limiter =
        ReadInt(path, L"override_fps_limiter", config.override_fps_limiter);
    config.override_texture_detail_level =
        ReadInt(path, L"override_texture_detail_level", config.override_texture_detail_level);
    config.override_world_density =
        ReadInt(path, L"override_world_density", config.override_world_density);
    config.override_rumble_enabled =
        ReadInt(path, L"override_rumble_enabled", config.override_rumble_enabled);
    config.vram_override_mb =
        ReadInt(path, L"vram_override_mb", config.vram_override_mb);
    config.force_cut_content_candidate =
        ReadInt(path, L"force_cut_content_candidate", config.force_cut_content_candidate);
    config.aa_variant_mode =
        ReadInt(path, L"aa_variant_mode", config.aa_variant_mode);
    config.aa_aux_mode =
        ReadInt(path, L"aa_aux_mode", config.aa_aux_mode);
    config.smaa_preset =
        ReadInt(path, L"smaa_preset", config.smaa_preset);
    config.aces_strength_percent =
        ReadInt(path, L"aces_strength_percent", config.aces_strength_percent);
    config.aces_exposure_scale_percent =
        ReadInt(path, L"aces_exposure_scale_percent", config.aces_exposure_scale_percent);
    config.summary_interval_ms =
        ReadUInt(path, L"summary_interval_ms", config.summary_interval_ms);
    config.max_verbose_events =
        ReadUInt(path, L"max_verbose_events", config.max_verbose_events);
    config.max_unique_callbacks =
        ReadUInt(path, L"max_unique_callbacks", config.max_unique_callbacks);

#if defined(SPATCH_FINAL_RELEASE)
    config.hook_queue_ready = false;
    config.hook_task_dispatch = false;
    config.hook_wait_helper = false;
    config.hook_scaleform_time = false;
    config.hook_scaleform_init = false;
    config.hook_nis_timing = false;
    config.hook_nis_runtime = false;
    config.hook_nis_owner = false;
    config.hook_frameflow = true;
    config.hook_scenery_builders = false;
    config.hook_ao_stage = false;
    config.hook_ao_material = false;
    config.hook_ao_compute = false;
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.smaa_debug_keys = false;
    config.hook_post_material_submit = false;
    config.hook_character_regression_probe = false;
    config.aces_debug_keys = false;
    config.probe_cut_content_slices = false;
    config.force_cut_content_simulate_rewards = false;
    config.force_cut_content_candidate = -1;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
    config.summary_interval_ms = 0;
    config.max_verbose_events = 0;
    config.max_unique_callbacks = 0;
#endif

    if (config.summary_interval_ms == 0) {
        config.summary_interval_ms = 2000;
    }

    if (config.max_unique_callbacks == 0) {
        config.max_unique_callbacks = 64;
    }

    if (config.time_step_smoothing_frames < -1) {
        config.time_step_smoothing_frames = -1;
    }

    if (config.disable_time_step_smoothing) {
        config.time_step_smoothing_frames = 0;
    }

    if (config.min_fog_slicing_interval < 1) {
        config.min_fog_slicing_interval = 1;
    }

    if (config.override_low_res_buffer < -1) {
        config.override_low_res_buffer = -1;
    } else if (config.override_low_res_buffer > 1) {
        config.override_low_res_buffer = 1;
    }

    if (config.override_shadow_filter < -1) {
        config.override_shadow_filter = -1;
    } else if (config.override_shadow_filter > 1) {
        config.override_shadow_filter = 1;
    }

    if (config.override_ssao < -1) {
        config.override_ssao = -1;
    } else if (config.override_ssao > 1) {
        config.override_ssao = 1;
    }

    if (config.override_fps_limiter < -1) {
        config.override_fps_limiter = -1;
    } else if (config.override_fps_limiter > 4) {
        config.override_fps_limiter = 4;
    }

    if (config.override_texture_detail_level < -1) {
        config.override_texture_detail_level = -1;
    } else if (config.override_texture_detail_level > 2) {
        config.override_texture_detail_level = 2;
    }

    if (config.override_world_density < -1) {
        config.override_world_density = -1;
    } else if (config.override_world_density > 4) {
        config.override_world_density = 4;
    }

    if (config.override_rumble_enabled < -1) {
        config.override_rumble_enabled = -1;
    } else if (config.override_rumble_enabled > 1) {
        config.override_rumble_enabled = 1;
    }

    if (config.vram_override_mb < 0) {
        config.vram_override_mb = 0;
    } else if (config.vram_override_mb > 131072) {
        config.vram_override_mb = 131072;
    }

    if (config.force_cut_content_candidate < -1) {
        config.force_cut_content_candidate = -1;
    } else if (config.force_cut_content_candidate > 3) {
        config.force_cut_content_candidate = 3;
    }

    if (config.smaa_preset < 0) {
        config.smaa_preset = 0;
    } else if (config.smaa_preset > 3) {
        config.smaa_preset = 3;
    }

    if (config.aa_variant_mode < 0) {
        config.aa_variant_mode = 0;
    } else if (config.aa_variant_mode > 2) {
        config.aa_variant_mode = 2;
    }

    if (config.aa_aux_mode < 0) {
        config.aa_aux_mode = 0;
    } else if (config.aa_aux_mode > 2) {
        config.aa_aux_mode = 2;
    }

    if (config.aces_strength_percent < 0) {
        config.aces_strength_percent = 0;
    } else if (config.aces_strength_percent > 100) {
        config.aces_strength_percent = 100;
    }

    if (config.aces_exposure_scale_percent < 25) {
        config.aces_exposure_scale_percent = 25;
    } else if (config.aces_exposure_scale_percent > 400) {
        config.aces_exposure_scale_percent = 400;
    }

    return config;
}

bool WriteBoolValue(const std::filesystem::path& path, const wchar_t* key, bool value) {
    if (!std::filesystem::exists(path)) {
        WriteDefaultConfig(path);
    }
    return WritePrivateProfileStringW(kSection, key, value ? L"1" : L"0", path.c_str()) != 0;
}

bool WriteIntValue(const std::filesystem::path& path, const wchar_t* key, int value) {
    if (!std::filesystem::exists(path)) {
        WriteDefaultConfig(path);
    }
    wchar_t buffer[32]{};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d", value);
    return WritePrivateProfileStringW(kSection, key, buffer, path.c_str()) != 0;
}

}  // namespace spatch
