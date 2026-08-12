#pragma once

namespace spatch::input {

inline constexpr int kStockDeadzone = -1;
inline constexpr int kMinimumDeadzonePercent = 0;
inline constexpr int kMaximumDeadzonePercent = 95;

constexpr int ClampDeadzonePercent(int value) noexcept {
    if (value < kStockDeadzone) {
        return kStockDeadzone;
    }
    if (value > kMaximumDeadzonePercent) {
        return kMaximumDeadzonePercent;
    }
    return value;
}

constexpr float DeadzoneFraction(int percent) noexcept {
    return percent <= kMinimumDeadzonePercent
               ? 0.0f
               : static_cast<float>(percent) / 100.0f;
}

constexpr float DeadzoneScale(float deadzone) noexcept {
    return 1.0f / (1.0f - deadzone);
}

}  // namespace spatch::input
