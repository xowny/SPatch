// Full-RGB AgX integration for Sleeping Dogs: Definitive Edition.
//
// Sleeping Dogs applies its stock scene curve in several scene shaders, then
// reconstructs scene-linear color in PS 0x67843125 immediately before the HUD.
// Replacing that exact pixel shader is the only observed boundary that covers
// the complete scene without processing the HUD. The replacement is installed
// during native pipeline creation, so the game's draw order, render targets,
// bindings, blend state, and draw calls remain untouched.
//
// The AgX analytic transform uses three.js's explicit linear-sRGB/Rec.2020
// conversion and Filament's default contrast polynomial. The packaged
// third-party notices carry the corresponding attribution and license text.

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <examples/utils/crc32_hash.hpp>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "SPatchGraphicsComponents.hpp"
#include "SPatchIni.hpp"
#include "SPatchReShadeCallbackSafety.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kFinalPreHudPixelShaderHash = 0x67843125;
constexpr std::size_t kFinalPreHudPixelShaderSize = 1852;
constexpr std::array<std::uint8_t, 16> kFinalPreHudDxbcChecksum = {
    0x08, 0xCD, 0x71, 0xA4, 0x19, 0x9D, 0x73, 0xDF,
    0x31, 0x4A, 0x99, 0xE7, 0x7C, 0x92, 0x3E, 0xCA,
};

constexpr char kAgxFinalPreHudShader[] = R"SPATCH_HLSL(
#ifndef SPATCH_AGX_EXPOSURE
#define SPATCH_AGX_EXPOSURE 1.0
#endif

#ifndef SPATCH_AGX_STRENGTH
#define SPATCH_AGX_STRENGTH 1.0
#endif

#ifndef SPATCH_AGX_FULL_STRENGTH
#define SPATCH_AGX_FULL_STRENGTH 1
#endif

#ifndef SPATCH_AGX_LOOK
#define SPATCH_AGX_LOOK 1
#endif

cbuffer cbShaderParams : register(b0)
{
  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);
}

SamplerState _texDiffuse_s : register(s0);
SamplerState _texHDRBloom_s : register(s1);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texHDRBloom : register(t1);

float3 LinearSrgbToLinearRec2020(float3 color)
{
  return float3(
    dot(color, float3(0.6274, 0.3293, 0.0433)),
    dot(color, float3(0.0691, 0.9195, 0.0113)),
    dot(color, float3(0.0164, 0.0880, 0.8956)));
}

float3 LinearRec2020ToLinearSrgb(float3 color)
{
  return float3(
    dot(color, float3(1.6605, -0.5876, -0.0728)),
    dot(color, float3(-0.1246, 1.1329, -0.0083)),
    dot(color, float3(-0.0182, -0.1006, 1.1187)));
}

float3 AgxInset(float3 color)
{
  return float3(
    dot(color, float3(0.856627153315983, 0.0951212405381588, 0.0482516061458583)),
    dot(color, float3(0.137318972929847, 0.761241990602591, 0.101439036467562)),
    dot(color, float3(0.11189821299995, 0.0767994186031903, 0.811302368396859)));
}

float3 AgxOutset(float3 color)
{
  return float3(
    dot(color, float3(1.1271005818144368, -0.11060664309660323, -0.016493938717834573)),
    dot(color, float3(-0.1413297634984383, 1.157823702216272, -0.016493938717834257)),
    dot(color, float3(-0.14132976349843826, -0.11060664309660294, 1.2519364065950405)));
}

float3 AgxDefaultContrastApprox(float3 x)
{
  const float3 x2 = x * x;
  const float3 x4 = x2 * x2;
  const float3 x6 = x4 * x2;
  return -17.86 * x6 * x
       + 78.01 * x6
       - 126.7 * x4 * x
       + 92.06 * x4
       - 28.72 * x2 * x
       + 4.361 * x2
       - 0.1718 * x
       + 0.002857;
}

