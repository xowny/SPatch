#pragma once

#include <filesystem>

namespace spatch {

struct Config {
    // Core
    bool enabled = true;
    // Permits build-independent settings on an unverified executable. Fixed
    // address hooks and executable patches remain disabled in this mode.
    bool allow_unverified_build = false;
    bool write_minidumps = true;
    // File logging is opt-in in user releases. Crash dumps remain independent
    // so a user can keep the low-overhead crash reporter without a live log.
    bool enable_logging = false;

    // Runtime probes and internal dependency hooks.  The cutscene fix turns
    // on its required timing hooks automatically; these switches only add
    // optional telemetry when enabled by an advanced user.
    bool hook_queue_ready = false;
    bool hook_task_dispatch = false;
    bool hook_wait_helper = false;
    bool hook_scaleform_time = false;
    bool hook_scaleform_init = false;
    bool hook_nis_timing = false;
    bool hook_nis_runtime = false;
    bool hook_nis_owner = false;
    bool hook_nis_actor_state = false;
    bool hook_twitch_probe = false;
    bool hook_frameflow = false;
    bool hook_scenery_builders = false;
    bool hook_fog_slicing_guard = true;
    bool hook_aa_probe = false;
    bool hook_aa_fx_probe = false;
    bool aa_variant_debug_keys = false;
    bool aa_aux_debug_keys = false;
    bool hook_smaa_present = true;
    bool smaa_disable_stock_aa = true;
    bool hook_post_material_submit = false;
    bool hook_character_regression_probe = false;

    // Stable fixes / restorations
    bool fix_cutscene_zero_dt = true;
    bool fix_cutscene_scene_time_step = true;
    bool fix_nis_actor_restore_duplicates = false;
    bool smaa_enable = true;
    bool smaa_debug_keys = false;
    bool restore_original_fog = true;
    bool restore_original_eye_reflections = true;
    bool restore_character_wetness = true;
    bool restore_character_sweat = true;
    bool fix_pedestrian_density_at_high_fps = true;
    bool fix_high_fps_average_windows = true;
    bool improve_spherical_reflections = true;
    bool remove_hidden_120_fps_cap = true;
    bool fix_first_run_resolution = true;
    bool fix_scaleform_qpc_clock = true;
    bool fix_file_timestamp_open_mode = true;
    bool fix_audio_file_open = true;
    bool fix_large_file_sizes = true;
    bool fix_vram_pool_lock = true;
    bool fix_vram_capacity_reporting = true;
    bool fix_resource_loading = true;
    bool fix_contact_list_overflow = true;
    bool fix_corrupt_save_handling = true;
    bool fix_thread_creation_failure = true;
    bool warn_low_res_buffer = true;

    // Input
    // The game already registers Windows Raw Input for the mouse. This forces
    // its hidden raw-input option instead of installing a second input stack.
    bool force_raw_mouse_input = true;
    // FollowCamera otherwise drains accumulated mouse delta over time before
    // applying yaw and pitch. Controller look is not temporally smoothed.
    bool disable_camera_smoothing = true;
    // Independent opt-in vehicle-camera behavior. Unsupported executable
    // profiles must force both off until their native mappings are verified.
    bool gta_iv_car_camera = false;
    bool gta_iv_bike_camera = false;
    // -1 preserves the stock radial filter. Zero exposes the direct XInput or
    // DirectInput stick values; positive values are radial percentages.
    int controller_left_stick_deadzone = -1;
    int controller_right_stick_deadzone = -1;

