#include "BootstrapPolicy.h"
#include "Config.h"
#include "DisplaySettings.h"
#include "Hooks.h"
#include "Logger.h"
#include "SystemLibrary.h"
#include "VersionGuard.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE,
                                          DWORD,
                                          HANDLE,
                                          MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

HMODULE g_module = nullptr;
spatch::Config g_config{};
std::filesystem::path g_module_directory;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous_exception_filter = nullptr;
std::atomic<bool> g_crash_filter_ready = false;
HMODULE g_dbghelp_module = nullptr;
MiniDumpWriteDumpFn g_mini_dump_write_dump = nullptr;
std::atomic<bool> g_minidumps_enabled = false;
std::atomic_flag g_dump_in_progress = ATOMIC_FLAG_INIT;
std::atomic<unsigned long> g_dump_serial = 0;
std::atomic<bool> g_detaching = false;
constexpr std::size_t kDumpPathCapacity = 32768;
std::array<wchar_t, kDumpPathCapacity> g_dump_path_buffer{};

void AbortNeverStartedBootstrapThread(HANDLE thread) noexcept {
    if (thread == nullptr) {
        return;
    }
    // The handle is created suspended and ResumeThread has never succeeded,
    // so no user code, TLS, or CRT cleanup can have run. Terminating this
    // never-started bootstrap object is the only way to avoid a detached start
    // address after DllMain rejects the load.
#pragma warning(suppress : 6258)
    (void)TerminateThread(thread, ERROR_DLL_INIT_FAILED);
}

std::wstring GetModulePath(HMODULE module) {
    try {
        std::wstring buffer(MAX_PATH, L'\0');

        for (;;) {
            const DWORD length =
                GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return {};
            }

            if (length < buffer.size()) {
                buffer.resize(length);
                return buffer;
            }
            // Windows paths are bounded well below this value in normal use;
            // keep a corrupt/hostile loader result from growing the bootstrap
            // string without limit.
            if (buffer.size() >= 32768 / 2) {
                return {};
            }
            buffer.resize(buffer.size() * 2);
        }
    } catch (...) {
        return {};
    }
}

bool FormatTimestampedDumpPath(unsigned long serial) {
    SYSTEMTIME system_time{};
    GetLocalTime(&system_time);

    const int written = _snwprintf_s(g_dump_path_buffer.data(),
                                     g_dump_path_buffer.size(),
                                     _TRUNCATE,
                                     L"%ls\\SPatch-%04u%02u%02u-%02u%02u%02u-%03u-p%lu-%lu.dmp",
                                     g_module_directory.c_str(),
                                     system_time.wYear,
                                     system_time.wMonth,
                                     system_time.wDay,
                                     system_time.wHour,
                                     system_time.wMinute,
                                     system_time.wSecond,
                                     system_time.wMilliseconds,
                                     GetCurrentProcessId(),
                                     serial);
    return written > 0;
}

std::string Sha256Hex(
    const std::array<unsigned char, 32>& hash) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (const unsigned char byte : hash) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

void LogModuleIdentity(
    const std::filesystem::path& module_path,
    bool logging_enabled) noexcept {
    if (!logging_enabled) {
        return;
    }
    try {
        std::error_code size_error;
        const std::uintmax_t file_size =
            std::filesystem::file_size(module_path, size_error);
        std::array<unsigned char, 32> sha256{};
        const bool hash_computed =
            spatch::ComputeSha256FromPath(module_path, sha256);
        spatch::log::InfoF(
            "module_identity path=%s file_size=%s sha256=%s",
            spatch::log::ToUtf8(module_path.wstring()).c_str(),
            size_error ? "unavailable" : std::to_string(file_size).c_str(),
            hash_computed ? Sha256Hex(sha256).c_str() : "unavailable");
    } catch (...) {
        spatch::log::Warn(
            "module_identity unavailable because diagnostics formatting failed");
    }
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exception_pointers);

