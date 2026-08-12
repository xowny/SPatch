#pragma once

#include <algorithm>
#include <cmath>

namespace spatch::character_wetness {

inline constexpr float kMaximumStepSeconds = 0.25f;
inline constexpr float kMinimumVisibleAmount = 0.001f;

struct Timing {
    float full_wet_seconds = 0.0f;
    float fade_seconds = 0.0f;
};

struct State {
    float amount = 0.0f;
    float hold_seconds = 0.0f;
};

struct StepResult {
    float amount = 0.0f;
    bool active = false;
};

inline float ClampAmount(float amount) noexcept {
    if (!std::isfinite(amount)) {
        return 0.0f;
    }
    return std::clamp(amount, 0.0f, 1.0f);
}

inline float SanitizeDelta(float delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0f) {
        return 0.0f;
    }
    return std::min(delta_seconds, kMaximumStepSeconds);
}

inline float SanitizeDuration(float seconds) noexcept {
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        return 0.0f;
    }
    return seconds;
}

inline void MarkWaterCollision(State& state, const Timing& timing) noexcept {
    state.amount = 1.0f;
    state.hold_seconds = SanitizeDuration(timing.full_wet_seconds);
}

inline bool ShouldYieldToStrongerOwner(float current,
                                       float last_written,
                                       float desired,
                                       float epsilon = 0.002f) noexcept {
    return std::isfinite(current) && std::isfinite(last_written) &&
           std::isfinite(desired) && std::fabs(current - last_written) > epsilon &&
           current > desired + epsilon;
}

inline StepResult Advance(State& state,
                          float delta_seconds,
                          bool raining,
                          float weather_surface_wetness,
                          const Timing& timing) noexcept {
    const float delta = SanitizeDelta(delta_seconds);
    const float full_wet_seconds = SanitizeDuration(timing.full_wet_seconds);
    const float fade_seconds = SanitizeDuration(timing.fade_seconds);
    state.amount = ClampAmount(state.amount);
    if (!std::isfinite(state.hold_seconds) || state.hold_seconds < 0.0f) {
        state.hold_seconds = 0.0f;
    }

    if (raining) {
        state.amount = std::max(state.amount, ClampAmount(weather_surface_wetness));
        // Keep the configured full-wet period after rain just like direct
        // water contact. This prevents a one-frame weather-state transition
        // from starting the dry fade while the last rain drops are visible.
        state.hold_seconds = std::max(state.hold_seconds, full_wet_seconds);
    } else if (delta > 0.0f) {
        float remaining_delta = delta;
        if (state.hold_seconds > 0.0f) {
            const float hold_delta = std::min(state.hold_seconds, remaining_delta);
            state.hold_seconds -= hold_delta;
            remaining_delta -= hold_delta;
        }
        if (remaining_delta > 0.0f && state.amount > 0.0f) {
            state.amount = fade_seconds > 0.0f
                               ? std::max(0.0f, state.amount - remaining_delta / fade_seconds)
                               : 0.0f;
        }
    }

    if (state.amount < kMinimumVisibleAmount) {
        state.amount = 0.0f;
    }
    return StepResult{state.amount, state.amount > 0.0f};
}

}  // namespace spatch::character_wetness
