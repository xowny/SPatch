#pragma once

#include <algorithm>
#include <cmath>

namespace spatch::agx {

// CPU reference for ShenLong's full-RGB shader in SPatchTonemapping.cpp. The color
// conversions follow three.js's linear-sRGB AgX integration; the contrast
// polynomial follows current Filament. Keeping this small reference in the
// base test target catches matrix, channel, allocation, and curve drift.
struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

enum class Look {
    Neutral = 0,
    MediumHigh = 1,
};

inline constexpr float kMinEv = -12.47393f;
inline constexpr float kMaxEv = 4.026069f;
inline constexpr float kMiddleGrayLog = 10.0f / 16.5f;
inline constexpr float kContrastTransitionStart = 0.35f;
inline constexpr float kShadowContrast = 1.10f;
inline constexpr float kHighlightContrast = 1.25f;
inline constexpr float kShadowChromaStart = 0.05f;
inline constexpr float kShadowChromaEnd = 0.20f;
inline constexpr float kHighlightChromaStart = 0.80f;
inline constexpr float kHighlightChromaEnd = 0.95f;
inline constexpr float kMidtoneSaturation = 1.04f;
inline constexpr float kHighlightSaturation = 1.02f;
inline constexpr float kDisplayPeakLimit = 252.0f / 255.0f;

[[nodiscard]] inline float Saturate(float value) noexcept {
    if (std::isnan(value) || value <= 0.0f) {
        return 0.0f;
    }
    return value >= 1.0f ? 1.0f : value;
}

[[nodiscard]] inline Rgb Transform(const Rgb& value,
                                   const float matrix[3][3]) noexcept {
    return Rgb{
        value.r * matrix[0][0] + value.g * matrix[0][1] + value.b * matrix[0][2],
        value.r * matrix[1][0] + value.g * matrix[1][1] + value.b * matrix[1][2],
        value.r * matrix[2][0] + value.g * matrix[2][1] + value.b * matrix[2][2],
    };
}

[[nodiscard]] inline float DefaultContrastApprox(float value) noexcept {
    const float x = Saturate(value);
    const float x2 = x * x;
    const float x4 = x2 * x2;
    const float x6 = x4 * x2;
    return -17.86f * x6 * x + 78.01f * x6 - 126.7f * x4 * x +
           92.06f * x4 - 28.72f * x2 * x + 4.361f * x2 -
           0.1718f * x + 0.002857f;
}

[[nodiscard]] inline float SmoothStep(float edge0, float edge1,
                                      float value) noexcept {
    const float t = Saturate((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] inline float MediumHighSaturationForPeak(float peak) noexcept {
    const float shadow_weight =
        SmoothStep(kShadowChromaStart, kShadowChromaEnd, peak);
    const float highlight_weight =
        SmoothStep(kHighlightChromaStart, kHighlightChromaEnd, peak);
    const float shadow_to_midtone =
        1.0f + (kMidtoneSaturation - 1.0f) * shadow_weight;
    return shadow_to_midtone +
           (kHighlightSaturation - shadow_to_midtone) * highlight_weight;
}

[[nodiscard]] inline Rgb LimitDisplayPeak(Rgb color,
                                          float strength = 1.0f) noexcept {
    const float peak = (std::max)(color.r, (std::max)(color.g, color.b));
    if (!std::isfinite(peak) || peak <= kDisplayPeakLimit) {
        return color;
    }
    const float scale = kDisplayPeakLimit / peak;
    const float amount = Saturate(strength);
    const float blended_scale = 1.0f + (scale - 1.0f) * amount;
    return Rgb{color.r * blended_scale,
               color.g * blended_scale,
               color.b * blended_scale};
}

[[nodiscard]] inline float ApplyLook(float value, Look look) noexcept {
    const float allocated = Saturate(value);
    if (look == Look::MediumHigh) {
        const float transition = Saturate(
            (allocated - kContrastTransitionStart) /
            (kMiddleGrayLog - kContrastTransitionStart));
        const float smooth_transition =
            transition * transition * (3.0f - 2.0f * transition);
        const float contrast =
            kShadowContrast +
            (kHighlightContrast - kShadowContrast) * smooth_transition;
        return Saturate(
            (allocated - kMiddleGrayLog) * contrast +
            kMiddleGrayLog);
    }
    return allocated;
}

[[nodiscard]] inline float AllocateAndCurve(
    float value, Look look = Look::MediumHigh) noexcept {
    // Match HLSL max/log behavior for the useful extended-domain cases:
    // negative infinity maps to the floor, while positive infinity allocates
    // to the top of the AgX range instead of wrapping back to black.
    const float safe = std::isnan(value) || value <= 0.0f
                           ? 1.0e-10f
                           : value;
    const float normalized = (std::log2(safe) - kMinEv) / (kMaxEv - kMinEv);
    return DefaultContrastApprox(ApplyLook(normalized, look));
}

[[nodiscard]] inline float SrgbEncode(float value) noexcept {
    value = Saturate(value);
    return value <= 0.0031308f
               ? 12.92f * value
               : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] inline Rgb EvaluateLinearRgb(Rgb color,
                                           float exposure = 1.0f,
                                           Look look = Look::MediumHigh) noexcept {
    if (!std::isfinite(exposure) || exposure <= 0.0f) {
        exposure = 0.0f;
    }
    color.r = (std::max)(color.r * exposure, 0.0f);
    color.g = (std::max)(color.g * exposure, 0.0f);
    color.b = (std::max)(color.b * exposure, 0.0f);

    constexpr float kSrgbToRec2020[3][3]{
        {0.6274f, 0.3293f, 0.0433f},
        {0.0691f, 0.9195f, 0.0113f},
        {0.0164f, 0.0880f, 0.8956f},
    };
    constexpr float kInset[3][3]{
        {0.8566271533f, 0.0951212405f, 0.0482516061f},
        {0.1373189729f, 0.7612419906f, 0.1014390365f},
        {0.1118982130f, 0.0767994186f, 0.8113023684f},
    };
    constexpr float kOutset[3][3]{
        {1.1271005818f, -0.1106066431f, -0.0164939387f},
        {-0.1413297635f, 1.1578237022f, -0.0164939387f},
        {-0.1413297635f, -0.1106066431f, 1.2519364066f},
    };
    constexpr float kRec2020ToSrgb[3][3]{
        {1.6605f, -0.5876f, -0.0728f},
        {-0.1246f, 1.1329f, -0.0083f},
        {-0.0182f, -0.1006f, 1.1187f},
    };

    color = Transform(Transform(color, kSrgbToRec2020), kInset);
    color.r = AllocateAndCurve(color.r, look);
    color.g = AllocateAndCurve(color.g, look);
    color.b = AllocateAndCurve(color.b, look);
    if (look == Look::MediumHigh) {
        const float luminance =
            0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
        const float peak = (std::max)(color.r, (std::max)(color.g, color.b));
        const float saturation = MediumHighSaturationForPeak(peak);
        color.r = luminance + saturation * (color.r - luminance);
        color.g = luminance + saturation * (color.g - luminance);
        color.b = luminance + saturation * (color.b - luminance);
    }
    color = Transform(color, kOutset);
    color.r = std::pow((std::max)(color.r, 0.0f), 2.2f);
    color.g = std::pow((std::max)(color.g, 0.0f), 2.2f);
    color.b = std::pow((std::max)(color.b, 0.0f), 2.2f);
    color = Transform(color, kRec2020ToSrgb);
    return Rgb{Saturate(color.r), Saturate(color.g), Saturate(color.b)};
}

// The game's final scene target is display-referred R8G8B8A8_UNORM. Convert
// the linear-sRGB AgX result once here; the native post-tone bloom and display
// adjustment remain outside the transform in the replacement shader.
[[nodiscard]] inline Rgb EvaluateRgb(Rgb color,
                                     float exposure = 1.0f,
                                     Look look = Look::MediumHigh) noexcept {
    const Rgb linear = EvaluateLinearRgb(color, exposure, look);
    return Rgb{
        SrgbEncode(linear.r),
        SrgbEncode(linear.g),
        SrgbEncode(linear.b),
    };
}

// Value0.y is the stock final-composition white scale. Both the native curve
// and the replacement must use it so strength blending stays in one display
// domain and the time-of-day exposure remains authored by the game.
[[nodiscard]] inline float SafeGameWhiteScale(float value) noexcept {
    return std::isfinite(value) && value > 1.0e-4f && value < 1.0e4f
               ? value
               : 1.0f;
}

[[nodiscard]] inline Rgb EvaluateGameMappedRgb(
    Rgb color,
    float game_white_scale,
    float exposure = 1.0f,
    Look look = Look::MediumHigh) noexcept {
    const Rgb mapped = EvaluateRgb(color, exposure, look);
    const float white = SafeGameWhiteScale(game_white_scale);
    return Rgb{mapped.r / white, mapped.g / white, mapped.b / white};
}

[[nodiscard]] inline float StockCurve(float scene_linear) noexcept {
    if (!std::isfinite(scene_linear)) {
        return scene_linear > 0.0f ? 1.0f : 0.0f;
    }
    const float input = (std::max)(scene_linear - 0.004f, 0.0f);
    return input * (6.2f * input + 0.5f) /
           (input * (6.2f * input + 1.7f) + 0.06f);
}

[[nodiscard]] inline float EvaluateStockMapped(
    float scene_linear, float game_white_scale) noexcept {
    return StockCurve(scene_linear) / SafeGameWhiteScale(game_white_scale);
}

// Retained only for parsing old diagnostic fixtures; runtime rendering uses
// EvaluateRgb in the graphics shader rather than a scalar engine-field remap.
[[nodiscard]] inline float EvaluateNeutralAxis(
    float scene_linear_exposure,
    float exposure = 1.0f,
    Look look = Look::MediumHigh) noexcept {
    if (!std::isfinite(scene_linear_exposure) || scene_linear_exposure <= 0.0f) {
        return 0.0f;
    }
    const Rgb output = EvaluateRgb(
        Rgb{scene_linear_exposure, scene_linear_exposure, scene_linear_exposure},
        exposure,
        look);
    return (output.r + output.g + output.b) / 3.0f;
}

}  // namespace spatch::agx
