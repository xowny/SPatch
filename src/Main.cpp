#include "Config.h"
#include "DisplaySettings.h"
#include "Hooks.h"
#include "Logger.h"
#include "VersionGuard.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace {

HMODULE g_module = nullptr;
spatch::Config g_config{};
std::filesystem::path g_module_directory;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_exception_filter = nullptr;
std::atomic<bool> g_bootstrap_finished = false;
std::atomic<bool> g_detaching = false;

void ApplySafeCompatibilityProfile(spatch::Config& config) {
    config.hook_queue_ready = false;
    config.hook_task_dispatch = false;
    config.hook_wait_helper = false;
    config.hook_scaleform_time = false;
    config.hook_scaleform_init = false;
    config.hook_nis_timing = false;
    config.hook_nis_runtime = false;
    config.hook_nis_owner = false;
    config.hook_frameflow = false;
    config.hook_scenery_builders = false;
    config.hook_fog_slicing_guard = false;
    config.hook_ao_stage = false;
    config.hook_ao_material = false;
    config.hook_ao_compute = false;
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.hook_smaa_present = false;
    config.smaa_disable_stock_aa = false;
    config.hook_post_material_submit = false;
    config.hook_aces_final = false;
    config.hook_aces_presentbuffer = false;
    config.hook_aces_display_curve = false;
    config.hook_vram_adapter_fix = false;
    config.hook_character_regression_probe = false;
    config.fix_cutscene_zero_dt = false;
    config.fix_cutscene_scene_time_step = false;
    config.smaa_enable = false;
    config.smaa_debug_keys = false;
    config.restore_character_visual_damage = false;
    config.aces_enable = false;
    config.aces_debug_keys = false;
    config.probe_cut_content_slices = false;
    config.force_cut_content_candidate = -1;
    config.force_cut_content_simulate_rewards = false;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
}

void ApplyLatestSteamCompatibilityProfile(spatch::Config& config) {
    // Latest Steam build keeps the broader character regression probe path off,
    // but the blood/bruising restore itself is allowed to run on the slimmer
    // direct-call path in Hooks.cpp.
    config.hook_character_regression_probe = false;
}

