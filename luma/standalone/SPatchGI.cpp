// Clean-room hierarchical screen-space GI for Sleeping Dogs: Definitive Edition.
//
// This callback runs first at the game's exact final-composition draw. It
// updates the HDR lighting input with indirect diffuse, but leaves
// the native draw to the last AO-coordinator callback so SSS can run between
// the two stages without duplicating the final composition.

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
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "SPatchGraphicsComponents.hpp"
#include "SPatchIni.hpp"
#include "SPatchReShadeCallbackSafety.hpp"
#include "../overlay/Shaders/Sleeping Dogs Definitive Edition/SPatchGIShared.hlsli"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kFinalCompositionPixelShaderHash = 0x1964CD11;
constexpr std::uint32_t kThreadGroupSize = 8;
constexpr std::uint32_t kTraceMipCount = SPATCH_GI_MAX_TRACE_MIP + 1u;
constexpr float kThickness = 0.20f;
constexpr float kDepthFadeStart = 60.0f;
constexpr float kDepthFadeEnd = 120.0f;

constexpr GUID kPixelShaderHashTag = {
    0x3b36d657,
    0x1f9c,
    0x4cf1,
    {0x8b, 0x15, 0xab, 0xd4, 0x76, 0x88, 0x64, 0x41},
};

struct Settings {
    bool enabled = false;
    int quality = 2;
    float strength = 1.0f;
    float radius = 15.0f;
};

struct alignas(16) GiConstants {
    std::array<float, 2> full_resolution;
    std::array<float, 2> gi_resolution;
    float gi_radius;
    float gi_strength;
    float thickness;
    float depth_fade_start;
    float depth_fade_end;
    std::array<std::uint32_t, 3> padding;
};

static_assert(sizeof(GiConstants) == 48);

#if defined(SPATCH_GI_DEVELOPMENT)
enum class GiGpuStage : std::size_t {
    Prepare = 0,
    Downsample,
    Visibility,
    FilterHorizontal,
    FilterVertical,
    Composite,
    Copy,
    Count,
};

constexpr std::size_t kGiGpuStageCount =
    static_cast<std::size_t>(GiGpuStage::Count);

struct GpuTimingSlot {
    ComPtr<ID3D11Query> disjoint;
    std::array<ComPtr<ID3D11Query>, kGiGpuStageCount + 1u> timestamps{};
    bool pending = false;
};
#endif

struct ScratchResources {
    std::uint32_t full_width = 0;
    std::uint32_t full_height = 0;
    std::uint32_t gi_width = 0;
    std::uint32_t gi_height = 0;
    std::uint32_t mip_count = 0;

    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11ShaderResourceView> depth_srv;
    std::vector<ComPtr<ID3D11ShaderResourceView>> depth_mip_srvs;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> depth_mip_uavs;

    ComPtr<ID3D11Texture2D> radiance;
    ComPtr<ID3D11ShaderResourceView> radiance_srv;
    std::vector<ComPtr<ID3D11ShaderResourceView>> radiance_mip_srvs;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> radiance_mip_uavs;

    ComPtr<ID3D11Texture2D> normals;
    ComPtr<ID3D11ShaderResourceView> normals_srv;
    std::vector<ComPtr<ID3D11ShaderResourceView>> normal_mip_srvs;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> normal_mip_uavs;

    ComPtr<ID3D11Texture2D> raw_gi;
    ComPtr<ID3D11ShaderResourceView> raw_gi_srv;
    ComPtr<ID3D11UnorderedAccessView> raw_gi_uav;
    ComPtr<ID3D11Texture2D> filtered_gi_a;
    ComPtr<ID3D11ShaderResourceView> filtered_gi_a_srv;
    ComPtr<ID3D11UnorderedAccessView> filtered_gi_a_uav;
    ComPtr<ID3D11Texture2D> filtered_gi_b;
    ComPtr<ID3D11ShaderResourceView> filtered_gi_b_srv;
    ComPtr<ID3D11UnorderedAccessView> filtered_gi_b_uav;

    bool Matches(
        std::uint32_t expected_full_width,
        std::uint32_t expected_full_height,
        std::uint32_t expected_gi_width,
        std::uint32_t expected_gi_height) const noexcept {
        return full_width == expected_full_width &&
            full_height == expected_full_height &&
            gi_width == expected_gi_width && gi_height == expected_gi_height &&
            mip_count != 0 && depth && depth_srv && radiance && radiance_srv &&
            normals && normals_srv &&
            depth_mip_srvs.size() == mip_count &&
            depth_mip_uavs.size() == mip_count &&
            radiance_mip_srvs.size() == mip_count &&
            radiance_mip_uavs.size() == mip_count &&
            normal_mip_srvs.size() == mip_count &&
            normal_mip_uavs.size() == mip_count && raw_gi_srv && raw_gi_uav &&
            filtered_gi_a_srv && filtered_gi_a_uav && filtered_gi_b_srv &&
            filtered_gi_b_uav;
    }
};

struct __declspec(uuid("7A77109A-7952-4AD7-B3CF-AC2D77B822CE")) DeviceData {
    Settings settings;
    ID3D11Device* native_device = nullptr;
    bool ready = false;

    ComPtr<ID3D11ComputeShader> prepare_gbuffer_shader;
    ComPtr<ID3D11ComputeShader> downsample_mip_shader;
    ComPtr<ID3D11ComputeShader> prepare_normals_shader;
    ComPtr<ID3D11ComputeShader> visibility_gi_shader;
    ComPtr<ID3D11ComputeShader> horizontal_filter_shader;
    ComPtr<ID3D11ComputeShader> vertical_filter_shader;
    ComPtr<ID3D11VertexShader> fullscreen_vertex_shader;
    ComPtr<ID3D11PixelShader> composite_pixel_shader;
    ComPtr<ID3D11SamplerState> point_sampler;
    ComPtr<ID3D11SamplerState> linear_sampler;
    ComPtr<ID3D11RasterizerState> fullscreen_rasterizer_state;
    ComPtr<ID3D11BlendState> additive_blend_state;
    ComPtr<ID3D11DepthStencilState> no_depth_state;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture2D> lighting_target;
    ComPtr<ID3D11RenderTargetView> lighting_target_rtv;
    ScratchResources scratch;

    bool logged_active = false;
    bool logged_input_failure = false;
    bool logged_scratch_failure = false;
    std::uint32_t consecutive_input_failures = 0;
    std::uint64_t applied_frames = 0;
    std::uint32_t failed_full_width = 0;
    std::uint32_t failed_full_height = 0;
    std::uint32_t failed_gi_width = 0;
    std::uint32_t failed_gi_height = 0;
    std::uint32_t scratch_retry_cooldown = 0;
#if defined(SPATCH_GI_DEVELOPMENT)
    std::array<GpuTimingSlot, 8> gpu_timing_slots{};
    std::array<double, 120> gpu_timing_samples{};
    std::array<std::array<double, kGiGpuStageCount>, 120>
        gpu_stage_timing_samples{};
    std::size_t gpu_timing_cursor = 0;
    std::size_t gpu_timing_sample_count = 0;
    bool gpu_timing_ready = false;
    bool gpu_timing_logged = false;
#endif
};

struct __declspec(uuid("DA4BFBDB-D2A1-459F-AC6A-F9B1A5741664")) CommandListData {
    std::uint32_t pixel_shader_hash = 0;
    bool prepared = false;
};

HMODULE g_module = nullptr;
thread_local bool g_inside_gi = false;
#if defined(SPATCH_GI_DEVELOPMENT)
std::atomic<bool> g_development_enabled = true;
std::atomic<std::uint64_t> g_present_count = 0;
bool g_f10_was_down = false;
#endif

bool IsRuntimeEnabled(const DeviceData* data) noexcept {
    if (!data || !data->settings.enabled || !data->ready) {
        return false;
    }
#if defined(SPATCH_GI_DEVELOPMENT)
    return g_development_enabled.load(std::memory_order_relaxed);
#else
    return true;
#endif
}

