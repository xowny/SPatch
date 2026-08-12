#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace spatch::average_window {

inline constexpr float kExpandedSampleRate = 1000.0f;
inline constexpr std::size_t kMaximumEntries = 4096;

struct CapacityDecision {
    float effective_sample_rate = 0.0f;
    std::size_t entry_count = 0;
    bool recognized_stock_rate = false;
    bool expanded = false;
};

inline CapacityDecision ResolveCapacity(float maximum_timespan, float stock_sample_rate) {
    CapacityDecision decision{.effective_sample_rate = stock_sample_rate};
    if (!std::isfinite(maximum_timespan) || !std::isfinite(stock_sample_rate) ||
        maximum_timespan <= 0.0f || stock_sample_rate <= 0.0f) {
        return decision;
    }

    decision.recognized_stock_rate = std::abs(stock_sample_rate - 30.0f) <= 0.01f ||
                                     std::abs(stock_sample_rate - 60.0f) <= 0.01f;
    if (!decision.recognized_stock_rate) {
        return decision;
    }

    const double entries = std::ceil(static_cast<double>(maximum_timespan) *
                                     static_cast<double>(kExpandedSampleRate)) +
                           2.0;
    if (!std::isfinite(entries) || entries < 2.0 ||
        entries > static_cast<double>(kMaximumEntries)) {
        return decision;
    }

    decision.effective_sample_rate = (std::max)(stock_sample_rate, kExpandedSampleRate);
    decision.entry_count = static_cast<std::size_t>(entries);
    decision.expanded = decision.effective_sample_rate != stock_sample_rate;
    return decision;
}

}  // namespace spatch::average_window