float3 AgxToLinearSrgb(float3 color)
{
  static const float AgxMinEv = -12.47393;
  static const float AgxMaxEv = 4.026069;
  static const float AgxMiddleGrayLog = 10.0 / 16.5;
  static const float AgxShadowChromaStart = 0.05;
  static const float AgxShadowChromaEnd = 0.20;
  static const float AgxHighlightChromaStart = 0.80;
  static const float AgxHighlightChromaEnd = 0.95;
  static const float AgxMidtoneSaturation = 1.04;
  static const float AgxHighlightSaturation = 1.02;

  color *= SPATCH_AGX_EXPOSURE;
  color = LinearSrgbToLinearRec2020(color);
  color = AgxInset(color);
  color = log2(max(color, 1e-10));
  color = saturate((color - AgxMinEv) / (AgxMaxEv - AgxMinEv));
#if SPATCH_AGX_LOOK == 1
  const float3 contrastBlend =
    smoothstep(0.35, AgxMiddleGrayLog, color);
  const float3 contrast = lerp(1.10, 1.25, contrastBlend);
  color = saturate((color - AgxMiddleGrayLog) * contrast + AgxMiddleGrayLog);
#elif SPATCH_AGX_LOOK != 0
#error Unsupported SPATCH_AGX_LOOK value
#endif
  color = AgxDefaultContrastApprox(color);
#if SPATCH_AGX_LOOK == 1
  const float lookLuminance =
    dot(color, float3(0.2126, 0.7152, 0.0722));
  const float lookPeak = max(color.x, max(color.y, color.z));
  const float shadowWeight =
    smoothstep(AgxShadowChromaStart, AgxShadowChromaEnd, lookPeak);
  const float highlightWeight =
    smoothstep(AgxHighlightChromaStart, AgxHighlightChromaEnd, lookPeak);
  const float shadowToMidtoneSaturation =
    lerp(1.0, AgxMidtoneSaturation, shadowWeight);
  const float lookSaturation =
    lerp(shadowToMidtoneSaturation, AgxHighlightSaturation, highlightWeight);
  color = lookLuminance + lookSaturation * (color - lookLuminance);
#endif
  color = AgxOutset(color);
  color = pow(max(color, 0.0), 2.2);
  color = LinearRec2020ToLinearSrgb(color);
  return saturate(color);
}

float SrgbEncodeChannel(float value)
{
  value = saturate(value);
  return value <= 0.0031308
    ? 12.92 * value
    : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 AgxToDisplaySrgb(float3 color)
{
  const float3 linearColor = AgxToLinearSrgb(color);
  return float3(
    SrgbEncodeChannel(linearColor.x),
    SrgbEncodeChannel(linearColor.y),
    SrgbEncodeChannel(linearColor.z));
}

float3 LimitDisplayPeak(float3 color)
{
  static const float AgxDisplayPeakLimit = 252.0 / 255.0;
  const float peak = max(color.x, max(color.y, color.z));
  [branch]
  if (peak > AgxDisplayPeakLimit)
  {
    color *= AgxDisplayPeakLimit / peak;
  }
  return color;
}

float GameWhiteScale()
{
  const float value = cbShaderParams.Value0.y;
  return (value > 1e-4 && value < 1e4) ? value : 1.0;
}

float GameGammaExponent()
{
  const float value = cbShaderParams.Value0.w;
  return (value > 1e-2 && value <= 4.0) ? value : 1.0;
}

float FiniteOrZero(float value)
{
  // Shader Model 4 comparisons are false for NaN. The magnitude bound also
  // rejects infinities while retaining the full finite FP16 scene domain.
  return (value == value && abs(value) <= 65504.0) ? value : 0.0;
}

float3 FiniteOrZero(float3 value)
{
  return float3(
    FiniteOrZero(value.x),
    FiniteOrZero(value.y),
    FiniteOrZero(value.z));
}

float3 DecodeSceneEncoding(float3 encodedScene)
{
  // The analytic encoding approaches 1.04 but never reaches it for finite
  // scene radiance. Clamp only values outside that mathematical domain so a
  // corrupt/rounded sample cannot divide by zero or inject NaNs into AgX.
  static const float MaxFiniteSceneEncoding = 1.04 - 1e-5;
  encodedScene = clamp(
    FiniteOrZero(encodedScene), 0.0, MaxFiniteSceneEncoding);
  return min(
    0.2 * encodedScene / max(1.04 - encodedScene, 1e-5), 65504.0);
}

float3 StockToneMap(float3 scene, float gameWhiteScale)
{
  const float3 stockInput = max(scene - 0.004, 0.0);
  return stockInput * (6.2 * stockInput + 0.5)
       / (stockInput * (6.2 * stockInput + 1.7) + 0.06)
       / gameWhiteScale;
}

void main(
  float4 position : SV_Position0,
  float2 texcoord : TEXCOORD0,
  out float4 output : SV_Target0)
{
  // texDiffuse contains the stock scene encoding produced by the earlier
  // scene shaders. This is the exact analytic inverse of 1.04*L/(L+0.2).
  const float3 encodedScene = texDiffuse.Sample(_texDiffuse_s, texcoord).rgb;
  const float4 sampledBloom = texHDRBloom.Sample(_texHDRBloom_s, texcoord);
  const float4 bloom = float4(
    FiniteOrZero(sampledBloom.rgb), FiniteOrZero(sampledBloom.a));
  const float3 linearScene = DecodeSceneEncoding(encodedScene);
  const float3 exposedScene = min(max(
    linearScene * (1.0 + bloom.a * (linearScene - 1.0)), 0.0), 65504.0);

  // Preserve the engine's dynamic final-composition white scale. Omitting
  // this divisor put AgX in a darker display domain than the stock curve and
  // made both the full-strength result and partial-strength blends washed out.
  const float gameWhiteScale = GameWhiteScale();
  const float3 agx =
    AgxToDisplaySrgb(max(exposedScene, 0.0)) / gameWhiteScale;
#if SPATCH_AGX_FULL_STRENGTH
  const float3 mapped = agx;
#else
  const float3 stock = StockToneMap(exposedScene, gameWhiteScale);
  const float3 mapped = lerp(stock, agx, SPATCH_AGX_STRENGTH);
#endif

  // Preserve the native display-referred bloom screen blend and the game's
  // final user-gamma exponent. The HUD is rendered after this shader.
  const float3 withBloom = 1.0 - (1.0 - bloom.rgb) * (1.0 - mapped);
  const float3 composed = pow(
    min(max(withBloom, 0.0), 65504.0), GameGammaExponent());
#if SPATCH_AGX_FULL_STRENGTH
  output.rgb = LimitDisplayPeak(composed);
#else
  output.rgb = lerp(
    composed, LimitDisplayPeak(composed), SPATCH_AGX_STRENGTH);
#endif
  output.a = 1.0;
}
)SPATCH_HLSL";