template <typename Interface, typename Handle>
Interface* NativePointer(Handle handle) noexcept {
    static_assert(std::is_integral_v<Handle>);
    static_assert(sizeof(Handle) >= sizeof(std::uintptr_t));
    return reinterpret_cast<Interface*>(  // NOLINT(performance-no-int-to-ptr)
        static_cast<std::uintptr_t>(handle));
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
        "[ShenLong-GI] ReShade callback dropped after %s%s%s; native lighting remains active.",
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

int ClampInt(int value, int minimum, int maximum) noexcept {
    return (std::max)(minimum, (std::min)(maximum, value));
}

float ClampFloat(float value, float minimum, float maximum) noexcept {
    if (!std::isfinite(value)) {
        return minimum;
    }
    return (std::max)(minimum, (std::min)(maximum, value));
}

Settings LoadSettings() {
    Settings settings;
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        settings.enabled = false;
        Log(reshade::log::level::error,
            "[ShenLong-GI] Could not resolve the add-on directory; GI is disabled.");
        return settings;
    }
    const std::wstring path = directory + L"ShenLong.ini";
    namespace ini = spatch::graphics::ini;
    constexpr auto enabled_keys = ini::SettingKeys(
        L"GlobalIllumination", L"GlobalIllumination", L"global_illumination");
    constexpr auto quality_keys = ini::SettingKeys(
        L"GlobalIllumination", L"GIQuality", L"gi_quality");
    constexpr auto radius_keys = ini::SettingKeys(
        L"GlobalIllumination", L"GIRadius", L"gi_radius");
    constexpr auto strength_keys = ini::SettingKeys(
        L"GlobalIllumination", L"GIStrength", L"gi_strength_percent");
    const bool master_enabled = ini::ReadBool(path, ini::kMasterEnabledKeys, false);
    const bool feature_enabled = ini::ReadBool(
        path, enabled_keys, settings.enabled);
    settings.enabled = master_enabled && feature_enabled;
    settings.quality = ClampInt(ini::ReadInt(
        path, quality_keys, settings.quality), 0, 4);
    settings.radius = ClampFloat(ini::ReadFloat(
        path, radius_keys, settings.radius), 0.25f, 30.0f);
    settings.strength = ClampFloat(ini::ReadFloat(
        path, strength_keys, settings.strength * 100.0f), 0.0f, 200.0f) / 100.0f;
    Log(reshade::log::level::info,
        "[ShenLong-GI] config: enabled=%d quality=%d radius=%.2fm strength=%.0f%%; AO is selected independently",
        settings.enabled ? 1 : 0,
        settings.quality,
        settings.radius,
        settings.strength * 100.0f);
    return settings;
}

#if defined(SPATCH_GI_DEVELOPMENT)
bool CompileBytecode(
    const std::wstring& shader_path,
    const char* entry_point,
    const char* profile,
    const D3D_SHADER_MACRO* macros,
    ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const HRESULT result = D3DCompileFromFile(
        shader_path.c_str(),
        macros,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry_point,
        profile,
        flags,
        0,
        bytecode.ReleaseAndGetAddressOf(),
        errors.GetAddressOf());
    if (SUCCEEDED(result) && bytecode) {
        return true;
    }
    const char* detail = errors && errors->GetBufferPointer()
        ? static_cast<const char*>(errors->GetBufferPointer())
        : "no compiler diagnostics";
    Log(reshade::log::level::error,
        "[ShenLong-GI] Shader compile failed for %s: %s", entry_point, detail);
    return false;
}
#endif

bool LoadShaderBytecode(
    const std::wstring& cache_path,
    const std::wstring& shader_path,
    const char* entry_point,
    const char* profile,
    const D3D_SHADER_MACRO* macros,
    ComPtr<ID3DBlob>& bytecode,
    bool& used_source_fallback) {
    const HRESULT load_result = D3DReadFileToBlob(
        cache_path.c_str(), bytecode.ReleaseAndGetAddressOf());
    if (SUCCEEDED(load_result) && bytecode) {
        return true;
    }

#if defined(SPATCH_GI_DEVELOPMENT)
    used_source_fallback = true;
    Log(reshade::log::level::warning,
        "[ShenLong-GI] Precompiled shader cache unavailable for %s/%s "
        "(HRESULT=0x%08X); using the Development source fallback.",
        entry_point,
        profile,
        static_cast<unsigned int>(load_result));
    return CompileBytecode(shader_path, entry_point, profile, macros, bytecode);
#else
    static_cast<void>(shader_path);
    static_cast<void>(macros);
    static_cast<void>(used_source_fallback);
    Log(reshade::log::level::error,
        "[ShenLong-GI] Required precompiled shader cache entry is missing or "
        "invalid for %s/%s (HRESULT=0x%08X).",
        entry_point,
        profile,
        static_cast<unsigned int>(load_result));
    return false;
#endif
}

#if defined(SPATCH_GI_DEVELOPMENT)
void ResetGpuTiming(ID3D11Device* device, DeviceData& data) noexcept {
    data.gpu_timing_slots = {};
    data.gpu_timing_samples = {};
    data.gpu_stage_timing_samples = {};
    data.gpu_timing_cursor = 0;
    data.gpu_timing_sample_count = 0;
    data.gpu_timing_ready = device != nullptr;
    data.gpu_timing_logged = false;
    if (!device) {
        return;
    }

    D3D11_QUERY_DESC query_desc{};
    for (GpuTimingSlot& slot : data.gpu_timing_slots) {
        query_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device->CreateQuery(
                &query_desc, slot.disjoint.ReleaseAndGetAddressOf()))) {
            data.gpu_timing_ready = false;
            break;
        }
        query_desc.Query = D3D11_QUERY_TIMESTAMP;
        for (ComPtr<ID3D11Query>& timestamp : slot.timestamps) {
            if (FAILED(device->CreateQuery(
                    &query_desc, timestamp.ReleaseAndGetAddressOf()))) {
                data.gpu_timing_ready = false;
                break;
            }
        }
        if (!data.gpu_timing_ready) {
            break;
        }
    }
    if (!data.gpu_timing_ready) {
        data.gpu_timing_slots = {};
    }
}
#endif