    // Numeric tuning
    // The stock setter uses interval * 8 as an unsigned divisor; only 1-4
    // produce a valid, non-stalled slicing cadence.
    int min_fog_slicing_interval = 1;
    // -1 keeps the game value, 0 disables the stock adaptive smoother, and 1
    // enables it. The game always uses a maximum of eight samples; this is not
    // an arbitrary frame-count control.
    int time_step_smoothing = 1;
    int override_low_res_buffer = 0;
    int override_shadow_filter = -1;
    int override_fps_limiter = -1;
    int override_texture_detail_level = -1;
    // -1 preserves the game setting. Supported forced values are the native
    // sampler-writer exponents represented as 4x, 8x, and 16x.
    int anisotropic_filtering = 16;
    // Promote the exact stock trilinear filter-selector branch.
    bool force_anisotropic_filtering = true;
    // -1 keeps the game setting, 0 disables motion blur, 1 is the stock
    // normal tier, and 2 is the stock high-quality tier.
    int override_motion_blur = -1;
    int override_world_density = -1;
    int override_rumble_enabled = -1;
    // 0 follows the active display width. Positive values are clamped to the
    // safe 1280..4096 range; the spherical target is always kept at 2:1.
    int spherical_reflection_width = 0;
    bool prefer_max_refresh_rate = false;
    int override_fullscreen = -1;
    int aa_variant_mode = 0;
    int aa_aux_mode = 0;
    int smaa_preset = 3;
    // Time in seconds spent fully wet after water contact, followed by the
    // duration of the linear dry-down. Both are end-user configurable.
    int wetness_full_time_seconds = 30;
    int wetness_fade_time_seconds = 270;
    // Sweat is accumulated only while a character is running or in combat.
    // The values are deliberately independent of render cadence and apply to
    // Wei and NPCs alike.
    int sweat_build_time_seconds = 150;
    int sweat_fade_time_seconds = 120;
    int sweat_onset_time_seconds = 30;
    float sweat_run_speed = 2.5f;
    int sweat_combat_time_seconds = 15;
    unsigned long summary_interval_ms = 0;
    unsigned long max_verbose_events = 0;
    unsigned long max_unique_callbacks = 0;

    // Cutscene timing
    // 0 follows the live game cadence (recommended). A positive value tells
    // the correction logic which externally selected game cadence to expect.
    // This is deliberately not a renderer/physics limiter.
    int cutscene_fps = 0;
};

inline constexpr int kCutsceneFpsAuto = 0;
inline constexpr int kCutsceneFpsMin = 15;
inline constexpr int kCutsceneFpsMax = 1000;
inline constexpr int kSphericalReflectionWidthAuto = 0;
inline constexpr int kSphericalReflectionWidthMin = 1280;
inline constexpr int kSphericalReflectionWidthMax = 4096;
inline constexpr int kWetnessTimeMinSeconds = 0;
inline constexpr int kWetnessTimeMaxSeconds = 3600;
inline constexpr int kSweatTimeMinSeconds = 0;
inline constexpr int kSweatTimeMaxSeconds = 3600;
inline constexpr float kSweatRunSpeedMin = 0.0f;
inline constexpr float kSweatRunSpeedMax = 100.0f;
inline constexpr int kConfigVersion = 44;

enum class ConfigPersistenceStatus {
    Unchanged,
    Created,
    Migrated,
    CreateFailed,
    BackupFailed,
    MigrationWriteFailed,
    SourceChanged,
    SourceUnavailable,
};

struct ConfigLoadReport {
    int source_version = 0;
    bool source_file_existed = false;
    ConfigPersistenceStatus persistence = ConfigPersistenceStatus::Unchanged;

    [[nodiscard]] bool persistence_succeeded() const noexcept {
        return persistence == ConfigPersistenceStatus::Unchanged ||
               persistence == ConfigPersistenceStatus::Created ||
               persistence == ConfigPersistenceStatus::Migrated;
    }
};

const char* ConfigPersistenceStatusName(ConfigPersistenceStatus status) noexcept;
// Migration backups live outside the game directory. The path is stable for
// one config path and target schema version so installers and tests can find
// the exact pre-migration bytes without leaving .bak files beside the game.
std::filesystem::path ConfigBackupPath(const std::filesystem::path& config_path);
std::filesystem::path LegacyConfigBackupPath(
    const std::filesystem::path& config_path);
Config LoadConfig(const std::filesystem::path& path, ConfigLoadReport* report = nullptr);
bool WriteBoolValue(const std::filesystem::path& path, const wchar_t* key, bool value);
bool WriteIntValue(const std::filesystem::path& path, const wchar_t* key, int value);

}  // namespace spatch