struct Settings {
    bool enabled = true;
    float strength = 1.0f;
    float exposure = 1.0f;
    int look = 1;
};

struct __declspec(uuid("7264E94D-F2F0-4768-9198-0C5FD3D07E4C")) DeviceData {
    Settings settings;
    bool ready = false;
    std::atomic<std::uint64_t> presents{0};
    std::atomic<std::uint64_t> replacement_requests{0};
    std::atomic<std::uint64_t> replacement_pipelines{0};
    std::atomic<bool> logged_active{false};
    std::atomic<bool> logged_identity_failure{false};
    std::atomic<bool> logged_missing_pipeline{false};
    ComPtr<ID3DBlob> replacement_bytecode;
};

HMODULE g_module = nullptr;

template <typename Interface, typename Handle>
Interface* NativePointer(Handle handle) noexcept {
    static_assert(std::is_integral_v<Handle>);
    static_assert(sizeof(Handle) >= sizeof(std::uintptr_t));
    return reinterpret_cast<Interface*>(static_cast<std::uintptr_t>(handle));
}

void Log(reshade::log::level level, const char* format, ...) noexcept {
    std::array<char, 1024> message{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message.data(), message.size(), _TRUNCATE, format, args);
    va_end(args);
    reshade::log::message(level, message.data());
}

void ReportCallbackFailure(
    spatch::graphics::detail::CallbackFailure failure,
    const char* detail) noexcept {
    const char* const kind =
        failure == spatch::graphics::detail::CallbackFailure::allocation
        ? "allocation failure"
        : failure == spatch::graphics::detail::CallbackFailure::standard
        ? "C++ exception"
        : "unknown C++ exception";
    Log(reshade::log::level::error,
        "[ShenLong-AgX] ReShade callback dropped after %s%s%s; the native tone mapper remains active.",
        kind,
        detail != nullptr ? ": " : "",
        detail != nullptr ? detail : "");
}

template <auto Callback>
using GuardedCallback = spatch::graphics::detail::ReShadeCallbackBoundary<
    Callback, ReportCallbackFailure>;

std::wstring ModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            g_module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        if (path.size() >= 32768) {
            return {};
        }
        path.resize((std::min)(path.size() * 2, std::size_t{32768}), L'\0');
    }
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    path.resize(separator + 1);
    return path;
}