bool InitializeShaders(ID3D11Device* device, DeviceData& data) {
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        return false;
    }
    const std::wstring shader_path =
        directory + L"ShenLong\\Shaders\\GI\\SPatchGI.hlsl";
    const std::wstring cache_root =
        directory + L"ShenLong\\ShaderCache\\v1\\GI\\";
    static constexpr std::array<const char*, 5> quality_values = {
        "0", "1", "2", "3", "4"};
    const char* half_resolution = data.settings.quality <= 3 ? "1" : "0";
    const std::wstring common_cache_suffix =
        L".q" + std::to_wstring(data.settings.quality) + L".half" +
        (data.settings.quality <= 3 ? L"1" : L"0");
    const D3D_SHADER_MACRO common_macros[] = {
        {"SPATCH_GI_QUALITY", quality_values[data.settings.quality]},
        {"SPATCH_GI_HALF_RES", half_resolution},
        {nullptr, nullptr},
    };
    const D3D_SHADER_MACRO no_macros[] = {
        {nullptr, nullptr},
    };
    const D3D_SHADER_MACRO horizontal_filter_macros[] = {
        {"SPATCH_GI_QUALITY", quality_values[data.settings.quality]},
        {"SPATCH_GI_HALF_RES", half_resolution},
        {"SPATCH_GI_FILTER_HORIZONTAL", "1"},
        {nullptr, nullptr},
    };
    const D3D_SHADER_MACRO vertical_filter_macros[] = {
        {"SPATCH_GI_QUALITY", quality_values[data.settings.quality]},
        {"SPATCH_GI_HALF_RES", half_resolution},
        {"SPATCH_GI_FILTER_HORIZONTAL", "0"},
        {nullptr, nullptr},
    };

    ComPtr<ID3DBlob> prepare;
    ComPtr<ID3DBlob> downsample;
    ComPtr<ID3DBlob> prepare_normals;
    ComPtr<ID3DBlob> visibility;
    ComPtr<ID3DBlob> horizontal_filter;
    ComPtr<ID3DBlob> vertical_filter;
    ComPtr<ID3DBlob> fullscreen_vertex;
    ComPtr<ID3DBlob> composite_pixel;
    bool used_source_fallback = false;
    if (!LoadShaderBytecode(
            cache_root + L"prepare_gbuffer_cs.cs_5_0" +
                common_cache_suffix + L".cso",
            shader_path, "prepare_gbuffer_cs", "cs_5_0", common_macros,
            prepare, used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"downsample_mip_cs.cs_5_0" +
                common_cache_suffix + L".cso",
            shader_path, "downsample_mip_cs", "cs_5_0", common_macros,
            downsample, used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"prepare_normals_cs.cs_5_0.cso",
            shader_path, "prepare_normals_cs", "cs_5_0", no_macros,
            prepare_normals, used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"visibility_gi_cs.cs_5_0" +
                common_cache_suffix + L".cso",
            shader_path, "visibility_gi_cs", "cs_5_0", common_macros,
            visibility, used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"spatial_filter_cs.cs_5_0" +
                common_cache_suffix + L".horizontal1.cso",
            shader_path, "spatial_filter_cs", "cs_5_0",
            horizontal_filter_macros, horizontal_filter,
            used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"spatial_filter_cs.cs_5_0" +
                common_cache_suffix + L".horizontal0.cso",
            shader_path, "spatial_filter_cs", "cs_5_0",
            vertical_filter_macros, vertical_filter, used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"FullscreenVS.vs_5_0" + common_cache_suffix + L".cso",
            shader_path, "FullscreenVS", "vs_5_0", common_macros,
            fullscreen_vertex, used_source_fallback) ||
        !LoadShaderBytecode(
            cache_root + L"CompositePS.ps_5_0" + common_cache_suffix + L".cso",
            shader_path, "CompositePS", "ps_5_0", common_macros,
            composite_pixel, used_source_fallback)) {
        return false;
    }

    const bool shaders_created =
        SUCCEEDED(device->CreateComputeShader(prepare->GetBufferPointer(),
            prepare->GetBufferSize(), nullptr,
            data.prepare_gbuffer_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateComputeShader(downsample->GetBufferPointer(),
            downsample->GetBufferSize(), nullptr,
            data.downsample_mip_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateComputeShader(prepare_normals->GetBufferPointer(),
            prepare_normals->GetBufferSize(), nullptr,
            data.prepare_normals_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateComputeShader(visibility->GetBufferPointer(),
            visibility->GetBufferSize(), nullptr,
            data.visibility_gi_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateComputeShader(horizontal_filter->GetBufferPointer(),
            horizontal_filter->GetBufferSize(), nullptr,
            data.horizontal_filter_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateComputeShader(vertical_filter->GetBufferPointer(),
            vertical_filter->GetBufferSize(), nullptr,
            data.vertical_filter_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateVertexShader(fullscreen_vertex->GetBufferPointer(),
            fullscreen_vertex->GetBufferSize(), nullptr,
            data.fullscreen_vertex_shader.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreatePixelShader(composite_pixel->GetBufferPointer(),
            composite_pixel->GetBufferSize(), nullptr,
            data.composite_pixel_shader.ReleaseAndGetAddressOf()));
    if (!shaders_created) {
        Log(reshade::log::level::error,
            "[ShenLong-GI] One or more D3D11 shader objects could not be created.");
        return false;
    }
    if (used_source_fallback) {
        Log(reshade::log::level::info,
            "[ShenLong-GI] Shader bytecode initialized with the Development "
            "source fallback.");
    } else {
        Log(reshade::log::level::info,
            "[ShenLong-GI] Precompiled shader cache v1 loaded "
            "(quality=%d, half_resolution=%s).",
            data.settings.quality,
            half_resolution);
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(
            &sampler_desc, data.point_sampler.ReleaseAndGetAddressOf()))) {
        return false;
    }
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(device->CreateSamplerState(
            &sampler_desc, data.linear_sampler.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer_desc,
            data.fullscreen_rasterizer_state.ReleaseAndGetAddressOf()))) {
        return false;
    }
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
        D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(device->CreateBlendState(
            &blend_desc, data.additive_blend_state.ReleaseAndGetAddressOf()))) {
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(
            &depth_desc, data.no_depth_state.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_BUFFER_DESC constant_desc{};
    constant_desc.ByteWidth = sizeof(GiConstants);
    constant_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(
            &constant_desc, nullptr, data.constants.ReleaseAndGetAddressOf()))) {
        return false;
    }

#if defined(SPATCH_GI_DEVELOPMENT)
    ResetGpuTiming(device, data);
#endif
    return true;
}

std::uint32_t CalculateMipCount(
    std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t count = 1;
    while (width > 1 || height > 1) {
        width = (std::max)(1u, width / 2u);
        height = (std::max)(1u, height / 2u);
        ++count;
    }
    return count;
}

bool CreateMipChain(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT format,
    std::uint32_t mip_count,
    ComPtr<ID3D11Texture2D>& texture,
    ComPtr<ID3D11ShaderResourceView>& full_srv,
    std::vector<ComPtr<ID3D11ShaderResourceView>>& mip_srvs,
    std::vector<ComPtr<ID3D11UnorderedAccessView>>& mip_uavs) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mip_count;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(
            &desc, nullptr, texture.ReleaseAndGetAddressOf()))) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC full_srv_desc{};
    full_srv_desc.Format = format;
    full_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    full_srv_desc.Texture2D.MostDetailedMip = 0;
    full_srv_desc.Texture2D.MipLevels = mip_count;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &full_srv_desc,
            full_srv.ReleaseAndGetAddressOf()))) {
        return false;
    }

    mip_srvs.clear();
    mip_uavs.clear();
    mip_srvs.resize(mip_count);
    mip_uavs.resize(mip_count);
    for (std::uint32_t mip = 0; mip < mip_count; ++mip) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = mip;
        srv_desc.Texture2D.MipLevels = 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = format;
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uav_desc.Texture2D.MipSlice = mip;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), &srv_desc,
                mip_srvs[mip].ReleaseAndGetAddressOf())) ||
            FAILED(device->CreateUnorderedAccessView(texture.Get(), &uav_desc,
                mip_uavs[mip].ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    return true;
}

bool CreateComputeTarget(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    ComPtr<ID3D11Texture2D>& texture,
    ComPtr<ID3D11ShaderResourceView>& srv,
    ComPtr<ID3D11UnorderedAccessView>& uav) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateTexture2D(
               &desc, nullptr, texture.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateShaderResourceView(
            texture.Get(), nullptr, srv.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(device->CreateUnorderedAccessView(
            texture.Get(), nullptr, uav.ReleaseAndGetAddressOf()));
}

bool BuildScratch(
    ID3D11Device* device,
    std::uint32_t full_width,
    std::uint32_t full_height,
    std::uint32_t gi_width,
    std::uint32_t gi_height,
    ScratchResources& output) {
    ScratchResources scratch;
    scratch.full_width = full_width;
    scratch.full_height = full_height;
    scratch.gi_width = gi_width;
    scratch.gi_height = gi_height;
    scratch.mip_count = (std::min)(
        CalculateMipCount(gi_width, gi_height), kTraceMipCount);
    if (!CreateMipChain(device, gi_width, gi_height, DXGI_FORMAT_R32_FLOAT,
            scratch.mip_count, scratch.depth, scratch.depth_srv,
            scratch.depth_mip_srvs, scratch.depth_mip_uavs) ||
        !CreateMipChain(device, gi_width, gi_height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, scratch.mip_count,
            scratch.radiance, scratch.radiance_srv,
            scratch.radiance_mip_srvs, scratch.radiance_mip_uavs) ||
        !CreateMipChain(device, gi_width, gi_height,
            DXGI_FORMAT_R16G16_FLOAT, scratch.mip_count,
            scratch.normals, scratch.normals_srv,
            scratch.normal_mip_srvs, scratch.normal_mip_uavs) ||
        !CreateComputeTarget(device, gi_width, gi_height, scratch.raw_gi,
            scratch.raw_gi_srv, scratch.raw_gi_uav) ||
        !CreateComputeTarget(device, gi_width, gi_height, scratch.filtered_gi_a,
            scratch.filtered_gi_a_srv, scratch.filtered_gi_a_uav) ||
        !CreateComputeTarget(device, gi_width, gi_height, scratch.filtered_gi_b,
            scratch.filtered_gi_b_srv, scratch.filtered_gi_b_uav)) {
        return false;
    }

    output = std::move(scratch);
    return true;
}

bool EnsureScratch(
    DeviceData& data,
    std::uint32_t full_width,
    std::uint32_t full_height) {
    const bool half_resolution = data.settings.quality <= 3;
    const std::uint32_t gi_width = half_resolution
        ? (full_width + 1u) / 2u
        : full_width;
    const std::uint32_t gi_height = half_resolution
        ? (full_height + 1u) / 2u
        : full_height;
    if (data.scratch.Matches(
            full_width, full_height, gi_width, gi_height)) {
        return true;
    }
    if (full_width < 16 || full_height < 16 || gi_width < 8 || gi_height < 8 ||
        !data.native_device) {
        return false;
    }

    const bool repeated_failure =
        data.failed_full_width == full_width &&
        data.failed_full_height == full_height &&
        data.failed_gi_width == gi_width &&
        data.failed_gi_height == gi_height;
    if (repeated_failure && data.scratch_retry_cooldown != 0) {
        --data.scratch_retry_cooldown;
        return false;
    }
    if (!repeated_failure) {
        data.logged_scratch_failure = false;
    }

    // Build the replacement transactionally. Keep the previous set alive until
    // every allocation succeeds so a transient resize failure does not drop a
    // working GI path; the move below releases it only after success.
    ScratchResources replacement;
    try {
        if (!BuildScratch(data.native_device, full_width, full_height,
                gi_width, gi_height, replacement)) {
            data.failed_full_width = full_width;
            data.failed_full_height = full_height;
            data.failed_gi_width = gi_width;
            data.failed_gi_height = gi_height;
            data.scratch_retry_cooldown = 120;
            if (!data.logged_scratch_failure) {
                data.logged_scratch_failure = true;
                Log(reshade::log::level::warning,
                    "[ShenLong-GI] Could not allocate the %ux%u GI resource graph for a %ux%u frame; native lighting remains active while allocation is retried.",
                    gi_width, gi_height, full_width, full_height);
            }
            return false;
        }
    } catch (...) {
        data.failed_full_width = full_width;
        data.failed_full_height = full_height;
        data.failed_gi_width = gi_width;
        data.failed_gi_height = gi_height;
        data.scratch_retry_cooldown = 120;
        if (!data.logged_scratch_failure) {
            data.logged_scratch_failure = true;
            Log(reshade::log::level::warning,
                "[ShenLong-GI] Resource allocation raised an exception for a %ux%u GI graph (%ux%u frame); native lighting remains active while allocation is retried.",
                gi_width, gi_height, full_width, full_height);
        }
        return false;
    }
    data.scratch = std::move(replacement);
    data.failed_full_width = 0;
    data.failed_full_height = 0;
    data.failed_gi_width = 0;
    data.failed_gi_height = 0;
    data.scratch_retry_cooldown = 0;
    data.logged_scratch_failure = false;
    data.logged_active = false;
#if defined(SPATCH_GI_DEVELOPMENT)
    ResetGpuTiming(data.native_device, data);
#endif
    return true;
}

template <typename Interface, std::size_t Count>
void AttachReturnedInterfaces(
    std::array<ComPtr<Interface>, Count>& destination,
    Interface* const* source) noexcept {
    for (std::size_t index = 0; index < destination.size(); ++index) {
        destination[index].Attach(source[index]);
    }
}

class ScopedComputeState {
public:
    explicit ScopedComputeState(ID3D11DeviceContext* context) noexcept
        : context_(context) {
        std::array<ID3D11ClassInstance*, 256> classes{};
        class_count_ = static_cast<UINT>(classes.size());
        context_->CSGetShader(shader_.GetAddressOf(), classes.data(), &class_count_);
        for (UINT index = 0; index < class_count_; ++index) {
            class_instances_[index].Attach(classes[index]);
        }
        std::array<ID3D11Buffer*, 2> buffers{};
        context_->CSGetConstantBuffers(9, 1, &buffers[0]);
        context_->CSGetConstantBuffers(11, 1, &buffers[1]);
        AttachReturnedInterfaces(constant_buffers_, buffers.data());
        std::array<ID3D11SamplerState*, 2> samplers{};
        context_->CSGetSamplers(0, 2, samplers.data());
        AttachReturnedInterfaces(samplers_, samplers.data());
        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        context_->CSGetShaderResources(
            0, static_cast<UINT>(srvs.size()), srvs.data());
        AttachReturnedInterfaces(srvs_, srvs.data());
        std::array<ID3D11UnorderedAccessView*, 2> uavs{};
        context_->CSGetUnorderedAccessViews(0, 2, uavs.data());
        AttachReturnedInterfaces(uavs_, uavs.data());
        context_->GetPredication(predicate_.GetAddressOf(), &predicate_value_);
        context_->SetPredication(nullptr, FALSE);
    }

    ScopedComputeState(const ScopedComputeState&) = delete;
    ScopedComputeState& operator=(const ScopedComputeState&) = delete;

    ~ScopedComputeState() noexcept {
        static constexpr std::array<ID3D11ShaderResourceView*, 3> null_srvs{};
        static constexpr std::array<ID3D11UnorderedAccessView*, 2> null_uavs{};
        context_->CSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context_->CSSetUnorderedAccessViews(0, 2, null_uavs.data(), nullptr);
        std::array<ID3D11UnorderedAccessView*, 2> uavs{};
        for (std::size_t index = 0; index < uavs.size(); ++index) {
            uavs[index] = uavs_[index].Get();
        }
        constexpr std::array<UINT, 2> keep_counters = {
            D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
            D3D11_KEEP_UNORDERED_ACCESS_VIEWS};
        context_->CSSetUnorderedAccessViews(0, 2, uavs.data(), keep_counters.data());
        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        for (std::size_t index = 0; index < srvs.size(); ++index) {
            srvs[index] = srvs_[index].Get();
        }
        context_->CSSetShaderResources(
            0, static_cast<UINT>(srvs.size()), srvs.data());
        ID3D11Buffer* buffer = constant_buffers_[0].Get();
        context_->CSSetConstantBuffers(9, 1, &buffer);
        buffer = constant_buffers_[1].Get();
        context_->CSSetConstantBuffers(11, 1, &buffer);
        std::array<ID3D11SamplerState*, 2> samplers{};
        for (std::size_t index = 0; index < samplers.size(); ++index) {
            samplers[index] = samplers_[index].Get();
        }
        context_->CSSetSamplers(0, 2, samplers.data());
        std::array<ID3D11ClassInstance*, 256> classes{};
        for (UINT index = 0; index < class_count_; ++index) {
            classes[index] = class_instances_[index].Get();
        }
        context_->CSSetShader(shader_.Get(), classes.data(), class_count_);
        context_->SetPredication(predicate_.Get(), predicate_value_);
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    ComPtr<ID3D11ComputeShader> shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> class_instances_{};
    UINT class_count_ = 0;
    std::array<ComPtr<ID3D11Buffer>, 2> constant_buffers_{};
    std::array<ComPtr<ID3D11SamplerState>, 2> samplers_{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> srvs_{};
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs_{};
    ComPtr<ID3D11Predicate> predicate_;
    BOOL predicate_value_ = FALSE;
};

class SavedGraphicsState {
public:
    explicit SavedGraphicsState(ID3D11DeviceContext* context) noexcept
        : context_(context) {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
            render_targets{};
        context_->OMGetRenderTargets(static_cast<UINT>(render_targets.size()),
            render_targets.data(), depth_stencil_.GetAddressOf());
        AttachReturnedInterfaces(render_targets_, render_targets.data());
        context_->OMGetBlendState(blend_state_.GetAddressOf(), blend_factor_.data(),
            &sample_mask_);
        context_->OMGetDepthStencilState(
            depth_state_.GetAddressOf(), &stencil_reference_);
        context_->RSGetState(rasterizer_state_.GetAddressOf());
        viewport_count_ = static_cast<UINT>(viewports_.size());
        context_->RSGetViewports(&viewport_count_, viewports_.data());
        scissor_count_ = static_cast<UINT>(scissors_.size());
        context_->RSGetScissorRects(&scissor_count_, scissors_.data());
        context_->IAGetInputLayout(input_layout_.GetAddressOf());
        context_->IAGetPrimitiveTopology(&topology_);
        std::array<ID3D11ClassInstance*, 256> classes{};
        vertex_class_count_ = static_cast<UINT>(classes.size());
        context_->VSGetShader(
            vertex_shader_.GetAddressOf(), classes.data(), &vertex_class_count_);
        AttachReturnedInterfaces(vertex_classes_, classes.data());
        classes.fill(nullptr);
        pixel_class_count_ = static_cast<UINT>(classes.size());
        context_->PSGetShader(
            pixel_shader_.GetAddressOf(), classes.data(), &pixel_class_count_);
        AttachReturnedInterfaces(pixel_classes_, classes.data());
        classes.fill(nullptr);
        geometry_class_count_ = static_cast<UINT>(classes.size());
        context_->GSGetShader(
            geometry_shader_.GetAddressOf(), classes.data(), &geometry_class_count_);
        AttachReturnedInterfaces(geometry_classes_, classes.data());
        classes.fill(nullptr);
        hull_class_count_ = static_cast<UINT>(classes.size());
        context_->HSGetShader(
            hull_shader_.GetAddressOf(), classes.data(), &hull_class_count_);
        AttachReturnedInterfaces(hull_classes_, classes.data());
        classes.fill(nullptr);
        domain_class_count_ = static_cast<UINT>(classes.size());
        context_->DSGetShader(
            domain_shader_.GetAddressOf(), classes.data(), &domain_class_count_);
        AttachReturnedInterfaces(domain_classes_, classes.data());
        std::array<ID3D11ShaderResourceView*, 9> srvs{};
        context_->PSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        AttachReturnedInterfaces(shader_resources_, srvs.data());
        std::array<ID3D11SamplerState*, 2> samplers{};
        context_->PSGetSamplers(0, 2, samplers.data());
        AttachReturnedInterfaces(samplers_, samplers.data());
        context_->PSGetConstantBuffers(9, 1, constant_buffer_9_.GetAddressOf());
        context_->PSGetConstantBuffers(11, 1, constant_buffer_11_.GetAddressOf());
    }

    SavedGraphicsState(const SavedGraphicsState&) = delete;
    SavedGraphicsState& operator=(const SavedGraphicsState&) = delete;

    ~SavedGraphicsState() noexcept {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
            render_targets{};
        for (std::size_t index = 0; index < render_targets.size(); ++index) {
            render_targets[index] = render_targets_[index].Get();
        }
        context_->OMSetRenderTargets(static_cast<UINT>(render_targets.size()),
            render_targets.data(), depth_stencil_.Get());
        context_->OMSetBlendState(
            blend_state_.Get(), blend_factor_.data(), sample_mask_);
        context_->OMSetDepthStencilState(
            depth_state_.Get(), stencil_reference_);
        context_->RSSetState(rasterizer_state_.Get());
        context_->RSSetViewports(
            viewport_count_, viewport_count_ != 0 ? viewports_.data() : nullptr);
        context_->RSSetScissorRects(
            scissor_count_, scissor_count_ != 0 ? scissors_.data() : nullptr);
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetPrimitiveTopology(topology_);
        std::array<ID3D11ClassInstance*, 256> classes{};
        for (UINT index = 0; index < vertex_class_count_; ++index) {
            classes[index] = vertex_classes_[index].Get();
        }
        context_->VSSetShader(
            vertex_shader_.Get(), classes.data(), vertex_class_count_);
        classes.fill(nullptr);
        for (UINT index = 0; index < pixel_class_count_; ++index) {
            classes[index] = pixel_classes_[index].Get();
        }
        context_->PSSetShader(pixel_shader_.Get(), classes.data(), pixel_class_count_);
        classes.fill(nullptr);
        for (UINT index = 0; index < geometry_class_count_; ++index) {
            classes[index] = geometry_classes_[index].Get();
        }
        context_->GSSetShader(
            geometry_shader_.Get(), classes.data(), geometry_class_count_);
        classes.fill(nullptr);
        for (UINT index = 0; index < hull_class_count_; ++index) {
            classes[index] = hull_classes_[index].Get();
        }
        context_->HSSetShader(hull_shader_.Get(), classes.data(), hull_class_count_);
        classes.fill(nullptr);
        for (UINT index = 0; index < domain_class_count_; ++index) {
            classes[index] = domain_classes_[index].Get();
        }
        context_->DSSetShader(
            domain_shader_.Get(), classes.data(), domain_class_count_);
        std::array<ID3D11ShaderResourceView*, 9> srvs{};
        for (std::size_t index = 0; index < srvs.size(); ++index) {
            srvs[index] = shader_resources_[index].Get();
        }
        context_->PSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        std::array<ID3D11SamplerState*, 2> samplers{};
        for (std::size_t index = 0; index < samplers.size(); ++index) {
            samplers[index] = samplers_[index].Get();
        }
        context_->PSSetSamplers(0, 2, samplers.data());
        ID3D11Buffer* buffer = constant_buffer_9_.Get();
        context_->PSSetConstantBuffers(9, 1, &buffer);
        buffer = constant_buffer_11_.Get();
        context_->PSSetConstantBuffers(11, 1, &buffer);
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        render_targets_{};
    ComPtr<ID3D11DepthStencilView> depth_stencil_;
    ComPtr<ID3D11BlendState> blend_state_;
    std::array<float, 4> blend_factor_{};
    UINT sample_mask_ = 0;
    ComPtr<ID3D11DepthStencilState> depth_state_;
    UINT stencil_reference_ = 0;
    ComPtr<ID3D11RasterizerState> rasterizer_state_;
    std::array<D3D11_VIEWPORT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
    UINT viewport_count_ = 0;
    std::array<D3D11_RECT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors_{};
    UINT scissor_count_ = 0;
    ComPtr<ID3D11InputLayout> input_layout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> vertex_classes_{};
    UINT vertex_class_count_ = 0;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> pixel_classes_{};
    UINT pixel_class_count_ = 0;
    ComPtr<ID3D11GeometryShader> geometry_shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> geometry_classes_{};
    UINT geometry_class_count_ = 0;
    ComPtr<ID3D11HullShader> hull_shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> hull_classes_{};
    UINT hull_class_count_ = 0;
    ComPtr<ID3D11DomainShader> domain_shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> domain_classes_{};
    UINT domain_class_count_ = 0;
    std::array<ComPtr<ID3D11ShaderResourceView>, 9> shader_resources_{};
    std::array<ComPtr<ID3D11SamplerState>, 2> samplers_{};
    ComPtr<ID3D11Buffer> constant_buffer_9_;
    ComPtr<ID3D11Buffer> constant_buffer_11_;
};

bool ReadTexture(
    ID3D11ShaderResourceView* view,
    D3D11_SHADER_RESOURCE_VIEW_DESC& view_desc,
    ComPtr<ID3D11Texture2D>& texture,
    D3D11_TEXTURE2D_DESC& texture_desc) {
    if (!view) {
        return false;
    }
    view->GetDesc(&view_desc);
    if (view_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        view_desc.Texture2D.MostDetailedMip != 0) {
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    if (!resource || FAILED(resource.As(&texture)) || !texture) {
        return false;
    }
    texture->GetDesc(&texture_desc);
    return true;
}

bool IsSupportedDepthFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
        format == DXGI_FORMAT_R32_FLOAT ||
        format == DXGI_FORMAT_R16_UNORM ||
        format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

struct CompositionInputs {
    ComPtr<ID3D11ShaderResourceView> albedo_srv;
    ComPtr<ID3D11ShaderResourceView> depth_srv;
    ComPtr<ID3D11ShaderResourceView> lighting_srv;
    ComPtr<ID3D11Texture2D> lighting_texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

bool ReadCompositionInputs(
    ID3D11DeviceContext* context,
    CompositionInputs& inputs) {
    std::array<ID3D11ShaderResourceView*, 4> views{};
    context->PSGetShaderResources(1, static_cast<UINT>(views.size()), views.data());
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> captured_views{};
    AttachReturnedInterfaces(captured_views, views.data());
    inputs.albedo_srv = captured_views[0];
    inputs.depth_srv = captured_views[1];
    inputs.lighting_srv = captured_views[3];
    if (!inputs.albedo_srv || !inputs.depth_srv || !inputs.lighting_srv) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC albedo_view{};
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_view{};
    D3D11_SHADER_RESOURCE_VIEW_DESC lighting_view{};
    ComPtr<ID3D11Texture2D> albedo_texture;
    ComPtr<ID3D11Texture2D> depth_texture;
    D3D11_TEXTURE2D_DESC albedo_desc{};
    D3D11_TEXTURE2D_DESC depth_desc{};
    D3D11_TEXTURE2D_DESC lighting_desc{};
    if (!ReadTexture(inputs.albedo_srv.Get(), albedo_view, albedo_texture, albedo_desc) ||
        !ReadTexture(inputs.depth_srv.Get(), depth_view, depth_texture, depth_desc) ||
        !ReadTexture(inputs.lighting_srv.Get(), lighting_view,
            inputs.lighting_texture, lighting_desc)) {
        return false;
    }

    const bool albedo_format = albedo_view.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        albedo_view.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool full_resolution_inputs =
        albedo_desc.Width == depth_desc.Width &&
        albedo_desc.Height == depth_desc.Height &&
        lighting_desc.Width == depth_desc.Width &&
        lighting_desc.Height == depth_desc.Height;
    const bool simple_textures =
        albedo_desc.ArraySize == 1 && depth_desc.ArraySize == 1 &&
        lighting_desc.ArraySize == 1 &&
        albedo_desc.SampleDesc.Count == 1 && depth_desc.SampleDesc.Count == 1 &&
        lighting_desc.SampleDesc.Count == 1 &&
        lighting_desc.MipLevels == 1;
    if (!albedo_format || !IsSupportedDepthFormat(depth_view.Format) ||
        lighting_view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        lighting_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        (lighting_desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 ||
        !full_resolution_inputs || !simple_textures ||
        depth_desc.Width < 16 || depth_desc.Height < 16) {
        return false;
    }
    inputs.width = depth_desc.Width;
    inputs.height = depth_desc.Height;
    return true;
}

bool EnsureLightingTarget(
    DeviceData& data,
    const CompositionInputs& inputs) {
    if (data.lighting_target.Get() == inputs.lighting_texture.Get() &&
        data.lighting_target_rtv) {
        return true;
    }
    if (!data.native_device || !inputs.lighting_texture) {
        return false;
    }

    ComPtr<ID3D11RenderTargetView> replacement_rtv;
    if (FAILED(data.native_device->CreateRenderTargetView(
            inputs.lighting_texture.Get(), nullptr,
            replacement_rtv.ReleaseAndGetAddressOf()))) {
        return false;
    }
    data.lighting_target = inputs.lighting_texture;
    data.lighting_target_rtv = std::move(replacement_rtv);
    return true;
}

bool UpdateConstants(
    ID3D11DeviceContext* context,
    const DeviceData& data) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(data.constants.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    const ScratchResources& scratch = data.scratch;
    const GiConstants constants = {
        {static_cast<float>(scratch.full_width),
            static_cast<float>(scratch.full_height)},
        {static_cast<float>(scratch.gi_width),
            static_cast<float>(scratch.gi_height)},
        data.settings.radius,
        data.settings.strength,
        kThickness,
        kDepthFadeStart,
        kDepthFadeEnd,
        {0u, 0u, 0u},
    };
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(data.constants.Get(), 0);
    return true;
}

void ClearComputeBindings(ID3D11DeviceContext* context) noexcept {
    static constexpr std::array<ID3D11ShaderResourceView*, 3> null_srvs{};
    static constexpr std::array<ID3D11UnorderedAccessView*, 2> null_uavs{};
    context->CSSetShaderResources(
        0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(null_uavs.size()), null_uavs.data(), nullptr);
}

#if defined(SPATCH_GI_DEVELOPMENT)
void RecordGpuTiming(
    DeviceData& data,
    double milliseconds,
    const std::array<double, kGiGpuStageCount>& stage_milliseconds) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
        data.gpu_timing_logged ||
        data.gpu_timing_sample_count >= data.gpu_timing_samples.size()) {
        return;
    }
    for (const double stage : stage_milliseconds) {
        if (!std::isfinite(stage) || stage < 0.0) {
            return;
        }
    }
    const std::size_t sample_index = data.gpu_timing_sample_count++;
    data.gpu_timing_samples[sample_index] = milliseconds;
    data.gpu_stage_timing_samples[sample_index] = stage_milliseconds;
    if (data.gpu_timing_sample_count != data.gpu_timing_samples.size()) {
        return;
    }
    std::array<double, 120> sorted = data.gpu_timing_samples;
    std::sort(sorted.begin(), sorted.end());
    const double average = std::accumulate(
        sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    std::array<double, kGiGpuStageCount> stage_averages{};
    for (const auto& sample : data.gpu_stage_timing_samples) {
        for (std::size_t stage = 0; stage < stage_averages.size(); ++stage) {
            stage_averages[stage] += sample[stage];
        }
    }
    for (double& stage_average : stage_averages) {
        stage_average /= static_cast<double>(data.gpu_stage_timing_samples.size());
    }
    const std::size_t p95_index = static_cast<std::size_t>(
        0.95 * static_cast<double>(sorted.size() - 1));
    data.gpu_timing_logged = true;
    Log(reshade::log::level::info,
        "[ShenLong-GI] GPU timing at %ux%u (GI %ux%u): average=%.3f ms, p95=%.3f ms, maximum=%.3f ms (%zu frames).",
        data.scratch.full_width,
        data.scratch.full_height,
        data.scratch.gi_width,
        data.scratch.gi_height,
        average,
        sorted[p95_index],
        sorted.back(),
        sorted.size());
    Log(reshade::log::level::info,
        "[ShenLong-GI] GPU stage timing: prepare=%.3f ms, downsample=%.3f ms, visibility=%.3f ms, filter-horizontal=%.3f ms, filter-vertical=%.3f ms, composite=%.3f ms, copy=%.3f ms.",
        stage_averages[static_cast<std::size_t>(GiGpuStage::Prepare)],
        stage_averages[static_cast<std::size_t>(GiGpuStage::Downsample)],
        stage_averages[static_cast<std::size_t>(GiGpuStage::Visibility)],
        stage_averages[static_cast<std::size_t>(GiGpuStage::FilterHorizontal)],
        stage_averages[static_cast<std::size_t>(GiGpuStage::FilterVertical)],
        stage_averages[static_cast<std::size_t>(GiGpuStage::Composite)],
        stage_averages[static_cast<std::size_t>(GiGpuStage::Copy)]);
}

bool ConsumeGpuTiming(
    ID3D11DeviceContext* context,
    DeviceData& data,
    GpuTimingSlot& slot) noexcept {
    if (!slot.pending) {
        return true;
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    std::array<UINT64, kGiGpuStageCount + 1u> timestamps{};
    constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (context->GetData(
            slot.disjoint.Get(), &disjoint, sizeof(disjoint), flags) != S_OK) {
        return false;
    }
    for (std::size_t index = 0; index < timestamps.size(); ++index) {
        if (context->GetData(slot.timestamps[index].Get(), &timestamps[index],
                sizeof(timestamps[index]), flags) != S_OK) {
            return false;
        }
    }
    slot.pending = false;
    if (!disjoint.Disjoint && disjoint.Frequency != 0) {
        std::array<double, kGiGpuStageCount> stages{};
        for (std::size_t stage = 0; stage < stages.size(); ++stage) {
            if (timestamps[stage + 1u] < timestamps[stage]) {
                return true;
            }
            stages[stage] =
                static_cast<double>(timestamps[stage + 1u] - timestamps[stage]) *
                1000.0 / static_cast<double>(disjoint.Frequency);
        }
        RecordGpuTiming(data,
            static_cast<double>(timestamps.back() - timestamps.front()) * 1000.0 /
                static_cast<double>(disjoint.Frequency),
            stages);
    }
    return true;
}

class GpuTimingScope {
public:
    GpuTimingScope(ID3D11DeviceContext* context, DeviceData& data) noexcept
        : context_(context), data_(data) {
        if (!data_.gpu_timing_ready || data_.gpu_timing_logged) {
            return;
        }
        for (std::size_t attempt = 0; attempt < data_.gpu_timing_slots.size(); ++attempt) {
            const std::size_t index =
                (data_.gpu_timing_cursor + attempt) % data_.gpu_timing_slots.size();
            GpuTimingSlot& candidate = data_.gpu_timing_slots[index];
            if (!ConsumeGpuTiming(context_, data_, candidate)) {
                continue;
            }
            slot_ = &candidate;
            data_.gpu_timing_cursor = (index + 1) % data_.gpu_timing_slots.size();
            context_->Begin(slot_->disjoint.Get());
            context_->End(slot_->timestamps[0].Get());
            return;
        }
    }

    GpuTimingScope(const GpuTimingScope&) = delete;
    GpuTimingScope& operator=(const GpuTimingScope&) = delete;

    void Mark(GiGpuStage stage) noexcept {
        if (!slot_ || invalid_ ||
            static_cast<std::size_t>(stage) != marked_stage_count_) {
            invalid_ = true;
            return;
        }
        context_->End(slot_->timestamps[marked_stage_count_ + 1u].Get());
        ++marked_stage_count_;
    }

    ~GpuTimingScope() {
        if (!slot_) {
            return;
        }
        context_->End(slot_->disjoint.Get());
        slot_->pending = !invalid_ && marked_stage_count_ == kGiGpuStageCount;
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    DeviceData& data_;
    GpuTimingSlot* slot_ = nullptr;
    std::size_t marked_stage_count_ = 0;
    bool invalid_ = false;
};
#endif

bool RunGlobalIllumination(
    ID3D11DeviceContext* context,
    DeviceData& data,
    const CompositionInputs& inputs,
    ID3D11Buffer* projection_constants) {
    if (!projection_constants || !EnsureScratch(data, inputs.width, inputs.height) ||
        !EnsureLightingTarget(data, inputs)) {
        return false;
    }
    ScratchResources& scratch = data.scratch;

#if defined(SPATCH_GI_DEVELOPMENT)
    GpuTimingScope gpu_timing(context, data);
#endif
    SavedGraphicsState graphics_state(context);
    ScopedComputeState compute_state(context);

    static constexpr std::array<ID3D11ShaderResourceView*, 9> null_ps_srvs{};
    context->PSSetShaderResources(0,
        static_cast<UINT>(null_ps_srvs.size()), null_ps_srvs.data());
    context->OMSetRenderTargets(0, nullptr, nullptr);
    // Sample the engine lighting before it is rebound as the direct-additive
    // composite target; this avoids a duplicate full-resolution HDR texture.

    ID3D11Buffer* buffer = projection_constants;
    context->CSSetConstantBuffers(9, 1, &buffer);
    buffer = data.constants.Get();
    context->CSSetConstantBuffers(11, 1, &buffer);
    std::array<ID3D11SamplerState*, 2> samplers = {
        data.point_sampler.Get(), data.linear_sampler.Get()};
    context->CSSetSamplers(0, 2, samplers.data());

    if (!UpdateConstants(context, data)) {
        return false;
    }
    ClearComputeBindings(context);
    std::array<ID3D11ShaderResourceView*, 3> compute_srvs = {
        inputs.depth_srv.Get(), inputs.lighting_srv.Get(), nullptr};
    std::array<ID3D11UnorderedAccessView*, 2> compute_uavs = {
        scratch.depth_mip_uavs[0].Get(), scratch.radiance_mip_uavs[0].Get()};
    context->CSSetShaderResources(0, 2, compute_srvs.data());
    context->CSSetUnorderedAccessViews(0, 2, compute_uavs.data(), nullptr);
    context->CSSetShader(data.prepare_gbuffer_shader.Get(), nullptr, 0);
    context->Dispatch((scratch.gi_width + kThreadGroupSize - 1u) / kThreadGroupSize,
        (scratch.gi_height + kThreadGroupSize - 1u) / kThreadGroupSize, 1);
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::Prepare);
#endif

    std::uint32_t mip_width = scratch.gi_width;
    std::uint32_t mip_height = scratch.gi_height;
    for (std::uint32_t mip = 1; mip < scratch.mip_count; ++mip) {
        ClearComputeBindings(context);
        compute_srvs = {scratch.depth_mip_srvs[mip - 1u].Get(),
            scratch.radiance_mip_srvs[mip - 1u].Get()};
        compute_uavs = {scratch.depth_mip_uavs[mip].Get(),
            scratch.radiance_mip_uavs[mip].Get()};
        context->CSSetShaderResources(0, 2, compute_srvs.data());
        context->CSSetUnorderedAccessViews(0, 2, compute_uavs.data(), nullptr);
        context->CSSetShader(data.downsample_mip_shader.Get(), nullptr, 0);
        mip_width = (std::max)(1u, mip_width / 2u);
        mip_height = (std::max)(1u, mip_height / 2u);
        context->Dispatch((mip_width + kThreadGroupSize - 1u) / kThreadGroupSize,
            (mip_height + kThreadGroupSize - 1u) / kThreadGroupSize, 1);
    }

    mip_width = scratch.gi_width;
    mip_height = scratch.gi_height;
    for (std::uint32_t mip = 0; mip < scratch.mip_count; ++mip) {
        ClearComputeBindings(context);
        compute_srvs = {scratch.depth_mip_srvs[mip].Get(), nullptr, nullptr};
        compute_uavs = {scratch.normal_mip_uavs[mip].Get(), nullptr};
        context->CSSetShaderResources(0, 1, compute_srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, compute_uavs.data(), nullptr);
        context->CSSetShader(data.prepare_normals_shader.Get(), nullptr, 0);
        context->Dispatch((mip_width + kThreadGroupSize - 1u) / kThreadGroupSize,
            (mip_height + kThreadGroupSize - 1u) / kThreadGroupSize, 1);
        mip_width = (std::max)(1u, mip_width / 2u);
        mip_height = (std::max)(1u, mip_height / 2u);
    }
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::Downsample);
#endif

    ClearComputeBindings(context);
    compute_srvs = {scratch.depth_srv.Get(), scratch.radiance_srv.Get(),
        scratch.normals_srv.Get()};
    compute_uavs = {scratch.raw_gi_uav.Get(), nullptr};
    context->CSSetShaderResources(0, 3, compute_srvs.data());
    context->CSSetUnorderedAccessViews(0, 2, compute_uavs.data(), nullptr);
    context->CSSetShader(data.visibility_gi_shader.Get(), nullptr, 0);
    context->Dispatch((scratch.gi_width + kThreadGroupSize - 1u) / kThreadGroupSize,
        (scratch.gi_height + kThreadGroupSize - 1u) / kThreadGroupSize, 1);
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::Visibility);
#endif

    ClearComputeBindings(context);
    compute_srvs = {scratch.raw_gi_srv.Get(), scratch.depth_mip_srvs[0].Get()};
    compute_uavs = {scratch.filtered_gi_a_uav.Get(), nullptr};
    context->CSSetShaderResources(0, 2, compute_srvs.data());
    context->CSSetUnorderedAccessViews(0, 2, compute_uavs.data(), nullptr);
    context->CSSetShader(data.horizontal_filter_shader.Get(), nullptr, 0);
    context->Dispatch((scratch.gi_width + kThreadGroupSize - 1u) / kThreadGroupSize,
        (scratch.gi_height + kThreadGroupSize - 1u) / kThreadGroupSize, 1);
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::FilterHorizontal);
#endif

    ClearComputeBindings(context);
    compute_srvs = {scratch.filtered_gi_a_srv.Get(), scratch.depth_mip_srvs[0].Get()};
    compute_uavs = {scratch.filtered_gi_b_uav.Get(), nullptr};
    context->CSSetShaderResources(0, 2, compute_srvs.data());
    context->CSSetUnorderedAccessViews(0, 2, compute_uavs.data(), nullptr);
    context->CSSetShader(data.vertical_filter_shader.Get(), nullptr, 0);
    context->Dispatch((scratch.gi_width + kThreadGroupSize - 1u) / kThreadGroupSize,
        (scratch.gi_height + kThreadGroupSize - 1u) / kThreadGroupSize, 1);
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::FilterVertical);
#endif
    ClearComputeBindings(context);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(data.fullscreen_vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(data.composite_pixel_shader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(data.fullscreen_rasterizer_state.Get());
    const D3D11_VIEWPORT viewport{0.0f, 0.0f,
        static_cast<float>(scratch.full_width),
        static_cast<float>(scratch.full_height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0,
        static_cast<LONG>(scratch.full_width),
        static_cast<LONG>(scratch.full_height)};
    context->RSSetScissorRects(1, &scissor);
    const FLOAT blend_factor[4]{};
    context->OMSetBlendState(
        data.additive_blend_state.Get(), blend_factor, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(data.no_depth_state.Get(), 0);
    ID3D11Buffer* ps_buffer = projection_constants;
    context->PSSetConstantBuffers(9, 1, &ps_buffer);
    ps_buffer = data.constants.Get();
    context->PSSetConstantBuffers(11, 1, &ps_buffer);
    context->PSSetSamplers(0, 2, samplers.data());
    std::array<ID3D11ShaderResourceView*, 4> composite_srvs = {
        scratch.filtered_gi_b_srv.Get(),
        scratch.depth_mip_srvs[0].Get(),
        inputs.depth_srv.Get(),
        inputs.albedo_srv.Get(),
    };
    context->PSSetShaderResources(
        0, static_cast<UINT>(composite_srvs.size()), composite_srvs.data());
    ID3D11RenderTargetView* target = data.lighting_target_rtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::Composite);
#endif

    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->PSSetShaderResources(
        0, static_cast<UINT>(null_ps_srvs.size()), null_ps_srvs.data());
#if defined(SPATCH_GI_DEVELOPMENT)
    gpu_timing.Mark(GiGpuStage::Copy);
#endif
    return true;
}

void RecordInputFailure(DeviceData& data) noexcept {
    // Menus and startup movies legitimately do not bind the gameplay G-buffer
    // inputs. Report only a regression after GI has succeeded at least once.
    if (data.applied_frames == 0) {
        data.consecutive_input_failures = 0;
        return;
    }
    constexpr std::uint32_t kFailureLogThreshold = 120;
    if (data.consecutive_input_failures < kFailureLogThreshold) {
        ++data.consecutive_input_failures;
    }
    if (data.consecutive_input_failures == kFailureLogThreshold &&
        !data.logged_input_failure) {
        data.logged_input_failure = true;
        Log(reshade::log::level::warning,
            "[ShenLong-GI] required final-composition inputs were unavailable for 120 consecutive attempts; affected frames use the native lighting path.");
    }
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
            "[ShenLong-GI] Unsupported graphics API; GI is disabled.");
        return;
    }
    data->native_device = NativePointer<ID3D11Device>(device->get_native());
    data->ready = data->native_device && InitializeShaders(data->native_device, *data);
    if (!data->ready) {
        Log(reshade::log::level::warning,
            "[ShenLong-GI] Initialization failed; native lighting remains active.");
    }
}

void OnDestroyDevice(reshade::api::device* device) {
    device->destroy_private_data<DeviceData>();
}

void OnInitCommandList(reshade::api::command_list* command_list) {
    spatch::graphics::detail::CreatePrivateData<CommandListData>(command_list);
}

void OnDestroyCommandList(reshade::api::command_list* command_list) {
    command_list->destroy_private_data<CommandListData>();
}

void OnResetCommandList(reshade::api::command_list* command_list) {
    if (CommandListData* data = command_list->get_private_data<CommandListData>()) {
        *data = {};
    }
}

void OnInitPipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!IsRuntimeEnabled(data) || pipeline.handle == 0 ||
        device->get_api() != reshade::api::device_api::d3d11) {
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
             shader_index < subobject.count; ++shader_index) {
            const auto& description = descriptions[shader_index];
            if (!description.code || description.code_size == 0) {
                continue;
            }
            const std::uint32_t hash = compute_crc32(
                static_cast<const std::uint8_t*>(description.code),
                description.code_size);
            if (hash != kFinalCompositionPixelShaderHash) {
                continue;
            }
            auto* native_shader = NativePointer<ID3D11PixelShader>(pipeline.handle);
            const HRESULT result = native_shader->SetPrivateData(
                kPixelShaderHashTag, sizeof(hash), &hash);
            if (FAILED(result)) {
                Log(reshade::log::level::warning,
                    "[ShenLong-GI] Could not tag the final composition shader (HRESULT=0x%08X).",
                    static_cast<unsigned int>(result));
            }
            return;
        }
    }
}

void OnBindPipeline(
    reshade::api::command_list* command_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline pipeline) {
    if (g_inside_gi ||
        (stages & reshade::api::pipeline_stage::pixel_shader) == 0) {
        return;
    }
    const DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!IsRuntimeEnabled(device_data)) {
        return;
    }
    CommandListData* data = command_list->get_private_data<CommandListData>();
    if (!data) {
        return;
    }
    data->pixel_shader_hash = 0;
    if (pipeline.handle == 0) {
        return;
    }
    UINT size = sizeof(data->pixel_shader_hash);
    NativePointer<ID3D11PixelShader>(pipeline.handle)->GetPrivateData(
        kPixelShaderHashTag, &size, &data->pixel_shader_hash);
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::int32_t,
    std::uint32_t) {
    if (g_inside_gi) {
        return false;
    }
    DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!IsRuntimeEnabled(device_data)) {
        return false;
    }
    CommandListData* command_data =
        command_list->get_private_data<CommandListData>();
    if (!command_data ||
        command_data->pixel_shader_hash != kFinalCompositionPixelShaderHash) {
        return false;
    }
    command_data->prepared = false;
    auto* context = NativePointer<ID3D11DeviceContext>(command_list->get_native());
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        RecordInputFailure(*device_data);
        return false;
    }
    CompositionInputs inputs;
    ID3D11Buffer* projection_constants =
        spatch::graphics::ao::GetNativeAoConstants(command_list);
    if (!ReadCompositionInputs(context, inputs) || !projection_constants) {
        RecordInputFailure(*device_data);
        return false;
    }
    g_inside_gi = true;
    const bool prepared = RunGlobalIllumination(
        context, *device_data, inputs, projection_constants);
    g_inside_gi = false;
    if (!prepared) {
        RecordInputFailure(*device_data);
        return false;
    }
    command_data->prepared = true;
    device_data->consecutive_input_failures = 0;
    device_data->logged_input_failure = false;
    return false;
}

}  // namespace

