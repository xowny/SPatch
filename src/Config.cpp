#include "Config.h"

#include "InputPolicy.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace spatch {
namespace {

constexpr wchar_t kSection[] = L"SPatch";
constexpr wchar_t kCutsceneSection[] = L"Cutscenes";
constexpr wchar_t kDisplaySection[] = L"Display";
constexpr wchar_t kGraphicsSection[] = L"Graphics";
constexpr wchar_t kTextureFilteringSection[] = L"TextureFiltering";
constexpr wchar_t kRendererSection[] = L"Renderer";
constexpr wchar_t kShadowSection[] = L"Shadows";
constexpr wchar_t kAaSection[] = L"AntiAliasing";
constexpr wchar_t kToneSection[] = L"Tonemapping";
constexpr wchar_t kAoSection[] = L"AmbientOcclusion";
constexpr wchar_t kGiSection[] = L"GlobalIllumination";
constexpr wchar_t kPbrSection[] = L"PhysicallyBasedRendering";
constexpr wchar_t kSssSection[] = L"SubsurfaceScattering";
constexpr wchar_t kMaterialScatteringSection[] = L"MaterialScattering";
constexpr wchar_t kInputSection[] = L"Input";
constexpr wchar_t kGameplaySection[] = L"Gameplay";
constexpr wchar_t kStabilitySection[] = L"Stability";
constexpr wchar_t kAdvancedSection[] = L"Advanced";
constexpr wchar_t kDiagnosticsSection[] = L"Diagnostics";
constexpr wchar_t kDebugSection[] = L"Debug";
constexpr unsigned long kMaxSummaryIntervalMs = 60UL * 60UL * 1000UL;
constexpr unsigned long kMaxVerboseEvents = 100000UL;
constexpr unsigned long kMaxUniqueCallbacks = 100000UL;
std::mutex g_config_write_mutex;
std::atomic<unsigned long> g_config_write_serial = 0;

struct FileIdentity {
    DWORD volume_serial = 0;
    DWORD file_index_high = 0;
    DWORD file_index_low = 0;
    DWORD size_high = 0;
    DWORD size_low = 0;
    FILETIME last_write_time{};