std::optional<std::wstring> ReadAgxValue(
    const std::wstring& path,
    const wchar_t* canonical_key,
    const wchar_t* legacy_public_key,
    const wchar_t* internal_key,
    const wchar_t* legacy_internal_key) {
    // Match the base plug-in's precedence exactly so a stale legacy duplicate
    // can never override an explicit end-user AgX value.
    const auto keys = spatch::graphics::ini::ExtendedSettingKeys(
        L"Tonemapping",
        canonical_key,
        legacy_public_key,
        internal_key,
        legacy_internal_key);
    return spatch::graphics::ini::ReadFirst(path, keys);
}

std::optional<std::wstring> ReadAgxLookValue(const std::wstring& path) {
    using spatch::graphics::ini::Key;
    constexpr std::array keys{
        Key{L"Tonemapping", L"AgXLook"},
        Key{L"ShenLong", L"AgXLook"},
        Key{L"Tonemapping", L"agx_look"},
        Key{L"ShenLong", L"agx_look"},
    };
    return spatch::graphics::ini::ReadFirst(path, keys);
}

int ParseAgxLook(const std::optional<std::wstring>& value, int fallback) {
    if (!value || value->empty()) {
        return fallback;
    }
    if (_wcsicmp(value->c_str(), L"Neutral") == 0 || *value == L"0") {
        return 0;
    }
    if (_wcsicmp(value->c_str(), L"MediumHigh") == 0 ||
        _wcsicmp(value->c_str(), L"Medium High") == 0 || *value == L"1") {
        return 1;
    }
    return fallback;
}

Settings LoadSettings() {
    Settings settings;
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        settings.enabled = false;
        Log(reshade::log::level::error,
            "[ShenLong-AgX] Could not resolve the add-on directory; retaining the native tone mapper.");
        return settings;
    }

    const std::wstring path = directory + L"ShenLong.ini";
    const bool master_enabled =
        spatch::graphics::ini::ReadBool(
            path, spatch::graphics::ini::kMasterEnabledKeys, false);
    const bool agx_enabled = spatch::graphics::ini::ParseBool(
        ReadAgxValue(path, L"AgX", L"ACES", L"agx_enable", L"aces_enable"),
        true);
    const int strength_percent = (std::clamp)(spatch::graphics::ini::ParseInt(
        ReadAgxValue(path,
                     L"AgXStrength",
                     L"ACESStrength",
                     L"agx_strength_percent",
                     L"aces_strength_percent"),
        100), 0, 100);
    const int exposure_percent = (std::clamp)(spatch::graphics::ini::ParseInt(
        ReadAgxValue(path,
                     L"AgXExposure",
                     L"ACESExposure",
                     L"agx_exposure_scale_percent",
                     L"aces_exposure_scale_percent"),
        100), 25, 400);
    const int look = ParseAgxLook(ReadAgxLookValue(path), 1);

    settings.enabled = master_enabled && agx_enabled && strength_percent > 0;
    settings.strength = static_cast<float>(strength_percent) / 100.0f;
    settings.exposure = static_cast<float>(exposure_percent) / 100.0f;
    settings.look = look;
    Log(reshade::log::level::info,
        "[ShenLong-AgX] configured enabled=%d look=%s strength=%d exposure=%d boundary=0x%08X pipeline_replace=1.",
        settings.enabled ? 1 : 0,
        settings.look == 0 ? "Neutral" : "MediumHigh",
        strength_percent,
        exposure_percent,
        kFinalPreHudPixelShaderHash);
    return settings;
}

