#pragma once

namespace spatch::character_hooks {

struct HookRequest {
    bool restore_wetness = false;
    bool restore_sweat = false;
    bool regression_probe = false;
    bool wetness_field_probe = false;
    bool sweat_field_probe = false;
};

struct HookPlan {
    bool install_effects_hooks = false;
    bool install_health_damage_hook = false;
    bool install_regression_hooks = false;
    bool require_simobject_component = false;
    bool use_sweat_only_health_fast_path = false;
};

[[nodiscard]] constexpr HookPlan BuildHookPlan(const HookRequest& request) noexcept {
    HookPlan plan{};
    plan.install_effects_hooks =
        request.regression_probe || request.wetness_field_probe || request.sweat_field_probe ||
        request.restore_wetness || request.restore_sweat;
    plan.install_health_damage_hook =
        request.regression_probe || request.restore_sweat;
    plan.install_regression_hooks = request.regression_probe;
    plan.require_simobject_component =
        plan.install_effects_hooks || plan.install_health_damage_hook;
    plan.use_sweat_only_health_fast_path =
        request.restore_sweat && !request.regression_probe;
    return plan;
}

}  // namespace spatch::character_hooks