std::wstring GetModulePath(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }

        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path MakeTimestampedDumpPath() {
    SYSTEMTIME system_time{};
    GetLocalTime(&system_time);

    wchar_t file_name[128]{};
    swprintf_s(file_name,
               L"SPatch-%04u%02u%02u-%02u%02u%02u-%03u.dmp",
               system_time.wYear,
               system_time.wMonth,
               system_time.wDay,
               system_time.wHour,
               system_time.wMinute,
               system_time.wSecond,
               system_time.wMilliseconds);

    return g_module_directory / file_name;
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exception_pointers) {
    if (!g_config.write_minidumps || g_module_directory.empty()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto dump_path = MakeTimestampedDumpPath();
    HANDLE file = CreateFileW(dump_path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        spatch::log::ErrorF("failed to create minidump file path=%s",
                            spatch::log::ToUtf8(dump_path.c_str()).c_str());
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception_pointers;
    info.ClientPointers = FALSE;

    const BOOL wrote = MiniDumpWriteDump(GetCurrentProcess(),
                                         GetCurrentProcessId(),
                                         file,
                                         MiniDumpNormal,
                                         &info,
                                         nullptr,
                                         nullptr);
    CloseHandle(file);

    if (wrote) {
        spatch::log::ErrorF("wrote minidump path=%s",
                            spatch::log::ToUtf8(dump_path.c_str()).c_str());
    } else {
        spatch::log::ErrorF("MiniDumpWriteDump failed path=%s error=%lu",
                            spatch::log::ToUtf8(dump_path.c_str()).c_str(),
                            GetLastError());
    }

    if (g_previous_exception_filter != nullptr &&
        g_previous_exception_filter != &TopLevelExceptionFilter) {
        return g_previous_exception_filter(exception_pointers);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI BootstrapThread(void*) {
    struct BootstrapFinishGuard {
        ~BootstrapFinishGuard() {
            g_bootstrap_finished.store(true);
        }
    } finish_guard;

    if (g_detaching.load()) {
        return 0;
    }

    const std::filesystem::path module_path = GetModulePath(g_module);
    g_module_directory = module_path.parent_path();

    const std::filesystem::path log_path = g_module_directory / "SPatch.log";
    if (!spatch::log::Initialize(log_path)) {
        return 0;
    }

    spatch::log::Info("SPatch bootstrap starting");

    const std::filesystem::path config_path = g_module_directory / "SPatch.ini";
    g_config = spatch::LoadConfig(config_path);

    spatch::log::InfoF(
        "config enabled=%d allow_unsupported_build=%d write_minidumps=%d "
        "cutscene_fix=%d/%d time_step_smoothing_disable=%d smoothing_frames=%d "
        "fog_guard=%d warn_low_res_buffer=%d blood_restore=%d "
        "shadow_filter=%d ssao=%d fps_limiter=%d texture_detail=%d world_density=%d low_res_buffer=%d rumble=%d "
        "vram_fix=%d vram_override_mb=%d "
        "smaa=%d preset=%d stock_aa_bypass=%d "
        "aces=%d strength=%d exposure_scale=%d",
        g_config.enabled,
        g_config.allow_unsupported_build,
        g_config.write_minidumps,
        g_config.fix_cutscene_zero_dt,
        g_config.fix_cutscene_scene_time_step,
        g_config.disable_time_step_smoothing,
        g_config.time_step_smoothing_frames,
        g_config.hook_fog_slicing_guard,
        g_config.warn_low_res_buffer,
        g_config.restore_character_visual_damage,
        g_config.override_shadow_filter,
        g_config.override_ssao,
        g_config.override_fps_limiter,
        g_config.override_texture_detail_level,
        g_config.override_world_density,
        g_config.override_low_res_buffer,
        g_config.override_rumble_enabled,
        g_config.hook_vram_adapter_fix,
        g_config.vram_override_mb,
        g_config.smaa_enable,
        g_config.smaa_preset,
        g_config.smaa_disable_stock_aa,
        g_config.aces_enable,
        g_config.aces_strength_percent,
        g_config.aces_exposure_scale_percent);

    if (g_config.write_minidumps) {
        g_previous_exception_filter = SetUnhandledExceptionFilter(&TopLevelExceptionFilter);
    }

    const spatch::BuildCheckResult build = spatch::InspectLoadedGame();
    spatch::log::Info(build.summary);

    if (g_detaching.load()) {
        return 0;
    }

    if (!g_config.enabled) {
        spatch::log::Warn("SPatch disabled by config");
        return 0;
    }

    spatch::ApplyDisplaySettingsPatches(g_module_directory / "data" / "DisplaySettings.xml",
                                        g_config);

    if (!build.supported && !g_config.allow_unsupported_build) {
        spatch::log::Warn("unsupported game build; hooks not installed");
        return 0;
    }

    if (build.supported && !build.hook_layout_supported) {
        ApplySafeCompatibilityProfile(g_config);
        spatch::log::WarnF(
            "recognized build running in safe compatibility mode build_id=%s; hook-based features disabled until this executable is remapped",
            build.build_id.c_str());
    }

    if (build.build_id == "latest_steam") {
        ApplyLatestSteamCompatibilityProfile(g_config);
        spatch::log::Warn(
            "latest_steam compatibility profile active; character regression probes disabled, slim visual-damage restore path enabled");
    }

    if (g_detaching.load()) {
        return 0;
    }

    if (!spatch::hooks::Initialize(g_config, config_path, build.build_id)) {
        spatch::log::Error("hook initialization failed");
        return 0;
    }

    spatch::log::Info("SPatch initialized");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        g_detaching.store(false);
        g_bootstrap_finished.store(false);
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &BootstrapThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        g_detaching.store(true);
        if (g_bootstrap_finished.load()) {
            spatch::hooks::Shutdown();
            spatch::log::Shutdown();
        }
    }

    return TRUE;
}
