#pragma once

namespace spatch {

inline constexpr int kMinimumFogSlicingInterval = 1;
inline constexpr int kMaximumFogSlicingInterval = 4;

[[nodiscard]] constexpr int ClampFogSlicingInterval(int requested_interval,
                                                     int configured_minimum) noexcept {
    const int minimum = configured_minimum < kMinimumFogSlicingInterval
                            ? kMinimumFogSlicingInterval
                        : configured_minimum > kMaximumFogSlicingInterval
                            ? kMaximumFogSlicingInterval
                            : configured_minimum;
    if (requested_interval < minimum) {
        return minimum;
    }
    if (requested_interval > kMaximumFogSlicingInterval) {
        return kMaximumFogSlicingInterval;
    }
    return requested_interval;
}

}  // namespace spatch