    bool operator==(const FileIdentity& other) const noexcept {
        return volume_serial == other.volume_serial &&
            file_index_high == other.file_index_high &&
            file_index_low == other.file_index_low &&
            size_high == other.size_high && size_low == other.size_low &&
            last_write_time.dwLowDateTime ==
                other.last_write_time.dwLowDateTime &&
            last_write_time.dwHighDateTime ==
                other.last_write_time.dwHighDateTime;
    }
};

bool ReadFileIdentity(const std::filesystem::path& path,
                      FileIdentity& identity) noexcept {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool succeeded =
        GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    if (!succeeded) {
        return false;
    }
    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_index_high = information.nFileIndexHigh;
    identity.file_index_low = information.nFileIndexLow;
    identity.size_high = information.nFileSizeHigh;
    identity.size_low = information.nFileSizeLow;
    identity.last_write_time = information.ftLastWriteTime;
    return true;
}

struct IniKeyAlias {
    std::wstring_view legacy_key;
    const wchar_t* section;
    const wchar_t* user_key;
};

constexpr std::array kIniKeyAliases{
    IniKeyAlias{L"config_version", kSection, L"ConfigVersion"},
    IniKeyAlias{L"enabled", kSection, L"Enabled"},
    IniKeyAlias{L"allow_unverified_build", kSection, L"AllowUnverifiedBuild"},
    IniKeyAlias{L"allow_unsupported_build", kSection, L"AllowUnsupportedBuild"},
    IniKeyAlias{L"write_minidumps", kDebugSection, L"WriteCrashDumps"},
    IniKeyAlias{L"enable_logging", kDebugSection, L"Logging"},
    IniKeyAlias{L"cutscene_fps", kCutsceneSection, L"CutsceneFPS"},
    IniKeyAlias{L"fix_cutscene_zero_dt", kCutsceneSection, L"FixCutsceneFPS"},
    IniKeyAlias{L"fix_cutscene_scene_time_step", kCutsceneSection, L"FixCutsceneFPS"},
    IniKeyAlias{L"fix_nis_actor_restore_duplicates", kCutsceneSection, L"FixDuplicateCutsceneActorRestore"},
    IniKeyAlias{L"disable_time_step_smoothing", kCutsceneSection, L"DisableTimeStepSmoothing"},
    IniKeyAlias{L"time_step_smoothing_frames", kCutsceneSection, L"TimeStepSmoothingFrames"},
    IniKeyAlias{L"time_step_smoothing", kCutsceneSection, L"TimeStepSmoothing"},
    IniKeyAlias{L"force_raw_mouse_input", kInputSection, L"ForceRawMouseInput"},
    IniKeyAlias{L"disable_camera_smoothing", kInputSection, L"DisableCameraSmoothing"},
    IniKeyAlias{L"gta_iv_car_camera", kInputSection, L"GTAIVCarCamera"},
    IniKeyAlias{L"gta_iv_bike_camera", kInputSection, L"GTAIVBikeCamera"},
    IniKeyAlias{L"controller_left_stick_deadzone", kInputSection, L"LeftStickDeadzone"},
    IniKeyAlias{L"controller_right_stick_deadzone", kInputSection, L"RightStickDeadzone"},
    IniKeyAlias{L"hook_fog_slicing_guard", kStabilitySection, L"FixFogSlicing"},
    IniKeyAlias{L"min_fog_slicing_interval", kStabilitySection, L"MinimumFogSlicingInterval"},
    IniKeyAlias{L"warn_low_res_buffer", kDisplaySection, L"WarnLowResolutionBuffer"},
    IniKeyAlias{L"restore_original_fog", kGraphicsSection, L"RestoreOriginalFogAndNeon"},
    IniKeyAlias{L"restore_original_eye_reflections", kGraphicsSection, L"RestoreOriginalEyeReflections"},
    IniKeyAlias{L"restore_character_wetness", kGraphicsSection, L"RestoreWetness"},
    IniKeyAlias{L"restore_character_sweat", kGraphicsSection, L"RestoreSweat"},
    IniKeyAlias{L"wetness_full_time_seconds", kGraphicsSection, L"WetnessFullTime"},
    IniKeyAlias{L"wetness_fade_time_seconds", kGraphicsSection, L"WetnessFadeTime"},
    IniKeyAlias{L"sweat_build_time_seconds", kGraphicsSection, L"SweatBuildTime"},
    IniKeyAlias{L"sweat_fade_time_seconds", kGraphicsSection, L"SweatFadeTime"},
    IniKeyAlias{L"sweat_onset_time_seconds", kGraphicsSection, L"SweatOnsetTime"},
    IniKeyAlias{L"sweat_run_speed", kGraphicsSection, L"SweatRunSpeed"},
    IniKeyAlias{L"sweat_combat_time_seconds", kGraphicsSection, L"SweatCombatTime"},
    IniKeyAlias{L"fix_pedestrian_density_at_high_fps", kGraphicsSection, L"FixPedestrianDensity"},
    IniKeyAlias{L"fix_high_fps_average_windows", kGraphicsSection, L"FixHighFPSAverages"},
    IniKeyAlias{L"improve_spherical_reflections", kGraphicsSection, L"ImproveSphericalReflections"},
    IniKeyAlias{L"spherical_reflection_width", kGraphicsSection, L"SphericalReflectionWidth"},
    IniKeyAlias{L"fix_vram_capacity_reporting", kGraphicsSection, L"FixVRAMReporting"},
    IniKeyAlias{L"remove_hidden_120_fps_cap", kDisplaySection, L"Remove120FPSCap"},
    IniKeyAlias{L"fix_first_run_resolution", kDisplaySection, L"FixFirstRunResolution"},
    IniKeyAlias{L"fix_scaleform_qpc_clock", kStabilitySection, L"FixScaleformTimerOverflow"},
    IniKeyAlias{L"fix_file_timestamp_open_mode", kStabilitySection, L"FixFileTimestampUpdates"},
    IniKeyAlias{L"fix_audio_file_open", kStabilitySection, L"FixAudioFileOpen"},
    IniKeyAlias{L"fix_large_file_sizes", kStabilitySection, L"FixLargeFileSizes"},
    IniKeyAlias{L"fix_vram_pool_lock", kStabilitySection, L"FixVRAMPoolLock"},
    IniKeyAlias{L"fix_resource_loading", kStabilitySection, L"FixResourceLoading"},
    IniKeyAlias{L"fix_contact_list_overflow", kStabilitySection, L"FixContactListOverflow"},
    IniKeyAlias{L"fix_corrupt_save_handling", kStabilitySection, L"FixCorruptSaveCrash"},
    IniKeyAlias{L"fix_thread_creation_failure", kStabilitySection, L"FixThreadCreationFailure"},
    IniKeyAlias{L"prefer_max_refresh_rate", kDisplaySection, L"UseMaximumRefreshRate"},
    IniKeyAlias{L"override_fullscreen", kDisplaySection, L"Fullscreen"},
    IniKeyAlias{L"override_fps_limiter", kDisplaySection, L"FrameLimiter"},
    IniKeyAlias{L"override_low_res_buffer", kDisplaySection, L"LowResolutionBuffer"},
    IniKeyAlias{L"override_shadow_filter", kShadowSection, L"OriginalShadowFilter"},
    IniKeyAlias{L"override_texture_detail_level", kGraphicsSection, L"TextureDetail"},
    IniKeyAlias{L"anisotropic_filtering", kTextureFilteringSection, L"AnisotropicFiltering"},
    IniKeyAlias{L"force_anisotropic_filtering", kTextureFilteringSection, L"ForceAnisotropicFiltering"},
    IniKeyAlias{L"override_motion_blur", kGraphicsSection, L"MotionBlur"},
    IniKeyAlias{L"override_world_density", kGameplaySection, L"WorldDensity"},
    IniKeyAlias{L"override_rumble_enabled", kGameplaySection, L"Rumble"},
    IniKeyAlias{L"hook_smaa_present", kAaSection, L"SMAA"},
    IniKeyAlias{L"smaa_enable", kAaSection, L"SMAA"},
    IniKeyAlias{L"smaa_disable_stock_aa", kAaSection, L"DisableStockAA"},
    IniKeyAlias{L"smaa_preset", kAaSection, L"SMAAPreset"},
    IniKeyAlias{L"summary_interval_ms", kDiagnosticsSection, L"SummaryInterval"},
    IniKeyAlias{L"max_verbose_events", kDiagnosticsSection, L"VerboseEvents"},
    IniKeyAlias{L"max_unique_callbacks", kDiagnosticsSection, L"UniqueCallbacks"},
};

const IniKeyAlias* FindIniKeyAlias(const wchar_t* key) {
    const std::wstring_view value(key == nullptr ? L"" : key);
    const auto it = std::find_if(
        kIniKeyAliases.begin(), kIniKeyAliases.end(), [value](const IniKeyAlias& alias) {
            return alias.legacy_key == value;
        });
    return it == kIniKeyAliases.end() ? nullptr : &*it;
}

const wchar_t* SectionForKey(const wchar_t* key) {
    const std::wstring_view value(key == nullptr ? L"" : key);
    if (value == L"enabled" || value == L"allow_unverified_build" ||
        value == L"allow_unsupported_build" ||
        value == L"config_version") {
        return kSection;
    }
    if (value == L"write_minidumps") {
        return kDebugSection;
    }
    if (value.find(L"cutscene") != std::wstring_view::npos ||
        value == L"disable_time_step_smoothing" || value == L"min_fog_slicing_interval" ||
        value == L"hook_fog_slicing_guard" ||
        value == L"time_step_smoothing_frames" || value == L"time_step_smoothing" ||
        value == L"fix_cutscene_zero_dt" || value == L"fix_cutscene_scene_time_step") {
        return kCutsceneSection;
    }
    if (value == L"force_raw_mouse_input" || value == L"disable_camera_smoothing" ||
        value == L"gta_iv_car_camera" || value == L"gta_iv_bike_camera" ||
        value == L"controller_left_stick_deadzone" ||
        value == L"controller_right_stick_deadzone") {
        return kInputSection;
    }
    if (value == L"restore_original_fog" ||
        value == L"restore_original_eye_reflections" ||
        value == L"restore_character_wetness" ||
        value == L"restore_character_sweat" ||
        value == L"wetness_full_time_seconds" ||
        value == L"wetness_fade_time_seconds" ||
        value == L"sweat_build_time_seconds" ||
        value == L"sweat_fade_time_seconds" ||
        value == L"sweat_onset_time_seconds" ||
        value == L"sweat_run_speed" ||
        value == L"sweat_combat_time_seconds" ||
        value == L"fix_high_fps_average_windows" ||
        value == L"fix_pedestrian_density_at_high_fps" ||
        value == L"improve_spherical_reflections" ||
        value == L"spherical_reflection_width") {
        return kGraphicsSection;
    }
    if (value == L"override_shadow_filter") {
        return kShadowSection;
    }
    if (value == L"anisotropic_filtering" ||
        value == L"force_anisotropic_filtering") {
        return kTextureFilteringSection;
    }
    if (value.starts_with(L"override_") || value == L"prefer_max_refresh_rate" ||
        value == L"remove_hidden_120_fps_cap" || value == L"warn_low_res_buffer") {
        return kDisplaySection;
    }
    if (value.starts_with(L"smaa")) {
        return kAaSection;
    }
    if (value == L"fix_corrupt_save_handling" ||
        value == L"fix_thread_creation_failure") {
        return kStabilitySection;
    }
    if (value == L"summary_interval_ms" || value == L"max_verbose_events" ||
        value == L"max_unique_callbacks") {
        return kDiagnosticsSection;
    }
    return kAdvancedSection;
}

constexpr wchar_t kMissingIniValueSentinel[] =
    L"{SPatch-key-not-present-6C46D9C1}";

bool HasExactIniKey(const std::filesystem::path& path,
                    const wchar_t* section,
                    const wchar_t* key) {
    wchar_t buffer[256]{};
    const DWORD length = GetPrivateProfileStringW(
        section, key, kMissingIniValueSentinel, buffer,
        static_cast<DWORD>(std::size(buffer)), path.c_str());
    return length != std::size(kMissingIniValueSentinel) - 1 ||
           std::wstring_view(buffer, length) !=
               std::wstring_view(kMissingIniValueSentinel);
}

std::optional<std::wstring> ReadRawValue(const std::filesystem::path& path,
                                         const wchar_t* section,
                                         const wchar_t* key) {
    wchar_t buffer[256]{};
    const DWORD length = GetPrivateProfileStringW(
        section, key, kMissingIniValueSentinel, buffer,
        static_cast<DWORD>(std::size(buffer)), path.c_str());
    if (length >= std::size(buffer) - 1) {
        return std::nullopt;
    }
    if (length == std::size(kMissingIniValueSentinel) - 1 &&
        std::wstring_view(buffer, length) ==
            std::wstring_view(kMissingIniValueSentinel)) {
        return std::nullopt;
    }
    // A present empty value now remains an engaged optional so canonical
    // malformed input reaches its parser fallback instead of leaking through
    // to a lower-priority legacy alias.
    return std::wstring(buffer, length);
}

bool HasMigratedRetiredRendererSettings(const std::filesystem::path& path) {
    const auto has_any = [&](const auto& sections, const auto& keys) {
        for (const wchar_t* section : sections) {
            for (const wchar_t* key : keys) {
                if (HasExactIniKey(path, section, key)) {
                    return true;
                }
            }
        }
        return false;
    };

    constexpr const wchar_t* tone_sections[]{kToneSection, kGraphicsSection, kSection};
    constexpr const wchar_t* tone_keys[]{
        L"AgX", L"AgXLook", L"AgXStrength", L"AgXExposure",
        L"ACES", L"ACESLook", L"ACESStrength", L"ACESExposure",
        L"agx_enable", L"agx_look", L"agx_strength_percent",
        L"agx_exposure_scale_percent", L"aces_enable", L"aces_look",
        L"aces_strength_percent", L"aces_exposure_scale_percent",
        L"hook_agx_final", L"hook_agx_presentbuffer", L"hook_agx_display_curve"};
    if (has_any(tone_sections, tone_keys)) {
        return true;
    }

    constexpr const wchar_t* shadow_sections[]{kShadowSection, kGraphicsSection, kSection};
    constexpr const wchar_t* shadow_keys[]{L"ShadowResolution", L"shadow_resolution"};
    if (has_any(shadow_sections, shadow_keys)) {
        return true;
    }

    constexpr const wchar_t* ao_sections[]{kAoSection, kGraphicsSection, kSection};
    constexpr const wchar_t* ao_keys[]{
        L"AmbientOcclusion", L"SDAO", L"GTAO", L"OriginalAOQuality", L"SSAO",
        L"SDAOQuality", L"SDAORadius", L"SDAOStrength",
        L"GTAOQuality", L"GTAORadius", L"GTAOStrength",
        L"GTAOLiteQuality", L"GTAOLiteRadius", L"GTAOLiteStrength",
        L"sdao_enable", L"gtao_enable", L"override_ssao", L"sdao_quality",
        L"sdao_radius", L"sdao_strength_percent", L"gtao_quality", L"gtao_radius",
        L"gtao_strength_percent", L"gtao_lite_quality", L"gtao_lite_radius",
        L"gtao_lite_strength_percent", L"hook_ao_stage", L"hook_ao_material",
        L"hook_ao_compute"};
    if (has_any(ao_sections, ao_keys)) {
        return true;
    }

    constexpr const wchar_t* gi_sections[]{kGiSection, kGraphicsSection, kSection};
    constexpr const wchar_t* gi_keys[]{
        L"GlobalIllumination", L"GIQuality", L"GIStrength", L"GIRadius",
        L"global_illumination", L"gi_quality", L"gi_strength_percent", L"gi_radius"};
    if (has_any(gi_sections, gi_keys)) {
        return true;
    }

    constexpr const wchar_t* pbr_sections[]{kPbrSection, kGraphicsSection, kSection};
    constexpr const wchar_t* pbr_keys[]{
        L"PhysicallyBasedRendering", L"PBR", L"physically_based_rendering", L"pbr"};
    if (has_any(pbr_sections, pbr_keys)) {
        return true;
    }

    constexpr const wchar_t* sss_sections[]{kSssSection, kGraphicsSection, kSection};
    constexpr const wchar_t* sss_keys[]{
        L"SubsurfaceScattering", L"StockHairBlur", L"SSSQuality", L"SSSStrength",
        L"SSSRadius", L"subsurface_scattering", L"stock_hair_blur", L"sss_quality",
        L"sss_strength_percent", L"sss_radius_percent"};
    if (has_any(sss_sections, sss_keys)) {
        return true;
    }

    constexpr const wchar_t* material_sections[]{
        kMaterialScatteringSection, kGraphicsSection, kSection};
    constexpr const wchar_t* material_keys[]{
        L"EyeScattering", L"HairScattering", L"TeethScattering",
        L"FoliageTransmission", L"WaterScattering", L"WaterVolumetricScattering",
        L"VolumetricScattering", L"WaterScatteringStrength", L"ScatteringStrength",
        L"WaterScatteringAnisotropy", L"ScatteringAnisotropy", L"eye_scattering",
        L"hair_scattering", L"teeth_scattering", L"foliage_transmission",
        L"water_scattering", L"water_volumetric_scattering", L"volumetric_scattering",
        L"water_sss", L"water_scattering_strength", L"scattering_strength",
        L"water_scattering_anisotropy", L"scattering_anisotropy"};
    if (has_any(material_sections, material_keys)) {
        return true;
    }

    constexpr const wchar_t* debug_sections[]{kDebugSection};
    constexpr const wchar_t* debug_keys[]{
        L"DumpShaders", L"CensusShadowConsumers"};
    return has_any(debug_sections, debug_keys);
}

std::optional<std::wstring> ReadConfiguredValue(const std::filesystem::path& path,
                                                 const wchar_t* key) {
    if (const IniKeyAlias* alias = FindIniKeyAlias(key); alias != nullptr) {
        if (auto value = ReadRawValue(path, alias->section, alias->user_key); value.has_value()) {
            return value;
        }
        // Config v5 used the same end-user names but wrote every option under
        // [SPatch]. Check that canonical legacy location before snake_case
        // fallbacks so migration never silently resets a user's settings.
        if (std::wstring_view(alias->section) != std::wstring_view(kSection)) {
            if (auto value = ReadRawValue(path, kSection, alias->user_key); value.has_value()) {
                return value;
            }
        }
    }

    const wchar_t* section = SectionForKey(key);
    if (auto value = ReadRawValue(path, section, key); value.has_value()) {
        return value;
    }
    if (std::wstring_view(section) != std::wstring_view(kSection)) {
        return ReadRawValue(path, kSection, key);
    }
    return std::nullopt;
}

std::optional<std::wstring> ReadLoggingValue(
    const std::filesystem::path& path) {
    // v40 moves the one public logging switch to [Debug] Logging. Prefer the
    // canonical spelling, then preserve the former public and implementation
    // names long enough to migrate them out of the rewritten file.
    for (const auto& [section, key] : std::array{
             std::pair{kDebugSection, L"Logging"},
             std::pair{kSection, L"Logging"},
             std::pair{kDiagnosticsSection, L"EnableLogging"},
             std::pair{kSection, L"EnableLogging"},
             std::pair{kDebugSection, L"EnableLogging"},
             std::pair{kDiagnosticsSection, L"enable_logging"},
             std::pair{kSection, L"enable_logging"}}) {
        if (auto value = ReadRawValue(path, section, key); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> ReadTextureFilteringValue(
    const std::filesystem::path& path,
    const wchar_t* canonical_key,
    const wchar_t* internal_key) {
    // v42 restores native sampler controls to SPatch. Prefer the dedicated
    // public section, then preserve the two historical locations and finally
    // the former implementation spellings during migration.
    for (const auto& [section, key] : std::array{
             std::pair{kTextureFilteringSection, canonical_key},
             std::pair{kGraphicsSection, canonical_key},
             std::pair{kSection, canonical_key},
             std::pair{kTextureFilteringSection, internal_key},
             std::pair{kGraphicsSection, internal_key},
             std::pair{kSection, internal_key}}) {
        if (auto value = ReadRawValue(path, section, key); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool HasRetiredTextureFilteringLocation(
    const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kTextureFilteringSection, L"anisotropic_filtering"},
             std::pair{kTextureFilteringSection, L"force_anisotropic_filtering"},
             std::pair{kGraphicsSection, L"AnisotropicFiltering"},
             std::pair{kGraphicsSection, L"ForceAnisotropicFiltering"},
             std::pair{kGraphicsSection, L"anisotropic_filtering"},
             std::pair{kGraphicsSection, L"force_anisotropic_filtering"},
             std::pair{kSection, L"AnisotropicFiltering"},
             std::pair{kSection, L"ForceAnisotropicFiltering"},
             std::pair{kSection, L"anisotropic_filtering"},
             std::pair{kSection, L"force_anisotropic_filtering"}}) {
        if (HasExactIniKey(path, section, key)) {
            return true;
        }
    }
    return false;
}

bool HasRetiredCrashDumpLocation(const std::filesystem::path& path) {
    // Before v42 the public key was written under [SPatch]. Keep it readable,
    // but rewrite it once so WriteCrashDumps is literally the final INI key.
    return HasExactIniKey(path, kSection, L"WriteCrashDumps") ||
           HasExactIniKey(path, kSection, L"write_minidumps") ||
           HasExactIniKey(path, kDebugSection, L"write_minidumps");
}

bool HasRetiredLoggingSettings(const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kDiagnosticsSection, L"EnableLogging"},
             std::pair{kSection, L"EnableLogging"},
             std::pair{kDebugSection, L"EnableLogging"},
             std::pair{kDiagnosticsSection, L"enable_logging"},
             std::pair{kSection, L"enable_logging"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}

bool HasRetiredVisualDamageSettings(const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kGraphicsSection, L"RestoreVisualDamage"},
             std::pair{kSection, L"RestoreVisualDamage"},
             std::pair{kGraphicsSection, L"restore_character_visual_damage"},
             std::pair{kSection, L"restore_character_visual_damage"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}


std::optional<std::wstring> ReadRendererValue(
    const std::filesystem::path& path,
    const wchar_t* canonical_key,
    const wchar_t* internal_key) {
    // Renderer/swap-chain overrides are retired. Detect every previously
    // accepted spelling so even a current-version file is rewritten once to
    // the stock native D3D11 contract.
    for (const auto& [section, key] : std::array{
             std::pair{kRendererSection, canonical_key},
             std::pair{kGraphicsSection, canonical_key},
             std::pair{kSection, canonical_key},
             std::pair{kGraphicsSection, internal_key},
             std::pair{kSection, internal_key}}) {
        if (auto value = ReadRawValue(path, section, key); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool HasRetiredUnverifiedSettings(const std::filesystem::path& path) {
    const auto has_key = [&](const wchar_t* section,
                             const wchar_t* canonical_key,
                             const wchar_t* internal_key) {
        return ReadRawValue(path, section, canonical_key).has_value() ||
               ReadRawValue(path, kSection, canonical_key).has_value() ||
               ReadRawValue(path, section, internal_key).has_value() ||
               ReadRawValue(path, kSection, internal_key).has_value();
    };

    return has_key(kGraphicsSection, L"FixFrameRateTimers",
                   L"fix_frame_counted_activity_timer") ||
           has_key(kGraphicsSection, L"FixFrameRateSmoothing",
                   L"fix_frame_counted_smoothing") ||
           has_key(kStabilitySection, L"FixId3TagDivide",
                   L"fix_id3_tag_divide") ||
           has_key(kStabilitySection, L"FixPathfindingDivide",
                   L"fix_pathfinding_divide") ||
           has_key(kStabilitySection, L"FixCompressedMeshDivide",
                   L"fix_compressed_mesh_divide") ||
           has_key(kStabilitySection, L"FixPercentDivide",
                   L"fix_percent_divide") ||
           has_key(kStabilitySection, L"FixElementDivide",
                   L"fix_element_divide") ||
           has_key(kStabilitySection, L"FixObjectCountDivide",
                   L"fix_object_count_divide") ||
           has_key(kStabilitySection, L"FixElementScaleDivide",
                   L"fix_element_scale_divide") ||
           has_key(kStabilitySection, L"FixTableWalkDivide",
                   L"fix_table_walk_divide") ||
           has_key(kStabilitySection, L"FixElementLookupDivide",
                   L"fix_element_lookup_divide") ||
           has_key(kStabilitySection, L"FixCountIndexDivide",
                   L"fix_count_index_divide") ||
           has_key(kStabilitySection, L"FixSlotCountDivide",
                   L"fix_slot_count_divide") ||
           has_key(kStabilitySection, L"FixBucketSizeDivide",
                   L"fix_bucket_size_divide") ||
           has_key(kStabilitySection, L"FixHashMixDivide",
                   L"fix_hash_mix_divide") ||
           ReadRawValue(path, kAdvancedSection,
                        L"hook_dispatch_probe").has_value() ||
           ReadRawValue(path, kSection, L"hook_dispatch_probe").has_value();
}

std::optional<std::wstring> ReadOriginalFogAndNeonValue(
    const std::filesystem::path& path) {
    // v15 made the fog and neon result explicit. Prefer the canonical name,
    // then accept the v13 and v12 public names and the pre-v6 implementation
    // name from their grouped or catch-all locations.
    if (auto value = ReadRawValue(path, kGraphicsSection, L"RestoreOriginalFogAndNeon");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"RestoreOriginalFogAndNeon");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"RestoreOriginalAtmosphere");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"RestoreOriginalAtmosphere");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"RestoreOriginalFog");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"RestoreOriginalFog"); value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"restore_original_fog");
        value.has_value()) {
        return value;
    }
    return ReadRawValue(path, kSection, L"restore_original_fog");
}




std::optional<std::wstring> ReadOriginalShadowFilterValue(
    const std::filesystem::path& path) {
    if (auto value = ReadRawValue(path, kShadowSection, L"OriginalShadowFilter");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"OriginalShadowFilter");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"ShadowFilter");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"ShadowFilter"); value.has_value()) {
        return value;
    }
    return ReadConfiguredValue(path, L"override_shadow_filter");
}