namespace spatch::graphics::gi {

bool DrawPreparedComposition(
    reshade::api::command_list* command_list,
    ID3D11ShaderResourceView* ambient_occlusion_override,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) noexcept {
    if (!command_list || g_inside_gi) {
        return false;
    }
    CommandListData* command_data =
        command_list->get_private_data<CommandListData>();
    DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!command_data || !IsRuntimeEnabled(device_data) ||
        !command_data->prepared) {
        return false;
    }
    command_data->prepared = false;
    auto* context = NativePointer<ID3D11DeviceContext>(command_list->get_native());
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> original_ao;
    if (ambient_occlusion_override) {
        context->PSGetShaderResources(7, 1, original_ao.GetAddressOf());
        context->PSSetShaderResources(7, 1, &ambient_occlusion_override);
    }
    g_inside_gi = true;
    if (instance_count == 1 && first_instance == 0) {
        context->DrawIndexed(index_count, first_index, vertex_offset);
    } else {
        context->DrawIndexedInstanced(index_count, instance_count, first_index,
            vertex_offset, first_instance);
    }
    g_inside_gi = false;
    if (ambient_occlusion_override) {
        ID3D11ShaderResourceView* restored_ao = original_ao.Get();
        context->PSSetShaderResources(7, 1, &restored_ao);
    }