LONG ContinueExceptionSearch(EXCEPTION_POINTERS* exception_pointers) {
    if (!g_crash_filter_ready.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER previous =
        g_previous_exception_filter.load(std::memory_order_acquire);
    if (previous != nullptr && previous != &TopLevelExceptionFilter) {
        return previous(exception_pointers);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exception_pointers) {
    if (!g_minidumps_enabled.load(std::memory_order_acquire) ||
        g_mini_dump_write_dump == nullptr || g_module_directory.empty()) {
        return ContinueExceptionSearch(exception_pointers);
    }
    if (g_dump_in_progress.test_and_set(std::memory_order_acquire)) {
        return ContinueExceptionSearch(exception_pointers);
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    constexpr unsigned int kMaximumNameAttempts = 32;
    for (unsigned int attempt = 0; attempt < kMaximumNameAttempts; ++attempt) {
        const unsigned long serial =
            g_dump_serial.fetch_add(1, std::memory_order_relaxed);
        if (!FormatTimestampedDumpPath(serial)) {
            break;
        }
        file = CreateFileW(g_dump_path_buffer.data(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        const DWORD create_error = GetLastError();
        if (file != INVALID_HANDLE_VALUE ||
            (create_error != ERROR_FILE_EXISTS &&
             create_error != ERROR_ALREADY_EXISTS)) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        g_dump_in_progress.clear(std::memory_order_release);
        return ContinueExceptionSearch(exception_pointers);
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception_pointers;
    info.ClientPointers = FALSE;

    const BOOL dump_written = g_mini_dump_write_dump(GetCurrentProcess(),
                                                     GetCurrentProcessId(),
                                                     file,
                                                     MiniDumpNormal,
                                                     &info,
                                                     nullptr,
                                                     nullptr);
    CloseHandle(file);
    if (!dump_written) {
        DeleteFileW(g_dump_path_buffer.data());
    }
    g_dump_in_progress.clear(std::memory_order_release);
    return ContinueExceptionSearch(exception_pointers);
}

DWORD BootstrapMain() {
    if (g_detaching.load()) {
        return 0;
    }

    const std::filesystem::path module_path = GetModulePath(g_module);
    if (!spatch::IsUsableBootstrapModulePath(module_path)) {
        OutputDebugStringA(
            "SPatch bootstrap aborted: module path is empty or not absolute\n");
        return ERROR_BAD_PATHNAME;
    }
    g_module_directory = module_path.parent_path();

    const std::filesystem::path config_path = g_module_directory / "SPatch.ini";
    spatch::ConfigLoadReport config_report{};
    g_config = spatch::LoadConfig(config_path, &config_report);

    const std::filesystem::path log_path = g_module_directory / "SPatch.log";
    if (!spatch::log::Initialize(log_path, g_config.enable_logging)) {
        // Diagnostics must never become a prerequisite for the patch itself.
        OutputDebugStringA("SPatch could not open SPatch.log; continuing without file logging\n");
    }

    LogModuleIdentity(module_path, g_config.enable_logging);
    spatch::log::Info("SPatch bootstrap starting");

    spatch::log::InfoF(
        "config core enabled=%d allow_unverified=%d crash_dumps=%d logging=%d "
        "source_version=%d schema_version=%d persistence=%s",
        g_config.enabled ? 1 : 0,
        g_config.allow_unverified_build ? 1 : 0,
        g_config.write_minidumps ? 1 : 0,
        g_config.enable_logging ? 1 : 0,
        config_report.source_version,
        spatch::kConfigVersion,
        spatch::ConfigPersistenceStatusName(config_report.persistence));
    if (!config_report.persistence_succeeded()) {
        spatch::log::WarnF("configuration persistence failed status=%s path=%s; "
                           "using the values that could be read without replacing the file",
                           spatch::ConfigPersistenceStatusName(config_report.persistence),
                           spatch::log::ToUtf8(config_path.wstring()).c_str());
    }
    spatch::log::InfoF(
        "requested_config timing cutscene_fix=%d cutscene_fps=%d smoothing=%d fog_guard=%d",
        (g_config.fix_cutscene_zero_dt || g_config.fix_cutscene_scene_time_step) ? 1 : 0,
        g_config.cutscene_fps,
        g_config.time_step_smoothing,
        g_config.hook_fog_slicing_guard ? 1 : 0);
    spatch::log::InfoF(
        "requested_config input gta_iv_car_camera=%d gta_iv_bike_camera=%d",
        g_config.gta_iv_car_camera ? 1 : 0,
        g_config.gta_iv_bike_camera ? 1 : 0);
    spatch::log::InfoF(
        "requested_config display maximum_refresh=%d fullscreen=%d limiter=%d remove_120_cap=%d",
        g_config.prefer_max_refresh_rate ? 1 : 0,
        g_config.override_fullscreen,
        g_config.override_fps_limiter,
        g_config.remove_hidden_120_fps_cap ? 1 : 0);
    spatch::log::InfoF(
        "requested_config texture_filtering anisotropy=%d force_verified_trilinear=%d",
        g_config.anisotropic_filtering,
        g_config.force_anisotropic_filtering ? 1 : 0);

    spatch::log::InfoF(
        "requested_engine_fix_config pedestrian_density=%d spherical_reflections=%d spherical_width=%d "
        "original_fog_and_neon=%d original_eye_reflections=%d character_wetness=%d "
        "character_sweat=%d "
        "high_fps_averages=%d remove_120_fps_cap=%d "
        "corrupt_save_guard=%d thread_failure_guard=%d",
        g_config.fix_pedestrian_density_at_high_fps ? 1 : 0,
        g_config.improve_spherical_reflections ? 1 : 0,
        g_config.spherical_reflection_width,
        g_config.restore_original_fog ? 1 : 0,
        g_config.restore_original_eye_reflections ? 1 : 0,
        g_config.restore_character_wetness ? 1 : 0,
        g_config.restore_character_sweat ? 1 : 0,
        g_config.fix_high_fps_average_windows ? 1 : 0,
        g_config.remove_hidden_120_fps_cap ? 1 : 0,
        g_config.fix_corrupt_save_handling ? 1 : 0,
        g_config.fix_thread_creation_failure ? 1 : 0);
    spatch::log::InfoF(
        "requested_engine_guard_config first_run_resolution=%d scaleform_clock=%d "
        "file_timestamp=%d audio_open=%d large_files=%d vram_pool_lock=%d "
        "vram_reporting=%d resource_loading=%d",
        g_config.fix_first_run_resolution ? 1 : 0,
        g_config.fix_scaleform_qpc_clock ? 1 : 0,
        g_config.fix_file_timestamp_open_mode ? 1 : 0,
        g_config.fix_audio_file_open ? 1 : 0,
        g_config.fix_large_file_sizes ? 1 : 0,
        g_config.fix_vram_pool_lock ? 1 : 0,
        g_config.fix_vram_capacity_reporting ? 1 : 0,
        g_config.fix_resource_loading ? 1 : 0);

    spatch::log::InfoF("requested_cutscene_timing target_fps=%d mode=%s zero_dt_fix=%d scene_time_fix=%d",
                       g_config.cutscene_fps,
                       g_config.cutscene_fps == 0 ? "auto" : "configured",
                       g_config.fix_cutscene_zero_dt ? 1 : 0,
                       g_config.fix_cutscene_scene_time_step ? 1 : 0);

    if (!g_config.enabled) {
        spatch::log::Warn("SPatch disabled by config");
        return 0;
    }

    if (g_config.write_minidumps) {
        g_dbghelp_module = spatch::LoadSystemLibrary(L"dbghelp.dll");
        if (g_dbghelp_module != nullptr) {
            g_mini_dump_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
                GetProcAddress(g_dbghelp_module, "MiniDumpWriteDump"));
        }
        if (g_mini_dump_write_dump != nullptr) {
            // The callback becomes visible inside SetUnhandledExceptionFilter.
            // Keep it in a disabled no-chain state until the returned previous
            // filter and all dump dependencies have been safely published.
            g_crash_filter_ready.store(false, std::memory_order_relaxed);
            const LPTOP_LEVEL_EXCEPTION_FILTER previous =
                SetUnhandledExceptionFilter(&TopLevelExceptionFilter);
            g_previous_exception_filter.store(previous, std::memory_order_release);
            g_crash_filter_ready.store(true, std::memory_order_release);
            g_minidumps_enabled.store(true, std::memory_order_release);
        } else {
            spatch::log::Warn("MiniDumpWriteDump unavailable; crash dumps disabled");
            if (g_dbghelp_module != nullptr) {
                FreeLibrary(g_dbghelp_module);
                g_dbghelp_module = nullptr;
            }
        }
    }

    const spatch::BuildCheckResult build = spatch::InspectLoadedGame();
    spatch::log::Info(build.summary);

    if (g_detaching.load()) {
        return 0;
    }

    const spatch::BootstrapPrepareResult prepare =
        spatch::PrepareBootstrapHooks(g_config, build, g_detaching.load());
    g_config = prepare.effective_config;

    if (prepare.status == spatch::BootstrapPrepareStatus::DetachedBeforeHooks) {
        return 0;
    }

    if (prepare.status == spatch::BootstrapPrepareStatus::UnsupportedBuild) {
        spatch::log::Warn("unsupported game build; hooks not installed");
        spatch::log::Info("hook_result requested=0 installed=0 reason=unsupported_build");
        return 0;
    }

    const bool hooks_requested =
        prepare.status ==
        spatch::BootstrapPrepareStatus::ReadyForHookInitialization;
    spatch::log::InfoF(
        "effective_profile build_id=%s hooks_requested=%d safe_compatibility=%d "
        "latest_steam=%d smaa=%d character_sweat=%d "
        "duplicate_actor_restore=%d gta_iv_car_camera=%d "
        "gta_iv_bike_camera=%d rumble=%d",
        build.build_id.c_str(),
        hooks_requested ? 1 : 0,
        prepare.safe_compatibility_mode ? 1 : 0,
        prepare.latest_steam_profile ? 1 : 0,
        (g_config.hook_smaa_present && g_config.smaa_enable) ? 1 : 0,
        g_config.restore_character_sweat ? 1 : 0,
        g_config.fix_nis_actor_restore_duplicates ? 1 : 0,
        g_config.gta_iv_car_camera ? 1 : 0,
        g_config.gta_iv_bike_camera ? 1 : 0,
        g_config.override_rumble_enabled);

    // Do not touch user settings until the executable policy has accepted the
    // build. Safe compatibility mode may apply only these build-independent
    // XML settings; fixed-address behavior has already been disabled above.
    const std::filesystem::path display_settings_path =
        spatch::ResolveDisplaySettingsPath(g_module_directory);
    if (spatch::ShouldApplyDisplaySettings(prepare.status)) {
        spatch::log::InfoF("display_settings selected_path=%s",
                           spatch::log::ToUtf8(display_settings_path.c_str()).c_str());
        spatch::ApplyDisplaySettingsPatches(display_settings_path, g_config);
    }

    if (prepare.safe_compatibility_mode) {
        spatch::log::WarnF("safe compatibility mode active build_id=%s; hook-based features disabled until this executable is remapped",
                           build.build_id.c_str());
        spatch::log::Info(
            "hook_result requested=0 installed=0 mode=settings_only");
        spatch::log::Info("SPatch initialized in settings-only mode");
        return 0;
    }

    if (prepare.latest_steam_profile) {
        spatch::log::WarnF(
            "latest_steam compatibility profile active; smaa=%d character_sweat=%d "
            "duplicate_actor_restore=%d gta_iv_car_camera=%d "
            "gta_iv_bike_camera=%d rumble=%d",
            (g_config.hook_smaa_present && g_config.smaa_enable) ? 1 : 0,
            g_config.restore_character_sweat ? 1 : 0,
            g_config.fix_nis_actor_restore_duplicates ? 1 : 0,
            g_config.gta_iv_car_camera ? 1 : 0,
            g_config.gta_iv_bike_camera ? 1 : 0,
            g_config.override_rumble_enabled);
    }

    if (g_detaching.load()) {
        return 0;
    }

    const spatch::BootstrapFinalizeStatus finalize =
        spatch::FinalizeBootstrapHooks(
            spatch::hooks::Initialize(g_config,
                                      config_path,
                                      display_settings_path,
                                      build.build_id,
                                      build.hook_layout_supported));
    if (finalize == spatch::BootstrapFinalizeStatus::HookInitializationFailed) {
        spatch::log::Error("hook initialization failed");
        spatch::log::Info("hook_result requested=1 installed=0");
        spatch::hooks::Shutdown();
        return 0;
    }

    spatch::log::Info("hook_result requested=1 installed=1");
    spatch::log::Info("SPatch initialized");
    return 0;
}

DWORD WINAPI BootstrapThread(void*) noexcept {
    try {
        return BootstrapMain();
    } catch (const std::exception& error) {
        try {
            spatch::hooks::Shutdown();
        } catch (...) {
        }
        spatch::log::ErrorF("bootstrap aborted by C++ exception: %s", error.what());
        OutputDebugStringA("SPatch bootstrap aborted by C++ exception\n");
    } catch (...) {
        try {
            spatch::hooks::Shutdown();
        } catch (...) {
        }
        spatch::log::Error("bootstrap aborted by unknown C++ exception");
        OutputDebugStringA("SPatch bootstrap aborted by unknown C++ exception\n");
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        g_detaching.store(false, std::memory_order_relaxed);

        // Create the worker before pinning so a resource failure can reject
        // the DLL load cleanly. It stays suspended until the process-lifetime
        // module pin is established and all globals above are published.
        HANDLE thread =
            CreateThread(nullptr, 0, &BootstrapThread, nullptr, CREATE_SUSPENDED, nullptr);
        if (thread == nullptr) {
            return FALSE;
        }

        // Every installed hook points back into this image. Pinning makes the
        // documented process-lifetime contract enforceable and prevents a
        // third-party FreeLibrary call from unmapping active detours.
        HMODULE pinned_module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(module),
                               &pinned_module)) {
            AbortNeverStartedBootstrapThread(thread);
            CloseHandle(thread);
            return FALSE;
        }
        if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
            AbortNeverStartedBootstrapThread(thread);
            CloseHandle(thread);
            return FALSE;
        }
        CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        (void)reserved;
        g_detaching.store(true);
    }

    return TRUE;
}