bool HasRetiredShadowSettings(const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kShadowSection, L"RestoreCharacterShadows"},
             std::pair{kGraphicsSection, L"RestoreCharacterShadows"},
             std::pair{kSection, L"RestoreCharacterShadows"},
             std::pair{kShadowSection, L"restore_character_shadow_resolution"},
             std::pair{kGraphicsSection, L"restore_character_shadow_resolution"},
             std::pair{kSection, L"restore_character_shadow_resolution"},
             std::pair{kShadowSection, L"CharacterShadowResolution"},
             std::pair{kGraphicsSection, L"CharacterShadowResolution"},
             std::pair{kSection, L"CharacterShadowResolution"},
             std::pair{kShadowSection, L"character_shadow_resolution"},
             std::pair{kGraphicsSection, L"character_shadow_resolution"},
             std::pair{kSection, L"character_shadow_resolution"},
             std::pair{kShadowSection, L"ShadowFilterScale"},
             std::pair{kSection, L"ShadowFilterScale"},
             std::pair{kShadowSection, L"shadow_filter_scale"},
             std::pair{kSection, L"shadow_filter_scale"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}

bool HasRetiredPcssSettings(const std::filesystem::path& path) {
    for (const wchar_t* section : {kShadowSection, kSection}) {
        for (const wchar_t* key : {
                 L"ShadowFiltering",
                 L"shadow_filtering",
                 L"PCSS",
                 L"PCSSQuality",
                 L"pcss_quality",
                 L"PCSSLightSize",
                 L"pcss_light_size_percent"}) {
            if (ReadRawValue(path, section, key).has_value()) {
                return true;
            }
        }
    }
    return false;
}

