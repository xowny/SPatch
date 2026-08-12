#include "PedestrianTiming.h"

#include <algorithm>
#include <cmath>

namespace spatch::pedestrian_timing {

StepResult FixedRateScheduler::Advance(double frame_delta_seconds) {
    if (!std::isfinite(frame_delta_seconds) || frame_delta_seconds < 0.0) {
        Reset();
        return {};
    }

    StepResult result{};
    const double accepted_delta =
        (std::min)(frame_delta_seconds, kMaximumAcceptedFrameDelta);
    result.frame_delta_clamped = accepted_delta != frame_delta_seconds;
    accumulator_seconds_ += accepted_delta;

    // The small epsilon keeps exact 30/60 Hz decimal inputs from missing a
    // step because of a one-ULP floating-point subtraction.
    constexpr double kStepEpsilon = 1.0e-9;
    const auto available_steps = static_cast<std::uint32_t>(
        (accumulator_seconds_ + kStepEpsilon) / kStockThrottleStepSeconds);
    result.steps = (std::min)(available_steps, kMaximumStepsPerFrame);

    if (available_steps > kMaximumStepsPerFrame) {
        // Do not carry a large hitch backlog into later frames. Retaining only
        // the fractional step avoids a burst of crowd changes after a pause or
        // streaming stall while preserving the long-term cadence.
        accumulator_seconds_ =
            std::fmod(accumulator_seconds_, kStockThrottleStepSeconds);
    } else {
        accumulator_seconds_ -=
            static_cast<double>(result.steps) * kStockThrottleStepSeconds;
    }

    if (accumulator_seconds_ < 0.0 && accumulator_seconds_ > -kStepEpsilon) {
        accumulator_seconds_ = 0.0;
    }
    return result;
}

void FixedRateScheduler::Reset() {
    accumulator_seconds_ = 0.0;
}

double FixedRateScheduler::remainder_seconds() const {
    return accumulator_seconds_;
}

}  // namespace spatch::pedestrian_timing