bool CompileReplacement(ID3D11Device* device, DeviceData& data) {
    if (!device) {
        return false;
    }

    std::array<char, 32> strength{};
    std::array<char, 32> exposure{};
    sprintf_s(strength.data(), strength.size(), "%.9g", data.settings.strength);
    sprintf_s(exposure.data(), exposure.size(), "%.9g", data.settings.exposure);
    const char* full_strength = data.settings.strength >= 1.0f ? "1" : "0";
    const char* look = data.settings.look == 0 ? "0" : "1";
    const std::array<D3D_SHADER_MACRO, 5> macros = {{
        {"SPATCH_AGX_EXPOSURE", exposure.data()},
        {"SPATCH_AGX_STRENGTH", strength.data()},
        {"SPATCH_AGX_FULL_STRENGTH", full_strength},
        {"SPATCH_AGX_LOOK", look},
        {nullptr, nullptr},
    }};

    constexpr UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 |
        D3DCOMPILE_WARNINGS_ARE_ERRORS;
    ComPtr<ID3DBlob> errors;
    const HRESULT compile_result = D3DCompile(
        kAgxFinalPreHudShader,
        sizeof(kAgxFinalPreHudShader) - 1,
        "SPatchTonemapping.hlsl",
        macros.data(),
        nullptr,
        "main",
        "ps_4_0",
        compile_flags,
        0,
        data.replacement_bytecode.ReleaseAndGetAddressOf(),
        errors.ReleaseAndGetAddressOf());
    if (FAILED(compile_result) || !data.replacement_bytecode) {
        const char* detail = errors
            ? static_cast<const char*>(errors->GetBufferPointer())
            : "no compiler detail";
        const int detail_length = errors
            ? static_cast<int>((std::min)(errors->GetBufferSize(), std::size_t{800}))
            : static_cast<int>(std::strlen(detail));
        Log(reshade::log::level::error,
            "[ShenLong-AgX] Shader compilation failed (HRESULT=0x%08X): %.*s",
            static_cast<unsigned int>(compile_result), detail_length, detail);
        data.replacement_bytecode.Reset();
        return false;
    }

    // Validate the driver path before modifying the game's pipeline request.
    // A compile or validation failure therefore leaves the native shader
    // completely untouched.
    ComPtr<ID3D11PixelShader> validation_shader;
    const HRESULT validation_result = device->CreatePixelShader(
        data.replacement_bytecode->GetBufferPointer(),
        data.replacement_bytecode->GetBufferSize(),
        nullptr,
        validation_shader.ReleaseAndGetAddressOf());
    if (FAILED(validation_result) || !validation_shader) {
        Log(reshade::log::level::error,
            "[ShenLong-AgX] Driver rejected the replacement shader (HRESULT=0x%08X); retaining the native tone mapper.",
            static_cast<unsigned int>(validation_result));
        data.replacement_bytecode.Reset();
        return false;
    }
    return true;
}

bool HasExpectedDxbcIdentity(const reshade::api::shader_desc& description) noexcept {
    if (!description.code || description.code_size != kFinalPreHudPixelShaderSize) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(description.code);
    if (std::memcmp(bytes, "DXBC", 4) != 0 ||
        std::memcmp(bytes + 4,
                    kFinalPreHudDxbcChecksum.data(),
                    kFinalPreHudDxbcChecksum.size()) != 0) {
        return false;
    }
    return compute_crc32(bytes, description.code_size) == kFinalPreHudPixelShaderHash;
}

bool LooksLikeMismatchedTarget(const reshade::api::shader_desc& description) noexcept {
    if (!description.code || description.code_size != kFinalPreHudPixelShaderSize) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(description.code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }
    const bool checksum_matches = std::memcmp(
        bytes + 4,
        kFinalPreHudDxbcChecksum.data(),
        kFinalPreHudDxbcChecksum.size()) == 0;
    const bool crc_matches =
        compute_crc32(bytes, description.code_size) == kFinalPreHudPixelShaderHash;
    return checksum_matches != crc_matches;
}

void OnInitDevice(reshade::api::device* device) {
    DeviceData* data =
        spatch::graphics::detail::CreatePrivateData<DeviceData>(device);
    if (!data) {
        return;
    }
    data->settings = LoadSettings();
    if (!data->settings.enabled) {
        return;
    }
    if (device->get_api() != reshade::api::device_api::d3d11) {
        Log(reshade::log::level::warning,
            "[ShenLong-AgX] Unsupported graphics API; retaining the native tone mapper.");
        return;
    }
    data->ready = CompileReplacement(
        NativePointer<ID3D11Device>(device->get_native()), *data);
    if (!data->ready) {
        data->replacement_bytecode.Reset();
        Log(reshade::log::level::warning,
            "[ShenLong-AgX] Initialization failed; retaining the native tone mapper.");
    }
}

void OnDestroyDevice(reshade::api::device* device) {
    device->destroy_private_data<DeviceData>();
}