std::wstring TrimIniValue(std::wstring value) {
    const std::size_t comment = value.find_first_of(L";#");
    if (comment != std::wstring::npos) {
        value.resize(comment);
    }
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool WriteConfigText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream.good()) {
        return false;
    }
    // Check the buffered write before the caller atomically publishes the
    // temporary file.  Testing good() alone only observes the stream buffer;
    // a close-time flush failure could otherwise replace a valid INI with a
    // truncated one.
    stream.flush();
    if (!stream.good()) {
        return false;
    }
    stream.close();
    return !stream.fail();
}

bool PathExistsNoThrow(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    return !error && exists;
}

std::optional<std::filesystem::path> LocalAppDataDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return std::nullopt;
    }
    std::filesystem::path root(std::wstring(buffer.data(), written));
    if (!root.is_absolute()) {
        return std::nullopt;
    }
    return root.lexically_normal();
}

std::wstring NormalizedConfigPathKey(
    const std::filesystem::path& config_path) {
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::absolute(config_path, error);
    if (error) {
        normalized = config_path;
    }
    std::wstring key = normalized.lexically_normal().native();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    if (!key.empty()) {
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    }
    return key;
}

std::filesystem::path BuildExternalBackupPath(
    const std::filesystem::path& config_path,
    const std::wstring& filename) {
    const auto local_app_data = LocalAppDataDirectory();
    if (!local_app_data.has_value()) {
        return {};
    }

    constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    std::uint64_t hash = kFnvOffset;
    const std::wstring key = NormalizedConfigPathKey(config_path);
    for (const wchar_t character : key) {
        const std::uint16_t unit = static_cast<std::uint16_t>(character);
        hash ^= static_cast<unsigned char>(unit & 0xFFU);
        hash *= kFnvPrime;
        hash ^= static_cast<unsigned char>((unit >> 8U) & 0xFFU);
        hash *= kFnvPrime;
    }

    std::wostringstream directory_name;
    directory_name << std::hex << std::setfill(L'0') << std::setw(16) << hash;
    return *local_app_data / L"SPatch" / L"ConfigBackups" /
           directory_name.str() / filename;
}

bool FilesHaveEqualBytes(const std::filesystem::path& first,
                         const std::filesystem::path& second) {
    std::error_code error;
    const auto first_size = std::filesystem::file_size(first, error);
    if (error) {
        return false;
    }
    const auto second_size = std::filesystem::file_size(second, error);
    if (error || first_size != second_size) {
        return false;
    }

    std::ifstream first_stream(first, std::ios::binary);
    std::ifstream second_stream(second, std::ios::binary);
    if (!first_stream || !second_stream) {
        return false;
    }
    std::array<char, 64 * 1024> first_buffer{};
    std::array<char, 64 * 1024> second_buffer{};
    while (first_stream && second_stream) {
        first_stream.read(first_buffer.data(), first_buffer.size());
        second_stream.read(second_buffer.data(), second_buffer.size());
        const auto first_count = first_stream.gcount();
        const auto second_count = second_stream.gcount();
        if (first_count != second_count ||
            !std::equal(first_buffer.begin(), first_buffer.begin() + first_count,
                        second_buffer.begin())) {
            return false;
        }
    }
    return first_stream.eof() && second_stream.eof();
}