    ++device_data->applied_frames;
    if (!device_data->logged_active) {
        device_data->logged_active = true;
        const ScratchResources& scratch = device_data->scratch;
        Log(reshade::log::level::info,
            "[ShenLong-GI] active at %ux%u (GI %ux%u); quality=%d, mips=%u, passes=%u (prepare + %u downsample + %u normal-cache + trace + 2 filter + direct-additive composite), HDR diffuse bounce enabled; AO is independently selected and only overridden by the AO coordinator.",
            scratch.full_width,
            scratch.full_height,
            scratch.gi_width,
            scratch.gi_height,
            device_data->settings.quality,
            scratch.mip_count,
            scratch.mip_count * 2u + 4u,
            scratch.mip_count - 1u,
            scratch.mip_count);
    }
    return true;
}

bool IsEnabled(reshade::api::device* device) noexcept {
    if (!device) {
        return false;
    }
    const DeviceData* data = device->get_private_data<DeviceData>();
    return IsRuntimeEnabled(data);
}

#if defined(SPATCH_GI_DEVELOPMENT)
void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain*,
    const reshade::api::rect*,
    const reshade::api::rect*,
    std::uint32_t,
    const reshade::api::rect*) {
    const std::uint64_t frame =
        g_present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !g_f10_was_down) {
        const bool enabled =
            !g_development_enabled.load(std::memory_order_relaxed);
        g_development_enabled.store(enabled, std::memory_order_relaxed);
        Log(reshade::log::level::info,
            "[ShenLong-GI] development toggle: enabled=%d present=%llu.",
            enabled ? 1 : 0,
            static_cast<unsigned long long>(frame));
    }
    g_f10_was_down = f10_down;
}
#endif

void Attach(HMODULE module) {
    g_module = module;
    reshade::register_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::register_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::init_command_list>(
        GuardedCallback<OnInitCommandList>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_command_list>(
        GuardedCallback<OnDestroyCommandList>::Invoke);
    reshade::register_event<reshade::addon_event::reset_command_list>(
        GuardedCallback<OnResetCommandList>::Invoke);
    reshade::register_event<reshade::addon_event::bind_pipeline>(
        GuardedCallback<OnBindPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
#if defined(SPATCH_GI_DEVELOPMENT)
    reshade::register_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
#endif
}

void Detach(bool process_terminating) noexcept {
    static_cast<void>(process_terminating);
#if defined(SPATCH_GI_DEVELOPMENT)
    reshade::unregister_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
#endif
    reshade::unregister_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(
        GuardedCallback<OnBindPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(
        GuardedCallback<OnResetCommandList>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(
        GuardedCallback<OnDestroyCommandList>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_command_list>(
        GuardedCallback<OnInitCommandList>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    g_module = nullptr;
}

}  // namespace spatch::graphics::gi