bool OnCreatePipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!data || !data->settings.enabled || !data->ready ||
        !data->replacement_bytecode ||
        device->get_api() != reshade::api::device_api::d3d11) {
        return false;
    }
    if (subobjects == nullptr && subobject_count != 0) {
        return false;
    }

    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto& subobject = subobjects[index];
        if (subobject.type != reshade::api::pipeline_subobject_type::pixel_shader ||
            subobject.count == 0 || !subobject.data) {
            continue;
        }
        auto* descriptions = static_cast<reshade::api::shader_desc*>(subobject.data);
        for (std::uint32_t shader_index = 0;
             shader_index < subobject.count;
             ++shader_index) {
            auto& description = descriptions[shader_index];
            if (!HasExpectedDxbcIdentity(description)) {
                if (LooksLikeMismatchedTarget(description) &&
                    !data->logged_identity_failure.exchange(
                        true, std::memory_order_relaxed)) {
                    Log(reshade::log::level::error,
                        "[ShenLong-AgX] Refused a near-match for shader 0x%08X because its DXBC identity was not exact; retaining the native shader.",
                        kFinalPreHudPixelShaderHash);
                }
                continue;
            }

            description.code = data->replacement_bytecode->GetBufferPointer();
            description.code_size = data->replacement_bytecode->GetBufferSize();
            data->replacement_requests.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void OnInitPipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!data || !data->ready || !data->replacement_bytecode ||
        pipeline.handle == 0) {
        return;
    }
    if (subobjects == nullptr && subobject_count != 0) {
        return;
    }

    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto& subobject = subobjects[index];
        if (subobject.type != reshade::api::pipeline_subobject_type::pixel_shader ||
            subobject.count == 0 || !subobject.data) {
            continue;
        }
        const auto* descriptions =
            static_cast<const reshade::api::shader_desc*>(subobject.data);
        for (std::uint32_t shader_index = 0;
             shader_index < subobject.count;
             ++shader_index) {
            const auto& description = descriptions[shader_index];
            if (description.code != data->replacement_bytecode->GetBufferPointer() ||
                description.code_size != data->replacement_bytecode->GetBufferSize()) {
                continue;
            }
            const std::uint64_t count = data->replacement_pipelines.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (!data->logged_active.exchange(true, std::memory_order_relaxed)) {
                Log(reshade::log::level::info,
                    "[ShenLong-AgX] Full-RGB AgX pipeline replacement confirmed for exact shader 0x%08X (original_size=%zu, replacement_size=%zu, replacements=%llu); native pre-HUD draw state and HUD path remain unchanged.",
                    kFinalPreHudPixelShaderHash,
                    kFinalPreHudPixelShaderSize,
                    data->replacement_bytecode->GetBufferSize(),
                    static_cast<unsigned long long>(count));
            }
            return;
        }
    }
}

void OnPresent(reshade::api::command_queue*, reshade::api::swapchain* swapchain,
               const reshade::api::rect*, const reshade::api::rect*,
               std::uint32_t, const reshade::api::rect*) {
    if (!swapchain) {
        return;
    }
    DeviceData* data = swapchain->get_device()->get_private_data<DeviceData>();
    if (!data || !data->settings.enabled || !data->ready ||
        data->logged_missing_pipeline.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t presents =
        data->presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (presents != 300) {
        return;
    }

    const std::uint64_t requested =
        data->replacement_requests.load(std::memory_order_relaxed);
    const std::uint64_t confirmed =
        data->replacement_pipelines.load(std::memory_order_relaxed);
    if (confirmed == 0 &&
        !data->logged_missing_pipeline.exchange(true, std::memory_order_relaxed)) {
        Log(reshade::log::level::warning,
            "[ShenLong-AgX] Exact final pre-HUD shader 0x%08X was not replaced after 300 presents (requests=%llu, confirmed=%llu); the native tone mapper remains active.",
            kFinalPreHudPixelShaderHash,
            static_cast<unsigned long long>(requested),
            static_cast<unsigned long long>(confirmed));
    }
}

}  // namespace

namespace spatch::graphics::tonemapping {

void Attach(HMODULE module) {
    g_module = module;
    reshade::register_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    reshade::register_event<reshade::addon_event::create_pipeline>(
        GuardedCallback<OnCreatePipeline>::Invoke);
    reshade::register_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
}

void Detach() noexcept {
    reshade::unregister_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::unregister_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::create_pipeline>(
        GuardedCallback<OnCreatePipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    g_module = nullptr;
}

}  // namespace spatch::graphics::tonemapping