bool WriteBackupCopy(const std::filesystem::path& source,
                     const std::filesystem::path& backup) {
    if (backup.empty()) {
        return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(backup.parent_path(), directory_error);
    if (directory_error) {
        return false;
    }
    const unsigned long serial = g_config_write_serial.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temp_path =
        backup.parent_path() /
        (backup.filename().wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(serial) + L".tmp");
    if (!CopyFileW(source.c_str(), temp_path.c_str(), FALSE)) {
        std::error_code error;
        std::filesystem::remove(temp_path, error);
        return false;
    }
    if (MoveFileExW(temp_path.c_str(),
                    backup.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    std::error_code error;
    std::filesystem::remove(temp_path, error);
    return false;
}

bool RelocateLegacyInGameBackup(const std::filesystem::path& config_path) {
    const std::filesystem::path legacy_path(
        config_path.wstring() + L".previous.bak");
    if (!PathExistsNoThrow(legacy_path)) {
        return true;
    }
    const std::filesystem::path destination =
        LegacyConfigBackupPath(config_path);
    if (!WriteBackupCopy(legacy_path, destination) ||
        !FilesHaveEqualBytes(legacy_path, destination)) {
        return false;
    }
    if (DeleteFileW(legacy_path.c_str()) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

// Versioned, user-facing template. All new files and migrations use concise
// SilentPatch-style names; legacy snake_case keys and old sections remain
// readable through the fallback parser below.
bool WriteOrganizedConfig(const std::filesystem::path& path, const Config& config) {
    const Config defaults{};
    std::ostringstream text;
    text << "; SPatch for Sleeping Dogs: Definitive Edition\n"
         << "; Change a value, save the file, then restart the game.\n"
         << "; For on/off options, 1 enables and 0 disables. Numeric options document their own values.\n\n"
         << "[SPatch]\n"
         << "; Internal format version. Do not change.\n"
         << "ConfigVersion=" << kConfigVersion << "\n\n"
         << "; Master switch for the patch.\n"
         << "Enabled=" << (config.enabled ? 1 : 0) << "\n";
    if (config.allow_unverified_build != defaults.allow_unverified_build) {
        text << "\n; Allow only build-independent settings on an unverified executable.\n"
             << "; Address-based hooks and executable patches remain disabled.\n"
             << "AllowUnverifiedBuild=" << (config.allow_unverified_build ? 1 : 0) << "\n";
    }

    text << "\n[Cutscenes]\n"
         << "; Fix brief 30 FPS/duplicate-frame cutscene timing fallbacks.\n"
         << "FixCutsceneFPS="
         << ((config.fix_cutscene_zero_dt || config.fix_cutscene_scene_time_step) ? 1 : 0)
         << "\n"
         << "; 0 follows live FPS. Any fixed target from 15 to 1000 is also accepted.\n"
         << "; A fixed target should match the cap selected in-game, in the driver, or externally.\n"
         << "CutsceneFPS=" << config.cutscene_fps << "\n\n"
         << "; Prevent duplicate actor restoration after affected cutscenes.\n"
         << "FixDuplicateCutsceneActorRestore="
         << (config.fix_nis_actor_restore_duplicates ? 1 : 0) << "\n\n"
         << "; -1 keeps the game value, 0 disables, 1 enables stock adaptive smoothing.\n"
         << "; The stock smoother always uses at most eight samples.\n"
         << "TimeStepSmoothing=" << config.time_step_smoothing << "\n";

    text << "\n[Input]\n"
         << "; Force the game's hidden Windows Raw Input path for mouse movement.\n"
         << "ForceRawMouseInput=" << (config.force_raw_mouse_input ? 1 : 0) << "\n\n"
         << "; Remove the stock FollowCamera easing from mouse look and aiming.\n"
         << "; Controller look is already sampled directly and has no temporal smoothing.\n"
         << "DisableCameraSmoothing=" << (config.disable_camera_smoothing ? 1 : 0) << "\n\n"
         << "; GTA IV-like camera for exact road-vehicle Drive/Flee paths, including trucks and buses.\n"
         << "; Adds a right-seat offset, loose follow, manual yaw/pitch, delayed recentering, and handbrake swing.\n"
         << "; Race, Hijack, Aim, Look, reverse, and special override paths stay stock.\n"
         << "; 1 enables and 0 disables. Changing this requires a restart.\n"
         << "GTAIVCarCamera=" << (config.gta_iv_car_camera ? 1 : 0) << "\n\n"
         << "; Apply the same GTA IV-like behavior independently to motorcycle/scooter Drive cameras.\n"
         << "; The exact Drive branch must be active even when Race or HijackFront aliases its block.\n"
         << "; Distinct Aim, Flee, look, passenger, and hijack camera blocks remain stock.\n"
         << "; 1 enables and 0 disables. Changing this requires a restart.\n"
         << "GTAIVBikeCamera=" << (config.gta_iv_bike_camera ? 1 : 0) << "\n\n"
         << "; Left-stick radial deadzone in percent: -1 keeps stock, 0 is unfiltered, 1-95 is custom.\n"
         << "LeftStickDeadzone=" << config.controller_left_stick_deadzone << "\n"
         << "; Right-stick radial deadzone in percent: -1 keeps stock, 0 is unfiltered, 1-95 is custom.\n"
         << "RightStickDeadzone=" << config.controller_right_stick_deadzone << "\n";

    text << "\n[Graphics]\n"
         << "; Restore the original game's clearer atmosphere and focused neon glow.\n"
         << "; Disables only DE's added volumetric fog. Changing this requires a restart.\n"
         << "RestoreOriginalFogAndNeon=" << (config.restore_original_fog ? 1 : 0) << "\n\n"
         << "; Restore iris detail and reflections on Wei's affected HD and gang models.\n"
         << "RestoreOriginalEyeReflections="
         << (config.restore_original_eye_reflections ? 1 : 0) << "\n\n"
         << "; Restore rain and post-swim wetness on character materials (Wei and NPCs).\n"
         << "RestoreWetness=" << (config.restore_character_wetness ? 1 : 0) << "\n"
         << "; Seconds characters remain fully wet after leaving water. Range: 0-3600.\n"
         << "WetnessFullTime=" << config.wetness_full_time_seconds << "\n"
         << "; Seconds wetness then takes to fade linearly. Range: 0-3600.\n"
         << "WetnessFadeTime=" << config.wetness_fade_time_seconds << "\n\n"
         << "; Restore sweat from sustained running and combat for Wei and NPCs.\n"
         << "RestoreSweat=" << (config.restore_character_sweat ? 1 : 0) << "\n\n"
         << "; Seconds of sustained exertion needed to reach full sweat. Range: 0-3600.\n"
         << "SweatBuildTime=" << config.sweat_build_time_seconds << "\n"
         << "; Seconds sweat takes to fade after exertion stops. Range: 0-3600.\n"
         << "SweatFadeTime=" << config.sweat_fade_time_seconds << "\n"
         << "; Seconds of continuous exertion before sweat becomes visible. Range: 0-3600.\n"
         << "SweatOnsetTime=" << config.sweat_onset_time_seconds << "\n"
         << "; Horizontal movement speed that counts as running. Range: 0-100.\n"
         << "SweatRunSpeed=" << config.sweat_run_speed << "\n"
         << "; Seconds an NPC remains exerting after taking melee damage. Range: 0-3600.\n"
         << "SweatCombatTime=" << config.sweat_combat_time_seconds << "\n\n"
         << "; Keep the pedestrian-density controller at its intended 30 Hz cadence.\n"
         << "FixPedestrianDensity="
         << (config.fix_pedestrian_density_at_high_fps ? 1 : 0) << "\n\n"
         << "; Preserve verified camera and vehicle-control history at high FPS.\n"
         << "FixHighFPSAverages=" << (config.fix_high_fps_average_windows ? 1 : 0) << "\n\n"
         << "; Scale the hardcoded 1280x640 spherical-reflection target.\n"
         << "ImproveSphericalReflections="
         << (config.improve_spherical_reflections ? 1 : 0) << "\n"
         << "; 0 follows the active display width; custom values are clamped to 1280-4096.\n"
         << "SphericalReflectionWidth=" << config.spherical_reflection_width << "\n\n"
         << "; Report the complete dedicated VRAM amount on the benchmark screen.\n"
         << "FixVRAMReporting=" << (config.fix_vram_capacity_reporting ? 1 : 0) << "\n\n"
         << "; -1 keeps the game setting, 0 is low, 1 medium, 2 high.\n"
         << "TextureDetail=" << config.override_texture_detail_level << "\n\n"
         << "; -1 keeps the game setting, 0 disables, 1 normal, 2 high quality.\n"
         << "MotionBlur=" << config.override_motion_blur << "\n";

    text << "\n[TextureFiltering]\n"
         << "; -1 keeps the game setting; supported forced levels are 4, 8, and 16.\n"
         << "AnisotropicFiltering=" << config.anisotropic_filtering << "\n"
         << "; Also promote the exact stock trilinear filter-selector branch.\n"
         << "ForceAnisotropicFiltering="
         << (config.force_anisotropic_filtering ? 1 : 0) << "\n";

    text << "\n[Shadows]\n"
         << "; Keep the game's native shadow filtering and quality selection.\n"
         << "; Original filtering quality: -1 follows the in-game option, 0 normal, 1 high.\n"
         << "OriginalShadowFilter=" << config.override_shadow_filter << "\n";

    text << "\n[Display]\n"
         << "; Correct the invalid 1920x1880 resolution on a new profile.\n"
         << "FixFirstRunResolution=" << (config.fix_first_run_resolution ? 1 : 0) << "\n\n"
         << "; Use the highest refresh rate exposed for the selected resolution.\n"
         << "UseMaximumRefreshRate=" << (config.prefer_max_refresh_rate ? 1 : 0) << "\n\n"
         << "; Remove the hidden 120 FPS wait when the stock limiter is Off.\n"
         << "; Explicit limiter modes and VSync are unchanged.\n"
         << "Remove120FPSCap=" << (config.remove_hidden_120_fps_cap ? 1 : 0) << "\n\n"
         << "; -1 keeps the game setting, 0 forces windowed, 1 forces fullscreen.\n"
         << "Fullscreen=" << config.override_fullscreen << "\n"
         << "; -1 keeps the game setting, 0 is Off, 1-4 select stock limiter modes.\n"
         << "FrameLimiter=" << config.override_fps_limiter << "\n"
         << "; -1 keeps the game setting, 0 disables, 1 enables.\n"
         << "LowResolutionBuffer=" << config.override_low_res_buffer << "\n";
    if (config.warn_low_res_buffer != defaults.warn_low_res_buffer) {
        text << "; Warn in SPatch.log when the low-resolution buffer remains enabled.\n"
             << "WarnLowResolutionBuffer=" << (config.warn_low_res_buffer ? 1 : 0) << "\n";
    }

    text << "\n[Gameplay]\n"
         << "; -1 keeps the game setting; 0-4 select a stock world-density level.\n"
         << "WorldDensity=" << config.override_world_density << "\n"
         << "; -1 keeps the game setting, 0 disables, 1 enables controller rumble.\n"
         << "Rumble=" << config.override_rumble_enabled << "\n";

    text << "\n[Stability]\n"
         << "; Guard the stock fog update path against invalid slicing intervals.\n"
         << "FixFogSlicing=" << (config.hook_fog_slicing_guard ? 1 : 0) << "\n";
    if (config.min_fog_slicing_interval != defaults.min_fog_slicing_interval) {
        text << "; Smallest accepted fog update interval. Supported range: 1-4.\n"
             << "MinimumFogSlicingInterval=" << config.min_fog_slicing_interval << "\n";
    }
    text << "\n; Keep Scaleform's performance-counter clock full-width on long-running systems.\n"
         << "FixScaleformTimerOverflow=" << (config.fix_scaleform_qpc_clock ? 1 : 0) << "\n\n"
         << "; Open existing files correctly when updating their timestamps.\n"
         << "FixFileTimestampUpdates=" << (config.fix_file_timestamp_open_mode ? 1 : 0) << "\n\n"
         << "; Handle missing audio metadata files without using an invalid mapping handle.\n"
         << "FixAudioFileOpen=" << (config.fix_audio_file_open ? 1 : 0) << "\n\n"
         << "; Preserve complete 64-bit file sizes for files of 4 GiB or larger.\n"
         << "FixLargeFileSizes=" << (config.fix_large_file_sizes ? 1 : 0) << "\n\n"
         << "; Balance the recursive VRAM-pool lock during pool shutdown.\n"
         << "FixVRAMPoolLock=" << (config.fix_vram_pool_lock ? 1 : 0) << "\n\n"
         << "; Recover cleanly from failed resource reads and reject malformed archives,\n"
         << "; compressed data, and resource chunks before they reach unsafe stock paths.\n"
         << "FixResourceLoading=" << (config.fix_resource_loading ? 1 : 0) << "\n\n"
         << "; Remove an unused contact-image formatter that can overflow its stack buffer.\n"
         << "FixContactListOverflow=" << (config.fix_contact_list_overflow ? 1 : 0) << "\n\n"
         << "; Reject truncated save payloads before the stock checksum/table reader.\n"
         << "FixCorruptSaveCrash=" << (config.fix_corrupt_save_handling ? 1 : 0) << "\n\n"
         << "; Translate CreateThread failure to the sentinel expected by the engine.\n"
         << "FixThreadCreationFailure="
         << (config.fix_thread_creation_failure ? 1 : 0) << "\n";

    text << "\n[AntiAliasing]\n"
         << "; Replace the stock post-process anti-aliasing with SMAA.\n"
         << "SMAA=" << ((config.hook_smaa_present && config.smaa_enable) ? 1 : 0) << "\n"
         << "; Stock AA is disabled only after SMAA proves the current render path ready.\n"
         << "; If SMAA cannot run, the game automatically keeps its native AA.\n"
         << "; SMAA quality: 0 low, 1 medium, 2 high, 3 ultra (recommended).\n"
         << "SMAAPreset=" << config.smaa_preset << "\n";

    text << "\n[Debug]\n"
         << "; Write SPatch.log next to the mod for troubleshooting. Disabled by default.\n"
         << "; Logging takes effect after restarting the game.\n"
         << "Logging=" << (config.enable_logging ? 1 : 0) << "\n\n"
         << "; Write a small diagnostic dump if the game crashes.\n"
         << "WriteCrashDumps=" << (config.write_minidumps ? 1 : 0);

    const unsigned long serial = g_config_write_serial.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temp_path =
        path.parent_path() /
        (path.filename().wstring() + L".spatch." + std::to_wstring(GetCurrentProcessId()) +
         L"." + std::to_wstring(serial) + L".tmp");
    if (!WriteConfigText(temp_path, text.str())) {
        std::error_code error;
        std::filesystem::remove(temp_path, error);
        return false;
    }

    const BOOL replaced = MoveFileExW(temp_path.c_str(),
                                      path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!replaced) {
        std::error_code error;
        std::filesystem::remove(temp_path, error);
        return false;
    }
    // The Win32 profile API caches section data per path.  Invalidate that
    // cache after an atomic replacement so the first post-migration read sees
    // the organized file rather than the legacy contents.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

bool ParseBool(const std::optional<std::wstring>& raw, bool fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value == L"1" || _wcsicmp(value.c_str(), L"true") == 0 ||
        _wcsicmp(value.c_str(), L"on") == 0 ||
        _wcsicmp(value.c_str(), L"yes") == 0) {
        return true;
    }
    if (value == L"0" || _wcsicmp(value.c_str(), L"false") == 0 ||
        _wcsicmp(value.c_str(), L"off") == 0 ||
        _wcsicmp(value.c_str(), L"no") == 0) {
        return false;
    }
    return fallback;
}

bool ParseBinaryBool(const std::optional<std::wstring>& raw, bool fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value == L"1") {
        return true;
    }
    if (value == L"0") {
        return false;
    }
    return fallback;
}

int ParseInt(const std::optional<std::wstring>& raw, int fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const long long parsed = std::wcstoll(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
        parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

float ParseFloat(const std::optional<std::wstring>& raw, float fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const float parsed = std::wcstof(value.c_str(), &end);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
        !std::isfinite(parsed)) {
        return fallback;
    }
    return parsed;
}

bool ReadBool(const std::filesystem::path& path, const wchar_t* key, bool fallback) {
    return ParseBool(ReadConfiguredValue(path, key), fallback);
}

int ReadInt(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    return ParseInt(ReadConfiguredValue(path, key), fallback);
}

float ReadFloat(const std::filesystem::path& path, const wchar_t* key, float fallback) {
    return ParseFloat(ReadConfiguredValue(path, key), fallback);
}

unsigned long ReadUInt(const std::filesystem::path& path,
                       const wchar_t* key,
                       unsigned long fallback) {
    const auto raw = ReadConfiguredValue(path, key);
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value.empty() || value.front() == L'-') {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
        parsed > std::numeric_limits<unsigned long>::max()) {
        return fallback;
    }
    return static_cast<unsigned long>(parsed);
}

void RetireLegacyDeveloperControls(Config& config) {
    // Config v6 deliberately removes raw hook/probe/debug switches from the
    // end-user contract. Stable features derive their hook dependencies from
    // the public feature switches, so carrying these implementation controls
    // into the migrated file would recreate invalid combinations and the old
    // developer-facing INI. The untouched source remains in the versioned
    // external config-backup directory.
    config.hook_queue_ready = false;
    config.hook_task_dispatch = false;
    config.hook_wait_helper = false;
    config.hook_scaleform_time = false;
    config.hook_scaleform_init = false;
    config.hook_nis_timing = false;
    config.hook_nis_runtime = false;
    config.hook_nis_owner = false;
    config.hook_nis_actor_state = false;
    config.hook_twitch_probe = false;
    config.hook_frameflow = false;
    config.hook_scenery_builders = false;
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.smaa_debug_keys = false;
    config.hook_post_material_submit = false;
    config.hook_character_regression_probe = false;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
}

}  // namespace

std::filesystem::path ConfigBackupPath(
    const std::filesystem::path& config_path) {
    return BuildExternalBackupPath(
        config_path,
        L"SPatch-pre-v" + std::to_wstring(kConfigVersion) + L".ini");
}

std::filesystem::path LegacyConfigBackupPath(
    const std::filesystem::path& config_path) {
    return BuildExternalBackupPath(config_path, L"SPatch-legacy-previous.ini");
}

const char* ConfigPersistenceStatusName(ConfigPersistenceStatus status) noexcept {
    switch (status) {
        case ConfigPersistenceStatus::Unchanged:
            return "unchanged";
        case ConfigPersistenceStatus::Created:
            return "created";
        case ConfigPersistenceStatus::Migrated:
            return "migrated";
        case ConfigPersistenceStatus::CreateFailed:
            return "create_failed";
        case ConfigPersistenceStatus::BackupFailed:
            return "backup_failed";
        case ConfigPersistenceStatus::MigrationWriteFailed:
            return "migration_write_failed";
        case ConfigPersistenceStatus::SourceChanged:
            return "source_changed";
        case ConfigPersistenceStatus::SourceUnavailable:
            return "source_unavailable";
    }
    return "unknown";
}

Config LoadConfig(const std::filesystem::path& path, ConfigLoadReport* report) {
    std::lock_guard<std::mutex> lock(g_config_write_mutex);
    ConfigLoadReport load_report{};
    load_report.source_file_existed = PathExistsNoThrow(path);
    if (!load_report.source_file_existed) {
        load_report.persistence = WriteOrganizedConfig(path, Config{})
                                      ? ConfigPersistenceStatus::Created
                                      : ConfigPersistenceStatus::CreateFailed;
    }

    // Callers may have edited the file with stdio or an atomic replacement
    // since the last profile-API read.  Drop the Win32 profile cache once per
    // load so tests and real migrations observe the bytes currently on disk.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    FileIdentity source_identity{};
    const bool source_identity_valid =
        ReadFileIdentity(path, source_identity);
    if (load_report.source_file_existed && !source_identity_valid) {
        load_report.persistence = ConfigPersistenceStatus::SourceUnavailable;
    }

    Config config;
    const int config_version = ReadInt(path, L"config_version", 0);
    load_report.source_version = config_version;
    config.enabled = ReadBool(path, L"enabled", config.enabled);
    if (ReadConfiguredValue(path, L"allow_unverified_build").has_value()) {
        config.allow_unverified_build =
            ReadBool(path, L"allow_unverified_build", config.allow_unverified_build);
    } else {
        // Config v5 and earlier used a name that incorrectly implied unsafe
        // fixed-address hooks would be enabled. Preserve the value while
        // migrating it to the truthful build-independent-only control.
        config.allow_unverified_build =
            ReadBool(path, L"allow_unsupported_build", config.allow_unverified_build);
    }
    config.write_minidumps =
        ParseBool(ReadConfiguredValue(path, L"write_minidumps"),
                  config.write_minidumps);
    config.enable_logging = ParseBool(ReadLoggingValue(path), config.enable_logging);
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
    config.hook_nis_actor_state =
        ReadBool(path, L"hook_nis_actor_state", config.hook_nis_actor_state);
    config.hook_twitch_probe =
        ReadBool(path, L"hook_twitch_probe", config.hook_twitch_probe);
    config.hook_frameflow =
        ReadBool(path, L"hook_frameflow", config.hook_frameflow);
    config.fix_cutscene_zero_dt =
        ReadBool(path, L"fix_cutscene_zero_dt", config.fix_cutscene_zero_dt);
    config.fix_cutscene_scene_time_step =
        ReadBool(path, L"fix_cutscene_scene_time_step", config.fix_cutscene_scene_time_step);
    config.fix_nis_actor_restore_duplicates = ReadBool(
        path, L"fix_nis_actor_restore_duplicates", config.fix_nis_actor_restore_duplicates);
    const bool has_time_step_smoothing =
        ReadConfiguredValue(path, L"time_step_smoothing").has_value();
    if (has_time_step_smoothing) {
        config.time_step_smoothing =
            ReadInt(path, L"time_step_smoothing", config.time_step_smoothing);
    } else {
        const bool legacy_disable =
            ReadBool(path, L"disable_time_step_smoothing", false);
        const bool has_legacy_frames =
            ReadConfiguredValue(path, L"time_step_smoothing_frames").has_value();
        if (legacy_disable) {
            config.time_step_smoothing = 0;
        } else if (has_legacy_frames) {
            const int legacy_frames = ReadInt(path, L"time_step_smoothing_frames", 2);
            config.time_step_smoothing = legacy_frames < 0 ? -1 : (legacy_frames < 2 ? 0 : 1);
        }
    }
    config.hook_scenery_builders =
        ReadBool(path, L"hook_scenery_builders", config.hook_scenery_builders);
    config.hook_fog_slicing_guard =
        ReadBool(path, L"hook_fog_slicing_guard", config.hook_fog_slicing_guard);
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
    // SMAA is a complete replacement. Keep hook ownership and stock-AA
    // suppression derived from the single public switch so end users cannot
    // accidentally stack both post-process AA implementations.
    config.hook_smaa_present = config.smaa_enable;
    config.smaa_disable_stock_aa = config.smaa_enable;
    config.hook_post_material_submit =
        ReadBool(path, L"hook_post_material_submit", config.hook_post_material_submit);
    config.hook_character_regression_probe =
        ReadBool(path, L"hook_character_regression_probe", config.hook_character_regression_probe);
    config.force_raw_mouse_input =
        ReadBool(path, L"force_raw_mouse_input", config.force_raw_mouse_input);
    config.disable_camera_smoothing =
        ReadBool(path, L"disable_camera_smoothing", config.disable_camera_smoothing);
    config.gta_iv_car_camera = ParseBinaryBool(
        ReadConfiguredValue(path, L"gta_iv_car_camera"),
        config.gta_iv_car_camera);
    config.gta_iv_bike_camera = ParseBinaryBool(
        ReadConfiguredValue(path, L"gta_iv_bike_camera"),
        config.gta_iv_bike_camera);
    config.controller_left_stick_deadzone = ReadInt(
        path, L"controller_left_stick_deadzone", config.controller_left_stick_deadzone);
    config.controller_right_stick_deadzone = ReadInt(
        path, L"controller_right_stick_deadzone", config.controller_right_stick_deadzone);
    config.restore_original_fog =
        ParseBool(ReadOriginalFogAndNeonValue(path), config.restore_original_fog);
    config.restore_original_eye_reflections = ReadBool(
        path, L"restore_original_eye_reflections", config.restore_original_eye_reflections);
    config.restore_character_wetness =
        ReadBool(path, L"restore_character_wetness", config.restore_character_wetness);
    config.restore_character_sweat =
        ReadBool(path, L"restore_character_sweat", config.restore_character_sweat);
    config.wetness_full_time_seconds =
        ReadInt(path, L"wetness_full_time_seconds", config.wetness_full_time_seconds);
    config.wetness_fade_time_seconds =
        ReadInt(path, L"wetness_fade_time_seconds", config.wetness_fade_time_seconds);
    config.sweat_build_time_seconds =
        ReadInt(path, L"sweat_build_time_seconds", config.sweat_build_time_seconds);
    config.sweat_fade_time_seconds =
        ReadInt(path, L"sweat_fade_time_seconds", config.sweat_fade_time_seconds);
    config.sweat_onset_time_seconds =
        ReadInt(path, L"sweat_onset_time_seconds", config.sweat_onset_time_seconds);
    config.sweat_run_speed =
        ReadFloat(path, L"sweat_run_speed", config.sweat_run_speed);
    config.sweat_combat_time_seconds =
        ReadInt(path, L"sweat_combat_time_seconds", config.sweat_combat_time_seconds);
    const bool obsolete_shadow_settings = HasRetiredShadowSettings(path);
    config.fix_pedestrian_density_at_high_fps = ReadBool(
        path,
        L"fix_pedestrian_density_at_high_fps",
        config.fix_pedestrian_density_at_high_fps);
    config.fix_high_fps_average_windows = ReadBool(
        path, L"fix_high_fps_average_windows", config.fix_high_fps_average_windows);
    config.improve_spherical_reflections = ReadBool(
        path, L"improve_spherical_reflections", config.improve_spherical_reflections);
    config.fix_vram_capacity_reporting = ReadBool(
        path, L"fix_vram_capacity_reporting", config.fix_vram_capacity_reporting);
    config.remove_hidden_120_fps_cap =
        ReadBool(path, L"remove_hidden_120_fps_cap", config.remove_hidden_120_fps_cap);
    config.fix_first_run_resolution =
        ReadBool(path, L"fix_first_run_resolution", config.fix_first_run_resolution);
    config.fix_scaleform_qpc_clock =
        ReadBool(path, L"fix_scaleform_qpc_clock", config.fix_scaleform_qpc_clock);
    config.fix_file_timestamp_open_mode = ReadBool(
        path, L"fix_file_timestamp_open_mode", config.fix_file_timestamp_open_mode);
    config.fix_audio_file_open =
        ReadBool(path, L"fix_audio_file_open", config.fix_audio_file_open);
    config.fix_large_file_sizes =
        ReadBool(path, L"fix_large_file_sizes", config.fix_large_file_sizes);
    config.fix_vram_pool_lock =
        ReadBool(path, L"fix_vram_pool_lock", config.fix_vram_pool_lock);
    config.fix_resource_loading =
        ReadBool(path, L"fix_resource_loading", config.fix_resource_loading);
    config.fix_contact_list_overflow = ReadBool(
        path, L"fix_contact_list_overflow", config.fix_contact_list_overflow);
    config.fix_corrupt_save_handling =
        ReadBool(path, L"fix_corrupt_save_handling", config.fix_corrupt_save_handling);
    config.fix_thread_creation_failure =
        ReadBool(path, L"fix_thread_creation_failure", config.fix_thread_creation_failure);
    config.warn_low_res_buffer =
        ReadBool(path, L"warn_low_res_buffer", config.warn_low_res_buffer);
    config.min_fog_slicing_interval =
        ReadInt(path, L"min_fog_slicing_interval", config.min_fog_slicing_interval);
    config.override_low_res_buffer =
        ReadInt(path, L"override_low_res_buffer", config.override_low_res_buffer);
    config.override_shadow_filter = ParseInt(
        ReadOriginalShadowFilterValue(path), config.override_shadow_filter);
    config.override_fps_limiter =
        ReadInt(path, L"override_fps_limiter", config.override_fps_limiter);
    config.override_texture_detail_level =
        ReadInt(path, L"override_texture_detail_level", config.override_texture_detail_level);
    const auto anisotropic_filtering = ReadTextureFilteringValue(
        path, L"AnisotropicFiltering", L"anisotropic_filtering");
    if (anisotropic_filtering.has_value()) {
        const int parsed = ParseInt(anisotropic_filtering, -1);
        config.anisotropic_filtering =
            parsed == -1 || parsed == 4 || parsed == 8 || parsed == 16
                ? parsed
                : -1;
    }
    config.force_anisotropic_filtering = ParseBool(
        ReadTextureFilteringValue(path,
                                  L"ForceAnisotropicFiltering",
                                  L"force_anisotropic_filtering"),
        config.force_anisotropic_filtering);
    config.override_motion_blur =
        ReadInt(path, L"override_motion_blur", config.override_motion_blur);
    const bool obsolete_renderer_keys =
        ReadRendererValue(path, L"RendererBackend", L"renderer_backend").has_value() ||
        ReadRendererValue(path, L"SwapChainFlipModel",
                          L"swap_chain_flip_model").has_value() ||
        ReadRendererValue(path, L"SwapChainTearing",
                          L"swap_chain_tearing").has_value() ||
        ReadRendererValue(path, L"SwapChainFrameLatency",
                          L"swap_chain_frame_latency").has_value();
    const bool obsolete_unverified_keys =
        HasRetiredUnverifiedSettings(path);
    config.override_world_density =
        ReadInt(path, L"override_world_density", config.override_world_density);
    config.prefer_max_refresh_rate =
        ReadBool(path, L"prefer_max_refresh_rate", config.prefer_max_refresh_rate);
    config.override_fullscreen =
        ReadInt(path, L"override_fullscreen", config.override_fullscreen);
    config.override_rumble_enabled =
        ReadInt(path, L"override_rumble_enabled", config.override_rumble_enabled);
    config.spherical_reflection_width =
        ReadInt(path, L"spherical_reflection_width", config.spherical_reflection_width);
    config.aa_variant_mode =
        ReadInt(path, L"aa_variant_mode", config.aa_variant_mode);
    config.aa_aux_mode =
        ReadInt(path, L"aa_aux_mode", config.aa_aux_mode);
    config.smaa_preset =
        ReadInt(path, L"smaa_preset", config.smaa_preset);
    config.summary_interval_ms =
        ReadUInt(path, L"summary_interval_ms", config.summary_interval_ms);
    config.max_verbose_events =
        ReadUInt(path, L"max_verbose_events", config.max_verbose_events);
    config.max_unique_callbacks =
        ReadUInt(path, L"max_unique_callbacks", config.max_unique_callbacks);
    config.cutscene_fps = ReadInt(path, L"cutscene_fps", config.cutscene_fps);

    if (config.summary_interval_ms > kMaxSummaryIntervalMs) {
        config.summary_interval_ms = kMaxSummaryIntervalMs;
    }
    if (config.max_verbose_events > kMaxVerboseEvents) {
        config.max_verbose_events = kMaxVerboseEvents;
    }
    if (config.max_unique_callbacks > kMaxUniqueCallbacks) {
        config.max_unique_callbacks = kMaxUniqueCallbacks;
    }

    if (config.wetness_full_time_seconds < kWetnessTimeMinSeconds) {
        config.wetness_full_time_seconds = kWetnessTimeMinSeconds;
    } else if (config.wetness_full_time_seconds > kWetnessTimeMaxSeconds) {
        config.wetness_full_time_seconds = kWetnessTimeMaxSeconds;
    }
    if (config.wetness_fade_time_seconds < kWetnessTimeMinSeconds) {
        config.wetness_fade_time_seconds = kWetnessTimeMinSeconds;
    } else if (config.wetness_fade_time_seconds > kWetnessTimeMaxSeconds) {
        config.wetness_fade_time_seconds = kWetnessTimeMaxSeconds;
    }
    if (config.sweat_build_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_build_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_build_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_build_time_seconds = kSweatTimeMaxSeconds;
    }
    if (config.sweat_fade_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_fade_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_fade_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_fade_time_seconds = kSweatTimeMaxSeconds;
    }
    if (config.sweat_onset_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_onset_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_onset_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_onset_time_seconds = kSweatTimeMaxSeconds;
    }
    if (!std::isfinite(config.sweat_run_speed) ||
        config.sweat_run_speed < kSweatRunSpeedMin) {
        config.sweat_run_speed = kSweatRunSpeedMin;
    } else if (config.sweat_run_speed > kSweatRunSpeedMax) {
        config.sweat_run_speed = kSweatRunSpeedMax;
    }
    if (config.sweat_combat_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_combat_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_combat_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_combat_time_seconds = kSweatTimeMaxSeconds;
    }

    const bool obsolete_pcss_keys = HasRetiredPcssSettings(path);
    const bool migrated_retired_renderer_keys =
        HasMigratedRetiredRendererSettings(path);
    const bool obsolete_logging_keys = HasRetiredLoggingSettings(path);
    const bool obsolete_crash_dump_location =
        HasRetiredCrashDumpLocation(path);
    const bool obsolete_texture_filtering_location =
        HasRetiredTextureFilteringLocation(path);
    const bool legacy_in_game_backup = PathExistsNoThrow(
        std::filesystem::path(path.wstring() + L".previous.bak"));
    const bool obsolete_visual_damage_keys =
        HasRetiredVisualDamageSettings(path);
    if (config_version < kConfigVersion ||
        obsolete_pcss_keys ||
        migrated_retired_renderer_keys ||
        obsolete_logging_keys ||
        obsolete_crash_dump_location ||
        obsolete_texture_filtering_location ||
        legacy_in_game_backup ||
        obsolete_visual_damage_keys ||
        obsolete_renderer_keys ||
        obsolete_unverified_keys ||
        obsolete_shadow_settings) {
        RetireLegacyDeveloperControls(config);
    }

#if defined(SPATCH_FINAL_RELEASE)
    config.hook_queue_ready = false;
    config.hook_task_dispatch = false;
    config.hook_wait_helper = false;
    config.hook_scaleform_time = false;
    config.hook_scaleform_init = false;
    config.hook_nis_timing = false;
    config.hook_nis_runtime = false;
    config.hook_nis_owner = false;
    config.hook_nis_actor_state = false;
    config.hook_twitch_probe = false;
    config.hook_frameflow = false;
    config.hook_scenery_builders = false;
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.smaa_debug_keys = false;
    config.hook_post_material_submit = false;
    config.hook_character_regression_probe = false;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
    config.summary_interval_ms = 0;
    config.max_verbose_events = 0;
    config.max_unique_callbacks = 0;
#endif

    if (config.time_step_smoothing < -1) {
        config.time_step_smoothing = -1;
    } else if (config.time_step_smoothing > 1) {
        config.time_step_smoothing = 1;
    }

    config.controller_left_stick_deadzone =
        input::ClampDeadzonePercent(config.controller_left_stick_deadzone);
    config.controller_right_stick_deadzone =
        input::ClampDeadzonePercent(config.controller_right_stick_deadzone);

    if (config.min_fog_slicing_interval < 1) {
        config.min_fog_slicing_interval = 1;
    } else if (config.min_fog_slicing_interval > 4) {
        config.min_fog_slicing_interval = 4;
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

    if (config.override_motion_blur < -1) {
        config.override_motion_blur = -1;
    } else if (config.override_motion_blur > 2) {
        config.override_motion_blur = 2;
    }

    if (config.override_world_density < -1) {
        config.override_world_density = -1;
    } else if (config.override_world_density > 4) {
        config.override_world_density = 4;
    }

    if (config.override_fullscreen < -1) {
        config.override_fullscreen = -1;
    } else if (config.override_fullscreen > 1) {
        config.override_fullscreen = 1;
    }

    if (config.override_rumble_enabled < -1) {
        config.override_rumble_enabled = -1;
    } else if (config.override_rumble_enabled > 1) {
        config.override_rumble_enabled = 1;
    }

    if (config.spherical_reflection_width < kSphericalReflectionWidthAuto) {
        config.spherical_reflection_width = kSphericalReflectionWidthAuto;
    } else if (config.spherical_reflection_width > kSphericalReflectionWidthAuto &&
               config.spherical_reflection_width < kSphericalReflectionWidthMin) {
        config.spherical_reflection_width = kSphericalReflectionWidthMin;
    } else if (config.spherical_reflection_width > kSphericalReflectionWidthMax) {
        config.spherical_reflection_width = kSphericalReflectionWidthMax;
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

    if (config.cutscene_fps < kCutsceneFpsAuto) {
        config.cutscene_fps = kCutsceneFpsAuto;
    } else if (config.cutscene_fps > kCutsceneFpsMax) {
        config.cutscene_fps = kCutsceneFpsMax;
    } else if (config.cutscene_fps > kCutsceneFpsAuto &&
               config.cutscene_fps < kCutsceneFpsMin) {
        config.cutscene_fps = kCutsceneFpsMin;
    }

    if (load_report.persistence != ConfigPersistenceStatus::CreateFailed &&
        load_report.persistence != ConfigPersistenceStatus::SourceUnavailable &&
        (config_version < kConfigVersion ||
         obsolete_pcss_keys ||
         migrated_retired_renderer_keys ||
         obsolete_logging_keys ||
         obsolete_crash_dump_location ||
         obsolete_texture_filtering_location ||
         legacy_in_game_backup ||
         obsolete_visual_damage_keys ||
         obsolete_renderer_keys ||
         obsolete_unverified_keys ||
         obsolete_shadow_settings)) {
        const auto backup_path = ConfigBackupPath(path);
        // Never rewrite a user's config unless the exact file being replaced
        // has first become the durable current backup.
        FileIdentity current_identity{};
        if (!source_identity_valid ||
            !ReadFileIdentity(path, current_identity) ||
            current_identity != source_identity) {
            load_report.persistence = ConfigPersistenceStatus::SourceChanged;
        } else if (!WriteBackupCopy(path, backup_path) ||
                   !FilesHaveEqualBytes(path, backup_path)) {
            load_report.persistence = ConfigPersistenceStatus::BackupFailed;
        } else if (!ReadFileIdentity(path, current_identity) ||
                   current_identity != source_identity) {
            load_report.persistence = ConfigPersistenceStatus::SourceChanged;
        } else if (!RelocateLegacyInGameBackup(path)) {
            load_report.persistence = ConfigPersistenceStatus::BackupFailed;
        } else if (!WriteOrganizedConfig(path, config)) {
            load_report.persistence = ConfigPersistenceStatus::MigrationWriteFailed;
        } else {
            load_report.persistence = ConfigPersistenceStatus::Migrated;
        }
    }

    if (report != nullptr) {
        *report = load_report;
    }
    return config;
}

bool WriteBoolValue(const std::filesystem::path& path, const wchar_t* key, bool value) {
    std::lock_guard<std::mutex> lock(g_config_write_mutex);
    if (key == nullptr || *key == L'\0' ||
        (!PathExistsNoThrow(path) && !WriteOrganizedConfig(path, Config{}))) {
        return false;
    }
    const IniKeyAlias* alias = FindIniKeyAlias(key);
    const wchar_t* section = alias != nullptr ? alias->section : SectionForKey(key);
    const wchar_t* output_key = alias != nullptr ? alias->user_key : key;
    return WritePrivateProfileStringW(section, output_key, value ? L"1" : L"0", path.c_str()) != 0;
}

bool WriteIntValue(const std::filesystem::path& path, const wchar_t* key, int value) {
    std::lock_guard<std::mutex> lock(g_config_write_mutex);
    if (key == nullptr || *key == L'\0' ||
        (!PathExistsNoThrow(path) && !WriteOrganizedConfig(path, Config{}))) {
        return false;
    }
    wchar_t buffer[32]{};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d", value);
    const IniKeyAlias* alias = FindIniKeyAlias(key);
    const wchar_t* section = alias != nullptr ? alias->section : SectionForKey(key);
    const wchar_t* output_key = alias != nullptr ? alias->user_key : key;
    return WritePrivateProfileStringW(section, output_key, buffer, path.c_str()) != 0;
}

}  // namespace spatch
