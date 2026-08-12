#pragma once

#include <cstdint>

namespace spatch::pedestrian_timing {

inline constexpr double kStockThrottleHz = 30.0;
inline constexpr double kStockThrottleStepSeconds = 1.0 / kStockThrottleHz;
inline constexpr double kMaximumAcceptedFrameDelta = 0.25;
// The stock controller observes one pedestrian-count snapshot per owner
// update. Replaying it multiple times in that same update would integrate
// several decisions against stale input and can overshoot after a hitch.
inline constexpr std::uint32_t kMaximumStepsPerFrame = 1;

struct StepResult {
    std::uint32_t steps = 0;
    bool frame_delta_clamped = false;
};

// Converts a per-render-frame engine callback back to its original 30 Hz
// cadence. The stock function has no delta-time input, so invoking it at the
// render rate makes its controller progressively more aggressive as FPS rises.
class FixedRateScheduler {
public:
    StepResult Advance(double frame_delta_seconds);
    void Reset();
    [[nodiscard]] double remainder_seconds() const;

private:
    double accumulator_seconds_ = 0.0;
};

}  // namespace spatch::pedestrian_timing
