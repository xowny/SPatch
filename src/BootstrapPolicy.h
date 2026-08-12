#pragma once

#include "Config.h"
#include "VersionGuard.h"

namespace spatch {

struct HookInstallPlan {
    Config effective_config{};
    bool install_hooks = false;
    bool safe_compatibility_mode = false;
    bool latest_steam_profile = false;
};

enum class BootstrapPrepareStatus {
    ReadyForHookInitialization,
    ReadyForSettingsOnly,
    DisabledByConfig,
    DetachedBeforeHooks,
    UnsupportedBuild,
};

enum class BootstrapFinalizeStatus {
    HookInitializationFailed,
    Initialized,
};

struct BootstrapPrepareResult {
    Config effective_config{};
    BootstrapPrepareStatus status = BootstrapPrepareStatus::UnsupportedBuild;
    bool safe_compatibility_mode = false;
    bool latest_steam_profile = false;
};

Config BuildSafeCompatibilityConfig(Config config);
Config BuildLatestSteamCompatibilityConfig(Config config);
bool IsUsableBootstrapModulePath(const std::filesystem::path& module_path);
HookInstallPlan BuildHookInstallPlan(const Config& config, const BuildCheckResult& build);
BootstrapPrepareResult PrepareBootstrapHooks(const Config& config,
                                             const BuildCheckResult& build,
                                             bool detaching_after_inspect);
bool ShouldApplyDisplaySettings(BootstrapPrepareStatus status);
BootstrapFinalizeStatus FinalizeBootstrapHooks(bool hook_initialize_succeeded);

}  // namespace spatch
